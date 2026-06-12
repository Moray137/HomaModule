// SPDX-License-Identifier: GPL-2.0
/*
 * kbench_server.c - In-kernel benchmark server for Homa / TCP.
 *
 * Loaded via insmod with module_param; stopped via rmmod.
 * Many-to-one model: one Homa socket (or one TCP listener) serves all clients.
 * Spawns one kthread per online CPU to handle requests concurrently.
 */

#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <net/sock.h>

#include "kbench_common.h"

static char *transport = "homa";
module_param(transport, charp, 0444);
MODULE_PARM_DESC(transport, "Transport: homa or tcp");

static int rx_actor_used;
module_param(rx_actor_used, int, 0444);
MODULE_PARM_DESC(rx_actor_used, "1=ZC (RX actor + TX splice), 0=copy");

static char *bind_ip = "192.168.11.93";
module_param(bind_ip, charp, 0444);
MODULE_PARM_DESC(bind_ip, "IP to bind server socket");

static int server_port = KBENCH_DEFAULT_PORT;
module_param(server_port, int, 0444);
MODULE_PARM_DESC(server_port, "Port to bind");

static struct socket *srv_sock;
static struct task_struct **workers;
static int nr_workers;
static volatile bool stopping;

/* Per-worker state. */
struct server_worker {
	int id;
	char *reply_buf;
	char *pool_region;
	struct kbench_actor_ctx actor_ctx;
};

static struct server_worker *worker_state;

/* ---------- Homa server ---------- */

static int homa_worker_fn(void *data)
{
	struct server_worker *w = data;
	struct homa_recvmsg_args recv_args;
	struct homa_sendmsg_args send_args;
	struct sockaddr_in peer;
	struct msghdr rmsg, smsg;
	struct kvec siov;
	int ret;

	while (!stopping && !kthread_should_stop()) {
		memset(&recv_args, 0, sizeof(recv_args));
		recv_args.id = 0;

		memset(&rmsg, 0, sizeof(rmsg));
		rmsg.msg_control = &recv_args;
		rmsg.msg_controllen = sizeof(recv_args);
		rmsg.msg_name = &peer;
		rmsg.msg_namelen = sizeof(peer);
		rmsg.msg_control_is_user = false;

		ret = kernel_recvmsg(srv_sock, &rmsg, NULL, 0, 0,
				     MSG_DONTWAIT);
		if (ret == -EAGAIN) {
			schedule_timeout_interruptible(1);
			continue;
		}
		if (ret < 0) {
			if (!stopping)
				pr_warn("kbench_server: recvmsg err %d\n", ret);
			continue;
		}

		/* Reset actor ctx for next message. */
		if (rx_actor_used)
			w->actor_ctx.bytes_received = 0;

		/* Build reply: 0x24 pattern, same size as request. */
		memset(w->reply_buf, KBENCH_REPLY_PATTERN,
		       min_t(int, ret, KBENCH_MAX_MSG_SIZE));

		/* Send response. */
		memset(&send_args, 0, sizeof(send_args));
		send_args.id = recv_args.id;

		memset(&smsg, 0, sizeof(smsg));
		smsg.msg_control = &send_args;
		smsg.msg_controllen = sizeof(send_args);
		smsg.msg_control_is_user = false;
		smsg.msg_name = NULL;
		smsg.msg_namelen = 0;

		if (rx_actor_used) {
			/* ZC TX: use bvec iter + MSG_SPLICE_PAGES */
			int npages, i, remaining, off;
			struct bio_vec *bvecs;
			struct iov_iter iter;

			npages = DIV_ROUND_UP(ret, PAGE_SIZE);
			bvecs = kmalloc_array(npages, sizeof(*bvecs),
					      GFP_KERNEL);
			if (!bvecs) {
				pr_warn("kbench_server: bvec alloc failed\n");
				continue;
			}
			remaining = ret;
			off = 0;
			for (i = 0; i < npages; i++) {
				int chunk = min_t(int, remaining, PAGE_SIZE);

				bvec_set_virt(&bvecs[i], w->reply_buf + off,
					      chunk);
				off += chunk;
				remaining -= chunk;
			}
			iov_iter_bvec(&iter, ITER_SOURCE, bvecs, npages, ret);
			smsg.msg_iter = iter;
			smsg.msg_flags |= MSG_SPLICE_PAGES;

			kernel_sendmsg(srv_sock, &smsg, NULL, 0, ret);
			kfree(bvecs);
		} else {
			siov.iov_base = w->reply_buf;
			siov.iov_len = ret;
			kernel_sendmsg(srv_sock, &smsg, &siov, 1, ret);
		}
	}
	return 0;
}

