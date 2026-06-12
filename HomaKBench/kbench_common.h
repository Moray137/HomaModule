/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KBENCH_COMMON_H
#define _KBENCH_COMMON_H

#include <linux/types.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <net/sock.h>
#include <net/inet_sock.h>

/* homa_sock.h depends on definitions provided by homa_impl.h (net headers,
 * union sockaddr_in_union, timetrace). Homa's own .c files always include
 * homa_impl.h first, so we do the same. homa_impl.h pulls in homa.h (uapi).
 */
#include "homa_impl.h"
#include "homa_sock.h"

#define KBENCH_DEFAULT_PORT	5000
#define KBENCH_MAX_MSG_SIZE	1400000
#define KBENCH_POOL_SIZE	(16 * 1024 * 1024)

#define KBENCH_REQ_PATTERN	0x42
#define KBENCH_REPLY_PATTERN	0x24

#define SOL_HOMA		IPPROTO_HOMA

static inline int kbench_homa_setsockopt(struct socket *sock, int optname,
					 void *optval, int optlen)
{
	return sock->sk->sk_prot->setsockopt(sock->sk, SOL_HOMA, optname,
					      KERNEL_SOCKPTR(optval), optlen);
}

/* TCP framing: 4-byte network-order length prefix before each message. */
#define KBENCH_TCP_HDR_SIZE	4

struct kbench_actor_ctx {
	char *app_buf;
	size_t buf_size;
	size_t bytes_received;
};

static inline int kbench_rx_actor(read_descriptor_t *desc, struct sk_buff *skb,
				  unsigned int offset, size_t len)
{
	struct kbench_actor_ctx *ctx = desc->arg.data;
	size_t space = ctx->buf_size - ctx->bytes_received;

	if (len > space)
		len = space;
	if (skb_copy_bits(skb, offset, ctx->app_buf + ctx->bytes_received, len))
		return -EFAULT;
	ctx->bytes_received += len;
	return len;
}

static inline int kbench_tcp_send_full(struct socket *sock, void *buf,
				       int len, int flags)
{
	int sent = 0;

	while (sent < len) {
		struct kvec iov = { .iov_base = buf + sent,
				    .iov_len = len - sent };
		struct msghdr msg = { .msg_flags = flags };
		int ret;

		ret = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
		if (ret <= 0)
			return ret ? ret : -EIO;
		sent += ret;
	}
	return sent;
}

static inline int kbench_tcp_recv_full(struct socket *sock, void *buf, int len)
{
	int recvd = 0;

	while (recvd < len) {
		struct kvec iov = { .iov_base = buf + recvd,
				    .iov_len = len - recvd };
		struct msghdr msg = {};
		int ret;

		ret = kernel_recvmsg(sock, &msg, &iov, 1, iov.iov_len, 0);
		if (ret <= 0)
			return ret ? ret : -EIO;
		recvd += ret;
	}
	return recvd;
}

#endif /* _KBENCH_COMMON_H */
