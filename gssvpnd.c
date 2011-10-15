#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pwd.h>
#include <fcntl.h>
#include <stdio.h>
#include <gssapi/gssapi.h>
#include <unistd.h>
#include <net/if.h>
#include <net/ethernet.h>
#if defined(HAVE_IF_TUN)
#include <linux/if_tun.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#ifdef DARWIN
#include <ev.h>
#else
#include <libev/ev.h>
#endif
#define GSSVPN_SERVER
#include "gssvpn.h"

struct conn * clients_ip[255];
struct conn * clients_ether[255];
gss_cred_id_t srvcreds = GSS_C_NO_CREDENTIAL;
int verbose = 0;
int daemonize = 0;
int reapclients = 36000;
char * netinit_util = NULL;

int tapfd = -1, netfd = -1;
const uint64_t ether_broadcast = 0xffffffffffffffff;
const uint64_t ether_empty = 0x0000000000000000;

int get_server_creds(gss_cred_id_t * sco, char * service_name) {
	gss_buffer_desc name_buff;
	gss_name_t server_name;
	OM_uint32 maj_stat, min_stat;

	name_buff.value = service_name;
	name_buff.length = strlen(service_name);
	maj_stat = gss_import_name(&min_stat, &name_buff,
					(gss_OID) GSS_C_NT_HOSTBASED_SERVICE,
				   	&server_name);

	maj_stat = gss_acquire_cred(&min_stat, server_name, 0,
					GSS_C_NO_OID_SET, GSS_C_ACCEPT,
					sco, NULL, NULL);

	gss_release_name(&min_stat, &server_name);
	if(maj_stat != GSS_S_COMPLETE) {
		logit(1, "Error acquiring server credentials.");
		display_gss_err(maj_stat, min_stat);
		return -1;
	} else if(verbose)
		logit(-1, "Acquired credentials for %s", service_name);
	return 0;
}

void handle_shutdown(struct conn * client) {
	OM_uint32 min;

	logit(0, "Shutting down client %s:%d (%s)",
		client->ipstr, client->addr.sin_port, client->princname);

	unlink_conn(client, CLIENT_ALL);
	if(client->context != GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&min, &client->context, NULL);

	if(client->ni) {
		if(ev_is_active(&client->nipipe))
			ev_io_stop(client->loop, &client->nipipe);
		if(ev_is_active(&client->nichild))
			ev_child_stop(client->loop, &client->nichild);
		free(client->ni);
	}

	if(ev_is_active(&client->conntimeout))
		ev_timer_stop(client->loop, &client->conntimeout);
	if(client->princname)
		free(client->princname);

	free(client);
}

void netinit_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	struct conn * c = (struct conn*)ios->data;
	uint8_t buf[1024];
	size_t r, offset = 0;
	const uint64_t ether_null = 0;

	if(!c->ni) {
		logit(1, "Called netinit read for a null pointer!!");
		return;
	}

	while((r = read(ios->fd, buf, 1024)) > 0) {
		size_t tocopy;
		if(memcmp(c->mac, &ether_null, sizeof(c->mac)) == 0) {
			struct ether_addr * laddr;
			uint8_t * lock = buf;
			while(*lock != '\n') lock++;
			*lock++ = 0;
			laddr = ether_aton(buf);
			memcpy(c->mac, laddr, sizeof(c->mac));
			offset = (lock - buf);
		}
		tocopy = r - offset;

		if(tocopy + c->ni->len > sizeof(c->ni->payload))
			tocopy = sizeof(c->ni->payload) - c->ni->len;

		memcpy(c->ni->payload + c->ni->len, buf + offset, tocopy);
		c->ni->len += tocopy;
		if(c->ni->len == sizeof(c->ni->payload)) {
			ev_io_stop(loop, ios);
			return;
		}
	}
}

