// SPDX-License-Identifier: GPL-2.0
/*
 * kbench_client.c - In-kernel benchmark client for Homa / TCP.
 *
 * Loaded via insmod with module_param. Runs for duration_sec, then reports
 * P50/P90/P99 latency via dmesg and debugfs. Unloaded via rmmod.
 *
 * Open-loop pacer: distributes RPCs across sockets at the target aggregate
 * rate. Each socket's kthread handles send+recv serially per RPC but can
 * have multiple RPCs queued by the pacer.
 */

#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/sort.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/hrtimer.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <net/sock.h>

#include "kbench_common.h"

/* Module parameters. */
static char *transport = "homa";
module_param(transport, charp, 0444);

static int zc;
module_param(zc, int, 0444);

static char *dst_ip = "192.168.11.93";
module_param(dst_ip, charp, 0444);

static int msg_size = 4096;
module_param(msg_size, int, 0444);

static int rate = 10000;
module_param(rate, int, 0444);

static int num_sockets = 4;
module_param(num_sockets, int, 0444);

static int simulate_app_copy;
module_param(simulate_app_copy, int, 0444);

static int duration_sec = 30;
module_param(duration_sec, int, 0444);

static int warmup_sec = 5;
module_param(warmup_sec, int, 0444);

static int server_port = KBENCH_DEFAULT_PORT;
module_param(server_port, int, 0444);

/* Max latency samples: 2M should be enough for 30s at 100k RPC/s. */
#define MAX_SAMPLES (2 * 1024 * 1024)

struct sock_ctx {
	int id;
	struct socket *sock;
	struct task_struct *thread;
	atomic_t pending;
	wait_queue_head_t wq;

	/* Buffers. */
	char *tx_buf;
	char *rx_buf;
	char *pool_region;
	char *app_copy_scratch;
	struct bio_vec *tx_bvecs;
	int nr_tx_bvecs;

	/* Homa-specific. */
	struct homa_recvmsg_args recv_args;
	struct kbench_actor_ctx actor_ctx;
};

static struct sock_ctx *sockets;
static u64 *latencies;
static atomic_t lat_count;
static u64 experiment_start_ns;
static volatile bool running;
static volatile bool past_warmup;
static struct task_struct *pacer_thread;
static struct dentry *dbg_dir;

/* Debugfs results buffer. */
static char *results_buf;
static int results_len;

/* ---------- Latency helpers ---------- */

static void record_latency(u64 ns)
{
	int idx = atomic_fetch_add(1, &lat_count);

	if (idx < MAX_SAMPLES)
		latencies[idx] = ns;
}

