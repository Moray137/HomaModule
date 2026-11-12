/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KBENCH_COMMON_H
#define _KBENCH_COMMON_H

#include <linux/types.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
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

/* Homa returns a whole message. MSG_DONTWAIT only affects the syscall:
 * a worker may sleep on socket readiness between nonblocking attempts.
 * Register the wait entry BEFORE the first attempt to avoid missed wakeups.
 */
static inline int kbench_homa_recv(struct socket *sock, struct msghdr *msg,
				  bool nonblocking)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	int ret;

	if (!nonblocking)
		return kernel_recvmsg(sock, msg, NULL, 0, 0, 0);

	add_wait_queue(sk_sleep(sock->sk), &wait);
	for (;;) {
		ret = kernel_recvmsg(sock, msg, NULL, 0, 0, MSG_DONTWAIT);
		if (ret != -EAGAIN)
			break;
		if (kthread_should_stop()) {
			ret = -EINTR;
			break;
		}
		wait_woken(&wait, TASK_INTERRUPTIBLE,
			   msecs_to_jiffies(100));
	}
	remove_wait_queue(sk_sleep(sock->sk), &wait);
	return ret;
}

/* Homa kernel sendmsg returns 0 on success. -EAGAIN is retryable before
 * constructing an RPC (the current Homa path checks TX memory first).
 * Restore the iterator on every attempt, particularly for MSG_SPLICE_PAGES.
 */
static inline int kbench_homa_send(struct socket *sock, struct msghdr *msg,
				  const struct kvec *kvec, size_t len,
				  const struct bio_vec *bvec, int nr_bvec,
				  bool tx_zc, bool nonblocking)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	int ret;

	if (nonblocking)
		add_wait_queue(sk_sleep(sock->sk), &wait);
	for (;;) {
		msg->msg_flags = (nonblocking ? MSG_DONTWAIT : 0) |
				 (tx_zc ? MSG_SPLICE_PAGES : 0);
		if (tx_zc) {
			iov_iter_bvec(&msg->msg_iter, ITER_SOURCE, bvec,
				      nr_bvec, len);
			ret = sock_sendmsg(sock, msg);
		} else {
			ret = kernel_sendmsg(sock, msg, (struct kvec *)kvec,
					     1, len);
		}
		if (ret != -EAGAIN || !nonblocking)
			break;
		if (kthread_should_stop()) {
			ret = -EINTR;
			break;
		}
		wait_woken(&wait, TASK_INTERRUPTIBLE,
			   msecs_to_jiffies(100));
	}
	if (nonblocking)
		remove_wait_queue(sk_sleep(sock->sk), &wait);
	return ret;
}

/* Cheap corruption sentinels for the benchmark's constant-fill payload.
 * This is not a full content hash; it avoids a second full scan of RX data.
 */
static inline bool kbench_pattern_ok(const char *buf, int len, u8 pattern)
{
	return len > 0 && (u8)buf[0] == pattern &&
		(u8)buf[len / 2] == pattern &&
		(u8)buf[len - 1] == pattern;
}

/* TCP framing: 4-byte network-order length prefix before each message. */
#define KBENCH_TCP_HDR_SIZE	4

struct kbench_actor_ctx {
	char *app_buf;
	size_t buf_size;
	size_t bytes_received;
	bool use_bvec;
	bool use_sgl;
	struct bio_vec *bvecs;
	struct page **pages;
	int nr_pages;
	struct iov_iter iter;
	/* Server destination: a real, page-backed Linux scatterlist. */
	struct sg_table sgt;
	struct scatterlist *sg_cursor;
	size_t sg_offset;
};

static inline void kbench_actor_free(struct kbench_actor_ctx *ctx)
{
	int i;

	if (ctx->pages) {
		for (i = 0; i < ctx->nr_pages; i++)
			if (ctx->pages[i])
				__free_page(ctx->pages[i]);
	}
	if (ctx->sgt.sgl)
		sg_free_table(&ctx->sgt);
	kfree(ctx->pages);
	kfree(ctx->bvecs);
	kvfree(ctx->app_buf);
	ctx->pages = NULL;
	ctx->bvecs = NULL;
	ctx->app_buf = NULL;
	ctx->sgt.sgl = NULL;
	ctx->sg_cursor = NULL;
}

static inline int kbench_actor_init(struct kbench_actor_ctx *ctx,
				   size_t length, bool use_bvec)
{
	int i, remaining;

	ctx->buf_size = length;
	ctx->use_bvec = use_bvec;
	if (!use_bvec) {
		ctx->app_buf = kvmalloc(length, GFP_KERNEL);
		return ctx->app_buf ? 0 : -ENOMEM;
	}

	ctx->nr_pages = DIV_ROUND_UP(length, PAGE_SIZE);
	ctx->pages = kcalloc(ctx->nr_pages, sizeof(*ctx->pages), GFP_KERNEL);
	ctx->bvecs = kcalloc(ctx->nr_pages, sizeof(*ctx->bvecs), GFP_KERNEL);
	if (!ctx->pages || !ctx->bvecs)
		goto error;
	remaining = length;
	for (i = 0; i < ctx->nr_pages; i++) {
		int chunk = min_t(int, remaining, PAGE_SIZE);

		ctx->pages[i] = alloc_page(GFP_KERNEL);
		if (!ctx->pages[i])
			goto error;
		bvec_set_page(&ctx->bvecs[i], ctx->pages[i], chunk, 0);
		remaining -= chunk;
	}
	return 0;
error:
	kbench_actor_free(ctx);
	return -ENOMEM;
}

