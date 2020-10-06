/*
 * Generic code for native (BSD-compatible) sockets
 *
 * Copyright 2000-2020 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <net/if.h>

#include <haproxy/api.h>
#include <haproxy/connection.h>
#include <haproxy/listener-t.h>
#include <haproxy/namespace.h>
#include <haproxy/sock.h>
#include <haproxy/sock_inet.h>
#include <haproxy/tools.h>

/* the list of remaining sockets transferred from an older process */
struct xfer_sock_list *xfer_sock_list = NULL;

/* Create a socket to connect to the server in conn->dst (which MUST be valid),
 * using the configured namespace if needed, or the one passed by the proxy
 * protocol if required to do so. It ultimately calls socket() or socketat()
 * and returns the FD or error code.
 */
int sock_create_server_socket(struct connection *conn)
{
	const struct netns_entry *ns = NULL;

#ifdef USE_NS
	if (objt_server(conn->target)) {
		if (__objt_server(conn->target)->flags & SRV_F_USE_NS_FROM_PP)
			ns = conn->proxy_netns;
		else
			ns = __objt_server(conn->target)->netns;
	}
#endif
	return my_socketat(ns, conn->dst->ss_family, SOCK_STREAM, 0);
}

/*
 * Retrieves the source address for the socket <fd>, with <dir> indicating
 * if we're a listener (=0) or an initiator (!=0). It returns 0 in case of
 * success, -1 in case of error. The socket's source address is stored in
 * <sa> for <salen> bytes.
 */
int sock_get_src(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	if (dir)
		return getsockname(fd, sa, &salen);
	else
		return getpeername(fd, sa, &salen);
}

/*
 * Retrieves the original destination address for the socket <fd>, with <dir>
 * indicating if we're a listener (=0) or an initiator (!=0). It returns 0 in
 * case of success, -1 in case of error. The socket's source address is stored
 * in <sa> for <salen> bytes.
 */
int sock_get_dst(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	if (dir)
		return getpeername(fd, sa, &salen);
	else
		return getsockname(fd, sa, &salen);
}

/* Try to retrieve exported sockets from worker at CLI <unixsocket>. These
 * ones will be placed into the xfer_sock_list for later use by function
 * sock_find_compatible_fd(). Returns 0 on success, -1 on failure.
 */