static int cmp_u64(const void *a, const void *b)
{
	u64 va = *(const u64 *)a;
	u64 vb = *(const u64 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static void compute_results(void)
{
	int n = min_t(int, atomic_read(&lat_count), MAX_SAMPLES);
	u64 p50, p90, p99;
	int achieved_rate;
	int elapsed_ms;

	if (n == 0) {
		pr_warn("kbench_client: no latency samples collected\n");
		return;
	}

	sort(latencies, n, sizeof(u64), cmp_u64, NULL);
	p50 = latencies[n / 2];
	p90 = latencies[(n * 90) / 100];
	p99 = latencies[(n * 99) / 100];

	elapsed_ms = (duration_sec - warmup_sec) * 1000;
	achieved_rate = elapsed_ms > 0 ? (int)((u64)n * 1000 / elapsed_ms) : 0;

	results_len = scnprintf(results_buf, PAGE_SIZE,
		"HomaKBench results:\n"
		"  transport=%s  zc=%d  msg_size=%d  rate=%d  num_sockets=%d\n"
		"  duration=%ds  warmup=%ds  simulate_app_copy=%d\n"
		"  completed RPCs: %d\n"
		"  P50: %llu.%01llu us\n"
		"  P90: %llu.%01llu us\n"
		"  P99: %llu.%01llu us\n"
		"  throughput: %d RPCs/sec (achieved)\n",
		transport, zc, msg_size, rate, num_sockets,
		duration_sec, warmup_sec, simulate_app_copy,
		n,
		p50 / 1000, (p50 % 1000) / 100,
		p90 / 1000, (p90 % 1000) / 100,
		p99 / 1000, (p99 % 1000) / 100,
		achieved_rate);

	pr_info("%s", results_buf);
}

/* ---------- Debugfs ---------- */

static ssize_t results_read(struct file *f, char __user *buf, size_t len,
			    loff_t *off)
{
	return simple_read_from_buffer(buf, len, off, results_buf, results_len);
}

static const struct file_operations results_fops = {
	.owner = THIS_MODULE,
	.read = results_read,
};

/* ---------- Homa RPC (one round-trip) ---------- */

static int do_homa_rpc(struct sock_ctx *sc)
{
	struct homa_sendmsg_args send_args;
	struct msghdr smsg, rmsg;
	struct sockaddr_in peer;
	int ret;

	/* TX */
	memset(&send_args, 0, sizeof(send_args));
	send_args.id = 0; /* new request */

	memset(&smsg, 0, sizeof(smsg));
	smsg.msg_control = &send_args;
	smsg.msg_controllen = sizeof(send_args);
	smsg.msg_control_is_user = false;
	smsg.msg_name = NULL;
	smsg.msg_namelen = 0;

	if (zc) {
		struct iov_iter iter;

		iov_iter_bvec(&smsg.msg_iter, ITER_SOURCE, sc->tx_bvecs,
			      sc->nr_tx_bvecs, msg_size);
		smsg.msg_flags |= MSG_SPLICE_PAGES;
		/* sock_sendmsg uses msg_iter as-is (kernel_sendmsg would
		 * overwrite it with the kvec).
		 */
		ret = sock_sendmsg(sc->sock, &smsg);
	} else {
		struct kvec iov = { .iov_base = sc->tx_buf,
				    .iov_len = msg_size };

		ret = kernel_sendmsg(sc->sock, &smsg, &iov, 1, msg_size);
	}
	if (ret < 0)
		return ret;

	/* RX. The worker is serial (one outstanding RPC per socket), so we wait
	 * for "any" reply (id=0). sc->recv_args persists across RPCs so its
	 * num_bpages/bpage_offsets carry the previous reply's bpages for Homa to
	 * recycle on this call.
	 */
	sc->recv_args.id = 0;
	sc->recv_args.completion_cookie = 0;

	if (zc)
		sc->actor_ctx.bytes_received = 0;

	memset(&rmsg, 0, sizeof(rmsg));
	rmsg.msg_control = &sc->recv_args;
	rmsg.msg_controllen = sizeof(sc->recv_args);
	rmsg.msg_name = &peer;
	rmsg.msg_namelen = sizeof(peer);
	rmsg.msg_control_is_user = false;

	ret = kernel_recvmsg(sc->sock, &rmsg, NULL, 0, 0, 0);
	if (ret < 0)
		return ret;

	/* Simulate app copy if configured and not using ZC. */
	if (simulate_app_copy && !zc && sc->app_copy_scratch &&
	    sc->recv_args.num_bpages > 0) {
		int copied = 0, i;

		for (i = 0; i < sc->recv_args.num_bpages && copied < ret; i++) {
			u32 off = sc->recv_args.bpage_offsets[i];
			int chunk = min_t(int, ret - copied,
					  HOMA_BPAGE_SIZE);

			memcpy(sc->app_copy_scratch + copied,
			       sc->pool_region + off, chunk);
			copied += chunk;
		}
	}

	return ret;
}

/* ---------- TCP RPC (one round-trip) ---------- */

static int do_tcp_rpc(struct sock_ctx *sc)
{
	__be32 net_len;
	int ret, reply_len;

	/* TX: length prefix + payload */
	net_len = htonl(msg_size);
	ret = kbench_tcp_send_full(sc->sock, &net_len, KBENCH_TCP_HDR_SIZE,
				   MSG_MORE);
	if (ret < 0)
		return ret;

	if (zc) {
		struct msghdr smsg = {};

		iov_iter_bvec(&smsg.msg_iter, ITER_SOURCE, sc->tx_bvecs,
			      sc->nr_tx_bvecs, msg_size);
		smsg.msg_flags = MSG_SPLICE_PAGES;
		ret = sock_sendmsg(sc->sock, &smsg);
	} else {
		ret = kbench_tcp_send_full(sc->sock, sc->tx_buf, msg_size, 0);
	}
	if (ret < 0)
		return ret;

	/* RX: length prefix + payload */
	ret = kbench_tcp_recv_full(sc->sock, &net_len, KBENCH_TCP_HDR_SIZE);
	if (ret < 0)
		return ret;

	reply_len = ntohl(net_len);
	ret = kbench_tcp_recv_full(sc->sock, sc->rx_buf, reply_len);
	return ret;
}

/* ---------- Per-socket worker thread ---------- */

static int worker_fn(void *data)
{
	struct sock_ctx *sc = data;
	bool is_homa = (strcmp(transport, "homa") == 0);

	while (!kthread_should_stop()) {
		u64 t_start, t_end;
		int ret;

		wait_event_interruptible(sc->wq,
			atomic_read(&sc->pending) > 0 ||
			kthread_should_stop());
		if (kthread_should_stop())
			break;
		if (atomic_read(&sc->pending) <= 0)
			continue;

		atomic_dec(&sc->pending);

		t_start = ktime_get_ns();
		ret = is_homa ? do_homa_rpc(sc) : do_tcp_rpc(sc);
		t_end = ktime_get_ns();

		if (ret >= 0 && past_warmup)
			record_latency(t_end - t_start);
	}
	return 0;
}

/* ---------- Pacer thread ---------- */

static int pacer_fn(void *data)
{
	u64 interval_ns = (rate > 0) ? (1000000000ULL / rate) : 1000000000ULL;
	int sock_idx = 0;
	u64 next_send;

	experiment_start_ns = ktime_get_ns();
	next_send = experiment_start_ns;

	while (running && !kthread_should_stop()) {
		u64 now = ktime_get_ns();
		u64 elapsed_ns = now - experiment_start_ns;

		if (elapsed_ns >= (u64)duration_sec * NSEC_PER_SEC) {
			running = false;
			break;
		}
		if (!past_warmup &&
		    elapsed_ns >= (u64)warmup_sec * NSEC_PER_SEC)
			past_warmup = true;

		if (now < next_send) {
			u64 wait_ns = next_send - now;

			if (wait_ns > 1000)
				ndelay(wait_ns);
			continue;
		}

		/* Dispatch RPC to next socket. */
		atomic_inc(&sockets[sock_idx].pending);
		wake_up(&sockets[sock_idx].wq);
		sock_idx = (sock_idx + 1) % num_sockets;
		next_send += interval_ns;
	}

	/* Signal all workers to stop. */
	running = false;
	{
		int i;
		for (i = 0; i < num_sockets; i++)
			wake_up(&sockets[i].wq);
	}

	/* Wait a bit for outstanding RPCs, then compute results. */
	msleep(2000);
	compute_results();
	return 0;
}

/* ---------- Socket setup ---------- */

static int setup_homa_socket(struct sock_ctx *sc, int idx)
{
	struct sockaddr_in src, dst;
	struct homa_rcvbuf_args rcvbuf;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_HOMA,
			       &sc->sock);
	if (ret)
		return ret;

	/* Bind source IP for RSS fan-out. */
	memset(&src, 0, sizeof(src));
	src.sin_family = AF_INET;
	src.sin_addr.s_addr = htonl(0x0A000001 + (idx % 16));
	ret = kernel_bind(sc->sock, (struct sockaddr *)&src, sizeof(src));
	if (ret) {
		pr_warn("kbench_client: bind src IP failed: %d\n", ret);
		goto err;
	}

	/* Connect to server. */
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = in_aton(dst_ip);
	dst.sin_port = htons(server_port);
	ret = kernel_connect(sc->sock, (struct sockaddr *)&dst, sizeof(dst), 0);
	if (ret) {
		pr_warn("kbench_client: connect failed: %d\n", ret);
		goto err;
	}

	/* Set up buffer pool. */
	sc->pool_region = kvmalloc(KBENCH_POOL_SIZE, GFP_KERNEL);
	if (!sc->pool_region) {
		ret = -ENOMEM;
		goto err;
	}
	rcvbuf.start = (u64)(uintptr_t)sc->pool_region;
	rcvbuf.length = KBENCH_POOL_SIZE;
	ret = kbench_homa_setsockopt(sc->sock, SO_HOMA_RCVBUF,
				    &rcvbuf, sizeof(rcvbuf));
	if (ret) {
		pr_warn("kbench_client: setsockopt RCVBUF failed: %d\n", ret);
		goto err;
	}

	/* Register rx_actor for ZC RX. */
	if (zc) {
		struct homa_sock *hsk = homa_sk(sc->sock->sk);

		homa_sock_set_rx_actor(hsk, kbench_rx_actor, &sc->actor_ctx);
	}

	return 0;
err:
	sock_release(sc->sock);
	sc->sock = NULL;
	return ret;
}

static int setup_tcp_socket(struct sock_ctx *sc, int idx)
{
	struct sockaddr_in src, dst;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &sc->sock);
	if (ret)
		return ret;

	memset(&src, 0, sizeof(src));
	src.sin_family = AF_INET;
	src.sin_addr.s_addr = htonl(0x0A000001 + (idx % 16));
	ret = kernel_bind(sc->sock, (struct sockaddr *)&src, sizeof(src));
	if (ret) {
		pr_warn("kbench_client: tcp bind failed: %d\n", ret);
		goto err;
	}

	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = in_aton(dst_ip);
	dst.sin_port = htons(server_port);
	ret = kernel_connect(sc->sock, (struct sockaddr *)&dst, sizeof(dst), 0);
	if (ret) {
		pr_warn("kbench_client: tcp connect failed: %d\n", ret);
		goto err;
	}

	return 0;
err:
	sock_release(sc->sock);
	sc->sock = NULL;
	return ret;
}