void conn_timeout_cb(struct ev_loop * loop, ev_timer * iot, int revents) {
	struct conn * c = (struct conn*)iot->data;

	logit(0, "Connection %s:%d (%s) has timed out.",
		c->ipstr, c->addr.sin_port, c->princname);

	send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN);
	handle_shutdown(c);
}

void netinit_child_cb(struct ev_loop * loop, ev_child * ioc, int revents) {
	struct conn * c = (struct conn*)ioc->data;
	const size_t tosend = c->ni->len + sizeof(c->ni->mac) + sizeof(uint16_t);
	gss_buffer_desc out;
	OM_uint32 maj, min, timeout;
	uint8_t eh;

	if(ioc->rstatus != 0) {
		logit(0, "Rejecting client %s:%d (%s)", c->ipstr,
			c->addr.sin_port, c->princname);
		send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN);
		handle_shutdown(c);
		return;
	}

	memcpy(c->mac, c->ni->mac, sizeof(c->mac));
	eh = hash(c->mac, sizeof(c->mac));
	unlink_conn(c, CLIENT_ETHERNET);
	c->ethernext = clients_ether[eh];
	clients_ether[eh] = c;

	maj = gss_context_time(&min, c->context, &timeout);
	if(maj != GSS_S_COMPLETE) {
		logit(1, "Unable to get remaining time for context");
		display_gss_err(maj, min);
		send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN);
		handle_shutdown(c);
		return;	
	}

	if(timeout < reapclients)
		timeout = reapclients;

	ev_timer_init(&c->conntimeout, conn_timeout_cb, timeout, 0);
	c->conntimeout.data = c;
	ev_start(loop, &c->conntimeout);

	c->ni->len = htons(c->ni->len);
	out.length = tosend;
	out.value = &c->ni;
	send_packet(netfd, &out, &c->addr, PAC_NETINIT);

	free(c->ni);
	c->ni = NULL;
	logit(0, "Client %s:%d (%s) is starting normal operation",
		c->ipstr, c->addr.sin_port, c->princname);
}

void tapfd_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	uint8_t framebuf[1550], dstmac[6];
	size_t size = read(ios->fd, framebuf, 1550);
	gss_buffer_desc plaintext = { size, framebuf }; 

	if(size == EAGAIN)
		return;

	memcpy(dstmac, framebuf, 6);
	if(memcmp(dstmac, &ether_broadcast, 6) == 0) {
		uint8_t i;
		for(i = 0; i < 255; i++) {
			struct conn * cur = clients_ether[i];
			while(cur) {
				send_packet(netfd, &plaintext, &cur->addr, PAC_DATA);
				cur = cur->ethernext;
			}
		}
		return;
	}
	uint8_t eh = hash(dstmac, 6);
	struct conn * client = clients_ether[eh];
	while(client && memcmp(client->mac, dstmac, 6) != 0)
		client = client->ethernext;
	if(!client) {
		if(verbose)
			logit(-1, "Received packet for unknown client");
		return;
	}

	send_packet(netfd, &plaintext, &client->addr, PAC_DATA);
}