/* Allocate a synthetic NVMe-SGL destination once per server worker.
 * There is no block device or DMA mapping involved.
 */
static inline int kbench_actor_init_sgl(struct kbench_actor_ctx *ctx,
				       size_t length)
{
	struct scatterlist *sg;
	int i, remaining, ret;

	ctx->buf_size = length;
	ctx->use_sgl = true;
	ctx->nr_pages = DIV_ROUND_UP(length, PAGE_SIZE);
	ctx->pages = kcalloc(ctx->nr_pages, sizeof(*ctx->pages), GFP_KERNEL);
	if (!ctx->pages)
		goto error;

	ret = sg_alloc_table(&ctx->sgt, ctx->nr_pages, GFP_KERNEL);
	if (ret)
		goto error;

	remaining = length;
	sg = ctx->sgt.sgl;
	for (i = 0; i < ctx->nr_pages; i++, sg = sg_next(sg)) {
		int chunk = min_t(int, remaining, PAGE_SIZE);

		ctx->pages[i] = alloc_page(GFP_KERNEL);
		if (!ctx->pages[i])
			goto error;
		sg_set_page(sg, ctx->pages[i], chunk, 0);
		remaining -= chunk;
	}
	return 0;
error:
	kbench_actor_free(ctx);
	return -ENOMEM;
}

static inline void kbench_actor_reset(struct kbench_actor_ctx *ctx)
{
	ctx->bytes_received = 0;
	if (ctx->use_bvec)
		iov_iter_bvec(&ctx->iter, ITER_DEST, ctx->bvecs,
			      ctx->nr_pages, ctx->buf_size);
	if (ctx->use_sgl) {
		ctx->sg_cursor = ctx->sgt.sgl;
		ctx->sg_offset = 0;
	}
}

/* One direct copy to the page-backed scatterlist, without an
 * intermediate linear bounce buffer. Source can be an skb or a pool page.
 */
static inline int kbench_sgl_copy(struct kbench_actor_ctx *ctx,
				  struct sk_buff *skb, unsigned int skb_offset,
				  const char *pool_src, size_t len)
{
	size_t copied = 0;

	if (!ctx->use_sgl || len > ctx->buf_size - ctx->bytes_received)
		return -EMSGSIZE;

	while (copied < len) {
		struct scatterlist *sg = ctx->sg_cursor;
		void *mapped;
		size_t chunk;
		int ret = 0;

		if (!sg || ctx->sg_offset >= sg->length)
			return -EFAULT;
		chunk = min_t(size_t, len - copied,
				      sg->length - ctx->sg_offset);
		mapped = kmap_local_page(sg_page(sg));
		if (skb)
			ret = skb_copy_bits(skb, skb_offset + copied,
					    (char *)mapped + sg->offset +
					    ctx->sg_offset, chunk);
		else
			memcpy((char *)mapped + sg->offset + ctx->sg_offset,
			       pool_src + copied, chunk);
		kunmap_local(mapped);
		if (ret)
			return -EFAULT;

		copied += chunk;
		ctx->sg_offset += chunk;
		if (ctx->sg_offset == sg->length) {
			ctx->sg_cursor = sg_next(sg);
			ctx->sg_offset = 0;
		}
	}
	ctx->bytes_received += len;
	return 0;
}

static inline bool kbench_actor_pattern_ok(struct kbench_actor_ctx *ctx,
					   int len, u8 pattern)
{
	int offsets[3] = { 0, len / 2, len - 1 };
	int i;

	if (len <= 0 || len > ctx->buf_size)
		return false;
	if (!ctx->use_bvec && !ctx->use_sgl)
		return kbench_pattern_ok(ctx->app_buf, len, pattern);
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		int page_idx = offsets[i] / PAGE_SIZE;
		int off = offsets[i] % PAGE_SIZE;
		void *addr = kmap_local_page(ctx->pages[page_idx]);
		bool ok = *((u8 *)addr + off) == pattern;

		kunmap_local(addr);
		if (!ok)
			return false;
	}
	return true;
}

static inline int kbench_rx_actor(read_descriptor_t *desc, struct sk_buff *skb,
				  unsigned int offset, size_t len)
{
	struct kbench_actor_ctx *ctx = desc->arg.data;

	if (!ctx || len > ctx->buf_size - ctx->bytes_received)
		return -EMSGSIZE;
	if (ctx->use_sgl)
		return kbench_sgl_copy(ctx, skb, offset, NULL, len) ?
			-EFAULT : len;
	if (ctx->use_bvec) {
		if (skb_copy_datagram_iter(skb, offset, &ctx->iter, len))
			return -EFAULT;
	} else {
		if (skb_copy_bits(skb, offset,
				  ctx->app_buf + ctx->bytes_received, len))
			return -EFAULT;
	}
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