/* ---------- Module init/exit ---------- */

static int __init kbench_client_init(void)
{
	bool is_homa = (strcmp(transport, "homa") == 0);
	int i, ret;

	if (msg_size <= 0 || msg_size > KBENCH_MAX_MSG_SIZE) {
		pr_err("kbench_client: invalid msg_size %d\n", msg_size);
		return -EINVAL;
	}
	if (num_sockets <= 0 || num_sockets > 64) {
		pr_err("kbench_client: invalid num_sockets %d\n", num_sockets);
		return -EINVAL;
	}

	latencies = kvmalloc_array(MAX_SAMPLES, sizeof(u64), GFP_KERNEL);
	if (!latencies)
		return -ENOMEM;
	atomic_set(&lat_count, 0);

	results_buf = (char *)get_zeroed_page(GFP_KERNEL);
	if (!results_buf) {
		kvfree(latencies);
		return -ENOMEM;
	}

	sockets = kcalloc(num_sockets, sizeof(*sockets), GFP_KERNEL);
	if (!sockets) {
		ret = -ENOMEM;
		goto err_lat;
	}

	/* Allocate per-socket buffers and create sockets. */
	for (i = 0; i < num_sockets; i++) {
		struct sock_ctx *sc = &sockets[i];

		sc->id = i;
		init_waitqueue_head(&sc->wq);
		atomic_set(&sc->pending, 0);

		sc->tx_buf = kvmalloc(msg_size, GFP_KERNEL);
		sc->rx_buf = kvmalloc(msg_size, GFP_KERNEL);
		if (!sc->tx_buf || !sc->rx_buf) {
			ret = -ENOMEM;
			goto err_socks;
		}
		memset(sc->tx_buf, KBENCH_REQ_PATTERN, msg_size);

		/* Prepare bvec array for ZC TX. */
		if (zc) {
			int npages = DIV_ROUND_UP(msg_size, PAGE_SIZE);
			int remaining = msg_size, off = 0, j;

			sc->tx_bvecs = kmalloc_array(npages,
						     sizeof(struct bio_vec),
						     GFP_KERNEL);
			if (!sc->tx_bvecs) {
				ret = -ENOMEM;
				goto err_socks;
			}
			sc->nr_tx_bvecs = npages;
			for (j = 0; j < npages; j++) {
				int chunk = min_t(int, remaining, PAGE_SIZE);

				bvec_set_virt(&sc->tx_bvecs[j],
					      sc->tx_buf + off, chunk);
				off += chunk;
				remaining -= chunk;
			}
		}

		/* Actor buffer for ZC RX. */
		if (zc && is_homa) {
			sc->actor_ctx.app_buf = kvmalloc(msg_size, GFP_KERNEL);
			if (!sc->actor_ctx.app_buf) {
				ret = -ENOMEM;
				goto err_socks;
			}
			sc->actor_ctx.buf_size = msg_size;
		}

		/* Pool-to-app scratch for simulate_app_copy. */
		if (simulate_app_copy && !zc && is_homa) {
			sc->app_copy_scratch = kvmalloc(msg_size, GFP_KERNEL);
			if (!sc->app_copy_scratch) {
				ret = -ENOMEM;
				goto err_socks;
			}
		}

		if (is_homa)
			ret = setup_homa_socket(sc, i);
		else
			ret = setup_tcp_socket(sc, i);
		if (ret)
			goto err_socks;
	}

	/* Spawn per-socket worker threads. */
	for (i = 0; i < num_sockets; i++) {
		sockets[i].thread = kthread_create(worker_fn, &sockets[i],
						   "kbench_cli/%d", i);
		if (IS_ERR(sockets[i].thread)) {
			ret = PTR_ERR(sockets[i].thread);
			sockets[i].thread = NULL;
			goto err_threads;
		}
		kthread_bind(sockets[i].thread, i % num_online_cpus());
		wake_up_process(sockets[i].thread);
	}

	/* Debugfs. */
	dbg_dir = debugfs_create_dir("homakbench", NULL);
	if (dbg_dir)
		debugfs_create_file("results", 0444, dbg_dir, NULL,
				    &results_fops);

	/* Start pacer. */
	running = true;
	past_warmup = false;
	pacer_thread = kthread_run(pacer_fn, NULL, "kbench_pacer");
	if (IS_ERR(pacer_thread)) {
		ret = PTR_ERR(pacer_thread);
		pacer_thread = NULL;
		goto err_threads;
	}

	pr_info("kbench_client: started transport=%s zc=%d dst=%s msg=%d rate=%d socks=%d\n",
		transport, zc, dst_ip, msg_size, rate, num_sockets);
	return 0;

err_threads:
	running = false;
	for (i = 0; i < num_sockets; i++) {
		if (sockets[i].thread)
			kthread_stop(sockets[i].thread);
	}
err_socks:
	for (i = 0; i < num_sockets; i++) {
		struct sock_ctx *sc = &sockets[i];

		if (sc->sock)
			sock_release(sc->sock);
		kvfree(sc->tx_buf);
		kvfree(sc->rx_buf);
		kvfree(sc->pool_region);
		kvfree(sc->app_copy_scratch);
		kvfree(sc->actor_ctx.app_buf);
		kfree(sc->tx_bvecs);
	}
	kfree(sockets);
err_lat:
	free_page((unsigned long)results_buf);
	kvfree(latencies);
	return ret;
}

static void __exit kbench_client_exit(void)
{
	int i;

	running = false;

	if (pacer_thread)
		kthread_stop(pacer_thread);

	for (i = 0; i < num_sockets; i++) {
		if (sockets[i].thread)
			kthread_stop(sockets[i].thread);
	}

	debugfs_remove_recursive(dbg_dir);

	for (i = 0; i < num_sockets; i++) {
		struct sock_ctx *sc = &sockets[i];

		if (sc->sock)
			sock_release(sc->sock);
		kvfree(sc->tx_buf);
		kvfree(sc->rx_buf);
		kvfree(sc->pool_region);
		kvfree(sc->app_copy_scratch);
		kvfree(sc->actor_ctx.app_buf);
		kfree(sc->tx_bvecs);
	}
	kfree(sockets);
	free_page((unsigned long)results_buf);
	kvfree(latencies);

	pr_info("kbench_client: stopped\n");
}

module_init(kbench_client_init);
module_exit(kbench_client_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HomaKBench client: latency benchmark for Homa/TCP with ZC");