void handle_netinit(struct ev_loop * loop, struct conn * client) {
	pid_t pid;
	int fds[2];

	if(client->ni)
		return;

	if(!netinit_util) {
		struct netinit ni;
		gss_buffer_desc out = { 8, &ni };
		int randfd = open("/dev/urandom", O_RDONLY);
		read(randfd, ni.mac, sizeof(ni.mac));
		ni.len = 0;
		close(randfd);
		logit(0, "Generating random MAC for %s:%d (%s)",
			client->ipstr, client->addr.sin_port, client->princname);
		send_packet(netfd, &out, &client->addr, PAC_NETINIT);
		return;
	}

	if(pipe(fds) < 0) {
		logit(1, "Error creating pipe during netinit %s", strerror(errno));
		send_packet(netfd, NULL, &client->addr, PAC_SHUTDOWN);
		handle_shutdown(client);
		return;
	}

	if(fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) {
		logit(1, "Error setting pipe to non-blocking during netinit %s",
			strerror(errno));
		send_packet(netfd, NULL, &client->addr, PAC_SHUTDOWN);
		handle_shutdown(client);
		return;
	}

	client->ni = malloc(sizeof(struct netinit));
	memset(client->ni, 0, sizeof(struct netinit));
	client->loop = loop;

	ev_io_init(&client->nipipe, netinit_read_cb, fds[0], EV_READ);
	client->nipipe.data = client;
	ev_io_start(loop, &client->nipipe);

	pid = fork();
	if(pid == 0) {
		char portstr[6];
		char * filename = netinit_util + (strlen(netinit_util) - 1);
		uint8_t i;
		OM_uint32 min;

		while(*filename != '/' && filename != netinit_util)
			filename--;
		if(*filename == '/')
			filename++;

		sprintf(portstr, "%d", client->addr.sin_port);
		close(fds[0]);
		close(tapfd);
		close(netfd);
		for(i = 0; i < 255; i++) {
			struct conn * c = clients_ip[i];
			while(c) {
				struct conn * save = c->ipnext;
				if(c->context != GSS_C_NO_CONTEXT)
					gss_delete_sec_context(&min, &c->context, NULL);
				free(c);
				c = save;
			}
		}

		dup2(fds[1], fileno(stdout));
		if(execl(netinit_util, filename, client->princname,
			client->ipstr, portstr, NULL) < 0)
			exit(-1);
	}
	
	if(verbose)
		logit(-1, "Waiting for netinit util to finish for %s:%d (%s)",
			client->ipstr, client->addr.sin_port, client->princname);
	ev_child_init(&client->nichild, netinit_child_cb, pid, 0);
	client->nichild.data = client;
	ev_child_start(loop, &client->nichild);
}

void handle_gssinit(struct ev_loop * loop, struct conn * client,
	gss_buffer_desc * intoken) {
	gss_name_t client_name;
	gss_buffer_desc output, nameout;
	OM_uint32 flags, lmin, maj, min;
	int nameeq = 0;

	if(client->gssstate == GSS_S_COMPLETE && 
					client->context != GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&lmin, &client->context, NULL);
	client->context = GSS_C_NO_CONTEXT;

	maj = gss_accept_sec_context(&min, &client->context, srvcreds, intoken,
					NULL, &client_name, NULL, &output, &flags, NULL, NULL);
	if(maj != GSS_S_COMPLETE && maj != GSS_S_CONTINUE_NEEDED) {
		logit(1, "Error accepting security context from %s", client->ipstr);
		display_gss_err(maj, min);
		return;
	}
	client->gssstate = maj;
	if(maj == GSS_S_CONTINUE_NEEDED) {
		send_packet(netfd, &output, &client->addr, PAC_GSSINIT);
		return;
	}

	logit(0, "Accepted connection for %s from %s",
		nameout.value, client->ipstr);
	client->princname = strdup(nameout.value);
	gss_release_buffer(&lmin, &nameout);
	gss_release_name(&lmin, &client_name);
	handle_netinit(loop, client);
}

