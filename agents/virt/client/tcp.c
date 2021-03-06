/*
  Copyright Red Hat, Inc. 2006-2012

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <nss.h>

/* Local includes */
#include "xvm.h"
#include "simple_auth.h"
#include "options.h"
#include "tcp.h"
#include "debug.h"
#include "fdops.h"
#include "client.h"

static int
tcp_exchange(int fd, fence_auth_type_t auth, void *key,
	      size_t key_len, int timeout)
{
	fd_set rfds;
	struct timeval tv;
	char ret = 1;

	/* Ok, we're connected */
	dbg_printf(3, "Issuing TCP challenge\n");
	if (sock_challenge(fd, auth, key, key_len, timeout) <= 0) {
		/* Challenge failed */
		printf("Invalid response to challenge\n");
		return 1;
	}

	/* Now they'll send us one, so we need to respond here */
	dbg_printf(3, "Responding to TCP challenge\n");
	if (sock_response(fd, auth, key, key_len, timeout) <= 0) {
		printf("Invalid response to challenge\n");
		return 1;
	}

	dbg_printf(2, "TCP Exchange + Authentication done... \n");

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	ret = 1;
	dbg_printf(3, "Waiting for return value from fence_virtd host\n");
	if (_select_retry(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
		return -1;

	/* Read return code */
	if (_read_retry(fd, &ret, 1, &tv) < 0)
		ret = 1;

	if (ret == (char)RESP_HOSTLIST) /* hostlist */ {
		do_read_hostlist(fd, timeout);
		ret = 0;
	}

	return ret;
}

int
tcp_fence_virt(fence_virt_args_t *args)
{
	char key[MAX_KEY_LEN];
	struct timeval tv;
	int key_len = 0, fd = -1;
	int ret;
	struct in_addr ina;
	struct in6_addr in6a;
	fence_req_t freq;

	/* Initialize NSS; required to do hashing, as silly as that
	   sounds... */
	if (NSS_NoDB_Init(NULL) != SECSuccess) {
		printf("Could not initialize NSS\n");
		return 1;
	}

	if (args->net.auth != AUTH_NONE || args->net.hash != HASH_NONE) {
		key_len = read_key_file(args->net.key_file, key, sizeof(key));
		if (key_len < 0) {
			printf("Could not read %s; trying without "
			       "authentication\n", args->net.key_file);
			args->net.auth = AUTH_NONE;
			args->net.hash = HASH_NONE;
			key_len = 0;
		}
	}

	/* Same wire protocol as fence_xvm */
	memset(&freq, 0, sizeof(freq));
	if (args->domain && strlen((char *)args->domain))
		strncpy((char *)freq.domain, args->domain, sizeof(freq.domain) - 1);
	freq.request = args->op;
	freq.hashtype = args->net.hash;
	freq.flags = 0;
	if (args->flags & F_USE_UUID)
		freq.flags |= RF_UUID;
	gettimeofday(&tv, NULL);
	freq.seqno = (uint32_t) tv.tv_usec;
	sign_request(&freq, key, key_len);

	/* XXX fixme */
	if (inet_pton(PF_INET, args->net.ipaddr, &ina)) {
		fd = ipv4_connect(&ina, args->net.port, 3);
	} else if (inet_pton(PF_INET6, args->net.ipaddr, &in6a)) {
		fd = ipv6_connect(&in6a, args->net.port, 3);
	}

	if (fd < 0) {
		printf("Unable to connect to fence_virtd host %s:%d %s\n",
			args->net.ipaddr, args->net.port, strerror(errno));
		return 1;
	}

	ret = _write_retry(fd, &freq, sizeof(freq), NULL);
	if (ret != sizeof(freq)) {
		perror("write");
		close(fd);
		return 1;
	}

	switch (args->net.auth) {
		case AUTH_NONE:
		case AUTH_SHA1:
		case AUTH_SHA256:
		case AUTH_SHA512:
			ret = tcp_exchange(fd, args->net.auth, key, key_len,
					    args->timeout);
			break;
		/* case AUTH_X509: 
			return ssl_exchange(...); */
		default:
			dbg_printf(3, "Unknown auth type: %d\n", args->net.auth);
			ret = 1;
			break;
	}

	close(fd);
	return ret;
}
