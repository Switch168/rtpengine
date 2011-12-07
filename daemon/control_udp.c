#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pcre.h>
#include <glib.h>
#include <time.h>

#include "control_udp.h"
#include "poller.h"
#include "aux.h"
#include "log.h"
#include "call.h"









static void control_udp_closed(int fd, void *p) {
	abort();
}

static void control_udp_incoming(int fd, void *p) {
	struct control_udp *u = p;
	int ret;
	char buf[8192];
	struct sockaddr_in sin;
	socklen_t sin_len;
	int ovec[60];
	const char **out;
	char *reply;
	struct msghdr mh;
	struct iovec iov[10];

	sin_len = sizeof(sin);
	ret = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &sin, &sin_len);
	if (ret <= 0) {
		mylog(LOG_WARNING, "Error reading from UDP socket");
		return;
	}

	buf[ret] = '\0';

	ret = pcre_exec(u->parse_re, u->parse_ree, buf, ret, 0, 0, ovec, G_N_ELEMENTS(ovec));
	if (ret <= 0) {
		mylog(LOG_WARNING, "Unable to parse command line from udp:" DF ": %s", DP(sin), buf);
		return;
	}

	mylog(LOG_INFO, "Got valid command from udp:" DF ": %s", DP(sin), buf);

	pcre_get_substring_list(buf, ovec, ret, &out);

	if (u->poller->now - u->oven_time >= 30) {
		g_hash_table_remove_all(u->stale_cookies);
		g_string_chunk_clear(u->stale_chunks);
		swap_ptrs(&u->stale_cookies, &u->fresh_cookies);
		swap_ptrs(&u->stale_chunks, &u->fresh_chunks);
		u->oven_time = u->poller->now;	/* baked new cookies! */
	}

	/* XXX better hashing */
	reply = g_hash_table_lookup(u->fresh_cookies, out[1]);
	if (!reply)
		reply = g_hash_table_lookup(u->stale_cookies, out[1]);
	if (reply) {
		mylog(LOG_INFO, "Detected command from udp:" DF " as a duplicate", DP(sin));
		sendto(fd, reply, strlen(reply), 0, (struct sockaddr *) &sin, sin_len);
		goto out;
	}

	if (out[2][0] == 'u' || out[2][0] == 'U')
		reply = call_update_udp(out, u->callmaster);
	else if (out[2][0] == 'l' || out[2][0] == 'L')
		reply = call_lookup_udp(out, u->callmaster);
	else if (out[9][0] == 'd' || out[9][0] == 'D')
		reply = call_delete_udp(out, u->callmaster);
	else if (out[12][0] == 'v' || out[12][0] == 'V') {
		ZERO(mh);
		mh.msg_name = &sin;
		mh.msg_namelen = sizeof(sin);
		mh.msg_iov = iov;
		mh.msg_iovlen = 2;

		iov[0].iov_base = (void *) out[1];
		iov[0].iov_len = strlen(out[1]);
		iov[1].iov_base = " ";
		iov[1].iov_len = 1;

		if (out[13][0] == 'f' || out[13][0] == 'F') {
			ret = 0;
			if (!strcmp(out[14], "20040107"))
				ret = 1;
			else if (!strcmp(out[14], "20050322"))
				ret = 1;
			else if (!strcmp(out[14], "20060704"))
				ret = 1;
			iov[2].iov_base = ret ? "1\n" : "0\n";
			iov[2].iov_len = 2;
			mh.msg_iovlen++;
		}
		else {
			iov[2].iov_base = "20040107\n";
			iov[2].iov_len = 9;
			mh.msg_iovlen++;
		}
		sendmsg(fd, &mh, 0);
	}

	if (reply) {
		sendto(fd, reply, strlen(reply), 0, (struct sockaddr *) &sin, sin_len);
		g_hash_table_insert(u->fresh_cookies, g_string_chunk_insert(u->fresh_chunks, out[1]),
			g_string_chunk_insert(u->fresh_chunks, reply));
		free(reply);
	}

out:
	pcre_free(out);
}

struct control_udp *control_udp_new(struct poller *p, u_int32_t ip, u_int16_t port, struct callmaster *m) {
	int fd;
	struct control_udp *c;
	struct poller_item i;
	struct sockaddr_in sin;
	const char *errptr;
	int erroff;

	if (!p || !m)
		return NULL;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1)
		return NULL;

	NONBLOCK(fd);
	REUSEADDR(fd);

	ZERO(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ip;
	sin.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)))
		goto fail;


	c = malloc(sizeof(*c));
	ZERO(*c);

	c->fd = fd;
	c->poller = p;
	c->callmaster = m;
	c->fresh_cookies = g_hash_table_new(g_str_hash, g_str_equal);
	c->stale_cookies = g_hash_table_new(g_str_hash, g_str_equal);
	c->fresh_chunks = g_string_chunk_new(4 * 1024);
	c->stale_chunks = g_string_chunk_new(4 * 1024);
	c->oven_time = p->now;
	c->parse_re = pcre_compile(
			/* cookie       cmd   flags    callid      addr        port   from_tag                 to_tag                                 cmd flags    callid */
			"^(\\S+)\\s+(?:([ul])(\\S*)\\s+(\\S+)\\s+([\\d.]+)\\s+(\\d+)\\s+(\\S+?)(?:;\\S+)?(?:\\s+(\\S+?)(?:;\\S+)?(?:\\s+.*)?)?\r?\n?$|(d)(\\S*)\\s+(\\S+)|(v)(\\S*)(?:\\s+(\\S+))?)",
			PCRE_DOLLAR_ENDONLY | PCRE_DOTALL | PCRE_CASELESS, &errptr, &erroff, NULL);
	c->parse_ree = pcre_study(c->parse_re, 0, &errptr);

	ZERO(i);
	i.fd = fd;
	i.closed = control_udp_closed;
	i.readable = control_udp_incoming;
	i.ptr = c;
	if (poller_add_item(p, &i))
		goto fail2;

	return c;

fail2:
	free(c);
fail:
	close(fd);
	return NULL;

}