static int start_homa_server(void)
{
	struct sockaddr_in addr;
	struct homa_rcvbuf_args rcvbuf;
	int ret, i;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_HOMA,
			       &srv_sock);
	if (ret) {
		pr_err("kbench_server: sock_create failed: %d\n", ret);
		return ret;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = in_aton(bind_ip);
	addr.sin_port = htons(server_port);
	ret = kernel_bind(srv_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret) {
		pr_err("kbench_server: bind failed: %d\n", ret);
		goto err_sock;
	}

	/* Set up buffer pool for the first worker (shared socket). */
	worker_state[0].pool_region = kvmalloc(KBENCH_POOL_SIZE, GFP_KERNEL);
	if (!worker_state[0].pool_region) {
		ret = -ENOMEM;
		goto err_sock;
	}
	rcvbuf.start = (u64)(uintptr_t)worker_state[0].pool_region;
	rcvbuf.length = KBENCH_POOL_SIZE;
	ret = kbench_homa_setsockopt(srv_sock, SO_HOMA_RCVBUF,
				    &rcvbuf, sizeof(rcvbuf));
	if (ret) {
		pr_err("kbench_server: setsockopt RCVBUF failed: %d\n", ret);
		goto err_pool;
	}

	/* Register actor if ZC. */
	if (rx_actor_used) {
		struct homa_sock *hsk = homa_sk(srv_sock->sk);

		homa_sock_set_rx_actor(hsk, kbench_rx_actor,
				       &worker_state[0].actor_ctx);
	}

	/* Spawn worker kthreads. */
	for (i = 0; i < nr_workers; i++) {
		workers[i] = kthread_create(homa_worker_fn, &worker_state[i],
					    "kbench_srv/%d", i);
		if (IS_ERR(workers[i])) {
			ret = PTR_ERR(workers[i]);
			workers[i] = NULL;
			goto err_threads;
		}
		kthread_bind(workers[i], i % num_online_cpus());
		wake_up_process(workers[i]);
	}

	pr_info("kbench_server: Homa server listening on %s:%d (zc=%d, workers=%d)\n",
		bind_ip, server_port, rx_actor_used, nr_workers);
	return 0;

err_threads:
	stopping = true;
	for (i = 0; i < nr_workers; i++) {
		if (workers[i])
			kthread_stop(workers[i]);
	}
err_pool:
	kvfree(worker_state[0].pool_region);
err_sock:
	sock_release(srv_sock);
	srv_sock = NULL;
	return ret;
}

/* ---------- TCP server ---------- */

static struct socket **tcp_conns;
static int nr_tcp_conns;
static DEFINE_MUTEX(tcp_conn_lock);