int sock_get_old_sockets(const char *unixsocket)
{
	char *cmsgbuf = NULL, *tmpbuf = NULL;
	int *tmpfd = NULL;
	struct sockaddr_un addr;
	struct cmsghdr *cmsg;
	struct msghdr msghdr;
	struct iovec iov;
	struct xfer_sock_list *xfer_sock = NULL;
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	int sock = -1;
	int ret = -1;
	int ret2 = -1;
	int fd_nb;
	int got_fd = 0;
	int cur_fd = 0;
	size_t maxoff = 0, curoff = 0;

	memset(&msghdr, 0, sizeof(msghdr));
	cmsgbuf = malloc(CMSG_SPACE(sizeof(int)) * MAX_SEND_FD);
	if (!cmsgbuf) {
		ha_warning("Failed to allocate memory to send sockets\n");
		goto out;
	}

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		ha_warning("Failed to connect to the old process socket '%s'\n", unixsocket);
		goto out;
	}

	strncpy(addr.sun_path, unixsocket, sizeof(addr.sun_path) - 1);
	addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
	addr.sun_family = PF_UNIX;

	ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		ha_warning("Failed to connect to the old process socket '%s'\n", unixsocket);
		goto out;
	}

	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));
	iov.iov_base = &fd_nb;
	iov.iov_len = sizeof(fd_nb);
	msghdr.msg_iov = &iov;
	msghdr.msg_iovlen = 1;

	if (send(sock, "_getsocks\n", strlen("_getsocks\n"), 0) != strlen("_getsocks\n")) {
		ha_warning("Failed to get the number of sockets to be transferred !\n");
		goto out;
	}

	/* First, get the number of file descriptors to be received */
	if (recvmsg(sock, &msghdr, MSG_WAITALL) != sizeof(fd_nb)) {
		ha_warning("Failed to get the number of sockets to be transferred !\n");
		goto out;
	}

	if (fd_nb == 0) {
		ret2 = 0;
		goto out;
	}

	tmpbuf = malloc(fd_nb * (1 + MAXPATHLEN + 1 + IFNAMSIZ + sizeof(int)));
	if (tmpbuf == NULL) {
		ha_warning("Failed to allocate memory while receiving sockets\n");
		goto out;
	}

	tmpfd = malloc(fd_nb * sizeof(int));
	if (tmpfd == NULL) {
		ha_warning("Failed to allocate memory while receiving sockets\n");
		goto out;
	}

	msghdr.msg_control = cmsgbuf;
	msghdr.msg_controllen = CMSG_SPACE(sizeof(int)) * MAX_SEND_FD;
	iov.iov_len = MAX_SEND_FD * (1 + MAXPATHLEN + 1 + IFNAMSIZ + sizeof(int));

	do {
		int ret3;

		iov.iov_base = tmpbuf + curoff;

		ret = recvmsg(sock, &msghdr, 0);

		if (ret == -1 && errno == EINTR)
			continue;

		if (ret <= 0)
			break;

		/* Send an ack to let the sender know we got the sockets
		 * and it can send some more
		 */
		do {
			ret3 = send(sock, &got_fd, sizeof(got_fd), 0);
		} while (ret3 == -1 && errno == EINTR);

		for (cmsg = CMSG_FIRSTHDR(&msghdr); cmsg != NULL; cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
				size_t totlen = cmsg->cmsg_len - CMSG_LEN(0);

				if (totlen / sizeof(int) + got_fd > fd_nb) {
					ha_warning("Got to many sockets !\n");
					goto out;
				}

				/*
				 * Be paranoid and use memcpy() to avoid any
				 * potential alignment issue.
				 */
				memcpy(&tmpfd[got_fd], CMSG_DATA(cmsg), totlen);
				got_fd += totlen / sizeof(int);
			}
		}
		curoff += ret;
	} while (got_fd < fd_nb);

	if (got_fd != fd_nb) {
		ha_warning("We didn't get the expected number of sockets (expecting %d got %d)\n",
			   fd_nb, got_fd);
		goto out;
	}

	maxoff = curoff;
	curoff = 0;

	for (cur_fd = 0; cur_fd < got_fd; cur_fd++) {
		int fd = tmpfd[cur_fd];
		socklen_t socklen;
		int val;
		int len;

		xfer_sock = calloc(1, sizeof(*xfer_sock));
		if (!xfer_sock) {
			ha_warning("Failed to allocate memory in get_old_sockets() !\n");
			break;
		}
		xfer_sock->fd = -1;

		socklen = sizeof(xfer_sock->addr);
		if (getsockname(fd, (struct sockaddr *)&xfer_sock->addr, &socklen) != 0) {
			ha_warning("Failed to get socket address\n");
			free(xfer_sock);
			xfer_sock = NULL;
			continue;
		}

		if (curoff >= maxoff) {
			ha_warning("Inconsistency while transferring sockets\n");
			goto out;
		}

		len = tmpbuf[curoff++];
		if (len > 0) {
			/* We have a namespace */
			if (curoff + len > maxoff) {
				ha_warning("Inconsistency while transferring sockets\n");
				goto out;
			}
			xfer_sock->namespace = malloc(len + 1);
			if (!xfer_sock->namespace) {
				ha_warning("Failed to allocate memory while transferring sockets\n");
				goto out;
			}
			memcpy(xfer_sock->namespace, &tmpbuf[curoff], len);
			xfer_sock->namespace[len] = 0;
			xfer_sock->ns_namelen = len;
			curoff += len;
		}

		if (curoff >= maxoff) {
			ha_warning("Inconsistency while transferring sockets\n");
			goto out;
		}

		len = tmpbuf[curoff++];
		if (len > 0) {
			/* We have an interface */
			if (curoff + len > maxoff) {
				ha_warning("Inconsistency while transferring sockets\n");
				goto out;
			}
			xfer_sock->iface = malloc(len + 1);
			if (!xfer_sock->iface) {
				ha_warning("Failed to allocate memory while transferring sockets\n");
				goto out;
			}
			memcpy(xfer_sock->iface, &tmpbuf[curoff], len);
			xfer_sock->iface[len] = 0;
			xfer_sock->if_namelen = len;
			curoff += len;
		}

		if (curoff + sizeof(int) > maxoff) {
			ha_warning("Inconsistency while transferring sockets\n");
			goto out;
		}

		/* we used to have 32 bits of listener options here but we don't
		 * use them anymore.
		 */
		curoff += sizeof(int);

		/* determine the foreign status directly from the socket itself */
		if (sock_inet_is_foreign(fd, xfer_sock->addr.ss_family))
			xfer_sock->options |= SOCK_XFER_OPT_FOREIGN;

		socklen = sizeof(val);
		if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &val, &socklen) == 0 && val == SOCK_DGRAM)
			xfer_sock->options |= SOCK_XFER_OPT_DGRAM;

