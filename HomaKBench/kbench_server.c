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

/* Compatibility parameter: enables both directions if the new independent
 * switches have not been set.
 */
static int rx_actor_used;
module_param(rx_actor_used, int, 0444);
MODULE_PARM_DESC(rx_actor_used, "Legacy: 1 enables both RX actor and TX splice");

static int tx_zc = -1;
module_param(tx_zc, int, 0444);
MODULE_PARM_DESC(tx_zc, "TX MSG_SPLICE_PAGES, independent of RX");

static int rx_single_copy = -1;
module_param(rx_single_copy, int, 0444);
/* Compatibility with the existing runner; only the client uses bio_vec.
 * The server always uses a synthetic NVMe-style scatterlist destination.
 */
static int rx_bvec = 1;
module_param(rx_bvec, int, 0444);
MODULE_PARM_DESC(rx_bvec, "Compatibility only; server RX destination is SGL");
MODULE_PARM_DESC(rx_single_copy, "RX full-message actor, independent of TX");

static int nonblocking;
module_param(nonblocking, int, 0444);
MODULE_PARM_DESC(nonblocking, "Use MSG_DONTWAIT with socket-readiness waits");

static int simulate_app_copy;
module_param(simulate_app_copy, int, 0444);
MODULE_PARM_DESC(simulate_app_copy, "Copy vanilla RX pool contents into an app-like buffer");

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
	struct page **reply_pages;
	int nr_reply_pages;
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

	memset(&recv_args, 0, sizeof(recv_args));

	while (!stopping && !kthread_should_stop()) {
		recv_args.id = 0;
		if (rx_single_copy) {
			kbench_actor_reset(&w->actor_ctx);
			recv_args.rx_actor_ctx =
				(__u64)(uintptr_t)&w->actor_ctx;
		} else {
			recv_args.rx_actor_ctx = 0;
		}

		memset(&rmsg, 0, sizeof(rmsg));
		rmsg.msg_control = &recv_args;
		rmsg.msg_controllen = sizeof(recv_args);
		rmsg.msg_name = &peer;
		rmsg.msg_namelen = sizeof(peer);
		rmsg.msg_control_is_user = false;

		ret = kbench_homa_recv(srv_sock, &rmsg, nonblocking);
		if (ret < 0) {
			if (stopping || kthread_should_stop())
				break;
			if (ret != -EINTR)
				pr_warn("kbench_server: recvmsg err %d\n", ret);
			continue;
		}
		if (ret <= 0 || ret > KBENCH_MAX_MSG_SIZE ||
		    (rx_single_copy && w->actor_ctx.bytes_received != ret) ||
		    (rx_single_copy &&
		     !kbench_actor_pattern_ok(&w->actor_ctx, ret,
					 KBENCH_REQ_PATTERN))) {
			pr_warn_ratelimited("kbench_server: invalid RX length or content %d\n", ret);
			continue;
		}
		if (!rx_single_copy && simulate_app_copy) {
			u64 app_copy_start = ktime_get_ns();
			/* Vanilla target: same final SGL destination as actor mode,
			 * but via Homa's receive pool (two copies total).
			 */
			int copied = 0, i, err = 0;
			char *pool = worker_state[0].pool_region;

			kbench_actor_reset(&w->actor_ctx);
			for (i = 0; i < recv_args.num_bpages && copied < ret; i++) {
				u32 off = recv_args.bpage_offsets[i];
				int len = min_t(int, ret - copied, HOMA_BPAGE_SIZE);

				err = kbench_sgl_copy(&w->actor_ctx, NULL, 0,
							    pool + off, len);
				if (err)
					break;
				copied += len;
			}
			INC_METRIC(rx_app_copy_ns,
				   ktime_get_ns() - app_copy_start);
			INC_METRIC(rx_app_copy_count, 1);
			INC_METRIC(rx_app_copy_bytes, copied);
			if (err || copied != ret ||
			    !kbench_actor_pattern_ok(&w->actor_ctx, ret,
							KBENCH_REQ_PATTERN)) {
				pr_warn_ratelimited("kbench_server: pool-to-SGL copy failed (%d/%d, err=%d)\n",
							    copied, ret, err);
				continue;
			}
		}

		/* Send response. */
		memset(&send_args, 0, sizeof(send_args));
		send_args.id = recv_args.id;

		memset(&smsg, 0, sizeof(smsg));
		smsg.msg_control = &send_args;
		smsg.msg_controllen = sizeof(send_args);
		smsg.msg_control_is_user = false;
		smsg.msg_name = &peer;
		smsg.msg_namelen = sizeof(peer);

		if (tx_zc) {
			/* Preallocated page-backed reply, no payload allocation
			 * inside the benchmark's timed request/response loop.
			 */
			int npages, i, remaining, reply_sz;
			struct bio_vec *bvecs;

			reply_sz = ret;
			npages = DIV_ROUND_UP(reply_sz, PAGE_SIZE);
			bvecs = kmalloc_array(npages, sizeof(*bvecs), GFP_KERNEL);
			if (!bvecs) {
				pr_warn("kbench_server: bvec allocation failed\n");
				continue;
			}
			remaining = reply_sz;
			for (i = 0; i < npages; i++) {
				int chunk = min_t(int, remaining, PAGE_SIZE);

				memset(page_address(w->reply_pages[i]),
				       KBENCH_REPLY_PATTERN, chunk);
				bvec_set_page(&bvecs[i], w->reply_pages[i],
					      chunk, 0);
				remaining -= chunk;
			}
			ret = kbench_homa_send(srv_sock, &smsg, NULL, reply_sz,
						bvecs, npages, true, nonblocking);
			kfree(bvecs);
		} else {
			int reply_sz = ret;

			memset(w->reply_buf, KBENCH_REPLY_PATTERN, reply_sz);
			siov.iov_base = w->reply_buf;
			siov.iov_len = reply_sz;
			ret = kbench_homa_send(srv_sock, &smsg, &siov, reply_sz,
						NULL, 0, false, nonblocking);
		}
		if (ret < 0 && !stopping && !kthread_should_stop())
			pr_warn_ratelimited("kbench_server: sendmsg error %d\n", ret);
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

	if (rx_single_copy) {
		struct homa_sock *hsk = homa_sk(srv_sock->sk);

		homa_sock_set_rx_actor(hsk, kbench_rx_actor);
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

	pr_info("kbench_server: Homa server listening on %s:%d (tx_zc=%d rx_sc=%d rx_dest=sgl nonblocking=%d workers=%d)\n",
		bind_ip, server_port, tx_zc, rx_single_copy,
		nonblocking, nr_workers);
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

	if (tx_zc < 0)
		tx_zc = !!rx_actor_used;
	if (rx_single_copy < 0)
		rx_single_copy = !!rx_actor_used;
	if ((tx_zc != 0 && tx_zc != 1) ||
	    (rx_bvec != 0 && rx_bvec != 1) ||
	    (rx_single_copy != 0 && rx_single_copy != 1) ||
	    (nonblocking != 0 && nonblocking != 1) ||
	    (simulate_app_copy != 0 && simulate_app_copy != 1))
		return -EINVAL;

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
		if (tx_zc && is_homa) {
			int npages = DIV_ROUND_UP(KBENCH_MAX_MSG_SIZE, PAGE_SIZE);
			int j;

			worker_state[i].reply_pages =
				kmalloc_array(npages, sizeof(struct page *),
					      GFP_KERNEL);
			if (!worker_state[i].reply_pages) {
				ret = -ENOMEM;
				goto err_bufs;
			}
			worker_state[i].nr_reply_pages = npages;
			for (j = 0; j < npages; j++) {
				worker_state[i].reply_pages[j] =
					alloc_page(GFP_KERNEL);
				if (!worker_state[i].reply_pages[j]) {
					ret = -ENOMEM;
					goto err_bufs;
				}
			}
		} else {
			worker_state[i].reply_buf =
				kvmalloc(KBENCH_MAX_MSG_SIZE, GFP_KERNEL);
			if (!worker_state[i].reply_buf) {
				ret = -ENOMEM;
				goto err_bufs;
			}
		}
		if (is_homa && (rx_single_copy || simulate_app_copy)) {
			/* Synthetic NVMe request SGL; no storage backend. */
			ret = kbench_actor_init_sgl(&worker_state[i].actor_ctx,
							   KBENCH_MAX_MSG_SIZE);
			if (ret)
				goto err_bufs;
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
		int j;

		if (worker_state[i].reply_pages) {
			for (j = 0; j < worker_state[i].nr_reply_pages; j++) {
				if (worker_state[i].reply_pages[j])
					__free_page(worker_state[i].reply_pages[j]);
			}
			kfree(worker_state[i].reply_pages);
		}
		kvfree(worker_state[i].reply_buf);
		kbench_actor_free(&worker_state[i].actor_ctx);
	}
	kfree(worker_state);
	kfree(workers);
	return ret;
}

static void __exit kbench_server_exit(void)
{
	int i;

	stopping = true;

	/* Shut down the listening socket first so that workers blocked in
	 * recvmsg/accept get woken with an error.
	 */
	if (srv_sock)
		kernel_sock_shutdown(srv_sock, SHUT_RDWR);

	/* Close TCP connections so TCP workers unblock. */
	if (tcp_conns) {
		for (i = 0; i < nr_tcp_conns; i++) {
			if (tcp_conns[i])
				kernel_sock_shutdown(tcp_conns[i], SHUT_RDWR);
		}
	}

	/* Now stop worker threads (they will see stopping==true or the
	 * socket error and exit).
	 */
	for (i = 0; i < nr_workers; i++) {
		if (workers[i])
			kthread_stop(workers[i]);
	}

	/* Release TCP connections. */
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
		int j;

		if (worker_state[i].reply_pages) {
			for (j = 0; j < worker_state[i].nr_reply_pages; j++) {
				if (worker_state[i].reply_pages[j])
					__free_page(worker_state[i].reply_pages[j]);
			}
			kfree(worker_state[i].reply_pages);
		}
		kvfree(worker_state[i].reply_buf);
		kbench_actor_free(&worker_state[i].actor_ctx);
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