static int tcp_conn_worker_fn(void *data)
{
	struct socket *conn = data;
	char *buf;
	__be32 net_len;
	int ret, msg_len;

	buf = kvmalloc(KBENCH_MAX_MSG_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (!stopping && !kthread_should_stop()) {
		/* Read length prefix. */
		ret = kbench_tcp_recv_full(conn, &net_len, KBENCH_TCP_HDR_SIZE);
		if (ret <= 0) {
			if (!stopping)
				pr_info("kbench_server: tcp conn closed\n");
			break;
		}
		msg_len = ntohl(net_len);
		if (msg_len <= 0 || msg_len > KBENCH_MAX_MSG_SIZE)
			break;

		/* Read payload. */
		ret = kbench_tcp_recv_full(conn, buf, msg_len);
		if (ret <= 0)
			break;

		/* Build and send reply. */
		memset(buf, KBENCH_REPLY_PATTERN, msg_len);
		net_len = htonl(msg_len);
		kbench_tcp_send_full(conn, &net_len, KBENCH_TCP_HDR_SIZE, MSG_MORE);
		kbench_tcp_send_full(conn, buf, msg_len, 0);
	}

	kvfree(buf);
	return 0;
}

static int tcp_accept_fn(void *data)
{
	while (!stopping && !kthread_should_stop()) {
		struct socket *conn;
		struct task_struct *t;
		int ret;

		ret = kernel_accept(srv_sock, &conn, 0);
		if (ret < 0) {
			if (!stopping)
				schedule_timeout_interruptible(1);
			continue;
		}

		mutex_lock(&tcp_conn_lock);
		if (nr_tcp_conns < 256) {
			tcp_conns[nr_tcp_conns] = conn;
			nr_tcp_conns++;
		}
		mutex_unlock(&tcp_conn_lock);

		t = kthread_run(tcp_conn_worker_fn, conn,
				"kbench_tcp/%d", nr_tcp_conns);
		if (IS_ERR(t)) {
			sock_release(conn);
			pr_warn("kbench_server: failed to spawn conn handler\n");
		}
	}
	return 0;
}

static int start_tcp_server(void)
{
	struct sockaddr_in addr;
	int ret, opt = 1;

	tcp_conns = kcalloc(256, sizeof(*tcp_conns), GFP_KERNEL);
	if (!tcp_conns)
		return -ENOMEM;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &srv_sock);
	if (ret) {
		pr_err("kbench_server: tcp sock_create failed: %d\n", ret);
		goto err_free;
	}

	sock_setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR,
			KERNEL_SOCKPTR(&opt), sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = in_aton(bind_ip);
	addr.sin_port = htons(server_port);

	ret = kernel_bind(srv_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret) {
		pr_err("kbench_server: tcp bind failed: %d\n", ret);
		goto err_sock;
	}

	ret = kernel_listen(srv_sock, 64);
	if (ret) {
		pr_err("kbench_server: tcp listen failed: %d\n", ret);
		goto err_sock;
	}

	/* Spawn acceptor thread. */
	workers[0] = kthread_run(tcp_accept_fn, NULL, "kbench_accept");
	if (IS_ERR(workers[0])) {
		ret = PTR_ERR(workers[0]);
		workers[0] = NULL;
		goto err_sock;
	}

	pr_info("kbench_server: TCP server listening on %s:%d\n",
		bind_ip, server_port);
	return 0;

err_sock:
	sock_release(srv_sock);
	srv_sock = NULL;
err_free:
	kfree(tcp_conns);
	return ret;
}

/* ---------- Module init/exit ---------- */

static int __init kbench_server_init(void)
{
	int i, ret;
	bool is_homa = (strcmp(transport, "homa") == 0);

	nr_workers = num_online_cpus();
	workers = kcalloc(nr_workers, sizeof(*workers), GFP_KERNEL);
	if (!workers)
		return -ENOMEM;

	worker_state = kcalloc(nr_workers, sizeof(*worker_state), GFP_KERNEL);
	if (!worker_state) {
		kfree(workers);
		return -ENOMEM;
	}

	for (i = 0; i < nr_workers; i++) {
		worker_state[i].id = i;
		worker_state[i].reply_buf = kvmalloc(KBENCH_MAX_MSG_SIZE,
						     GFP_KERNEL);
		if (!worker_state[i].reply_buf) {
			ret = -ENOMEM;
			goto err_bufs;
		}
		if (rx_actor_used && is_homa) {
			worker_state[i].actor_ctx.app_buf =
				kvmalloc(KBENCH_MAX_MSG_SIZE, GFP_KERNEL);
			if (!worker_state[i].actor_ctx.app_buf) {
				ret = -ENOMEM;
				goto err_bufs;
			}
			worker_state[i].actor_ctx.buf_size = KBENCH_MAX_MSG_SIZE;
		}
	}

	if (is_homa)
		ret = start_homa_server();
	else
		ret = start_tcp_server();

	if (ret)
		goto err_bufs;
	return 0;

err_bufs:
	for (i = 0; i < nr_workers; i++) {
		kvfree(worker_state[i].reply_buf);
		kvfree(worker_state[i].actor_ctx.app_buf);
	}
	kfree(worker_state);
	kfree(workers);
	return ret;
}

static void __exit kbench_server_exit(void)
{
	int i;

	stopping = true;

	/* Stop worker threads. */
	for (i = 0; i < nr_workers; i++) {
		if (workers[i])
			kthread_stop(workers[i]);
	}

	/* Close TCP connections. */
	if (tcp_conns) {
		for (i = 0; i < nr_tcp_conns; i++) {
			if (tcp_conns[i])
				sock_release(tcp_conns[i]);
		}
		kfree(tcp_conns);
	}

	if (srv_sock)
		sock_release(srv_sock);

	for (i = 0; i < nr_workers; i++) {
		kvfree(worker_state[i].reply_buf);
		kvfree(worker_state[i].actor_ctx.app_buf);
		kvfree(worker_state[i].pool_region);
	}
	kfree(worker_state);
	kfree(workers);

	pr_info("kbench_server: stopped\n");
}

module_init(kbench_server_init);
module_exit(kbench_server_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HomaKBench server: echo benchmark for Homa/TCP with ZC");
