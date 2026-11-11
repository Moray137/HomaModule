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
#include <linux/random.h>
#include <net/sock.h>

#include "kbench_common.h"

/* Module parameters. */
static char *transport = "homa";
module_param(transport, charp, 0444);

static int zc;
module_param(zc, int, 0444);

/* tx_zc supersedes the old zc parameter if explicitly supplied. */
static int tx_zc = -1;
module_param(tx_zc, int, 0444);

static int rx_single_copy;
module_param(rx_single_copy, int, 0444);

static int nonblocking;
module_param(nonblocking, int, 0444);

/* Synthetic six-size workload (request fractions in percent).
 * 8KiB dominates and 4KiB represents the bottom 10% critical class.
 * This is NOT the exact original blksw trace distribution.
 */
#define KBENCH_MIX_CLASSES 6
static const int mix_sizes[KBENCH_MIX_CLASSES] = {
	4096, 8192, 12288, 16384, 131072, 1048576
};
static int blksw_weights[KBENCH_MIX_CLASSES] = { 10, 57, 10, 10, 8, 5 };
static unsigned int weights_count = KBENCH_MIX_CLASSES;
module_param_array_named(blksw_weights, blksw_weights, int,
			 &weights_count, 0444);
static char *workload = "fixed";
module_param(workload, charp, 0444);
static bool mixed_workload;
static int max_msg_size;
static int mix_cumulative[KBENCH_MIX_CLASSES];

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
	struct page **tx_pages;
	int nr_tx_pages;

	/* Homa-specific. */
	struct homa_recvmsg_args recv_args;
	struct kbench_actor_ctx actor_ctx;
};

static struct sock_ctx *sockets;
static u64 *latencies;
static u8 *lat_classes;
static atomic_t lat_count;
static atomic_t offered_count;
static atomic_t skipped_count;
static atomic_t failures;
static u64 experiment_start_ns;
static volatile bool running;
static volatile bool past_warmup;
static struct task_struct *pacer_thread;
static struct dentry *dbg_dir;

/* Debugfs results buffer. */
static char *results_buf;
static int results_len;

/* ---------- Latency helpers ---------- */

static int kbench_pick_size(void)
{
	u32 draw;
	int i;

	if (!mixed_workload)
		return msg_size;
	draw = get_random_u32_below(100);
	for (i = 0; i < KBENCH_MIX_CLASSES; i++)
		if (draw < mix_cumulative[i])
			return mix_sizes[i];
	return mix_sizes[KBENCH_MIX_CLASSES - 1];
}

static u8 kbench_size_class(int size)
{
	int i;

	for (i = 0; i < KBENCH_MIX_CLASSES; i++)
		if (mix_sizes[i] == size)
			return i;
	return U8_MAX;
}