#if defined(IPV6_V6ONLY)
		/* keep only the v6only flag depending on what's currently
		 * active on the socket, and always drop the v4v6 one.
		 */
		socklen = sizeof(val);
		if (xfer_sock->addr.ss_family == AF_INET6 &&
		    getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, &socklen) == 0 && val > 0)
			xfer_sock->options |= SOCK_XFER_OPT_V6ONLY;
#endif

		xfer_sock->fd = fd;
		if (xfer_sock_list)
			xfer_sock_list->prev = xfer_sock;
		xfer_sock->next = xfer_sock_list;
		xfer_sock->prev = NULL;
		xfer_sock_list = xfer_sock;
		xfer_sock = NULL;
	}

	ret2 = 0;
out:
	/* If we failed midway make sure to close the remaining
	 * file descriptors
	 */
	if (tmpfd != NULL && cur_fd < got_fd) {
		for (; cur_fd < got_fd; cur_fd++) {
			close(tmpfd[cur_fd]);
		}
	}

	free(tmpbuf);
	free(tmpfd);
	free(cmsgbuf);

	if (sock != -1)
		close(sock);

	if (xfer_sock) {
		free(xfer_sock->namespace);
		free(xfer_sock->iface);
		if (xfer_sock->fd != -1)
			close(xfer_sock->fd);
		free(xfer_sock);
	}
	return (ret2);
}

/* When binding the receivers, check if a socket has been sent to us by the
 * previous process that we could reuse, instead of creating a new one. Note
 * that some address family-specific options are checked on the listener and
 * on the socket. Typically for AF_INET and AF_INET6, we check for transparent
 * mode, and for AF_INET6 we also check for "v4v6" or "v6only". The reused
 * socket is automatically removed from the list so that it's not proposed
 * anymore.
 */
int sock_find_compatible_fd(const struct receiver *rx)
{
	struct xfer_sock_list *xfer_sock = xfer_sock_list;
	int options = 0;
	int if_namelen = 0;
	int ns_namelen = 0;
	int ret = -1;

	if (!rx->proto->fam->addrcmp)
		return -1;

	if (rx->proto->sock_type == SOCK_DGRAM)
		options |= SOCK_XFER_OPT_DGRAM;

	if (rx->settings->options & RX_O_FOREIGN)
		options |= SOCK_XFER_OPT_FOREIGN;

	if (rx->addr.ss_family == AF_INET6) {
		/* Prepare to match the v6only option against what we really want. Note
		 * that sadly the two options are not exclusive to each other and that
		 * v6only is stronger than v4v6.
		 */
		if ((rx->settings->options & RX_O_V6ONLY) ||
		    (sock_inet6_v6only_default && !(rx->settings->options & RX_O_V4V6)))
			options |= SOCK_XFER_OPT_V6ONLY;
	}

	if (rx->settings->interface)
		if_namelen = strlen(rx->settings->interface);
#ifdef USE_NS
	if (rx->settings->netns)
		ns_namelen = rx->settings->netns->name_len;
#endif

	while (xfer_sock) {
		if ((options == xfer_sock->options) &&
		    (if_namelen == xfer_sock->if_namelen) &&
		    (ns_namelen == xfer_sock->ns_namelen) &&
		    (!if_namelen || strcmp(rx->settings->interface, xfer_sock->iface) == 0) &&
#ifdef USE_NS
		    (!ns_namelen || strcmp(rx->settings->netns->node.key, xfer_sock->namespace) == 0) &&
#endif
		    rx->proto->fam->addrcmp(&xfer_sock->addr, &rx->addr) == 0)
			break;
		xfer_sock = xfer_sock->next;
	}

	if (xfer_sock != NULL) {
		ret = xfer_sock->fd;
		if (xfer_sock == xfer_sock_list)
			xfer_sock_list = xfer_sock->next;
		if (xfer_sock->prev)
			xfer_sock->prev->next = xfer_sock->next;
		if (xfer_sock->next)
			xfer_sock->next->prev = xfer_sock->prev;
		free(xfer_sock->iface);
		free(xfer_sock->namespace);
		free(xfer_sock);
	}
	return ret;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