void netfd_read_cb(struct ev_loop * loop, ev_io * ios, int revents) {
	gss_buffer_desc packet = GSS_C_EMPTY_BUFFER;
	char pac;
	struct sockaddr_in peer;
	struct conn * client;
	OM_uint32 min;

	if(recv_packet(netfd, &packet, &pac, &peer) != 0)
		return;

	client = get_conn(&peer);
	if(!client)
		return;

	if(client->gssstate == GSS_S_CONTINUE_NEEDED && pac != PAC_GSSINIT) {
		send_packet(netfd, NULL, &client->addr, PAC_GSSINIT);
		if(packet.length)
			gss_release_buffer(&min, &packet);
		return;
	}

	if(pac == PAC_DATA && memcmp(client->mac, &ether_empty,
		sizeof(ether_empty)) == 0) {
		send_packet(netfd, NULL, &client->addr, PAC_NETINIT);
		if(packet.length)
			gss_release_buffer(&min, &packet);
		return;
	}

	if(pac == PAC_DATA && packet.length > 0) {
		if(verbose)
			logit(-1, "Writing %d bytes to tap", packet.length);
		size_t s = write(tapfd, packet.value, packet.length);
		if(s < 0)
			logit(1, "Error writing to tap: %s", strerror(errno));
		else if(s < packet.length && verbose)
			logit(1, "Wrote less than expected to tap: %s < %s",
				s, packet.length);
		gss_release_buffer(&min, &packet);
	}
	else if(pac == PAC_GSSINIT)
		handle_gssinit(loop, client, &packet);
	else if(pac == PAC_NETINIT)
		handle_netinit(loop, client);
	else if(pac == PAC_SHUTDOWN)
		handle_shutdown(client);

	if(packet.value)
		gss_release_buffer(&min, &packet);
}

void term_cb(struct ev_loop * l, ev_signal * w, int r) {
	OM_uint32 min;
	uint8_t i;
	
	for(i = 0; i < 255; i++) {
		struct conn * c = clients_ip[i];
		while(c) {
			struct conn * save = c->ipnext;
			send_packet(netfd, NULL, &c->addr, PAC_SHUTDOWN);
			handle_shutdown(c);
			c = save;
		}
	}

	close(tapfd);
	close(netfd);

	ev_break(l, EVBREAK_ALL);
}

int main(int argc, char ** argv) {
	int rc;
	ev_io tapio, netio;
	ev_signal term;
	struct ev_loop * loop;
	openlog("gssvpnd", 0, LOG_DAEMON);
	char ch;
	short port = 2106;
	struct oc * cur;
	uid_t dropto = 0;

	while((ch = getopt(argc, argv, "ds:p:i:va:u:t:")) != -1) {
		switch(ch) {
			case 'v':
				verbose = 1;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'i':
				tapfd = open_tap("tap0");
				break;
			case 's':
				rc = get_server_creds(&srvcreds, optarg);
				if(rc != 0)
					return -1;
				break;
			case 'a': {
				if(!access(optarg, R_OK|X_OK)) {
					logit(1, "Unable to access %s for read/execute: %s",
						optarg, strerror(errno));
					return -1;
				}
				netinit_util = strdup(optarg);
				break;
			}
			case 'u': {
				struct passwd * u = getpwnam(optarg);
				if(!u) {
					logit(1, "Error doing user lookup for %s: (%s)",
						optarg, strerror(errno));
					return -1;
				}
				dropto = u->pw_uid;
			}
			case 't':
				reapclients = atoi(optarg);
			case 'd':
				daemonize = 1;
		}
	}

	if(srvcreds == GSS_C_NO_CREDENTIAL) {
		rc = get_server_creds(&srvcreds, "gssvpn");
		if(rc != 0)
			return -1;
	}

	netfd = open_net(port);
	if(netfd < 0)
		return -1;

	if(tapfd < 0) {
		logit(1, "No tap device defined");
		return -1;
	}

	if(dropto)
		setuid(dropto);
	
	hup_cb(loop, NULL, 0);
	if(daemonize)
		daemon(0, 0);
	
	memset(clients_ip, 0, sizeof(struct conn*) * 255);
	memset(clients_ether, 0, sizeof(struct conn*) * 255);

	loop = ev_default_loop(0);
	ev_io_init(&netio, netfd_read_cb, netfd, EV_READ);
	ev_io_start(loop, &netio);
	ev_io_init(&tapio, tapfd_read_cb, tapfd, EV_READ);
	ev_io_start(loop, &tapio);
	ev_signal_init(&term, term_cb, SIGTERM | SIGQUIT);
	ev_signal_start(loop, &term);
	ev_run(loop, 0);

	return 0;
}