static void record_latency(u64 ns, u8 cls)
{
	int idx = atomic_fetch_add(1, &lat_count);

	if (idx < MAX_SAMPLES) {
		latencies[idx] = ns;
		lat_classes[idx] = cls;
	}
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
	int achieved_rate, elapsed_ms;
	u64 *scratch = NULL;
	u64 *sorted;
	int cls, i;

	if (n == 0) {
		pr_warn("kbench_client: no latency samples collected\n");
		return;
	}

	/* Preserve the unsorted latency/class pairs for per-size quantiles.
	 * Size-class logging is only one byte per completed RPC.
	 */
	if (mixed_workload)
		scratch = kvmalloc_array(n, sizeof(*scratch), GFP_KERNEL);
	if (scratch) {
		memcpy(scratch, latencies, n * sizeof(*scratch));
		sorted = scratch;
	} else {
		sorted = latencies;
	}

	sort(sorted, n, sizeof(*sorted), cmp_u64, NULL);
	p50 = sorted[n / 2];
	p90 = sorted[(n * 90) / 100];
	p99 = sorted[(n * 99) / 100];

	elapsed_ms = (duration_sec - warmup_sec) * 1000;
	achieved_rate = elapsed_ms > 0 ?
		(int)((u64)n * 1000 / elapsed_ms) : 0;

	results_len = scnprintf(results_buf, PAGE_SIZE,
		"HomaKBench results:\n"
		"  transport=%s tx_zc=%d rx_single_copy=%d nonblocking=%d\n"
		"  workload=%s fixed_msg_size=%d rate=%d num_sockets=%d\n"
		"  duration=%ds warmup=%ds simulate_app_copy=%d\n"
		"  offered_slots_total=%d skipped_busy_total=%d failed_rpc=%d\n"
		"  completed RPCs: %d\n"
		"  P50: %llu.%01llu us\n"
		"  P90: %llu.%01llu us\n"
		"  P99: %llu.%01llu us\n"
		"  throughput: %d RPCs/sec (achieved)\n",
		transport, tx_zc, rx_single_copy, nonblocking,
		workload, msg_size, rate, num_sockets,
		duration_sec, warmup_sec, simulate_app_copy,
		atomic_read(&offered_count),
		atomic_read(&skipped_count), atomic_read(&failures),
		n,
		p50 / 1000, (p50 % 1000) / 100,
		p90 / 1000, (p90 % 1000) / 100,
		p99 / 1000, (p99 % 1000) / 100,
		achieved_rate);

	/* At end of the run only: derive each size-class percentile from
	 * the recorded samples. Failure to allocate scratch affects only
	 * the breakdown, not aggregate results.
	 */
	if (mixed_workload && scratch) {
		for (cls = 0; cls < KBENCH_MIX_CLASSES; cls++) {
			int count = 0;

			for (i = 0; i < n; i++)
				if (lat_classes[i] == cls)
					scratch[count++] = latencies[i];
			if (!count)
				continue;
			sort(scratch, count, sizeof(*scratch), cmp_u64, NULL);
			results_len += scnprintf(results_buf + results_len,
				PAGE_SIZE - results_len,
				"  %d KiB: n=%d p50=%llu.%01llu us p99=%llu.%01llu us\n",
				mix_sizes[cls] / 1024, count,
				scratch[count / 2] / 1000,
				(scratch[count / 2] % 1000) / 100,
				scratch[(count * 99) / 100] / 1000,
				(scratch[(count * 99) / 100] % 1000) / 100);
		}
	}
	kvfree(scratch);
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

static int do_homa_rpc(struct sock_ctx *sc, int size)
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

	{
		struct kvec iov = { .iov_base = sc->tx_buf,
				    .iov_len = size };

		ret = kbench_homa_send(sc->sock, &smsg, &iov, size,
					 sc->tx_bvecs, sc->nr_tx_bvecs,
					 tx_zc, nonblocking);
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

	if (rx_single_copy) {
		sc->actor_ctx.bytes_received = 0;
		sc->recv_args.rx_actor_ctx =
			(__u64)(uintptr_t)&sc->actor_ctx;
	} else {
		sc->recv_args.rx_actor_ctx = 0;
	}

	memset(&rmsg, 0, sizeof(rmsg));
	rmsg.msg_control = &sc->recv_args;
	rmsg.msg_controllen = sizeof(sc->recv_args);
	rmsg.msg_name = &peer;
	rmsg.msg_namelen = sizeof(peer);
	rmsg.msg_control_is_user = false;

	ret = kbench_homa_recv(sc->sock, &rmsg, nonblocking);
	if (ret < 0)
		return ret;
	if (ret != size ||
	    (rx_single_copy && sc->actor_ctx.bytes_received != ret))
		return -EIO;

	/* Simulate the second pool-to-app copy for baseline mode. */
	if (simulate_app_copy && !rx_single_copy && sc->app_copy_scratch &&
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

static int do_tcp_rpc(struct sock_ctx *sc, int size)
{
	__be32 net_len;
	int ret, reply_len;

	/* TX: length prefix + payload */
	net_len = htonl(size);
	ret = kbench_tcp_send_full(sc->sock, &net_len, KBENCH_TCP_HDR_SIZE,
				   MSG_MORE);
	if (ret < 0)
		return ret;

	if (tx_zc) {
		struct msghdr smsg = {};

		iov_iter_bvec(&smsg.msg_iter, ITER_SOURCE, sc->tx_bvecs,
			      sc->nr_tx_bvecs, size);
		smsg.msg_flags = MSG_SPLICE_PAGES;
		ret = sock_sendmsg(sc->sock, &smsg);
	} else {
		ret = kbench_tcp_send_full(sc->sock, sc->tx_buf, size, 0);
	}
	if (ret < 0)
		return ret;

	/* RX: length prefix + payload */
	ret = kbench_tcp_recv_full(sc->sock, &net_len, KBENCH_TCP_HDR_SIZE);
	if (ret < 0)
		return ret;

	reply_len = ntohl(net_len);
	if (reply_len != size || reply_len > max_msg_size)
		return -EIO;
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

		/* pending remains 1 while this RPC is in flight. The pacer
		 * can schedule at most one request per socket.
		 */
		{
			int size = kbench_pick_size();

			t_start = ktime_get_ns();
			ret = is_homa ? do_homa_rpc(sc, size) :
					do_tcp_rpc(sc, size);
			t_end = ktime_get_ns();

			if (ret >= 0 && past_warmup)
				record_latency(t_end - t_start,
					       kbench_size_class(size));
			else if (ret < 0) {
				atomic_inc(&failures);
				pr_warn_ratelimited("kbench_client: RPC failed: %d\n", ret);
			}
		}
		atomic_set(&sc->pending, 0);
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

			if (wait_ns > 10000) {
				unsigned long us = (unsigned long)(wait_ns / 1000);
				usleep_range(us, us + 1);
			} else if (wait_ns > 100) {
				ndelay(wait_ns);
			}
			continue;
		}

		/* Rate-controlled offered slots, bounded by one in-flight RPC
		 * per socket. Count skipped slots instead of building a backlog.
		 */
		atomic_inc(&offered_count);
		if (atomic_cmpxchg(&sockets[sock_idx].pending, 0, 1) == 0)
			wake_up(&sockets[sock_idx].wq);
		else
			atomic_inc(&skipped_count);
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

	/* Park until rmmod calls kthread_stop(); returning early would free
	 * the task_struct, causing a use-after-free in kbench_client_exit.
	 */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
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

	if (rx_single_copy)
		homa_sock_set_rx_actor(homa_sk(sc->sock->sk),
				       kbench_rx_actor);

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

	int j, total = 0;

	if (tx_zc < 0)
		tx_zc = zc;
	if ((tx_zc != 0 && tx_zc != 1) ||
	    (rx_single_copy != 0 && rx_single_copy != 1) ||
	    (nonblocking != 0 && nonblocking != 1) ||
	    rate <= 0 || duration_sec <= warmup_sec || warmup_sec < 0)
		return -EINVAL;

	if (!strcmp(workload, "blksw6")) {
		mixed_workload = true;
		if (weights_count != KBENCH_MIX_CLASSES)
			return -EINVAL;
		for (j = 0; j < KBENCH_MIX_CLASSES; j++) {
			if (blksw_weights[j] < 0)
				return -EINVAL;
			total += blksw_weights[j];
			mix_cumulative[j] = total;
		}
		if (total != 100)
			return -EINVAL;
		max_msg_size = mix_sizes[KBENCH_MIX_CLASSES - 1];
	} else if (!strcmp(workload, "fixed")) {
		max_msg_size = msg_size;
	} else {
		return -EINVAL;
	}
	if (max_msg_size > KBENCH_MAX_MSG_SIZE ||
	    (is_homa && max_msg_size > HOMA_MAX_MESSAGE_LENGTH))
		return -EINVAL;

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
	atomic_set(&offered_count, 0);
	atomic_set(&skipped_count, 0);
	atomic_set(&failures, 0);

	lat_classes = kvmalloc_array(MAX_SAMPLES, sizeof(*lat_classes),
				     GFP_KERNEL);
	if (!lat_classes) {
		kvfree(latencies);
		return -ENOMEM;
	}

	results_buf = (char *)get_zeroed_page(GFP_KERNEL);
	if (!results_buf) {
		kvfree(lat_classes);
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

		sc->rx_buf = kvmalloc(max_msg_size, GFP_KERNEL);
		if (!sc->rx_buf) {
			ret = -ENOMEM;
			goto err_socks;
		}

		if (tx_zc) {
			/* ZC TX: use order-4 compound pages (64KB physically
			 * contiguous) to match the frag layout of Homa's
			 * non-ZC page pool (HOMA_SKB_PAGE_ORDER=4). This
			 * isolates the copy-vs-pin difference from DMA
			 * scatter-gather overhead.
			 */
#define TX_PAGE_ORDER 4
#define TX_PAGE_SIZE (PAGE_SIZE << TX_PAGE_ORDER)
			int ncompound = DIV_ROUND_UP(max_msg_size, TX_PAGE_SIZE);
			int remaining = max_msg_size, j;

			sc->tx_pages = kmalloc_array(ncompound,
						     sizeof(struct page *),
						     GFP_KERNEL);
			sc->tx_bvecs = kmalloc_array(ncompound,
						     sizeof(struct bio_vec),
						     GFP_KERNEL);
			if (!sc->tx_pages || !sc->tx_bvecs) {
				ret = -ENOMEM;
				goto err_socks;
			}
			sc->nr_tx_pages = ncompound;
			sc->nr_tx_bvecs = ncompound;
			for (j = 0; j < ncompound; j++) {
				int chunk = min_t(int, remaining,
						  TX_PAGE_SIZE);

				sc->tx_pages[j] = alloc_pages(
						GFP_KERNEL | __GFP_COMP,
						TX_PAGE_ORDER);
				if (!sc->tx_pages[j]) {
					ret = -ENOMEM;
					goto err_socks;
				}
				memset(page_address(sc->tx_pages[j]),
				       KBENCH_REQ_PATTERN, chunk);
				bvec_set_page(&sc->tx_bvecs[j],
					      sc->tx_pages[j], chunk, 0);
				remaining -= chunk;
			}
		} else {
			sc->tx_buf = kvmalloc(max_msg_size, GFP_KERNEL);
			if (!sc->tx_buf) {
				ret = -ENOMEM;
				goto err_socks;
			}
			memset(sc->tx_buf, KBENCH_REQ_PATTERN, max_msg_size);
		}

		/* Actor destination for RX single-copy (independent of TX). */
		if (rx_single_copy && is_homa) {
			sc->actor_ctx.app_buf = kvmalloc(max_msg_size, GFP_KERNEL);
			if (!sc->actor_ctx.app_buf) {
				ret = -ENOMEM;
				goto err_socks;
			}
			sc->actor_ctx.buf_size = max_msg_size;
		}

		/* Pool-to-app scratch for simulate_app_copy. */
		if (simulate_app_copy && !rx_single_copy && is_homa) {
			sc->app_copy_scratch = kvmalloc(max_msg_size, GFP_KERNEL);
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

	pr_info("kbench_client: started transport=%s tx_zc=%d rx_sc=%d nonblocking=%d workload=%s max_msg=%d rate=%d sockets=%d\n",
		transport, tx_zc, rx_single_copy, nonblocking, workload,
		max_msg_size, rate, num_sockets);
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
		int j;

		if (sc->sock)
			sock_release(sc->sock);
		if (sc->tx_pages) {
			for (j = 0; j < sc->nr_tx_pages; j++) {
				if (sc->tx_pages[j])
					__free_pages(sc->tx_pages[j],
						     TX_PAGE_ORDER);
			}
			kfree(sc->tx_pages);
		}
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
	kvfree(lat_classes);
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
		int j;

		if (sc->sock)
			sock_release(sc->sock);
		if (sc->tx_pages) {
			for (j = 0; j < sc->nr_tx_pages; j++) {
				if (sc->tx_pages[j])
					__free_pages(sc->tx_pages[j],
						     TX_PAGE_ORDER);
			}
			kfree(sc->tx_pages);
		}
		kvfree(sc->tx_buf);
		kvfree(sc->rx_buf);
		kvfree(sc->pool_region);
		kvfree(sc->app_copy_scratch);
		kvfree(sc->actor_ctx.app_buf);
		kfree(sc->tx_bvecs);
	}
	kfree(sockets);
	free_page((unsigned long)results_buf);
	kvfree(lat_classes);
	kvfree(latencies);

	pr_info("kbench_client: stopped\n");
}

module_init(kbench_client_init);
module_exit(kbench_client_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HomaKBench client: latency benchmark for Homa/TCP with ZC");
