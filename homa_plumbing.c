// SPDX-License-Identifier: BSD-2-Clause

/* This file consists mostly of "glue" that hooks Homa into the rest of
 * the Linux kernel. The guts of the protocol are in other files.
 */

#include "homa_impl.h"
#ifndef __STRIP__ /* See strip.py */
#include "homa_grant.h"
#include "homa_offload.h"
#endif /* See strip.py */
#include "homa_pacer.h"
#include "homa_peer.h"
#include "homa_pool.h"

/* Identifier for retrieving Homa-specific data for a struct net. */
unsigned int homa_net_id;

/* This structure defines functions that allow Homa to be used as a
 * pernet subsystem.
 */
static struct pernet_operations homa_net_ops = {
	.init = homa_net_start,
	.exit = homa_net_exit,
	.id = &homa_net_id,
	.size = sizeof(struct homa_net)
};

/* Global data for Homa. Never reference homa_data directly. Always use
 * the global_homa variable instead (or, even better, a homa pointer
 * stored in a struct or passed via a parameter); this allows overriding
 * during unit tests.
 */
static struct homa homa_data;

/* This variable contains the address of the statically-allocated struct homa
 * used throughout Homa. This variable should almost never be used directly:
 * it should be passed as a parameter to functions that need it. This
 * variable is used only by a few functions called from Linux where there
 * is no struct homa* available.
 */
struct homa *global_homa = &homa_data;

/* This structure defines functions that handle various operations on
 * Homa sockets. These functions are relatively generic: they are called
 * to implement top-level system calls. Many of these operations can
 * be implemented by PF_INET6 functions that are independent of the
 * Homa protocol.
 */
static const struct proto_ops homa_proto_ops = {
	.family		   = PF_INET,
	.owner		   = THIS_MODULE,
	.release	   = inet_release,
	.bind		   = homa_bind,
	.connect	   = inet_dgram_connect,
	.socketpair	   = sock_no_socketpair,
	.accept		   = sock_no_accept,
	.getname	   = inet_getname,
	.poll		   = homa_poll,
	.ioctl		   = inet_ioctl,
	.listen		   = sock_no_listen,
	.shutdown	   = homa_shutdown,
	.setsockopt	   = sock_common_setsockopt,
	.getsockopt	   = sock_common_getsockopt,
	.sendmsg	   = inet_sendmsg,
	.recvmsg	   = inet_recvmsg,
	.mmap		   = sock_no_mmap,
	.set_peek_off	   = sk_set_peek_off,
};

static const struct proto_ops homav6_proto_ops = {
	.family		   = PF_INET6,
	.owner		   = THIS_MODULE,
	.release	   = inet6_release,
	.bind		   = homa_bind,
	.connect	   = inet_dgram_connect,
	.socketpair	   = sock_no_socketpair,
	.accept		   = sock_no_accept,
	.getname	   = inet6_getname,
	.poll		   = homa_poll,
	.ioctl		   = inet6_ioctl,
	.listen		   = sock_no_listen,
	.shutdown	   = homa_shutdown,
	.setsockopt	   = sock_common_setsockopt,
	.getsockopt	   = sock_common_getsockopt,
	.sendmsg	   = inet_sendmsg,
	.recvmsg	   = inet_recvmsg,
	.mmap		   = sock_no_mmap,
	.set_peek_off	   = sk_set_peek_off,
};

/* This structure also defines functions that handle various operations
 * on Homa sockets. However, these functions are lower-level than those
 * in homa_proto_ops: they are specific to the PF_INET or PF_INET6
 * protocol family, and in many cases they are invoked by functions in
 * homa_proto_ops. Most of these functions have Homa-specific implementations.
 */
static struct proto homa_prot = {
	.name		   = "HOMA",
	.owner		   = THIS_MODULE,
	.close		   = homa_close,
	.connect       = homa_connect,
	.ioctl		   = homa_ioctl,
	.init		   = homa_socket,
	.destroy	   = homa_sock_destroy,
	.setsockopt	   = homa_setsockopt,
	.getsockopt	   = homa_getsockopt,
	.sendmsg	   = homa_sendmsg,
	.recvmsg	   = homa_recvmsg,
	.hash		   = homa_hash,
	.unhash		   = homa_unhash,
	.obj_size	   = sizeof(struct homa_sock),
	.no_autobind       = 1,
};

static struct proto homav6_prot = {
	.name		   = "HOMAv6",
	.owner		   = THIS_MODULE,
	.connect       = homa_connect,
	.close		   = homa_close,
	.ioctl		   = homa_ioctl,
	.init		   = homa_socket,
	.destroy	   = homa_sock_destroy,
	.setsockopt	   = homa_setsockopt,
	.getsockopt	   = homa_getsockopt,
	.sendmsg	   = homa_sendmsg,
	.recvmsg	   = homa_recvmsg,
	.hash		   = homa_hash,
	.unhash		   = homa_unhash,
	.obj_size	   = sizeof(struct homa_v6_sock),
	.ipv6_pinfo_offset = offsetof(struct homa_v6_sock, inet6),

	.no_autobind       = 1,
};

/* Top-level structure describing the Homa protocol. */
static struct inet_protosw homa_protosw = {
	.type              = SOCK_DGRAM,
	.protocol          = IPPROTO_HOMA,
	.prot              = &homa_prot,
	.ops               = &homa_proto_ops,
	.flags             = INET_PROTOSW_REUSE,
};

static struct inet_protosw homav6_protosw = {
	.type              = SOCK_DGRAM,
	.protocol          = IPPROTO_HOMA,
	.prot              = &homav6_prot,
	.ops               = &homav6_proto_ops,
	.flags             = INET_PROTOSW_REUSE,
};

/* This structure is used by IP to deliver incoming Homa packets to us. */
static struct net_protocol homa_protocol = {
	.handler =	homa_softirq,
	.err_handler =	homa_err_handler_v4,
	.no_policy =     1,
};

static struct inet6_protocol homav6_protocol = {
	.handler =	homa_softirq,
	.err_handler =	homa_err_handler_v6,
	.flags =        INET6_PROTO_NOPOLICY | INET6_PROTO_FINAL,
};

#ifndef __STRIP__ /* See strip.py */
/* Used to configure sysctl access to Homa configuration parameters. The
 * @data fields are actually offsets within a struct homa; these are converted
 * to pointers into a net-specific struct homa later.
 */
#define OFFSET(field) ((void *)offsetof(struct homa, field))
static struct ctl_table homa_ctl_table[] = {
	{
		.procname	= "accept_bits",
		.data		= OFFSET(accept_bits),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "action",
		.data		= OFFSET(sysctl_action),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "bpage_lease_usecs",
		.data		= OFFSET(bpage_lease_usecs),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "busy_usecs",
		.data		= OFFSET(busy_usecs),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "cutoff_version",
		.data		= OFFSET(cutoff_version),
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "dead_buffs_limit",
		.data		= OFFSET(dead_buffs_limit),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "drop_bits",
		.data		= OFFSET(drop_bits),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "flags",
		.data		= OFFSET(flags),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "freeze_type",
		.data		= OFFSET(freeze_type),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "gen3_softirq_cores",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0644,
		.proc_handler	= homa_sysctl_softirq_cores
	},
	{
		.procname	= "gro_busy_usecs",
		.data		= OFFSET(gro_busy_usecs),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "gro_policy",
		.data		= OFFSET(gro_policy),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "gso_force_software",
		.data		= OFFSET(gso_force_software),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "hijack_tcp",
		.data		= OFFSET(hijack_tcp),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "max_dead_buffs",
		.data		= OFFSET(max_dead_buffs),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "max_gro_skbs",
		.data		= OFFSET(max_gro_skbs),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "max_gso_size",
		.data		= OFFSET(max_gso_size),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "max_sched_prio",
		.data		= OFFSET(max_sched_prio),
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "next_id",
		.data		= OFFSET(next_id),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "num_priorities",
		.data		= OFFSET(num_priorities),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "poll_usecs",
		.data		= OFFSET(poll_usecs),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "priority_map",
		.data		= OFFSET(priority_map),
		.maxlen		= HOMA_MAX_PRIORITIES * sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "reap_limit",
		.data		= OFFSET(reap_limit),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "request_ack_ticks",
		.data		= OFFSET(request_ack_ticks),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "resend_interval",
		.data		= OFFSET(resend_interval),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "resend_ticks",
		.data		= OFFSET(resend_ticks),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "skb_page_frees_per_sec",
		.data		= OFFSET(skb_page_frees_per_sec),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "skb_page_pool_min_kb",
		.data		= OFFSET(skb_page_pool_min_kb),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "temp",
		.data		= OFFSET(temp[0]),
		.maxlen		= sizeof(((struct homa *)0)->temp),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "timeout_resends",
		.data		= OFFSET(timeout_resends),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "timeout_ticks",
		.data		= OFFSET(timeout_ticks),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "unsched_bytes",
		.data		= OFFSET(unsched_bytes),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "unsched_cutoffs",
		.data		= OFFSET(unsched_cutoffs),
		.maxlen		= HOMA_MAX_PRIORITIES * sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "verbose",
		.data		= OFFSET(verbose),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
	{
		.procname	= "wmem_max",
		.data		= OFFSET(wmem_max),
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= homa_dointvec
	},
};
#endif /* See strip.py */

/* Sizes of the headers for each Homa packet type, in bytes. */
#ifndef __STRIP__ /* See strip.py */
static u16 header_lengths[] = {
	sizeof(struct homa_data_hdr),
	sizeof(struct homa_grant_hdr),
	sizeof(struct homa_resend_hdr),
	sizeof(struct homa_rpc_unknown_hdr),
	sizeof(struct homa_busy_hdr),
	sizeof(struct homa_cutoffs_hdr),
	sizeof(struct homa_freeze_hdr),
	sizeof(struct homa_need_ack_hdr),
	sizeof(struct homa_ack_hdr)
};
#else /* See strip.py */
static u16 header_lengths[] = {
	sizeof(struct homa_data_hdr),
	0,
	sizeof(struct homa_resend_hdr),
	sizeof(struct homa_rpc_unknown_hdr),
	sizeof(struct homa_busy_hdr),
	0,
	0,
	sizeof(struct homa_need_ack_hdr),
	sizeof(struct homa_ack_hdr)
};
#endif /* See strip.py */

#ifndef __STRIP__ /* See strip.py */
/* Used to remove sysctl values when the module is unloaded. */
static struct ctl_table_header *homa_ctl_header;
#endif /* See strip.py */

/* Thread that runs timer code to detect lost packets and crashed peers. */
static struct task_struct *timer_kthread;
static DECLARE_COMPLETION(timer_thread_done);

/* Used to wakeup timer_kthread at regular intervals. */
static struct hrtimer hrtimer;

/* Nonzero is an indication to the timer thread that it should exit. */
static int timer_thread_exit;

/**
 * homa_load() - invoked when this module is loaded into the Linux kernel
 * Return: 0 on success, otherwise a negative errno.
 */
int __init homa_load(void)
{
	struct homa *homa = global_homa;
	int status;

	/* Compile-time validations that no packet header is longer
	 * than HOMA_MAX_HEADER.
	 */
	BUILD_BUG_ON(sizeof(struct homa_data_hdr) > HOMA_MAX_HEADER);
#ifndef __STRIP__ /* See strip.py */
	BUILD_BUG_ON(sizeof(struct homa_grant_hdr) > HOMA_MAX_HEADER);
#endif /* See strip.py */
	BUILD_BUG_ON(sizeof(struct homa_resend_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_rpc_unknown_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_busy_hdr) > HOMA_MAX_HEADER);
#ifndef __STRIP__ /* See strip.py */
	BUILD_BUG_ON(sizeof(struct homa_cutoffs_hdr) > HOMA_MAX_HEADER);
#endif /* See strip.py */
#ifndef __UPSTREAM__ /* See strip.py */
	BUILD_BUG_ON(sizeof(struct homa_freeze_hdr) > HOMA_MAX_HEADER);
#endif /* See strip.py */
	BUILD_BUG_ON(sizeof(struct homa_need_ack_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_ack_hdr) > HOMA_MAX_HEADER);

	/* Extra constraints on data packets:
	 * - Ensure minimum header length so Homa doesn't have to worry about
	 *   padding data packets.
	 * - Make sure data packet headers are a multiple of 4 bytes (needed
	 *   for TCP/TSO compatibility).
	 */
	BUILD_BUG_ON(sizeof(struct homa_data_hdr) < HOMA_MIN_PKT_LENGTH);
	BUILD_BUG_ON((sizeof(struct homa_data_hdr) -
		      sizeof(struct homa_seg_hdr)) & 0x3);

#ifndef __STRIP__ /* See strip.py */
	/* Homa requires at least 8 priority levels. */
	BUILD_BUG_ON(HOMA_MAX_PRIORITIES < 8);
#endif /* See strip.py */

	/* Detect size changes in uAPI structs. */
	BUILD_BUG_ON(sizeof(struct homa_sendmsg_args) != 24);
	BUILD_BUG_ON(sizeof(struct homa_recvmsg_args) != 88);
#ifndef __STRIP__ /* See strip.py */
	BUILD_BUG_ON(sizeof(struct homa_abort_args) != 32);
#endif /* See strip.py */

	pr_err("Homa module loading\n");
#ifndef __STRIP__ /* See strip.py */
	pr_notice("Homa structure sizes: homa_data_hdr %lu, homa_seg_hdr %lu, ack %lu, peer %lu, ip_hdr %lu flowi %lu ipv6_hdr %lu, flowi6 %lu tcp_sock %lu homa_rpc %lu sk_buff %lu rcvmsg_control %lu union sockaddr_in_union %lu HOMA_MAX_BPAGES %u NR_CPUS %u nr_cpu_ids %u, MAX_NUMNODES %d\n",
		  sizeof(struct homa_data_hdr),
		  sizeof(struct homa_seg_hdr),
		  sizeof(struct homa_ack),
		  sizeof(struct homa_peer),
		  sizeof(struct iphdr),
		  sizeof(struct flowi),
		  sizeof(struct ipv6hdr),
		  sizeof(struct flowi6),
		  sizeof(struct tcp_sock),
		  sizeof(struct homa_rpc),
		  sizeof(struct sk_buff),
		  sizeof(struct homa_recvmsg_args),
		  sizeof(union sockaddr_in_union),
		  HOMA_MAX_BPAGES,
		  NR_CPUS,
		  nr_cpu_ids,
		  MAX_NUMNODES);
#endif /* See strip.py */
	status = proto_register(&homa_prot, 1);
	if (status != 0) {
		pr_err("proto_register failed for homa_prot: %d\n", status);
		goto proto_register_err;
	}
	status = proto_register(&homav6_prot, 1);
	if (status != 0) {
		pr_err("proto_register failed for homav6_prot: %d\n", status);
		goto proto_register_v6_err;
	}
	inet_register_protosw(&homa_protosw);
	status = inet6_register_protosw(&homav6_protosw);
	if (status != 0) {
		pr_err("inet6_register_protosw failed in %s: %d\n", __func__,
		       status);
		goto register_protosw_v6_err;
	}
	status = inet_add_protocol(&homa_protocol, IPPROTO_HOMA);
	if (status != 0) {
		pr_err("inet_add_protocol failed in %s: %d\n", __func__,
		       status);
		goto add_protocol_err;
	}
	status = inet6_add_protocol(&homav6_protocol, IPPROTO_HOMA);
	if (status != 0) {
		pr_err("inet6_add_protocol failed in %s: %d\n",  __func__,
		       status);
		goto add_protocol_v6_err;
	}
	status = homa_init(homa);
	if (status)
		goto homa_init_err;

#ifndef __STRIP__ /* See strip.py */
	status = homa_metrics_init();
	if (status != 0)
		goto metrics_err;

	homa_ctl_header = register_net_sysctl(&init_net, "net/homa",
					      homa_ctl_table);
	if (!homa_ctl_header) {
		pr_err("couldn't register Homa sysctl parameters\n");
		status = -ENOMEM;
		goto sysctl_err;
	}

	status = homa_offload_init();
	if (status != 0) {
		pr_err("Homa couldn't init offloads\n");
		goto offload_err;
	}
#endif /* See strip.py */

	status = register_pernet_subsys(&homa_net_ops);
	if (status != 0) {
		pr_err("Homa got error from register_pernet_subsys: %d\n",
		       status);
		goto net_err;
	}
	timer_kthread = kthread_run(homa_timer_main, homa, "homa_timer");
	if (IS_ERR(timer_kthread)) {
		status = PTR_ERR(timer_kthread);
		pr_err("couldn't create Homa pacer thread: error %d\n",
		       status);
		timer_kthread = NULL;
		goto timer_err;
	}

#ifndef __STRIP__ /* See strip.py */
	homa_gro_hook_tcp();
#endif /* See strip.py */
#ifndef __UPSTREAM__ /* See strip.py */
	tt_init("timetrace");
	tt_set_temp(homa->temp);
#endif /* See strip.py */

	return 0;

timer_err:
	unregister_pernet_subsys(&homa_net_ops);
net_err:
#ifndef __STRIP__ /* See strip.py */
	homa_offload_end();
offload_err:
	unregister_net_sysctl_table(homa_ctl_header);
sysctl_err:
	homa_metrics_end();
metrics_err:
#endif /* See strip.py */
	homa_destroy(homa);
homa_init_err:
	inet6_del_protocol(&homav6_protocol, IPPROTO_HOMA);
add_protocol_v6_err:
	inet_del_protocol(&homa_protocol, IPPROTO_HOMA);
add_protocol_err:
	inet6_unregister_protosw(&homav6_protosw);
register_protosw_v6_err:
	inet_unregister_protosw(&homa_protosw);
	proto_unregister(&homav6_prot);
proto_register_v6_err:
	proto_unregister(&homa_prot);
proto_register_err:
	return status;
}

/**
 * homa_unload() - invoked when this module is unloaded from the Linux kernel.
 */
void __exit homa_unload(void)
{
	struct homa *homa = global_homa;

	pr_notice("Homa module unloading\n");

#ifndef __STRIP__ /* See strip.py */
	homa_gro_unhook_tcp();
	if (timer_kthread) {
		timer_thread_exit = 1;
		wake_up_process(timer_kthread);
		wait_for_completion(&timer_thread_done);
	}
	if (homa_offload_end() != 0)
		pr_err("Homa couldn't stop offloads\n");
	unregister_net_sysctl_table(homa_ctl_header);
	homa_metrics_end();
#endif /* See strip.py */
	unregister_pernet_subsys(&homa_net_ops);
	homa_destroy(homa);
	inet_del_protocol(&homa_protocol, IPPROTO_HOMA);
	inet_unregister_protosw(&homa_protosw);
	inet6_del_protocol(&homav6_protocol, IPPROTO_HOMA);
	inet6_unregister_protosw(&homav6_protosw);
	proto_unregister(&homa_prot);
	proto_unregister(&homav6_prot);
#ifndef __UPSTREAM__ /* See strip.py */
	tt_destroy();
#endif /* See strip.py */
}

module_init(homa_load);
module_exit(homa_unload);

/**
 * homa_net_start() - Initialize Homa for a new network namespace.
 * @net:    The net that Homa will be associated with.
 * Return:  0 on success, otherwise a negative errno.
 */
int homa_net_start(struct net *net)
{
	pr_notice("Homa attaching to net namespace\n");
	return homa_net_init(homa_net_from_net(net), net, global_homa);
}

/**
 * homa_net_exit() - Perform Homa cleanup needed when a network namespace
 * is destroyed.
 * @net:    The net from which Homa should be removed.
 */
void homa_net_exit(struct net *net)
{
	pr_notice("Homa detaching from net namespace\n");
	homa_net_destroy(homa_net_from_net(net));
}

/**
 * homa_bind() - Implements the bind system call for Homa sockets: associates
 * a well-known service port with a socket. Unlike other AF_INET6 protocols,
 * there is no need to invoke this system call for sockets that are only
 * used as clients.
 * @sock:     Socket on which the system call was invoked.
 * @addr:    Contains the desired port number.
 * @addr_len: Number of bytes in uaddr.
 * Return:    0 on success, otherwise a negative errno.
 */
int homa_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	union sockaddr_in_union *addr_in = (union sockaddr_in_union *)addr;
	struct homa_sock *hsk = homa_sk(sock->sk);
	int port = 0;

	if (unlikely(addr->sa_family != sock->sk->sk_family))
		return -EAFNOSUPPORT;
	if (addr_in->in6.sin6_family == AF_INET6) {
		if (addr_len < sizeof(struct sockaddr_in6))
			return -EINVAL;
		port = ntohs(addr_in->in4.sin_port);
	} else if (addr_in->in4.sin_family == AF_INET) {
		if (addr_len < sizeof(struct sockaddr_in))
			return -EINVAL;
		port = ntohs(addr_in->in6.sin6_port);
	}
	return homa_sock_bind(hsk->hnet, hsk, port);
}

/**
 * homa_close() - Invoked when close system call is invoked on a Homa socket.
 * @sk:      Socket being closed
 * @timeout: ??
 */
void homa_close(struct sock *sk, long timeout)
{
	struct homa_sock *hsk = homa_sk(sk);
#ifndef __UPSTREAM__ /* See strip.py */
	int port = hsk->port;
#endif/* See strip.py */

	homa_sock_shutdown(hsk);
	sk_common_release(sk);
	tt_record1("closed socket, port %d", port);
}

/**
 * homa_shutdown() - Implements the shutdown system call for Homa sockets.
 * @sock:    Socket to shut down.
 * @how:     Ignored: for other sockets, can independently shut down
 *           sending and receiving, but for Homa any shutdown will
 *           shut down everything.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int homa_shutdown(struct socket *sock, int how)
{
	homa_sock_shutdown(homa_sk(sock->sk));
	return 0;
}

#ifndef __STRIP__ /* See strip.py */
/**
 * homa_ioc_abort() - The top-level function for the ioctl that implements
 * the homa_abort user-level API.
 * @sk:       Socket for this request.
 * @karg:     Used to pass information from user space.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int homa_ioc_abort(struct sock *sk, int *karg)
{
	int ret = 0;
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_abort_args args;
	struct homa_rpc *rpc;

	if (unlikely(copy_from_user(&args, (void __user *)karg, sizeof(args))))
		return -EFAULT;

	if (args._pad1 || args._pad2[0] || args._pad2[1])
		return -EINVAL;
	if (args.id == 0) {
		homa_abort_sock_rpcs(hsk, -args.error);
		return 0;
	}

	rpc = homa_rpc_find_client(hsk, args.id);
	if (!rpc)
		return -EINVAL;
	if (args.error == 0)
		homa_rpc_end(rpc);
	else
		homa_rpc_abort(rpc, -args.error);
	homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_client. */
	return ret;
}
#endif /* See strip.py */

/**
 * homa_ioctl() - Implements the ioctl system call for Homa sockets.
 * @sk:    Socket on which the system call was invoked.
 * @cmd:   Identifier for a particular ioctl operation.
 * @karg:  Operation-specific argument; typically the address of a block
 *         of data in user address space.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int homa_ioctl(struct sock *sk, int cmd, int *karg)
{
#ifndef __STRIP__ /* See strip.py */
	int result;
	u64 start = homa_clock();

	if (cmd == HOMAIOCABORT) {
		result = homa_ioc_abort(sk, karg);
		INC_METRIC(abort_calls, 1);
		INC_METRIC(abort_cycles, homa_clock() - start);
	} else if (cmd == HOMAIOCFREEZE) {
		tt_record1("Freezing timetrace because of HOMAIOCFREEZE ioctl, pid %d",
			   current->pid);
		tt_freeze();
		result = 0;
	} else {
		pr_notice("Unknown Homa ioctl: %d\n", cmd);
		result = -EINVAL;
	}
	return result;
#else /* See strip.py */
	return -EINVAL;
#endif /* See strip.py */
}

/**
 * homa_socket() - Implements the socket(2) system call for sockets.
 * @sk:    Socket on which the system call was invoked. The non-Homa
 *         parts have already been initialized.
 *
 * Return: always 0 (success).
 */
int homa_socket(struct sock *sk)
{
	struct homa_sock *hsk = homa_sk(sk);
	int result;

	result = homa_sock_init(hsk);
	if (result != 0) {
		homa_sock_shutdown(hsk);
		homa_sock_destroy(&hsk->sock);
	}
	return result;
}

/**
 * homa_setsockopt() - Implements the getsockopt system call for Homa sockets.
 * @sk:      Socket on which the system call was invoked.
 * @level:   Level at which the operation should be handled; will always
 *           be IPPROTO_HOMA.
 * @optname: Identifies a particular setsockopt operation.
 * @optval:  Address in user space of information about the option.
 * @optlen:  Number of bytes of data at @optval.
 * Return:   0 on success, otherwise a negative errno.
 */
int homa_setsockopt(struct sock *sk, int level, int optname,
		    sockptr_t optval, unsigned int optlen)
{
	struct homa_sock *hsk = homa_sk(sk);
	int ret;
	// This boolean value checks whether the call is from kernel.
	bool in_kernel = (current->mm == NULL);

	if (level != IPPROTO_HOMA)
		return -ENOPROTOOPT;

	if (optname == SO_HOMA_RCVBUF) {
		struct homa_rcvbuf_args args;
#ifndef __STRIP__ /* See strip.py */
		u64 start = homa_clock();
#endif /* See strip.py */

		if (optlen != sizeof(struct homa_rcvbuf_args))
			return -EINVAL;

		if (copy_from_sockptr(&args, optval, optlen))
			return -EFAULT;

		if (in_kernel) {
			ret = homa_pool_set_region(hsk, (void *)(uintptr_t)args.start, args.length, true);
		}
		else {
			/* Do a trivial test to make sure we can at least write the
		 * first page of the region.
		 */
			if (copy_to_user(u64_to_user_ptr(args.start), &args,
					 sizeof(args)))
				return -EFAULT;

			ret = homa_pool_set_region(hsk, u64_to_user_ptr(args.start),
									   args.length, false);
		}
		INC_METRIC(so_set_buf_calls, 1);
		INC_METRIC(so_set_buf_cycles, homa_clock() - start);
	} else if (optname == SO_HOMA_SERVER) {
		int arg;

		if (optlen != sizeof(arg))
			return -EINVAL;

		if (copy_from_sockptr(&arg, optval, optlen))
			return -EFAULT;

		if (arg)
			hsk->is_server = true;
		else
			hsk->is_server = false;
		ret = 0;
	} else {
		ret = -ENOPROTOOPT;
	}
	return ret;
}

/**
 * homa_getsockopt() - Implements the getsockopt system call for Homa sockets.
 * @sk:      Socket on which the system call was invoked.
 * @level:   Selects level in the network stack to handle the request;
 *           must be IPPROTO_HOMA.
 * @optname: Identifies a particular setsockopt operation.
 * @optval:  Address in user space where the option's value should be stored.
 * @optlen:  Number of bytes available at optval; will be overwritten with
 *           actual number of bytes stored.
 * Return:   0 on success, otherwise a negative errno.
 */
int homa_getsockopt(struct sock *sk, int level, int optname,
		    char __user *optval, int __user *optlen)
{
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_rcvbuf_args rcvbuf_args;
	void *result;
	int is_server;
	int len;

	if (copy_from_sockptr(&len, USER_SOCKPTR(optlen), sizeof(int)))
		return -EFAULT;

	if (level != IPPROTO_HOMA)
		return -ENOPROTOOPT;
	if (optname == SO_HOMA_RCVBUF) {
		if (len < sizeof(rcvbuf_args))
			return -EINVAL;

		homa_sock_lock(hsk);
		homa_pool_get_rcvbuf(hsk->buffer_pool, &rcvbuf_args);
		homa_sock_unlock(hsk);
		len = sizeof(rcvbuf_args);
		result = &rcvbuf_args;
	} else if (optname == SO_HOMA_SERVER) {
		if (len < sizeof(is_server))
			return -EINVAL;

		is_server = hsk->is_server;
		len = sizeof(is_server);
		result = &is_server;
	} else {
		return -ENOPROTOOPT;
	}

	if (copy_to_sockptr(USER_SOCKPTR(optlen), &len, sizeof(int)))
		return -EFAULT;

	if (copy_to_sockptr(USER_SOCKPTR(optval), result, len))
		return -EFAULT;

	return 0;
}

static int __homa_connect(struct sock *sk, struct sockaddr *addr, int addrlen) {
	struct homa_sock *hsk = homa_sk(sk);
	if (hsk->connected) {
		return -EISCONN;
	}
	if (hsk->shutdown) {
		return -ESHUTDOWN;
	}
	if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
		return -EAFNOSUPPORT;
	}
	if (addr->sa_family == AF_INET) {
		if (addrlen < sizeof(struct sockaddr_in)) {
			return -EINVAL;
		}
		memset(&hsk->target_addr, 0, sizeof(hsk->target_addr));
		memcpy(&hsk->target_addr, addr, sizeof(struct sockaddr_in));
		hsk->connected = true;
		return 0;
	}
	if (addr->sa_family == AF_INET6) {
		if (addrlen < sizeof(struct sockaddr_in6)) {
			return -EINVAL;
		}
		memset(&hsk->target_addr, 0, sizeof(hsk->target_addr));
		memcpy(&hsk->target_addr, addr, sizeof(struct sockaddr_in6));
		hsk->connected = true;
		return 0;
	}
	return -EINVAL;
}

/**
 * homa_connect() - For NVMe/Homa. UDP-style connect() by specifying the target
 * address.
 * Note that target side sockets never call this method as they need to stay
 * CONNECTIONLESSS.
 * @sk:       Socket on which the system call was invoked.
 * @addr:     Structure storing the target address
 * @addr_len: The length of the address
 * @flags:    Not used by Homa
 */
int homa_connect(struct sock *sk, struct sockaddr *addr, int addrlen) {
	int res;
	homa_sock_lock(homa_sk(sk));
	res = __homa_connect(sk, addr, addrlen);
	homa_sock_unlock(homa_sk(sk));
	return res;
}

static int homa_sendmsg_user_connected(struct sock *sk, struct msghdr *msg, size_t length) {
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_sendmsg_args args;
	union sockaddr_in_union *addr = &hsk->target_addr;
#ifndef __STRIP__ /* See strip.py */
	u64 start = homa_clock();
#endif /* See strip.py */
	struct homa_rpc *rpc = NULL;
	int result = 0;
#ifndef __STRIP__ /* See strip.py */
	u64 finish;

	per_cpu(homa_offload_core, raw_smp_processor_id()).last_app_active =
			start;
#endif /* See strip.py */

	// Checks for the connected socket's NULL addr requirement
	if (msg->msg_name != NULL) {
		result = -EINVAL;
		goto error;
	}

	if (unlikely(!msg->msg_control_is_user)) {
		tt_record("homa_sendmsg error: !msg->msg_control_is_user");
		result = -EINVAL;
		goto error;
	}
	if (unlikely(copy_from_user(&args, (void __user *)msg->msg_control,
				    sizeof(args)))) {
		result = -EFAULT;
		goto error;
	}
	if (args.flags & ~HOMA_SENDMSG_VALID_FLAGS ||
	    args.reserved != 0) {
		result = -EINVAL;
		goto error;
	}

	if (!homa_sock_wmem_avl(hsk)) {
		result = homa_sock_wait_wmem(hsk,
					     msg->msg_flags & MSG_DONTWAIT);
		if (result != 0)
			goto error;
	}

	if (addr->sa.sa_family != sk->sk_family) {
		result = -EAFNOSUPPORT;
		goto error;
	}
	if (msg->msg_namelen != 0) {
		tt_record("homa_sendmsg error: msg_namelen shall always be 0");
		result = -EINVAL;
		goto error;
	}

	if (!args.id) {
		/* This is a request message. */
		rpc = homa_rpc_alloc_client(hsk, addr);
		if (IS_ERR(rpc)) {
			result = PTR_ERR(rpc);
			rpc = NULL;
			goto error;
		}
		if (args.flags & HOMA_SENDMSG_PRIVATE)
			atomic_or(RPC_PRIVATE, &rpc->flags);
		INC_METRIC(send_calls, 1);
		tt_record4("homa_sendmsg request, target 0x%x:%d, id %u, length %d",
			  (addr->in6.sin6_family == AF_INET)
			  ? ntohl(addr->in4.sin_addr.s_addr)
			  : tt_addr(addr->in6.sin6_addr),
			  ntohs(addr->in6.sin6_port), rpc->id, length);
		rpc->completion_cookie = args.completion_cookie;
		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result)
			goto error;
		args.id = rpc->id;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_alloc_client. */
		rpc = NULL;

		if (unlikely(copy_to_user((void __user *)msg->msg_control,
					  &args, sizeof(args)))) {
			rpc = homa_rpc_find_client(hsk, args.id);
			result = -EFAULT;
			goto error;
		}
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(send_cycles, finish - start);
	} else {
		/* This is a response message. */
		struct in6_addr canonical_dest;

		INC_METRIC(reply_calls, 1);
		tt_record4("homa_sendmsg response, id %llu, port %d, pid %d, length %d",
			   args.id, hsk->port, current->pid, length);
		if (args.completion_cookie != 0) {
			tt_record("homa_sendmsg error: nonzero cookie");
			result = -EINVAL;
			goto error;
		}
		canonical_dest = canonical_ipv6_addr(addr);

		rpc = homa_rpc_find_server(hsk, &canonical_dest, args.id);
		if (!rpc) {
			/* Return without an error if the RPC doesn't exist;
			 * this could be totally valid (e.g. client is
			 * no longer interested in it).
			 */
			tt_record2("homa_sendmsg error: RPC id %d, peer 0x%x, doesn't exist",
				   args.id, tt_addr(canonical_dest));
			return 0;
		}
		if (rpc->error) {
			result = rpc->error;
			goto error;
		}
		if (rpc->state != RPC_IN_SERVICE) {
			tt_record2("homa_sendmsg error: RPC id %d in bad state %d",
				   rpc->id, rpc->state);
			/* Locked by homa_rpc_find_server. */
			homa_rpc_unlock(rpc);
			rpc = NULL;
			result = -EINVAL;
			goto error;
		}
		rpc->state = RPC_OUTGOING;

		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result && rpc->state != RPC_DEAD)
			goto error;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(reply_cycles, finish - start);
	}
	tt_record1("homa_sendmsg finished, id %d", args.id);
	return 0;

error:
	if (rpc) {
		homa_rpc_end(rpc);
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
	}
	tt_record2("homa_sendmsg returning error %d for id %d",
		   result, args.id);
	return result;
}

static int homa_sendmsg_user_unconnected(struct sock *sk, struct msghdr *msg, size_t length) {
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_sendmsg_args args;
	union sockaddr_in_union *addr;
#ifndef __STRIP__ /* See strip.py */
	u64 start = homa_clock();
#endif /* See strip.py */
	struct homa_rpc *rpc = NULL;
	int result = 0;
#ifndef __STRIP__ /* See strip.py */
	u64 finish;

	per_cpu(homa_offload_core, raw_smp_processor_id()).last_app_active =
			start;
#endif /* See strip.py */

	addr = (union sockaddr_in_union *)msg->msg_name;
	if (!addr) {
		result = -EINVAL;
		goto error;
	}

	if (unlikely(!msg->msg_control_is_user)) {
		tt_record("homa_sendmsg error: !msg->msg_control_is_user");
		result = -EINVAL;
		goto error;
	}
	if (unlikely(copy_from_user(&args, (void __user *)msg->msg_control,
				    sizeof(args)))) {
		result = -EFAULT;
		goto error;
	}
	if (args.flags & ~HOMA_SENDMSG_VALID_FLAGS ||
	    args.reserved != 0) {
		result = -EINVAL;
		goto error;
	}

	if (!homa_sock_wmem_avl(hsk)) {
		result = homa_sock_wait_wmem(hsk,
					     msg->msg_flags & MSG_DONTWAIT);
		if (result != 0)
			goto error;
	}

	if (addr->sa.sa_family != sk->sk_family) {
		result = -EAFNOSUPPORT;
		goto error;
	}
	if (msg->msg_namelen < sizeof(struct sockaddr_in) ||
	    (msg->msg_namelen < sizeof(struct sockaddr_in6) &&
	     addr->in6.sin6_family == AF_INET6)) {
		tt_record("homa_sendmsg error: msg_namelen too short");
		result = -EINVAL;
		goto error;
	}

	if (!args.id) {
		/* This is a request message. */
		rpc = homa_rpc_alloc_client(hsk, addr);
		if (IS_ERR(rpc)) {
			result = PTR_ERR(rpc);
			rpc = NULL;
			goto error;
		}
		if (args.flags & HOMA_SENDMSG_PRIVATE)
			atomic_or(RPC_PRIVATE, &rpc->flags);
		INC_METRIC(send_calls, 1);
		tt_record4("homa_sendmsg request, target 0x%x:%d, id %u, length %d",
			   (addr->in6.sin6_family == AF_INET)
			   ? ntohl(addr->in4.sin_addr.s_addr)
			   : tt_addr(addr->in6.sin6_addr),
			   ntohs(addr->in6.sin6_port), rpc->id, length);
		rpc->completion_cookie = args.completion_cookie;
		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result)
			goto error;
		args.id = rpc->id;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_alloc_client. */
		rpc = NULL;

		if (unlikely(copy_to_user((void __user *)msg->msg_control,
					  &args, sizeof(args)))) {
			rpc = homa_rpc_find_client(hsk, args.id);
			result = -EFAULT;
			goto error;
		}
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(send_cycles, finish - start);
	} else {
		/* This is a response message. */
		struct in6_addr canonical_dest;

		INC_METRIC(reply_calls, 1);
		tt_record4("homa_sendmsg response, id %llu, port %d, pid %d, length %d",
			   args.id, hsk->port, current->pid, length);
		if (args.completion_cookie != 0) {
			tt_record("homa_sendmsg error: nonzero cookie");
			result = -EINVAL;
			goto error;
		}
		canonical_dest = canonical_ipv6_addr(addr);

		rpc = homa_rpc_find_server(hsk, &canonical_dest, args.id);
		if (!rpc) {
			/* Return without an error if the RPC doesn't exist;
			 * this could be totally valid (e.g. client is
			 * no longer interested in it).
			 */
			tt_record2("homa_sendmsg error: RPC id %d, peer 0x%x, doesn't exist",
				   args.id, tt_addr(canonical_dest));
			return 0;
		}
		if (rpc->error) {
			result = rpc->error;
			goto error;
		}
		if (rpc->state != RPC_IN_SERVICE) {
			tt_record2("homa_sendmsg error: RPC id %d in bad state %d",
				   rpc->id, rpc->state);
			/* Locked by homa_rpc_find_server. */
			homa_rpc_unlock(rpc);
			rpc = NULL;
			result = -EINVAL;
			goto error;
		}
		rpc->state = RPC_OUTGOING;

		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result && rpc->state != RPC_DEAD)
			goto error;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(reply_cycles, finish - start);
	}
	tt_record1("homa_sendmsg finished, id %d", args.id);
	return 0;

error:
	if (rpc) {
		homa_rpc_end(rpc);
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
	}
	tt_record2("homa_sendmsg returning error %d for id %d",
		   result, args.id);
	return result;
}

static int homa_sendmsg_in_kernel_connected(struct sock *sk, struct msghdr *msg, size_t length) {
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_sendmsg_args args;
	union sockaddr_in_union *addr = &hsk->target_addr;
#ifndef __STRIP__ /* See strip.py */
	u64 start = homa_clock();
#endif /* See strip.py */
	struct homa_rpc *rpc = NULL;
	int result = 0;
#ifndef __STRIP__ /* See strip.py */
	u64 finish;

	per_cpu(homa_offload_core, raw_smp_processor_id()).last_app_active =
			start;
#endif /* See strip.py */

	// Checks for the connected socket's NULL addr requirement
	if (msg->msg_name != NULL) {
		result = -EINVAL;
		goto error;
	}

	if (args.flags & ~HOMA_SENDMSG_VALID_FLAGS ||
	    args.reserved != 0) {
		result = -EINVAL;
		goto error;
	}

	if (!homa_sock_wmem_avl(hsk)) {
		result = homa_sock_wait_wmem(hsk,
					     msg->msg_flags & MSG_DONTWAIT);
		if (result != 0)
			goto error;
	}

	if (addr->sa.sa_family != sk->sk_family) {
		result = -EAFNOSUPPORT;
		goto error;
	}
	if (msg->msg_namelen != 0) {
		tt_record("homa_sendmsg error: msg_namelen shall always be 0");
		result = -EINVAL;
		goto error;
	}

	// Copy from the msg_control, probably needs modification as zero-copy needed
	memcpy(&args, msg->msg_control, sizeof(args));

	if (!args.id) {
		/* This is a request message. */
		rpc = homa_rpc_alloc_client(hsk, addr);
		if (IS_ERR(rpc)) {
			result = PTR_ERR(rpc);
			rpc = NULL;
			goto error;
		}
		if (args.flags & HOMA_SENDMSG_PRIVATE)
			atomic_or(RPC_PRIVATE, &rpc->flags);
		INC_METRIC(send_calls, 1);
		tt_record4("homa_sendmsg request, target 0x%x:%d, id %u, length %d",
			  (addr->in6.sin6_family == AF_INET)
			  ? ntohl(addr->in4.sin_addr.s_addr)
			  : tt_addr(addr->in6.sin6_addr),
			  ntohs(addr->in6.sin6_port), rpc->id, length);
		rpc->completion_cookie = args.completion_cookie;
		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result)
			goto error;
		args.id = rpc->id;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_alloc_client. */
		rpc = NULL;
		// in-kernel copy to update the rpc id, needs modification as zero-copy needed.
		memcpy(msg->msg_control, &args, sizeof(args));
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(send_cycles, finish - start);
	} else {
		/* This is a response message. */
		struct in6_addr canonical_dest;

		INC_METRIC(reply_calls, 1);
		tt_record4("homa_sendmsg response, id %llu, port %d, pid %d, length %d",
			   args.id, hsk->port, current->pid, length);
		if (args.completion_cookie != 0) {
			tt_record("homa_sendmsg error: nonzero cookie");
			result = -EINVAL;
			goto error;
		}
		canonical_dest = canonical_ipv6_addr(addr);

		rpc = homa_rpc_find_server(hsk, &canonical_dest, args.id);
		if (!rpc) {
			/* Return without an error if the RPC doesn't exist;
			 * this could be totally valid (e.g. client is
			 * no longer interested in it).
			 */
			tt_record2("homa_sendmsg error: RPC id %d, peer 0x%x, doesn't exist",
				   args.id, tt_addr(canonical_dest));
			return 0;
		}
		if (rpc->error) {
			result = rpc->error;
			goto error;
		}
		if (rpc->state != RPC_IN_SERVICE) {
			tt_record2("homa_sendmsg error: RPC id %d in bad state %d",
				   rpc->id, rpc->state);
			/* Locked by homa_rpc_find_server. */
			homa_rpc_unlock(rpc);
			rpc = NULL;
			result = -EINVAL;
			goto error;
		}
		rpc->state = RPC_OUTGOING;

		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result && rpc->state != RPC_DEAD)
			goto error;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(reply_cycles, finish - start);
	}
	tt_record1("homa_sendmsg finished, id %d", args.id);
	printk("You just sent out a in-kernel msg via a connected Homa socket. \n");
	return 0;

error:
	if (rpc) {
		homa_rpc_end(rpc);
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
	}
	tt_record2("homa_sendmsg returning error %d for id %d",
		   result, args.id);
	return result;
}

/**
 * For NVMe/Homa. Should be used by target side. Note that the host side will
 * never use this method as it aligns with NVMe's connection-oriented semantics
 * See comment of homa_sendmsg() for param explanation.
 */
static int homa_sendmsg_in_kernel_unconnected(struct sock *sk, struct msghdr *msg, size_t length) {
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_sendmsg_args args;
	union sockaddr_in_union *addr;
#ifndef __STRIP__ /* See strip.py */
	u64 start = homa_clock();
#endif /* See strip.py */
	struct homa_rpc *rpc = NULL;
	int result = 0;
#ifndef __STRIP__ /* See strip.py */
	u64 finish;

	per_cpu(homa_offload_core, raw_smp_processor_id()).last_app_active =
			start;
#endif /* See strip.py */

	addr = (union sockaddr_in_union *)msg->msg_name;
	if (!addr) {
		result = -EINVAL;
		goto error;
	}
	// same as connected version
	memcpy(&args, msg->msg_control, sizeof(args));
	if (args.flags & ~HOMA_SENDMSG_VALID_FLAGS ||
	    args.reserved != 0) {
		result = -EINVAL;
		goto error;
	}

	if (!homa_sock_wmem_avl(hsk)) {
		result = homa_sock_wait_wmem(hsk,
					     msg->msg_flags & MSG_DONTWAIT);
		if (result != 0)
			goto error;
	}

	if (addr->sa.sa_family != sk->sk_family) {
		result = -EAFNOSUPPORT;
		goto error;
	}
	if (msg->msg_namelen < sizeof(struct sockaddr_in) ||
	    (msg->msg_namelen < sizeof(struct sockaddr_in6) &&
	     addr->in6.sin6_family == AF_INET6)) {
		tt_record("homa_sendmsg error: msg_namelen too short");
		result = -EINVAL;
		goto error;
	}

	if (!args.id) {
		/* This is a request message. */
		rpc = homa_rpc_alloc_client(hsk, addr);
		if (IS_ERR(rpc)) {
			result = PTR_ERR(rpc);
			rpc = NULL;
			goto error;
		}
		if (args.flags & HOMA_SENDMSG_PRIVATE)
			atomic_or(RPC_PRIVATE, &rpc->flags);
		INC_METRIC(send_calls, 1);
		tt_record4("homa_sendmsg request, target 0x%x:%d, id %u, length %d",
			   (addr->in6.sin6_family == AF_INET)
			   ? ntohl(addr->in4.sin_addr.s_addr)
			   : tt_addr(addr->in6.sin6_addr),
			   ntohs(addr->in6.sin6_port), rpc->id, length);
		rpc->completion_cookie = args.completion_cookie;
		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result)
			goto error;
		args.id = rpc->id;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_alloc_client. */
		rpc = NULL;

		memcpy(msg->msg_control, &args, sizeof(args));
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(send_cycles, finish - start);
	} else {
		/* This is a response message. */
		struct in6_addr canonical_dest;

		INC_METRIC(reply_calls, 1);
		tt_record4("homa_sendmsg response, id %llu, port %d, pid %d, length %d",
			   args.id, hsk->port, current->pid, length);
		if (args.completion_cookie != 0) {
			tt_record("homa_sendmsg error: nonzero cookie");
			result = -EINVAL;
			goto error;
		}
		canonical_dest = canonical_ipv6_addr(addr);

		rpc = homa_rpc_find_server(hsk, &canonical_dest, args.id);
		if (!rpc) {
			/* Return without an error if the RPC doesn't exist;
			 * this could be totally valid (e.g. client is
			 * no longer interested in it).
			 */
			tt_record2("homa_sendmsg error: RPC id %d, peer 0x%x, doesn't exist",
				   args.id, tt_addr(canonical_dest));
			return 0;
		}
		if (rpc->error) {
			result = rpc->error;
			goto error;
		}
		if (rpc->state != RPC_IN_SERVICE) {
			tt_record2("homa_sendmsg error: RPC id %d in bad state %d",
				   rpc->id, rpc->state);
			/* Locked by homa_rpc_find_server. */
			homa_rpc_unlock(rpc);
			rpc = NULL;
			result = -EINVAL;
			goto error;
		}
		rpc->state = RPC_OUTGOING;

		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result && rpc->state != RPC_DEAD)
			goto error;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
#ifndef __STRIP__ /* See strip.py */
		finish = homa_clock();
#endif /* See strip.py */
		INC_METRIC(reply_cycles, finish - start);
	}
	tt_record1("homa_sendmsg finished, id %d", args.id);
	return 0;

error:
	if (rpc) {
		homa_rpc_end(rpc);
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
	}
	tt_record2("homa_sendmsg returning error %d for id %d",
		   result, args.id);
	return result;
}

/**
 * homa_sendmsg() - Send a request or response message on a Homa socket.
 * @sk:     Socket on which the system call was invoked.
 * @msg:    Structure describing the message to send; the msg_control
 *          field points to additional information.
 * @length: Number of bytes of the message.
 * Return: 0 on success, otherwise a negative errno.
 */
int homa_sendmsg(struct sock *sk, struct msghdr *msg, size_t length)
{
	struct homa_sock *hsk = homa_sk(sk);
	if (msg->msg_control_is_user) {
		if (hsk->connected)
			return homa_sendmsg_user_connected(sk, msg, length);
		return homa_sendmsg_user_unconnected(sk, msg, length);
	}
	if (hsk->connected)
		return homa_sendmsg_in_kernel_connected(sk, msg, length);
	return homa_sendmsg_in_kernel_unconnected(sk, msg, length);
}

/**
 * homa_recvmsg() - Receive a message from a Homa socket.
 * @sk:          Socket on which the system call was invoked.
 * @msg:         Controlling information for the receive.
 * @len:         Total bytes of space available in msg->msg_iov; not used.
 * @flags:       Flags from system call; only MSG_DONTWAIT is used.
 * @addr_len:    Store the length of the sender address here
 * Return:       The length of the message on success, otherwise a negative
 *               errno.
 */
int homa_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags,
		 int *addr_len)
{
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_recvmsg_args control;
	struct homa_rpc *rpc;
	int nonblocking;
#ifndef __STRIP__ /* See strip.py */
	u64 start = homa_clock();
	u64 finish;
#endif /* See strip.py */
	bool in_kernel = hsk->in_kernel;
	int result;

	INC_METRIC(recv_calls, 1);
#ifndef __STRIP__ /* See strip.py */
	per_cpu(homa_offload_core, raw_smp_processor_id()).last_app_active = start;
#endif /* See strip.py */
	if (unlikely(!msg->msg_control)) {
		/* This test isn't strictly necessary, but it provides a
		 * hook for testing kernel call times.
		 */
		return -EINVAL;
	}
	if (msg->msg_controllen != sizeof(control))
		return -EINVAL;
	if (in_kernel) {
		memcpy(&control, msg->msg_control, sizeof(control));
	}
	else {
		if (unlikely(copy_from_user(&control, (void __user *)msg->msg_control,
					sizeof(control))))
			return -EFAULT;
	}
	control.completion_cookie = 0;
	tt_record2("homa_recvmsg starting, port %d, pid %d",
		   hsk->port, current->pid);

	if (control.num_bpages > HOMA_MAX_BPAGES) {
		result = -EINVAL;
		goto done;
	}
	if (!hsk->buffer_pool) {
		result = -EINVAL;
		goto done;
	}
	result = homa_pool_release_buffers(hsk->buffer_pool, control.num_bpages,
					   control.bpage_offsets);
	control.num_bpages = 0;
	if (result != 0)
		goto done;

	nonblocking = flags & MSG_DONTWAIT;
	if (control.id != 0) {
		rpc = homa_rpc_find_client(hsk, control.id); /* Locks RPC. */
		if (!rpc) {
			result = -EINVAL;
			goto done;
		}
		result = homa_wait_private(rpc, nonblocking);
		if (result != 0) {
			homa_rpc_unlock(rpc);
			control.id = 0;
			goto done;
		}
	} else {
		rpc = homa_wait_shared(hsk, nonblocking);
		if (IS_ERR(rpc)) {
			/* If we get here, it means there was an error that
			 * prevented us from finding an RPC to return. Errors
			 * in the RPC itself are handled below.
			 */
			result = PTR_ERR(rpc);
			goto done;
		}
	}
	result = rpc->error ? rpc->error : rpc->msgin.length;

#ifndef __STRIP__ /* See strip.py */
	/* Generate time traces on both ends for long elapsed times (used
	 * for performance debugging).
	 */
	if (rpc->hsk->homa->freeze_type == SLOW_RPC) {
		u64 elapsed = (homa_clock() - rpc->start_time) >> 10;

		if (elapsed <= hsk->homa->temp[1] &&
		    elapsed >= hsk->homa->temp[0] &&
		    homa_is_client(rpc->id) &&
		    rpc->msgin.length >= hsk->homa->temp[2] &&
		    rpc->msgin.length < hsk->homa->temp[3]) {
			tt_record4("Long RTT: kcycles %d, id %d, peer 0x%x, length %d",
				   elapsed, rpc->id, tt_addr(rpc->peer->addr),
				   rpc->msgin.length);
			homa_freeze(rpc, SLOW_RPC,
				    "Freezing because of long elapsed time for RPC id %d, peer 0x%x");
		}
	}
#endif /* See strip.py */

	/* Collect result information. */
	control.id = rpc->id;
	control.completion_cookie = rpc->completion_cookie;
	if (likely(rpc->msgin.length >= 0)) {
		control.num_bpages = rpc->msgin.num_bpages;
		memcpy(control.bpage_offsets, rpc->msgin.bpage_offsets,
		       sizeof(rpc->msgin.bpage_offsets));
	}
	if (sk->sk_family == AF_INET6) {
		struct sockaddr_in6 *in6 = msg->msg_name;

		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(rpc->dport);
		in6->sin6_addr = rpc->peer->addr;
		*addr_len = sizeof(*in6);
	} else {
		struct sockaddr_in *in4 = msg->msg_name;

		in4->sin_family = AF_INET;
		in4->sin_port = htons(rpc->dport);
		in4->sin_addr.s_addr = ipv6_to_ipv4(rpc->peer->addr);
		*addr_len = sizeof(*in4);
	}

	/* This indicates that the application now owns the buffers, so
	 * we won't free them in homa_rpc_end.
	 */
	rpc->msgin.num_bpages = 0;

	/* Must release the RPC lock (and potentially free the RPC) before
	 * copying the results back to user space.
	 */
	if (homa_is_client(rpc->id)) {
		homa_peer_add_ack(rpc);
		homa_rpc_end(rpc);
	} else {
		if (result < 0)
			homa_rpc_end(rpc);
		else
			rpc->state = RPC_IN_SERVICE;
	}
	homa_rpc_unlock(rpc); /* Locked by homa_wait_shared/private. */

	if (test_bit(SOCK_NOSPACE, &hsk->sock.sk_socket->flags)) {
		/* There are tasks waiting for tx memory, so reap
		 * immediately.
		 */
		homa_rpc_reap(hsk, true);
	}

done:
	if (in_kernel) {
		memcpy(msg->msg_control, &control, sizeof(control));
	}
	else {
		if (unlikely(copy_to_user((__force void __user *)msg->msg_control,
				  &control, sizeof(control)))) {
#ifndef __UPSTREAM__ /* See strip.py */
			/* Note: in this case the message's buffers will be leaked. */
			pr_notice("%s couldn't copy back args to 0x%px\n",
				  __func__, msg->msg_control);
#endif /* See strip.py */
			result = -EFAULT;
				  }
	}
#ifndef __STRIP__ /* See strip.py */
	finish = homa_clock();
#endif /* See strip.py */
	INC_METRIC(recv_cycles, finish - start);
	tt_record2("homa_recvmsg returning status %d, id %d", result,
		   control.id);
	return result;
}

/**
 * homa_hash() - Not needed for Homa.
 * @sk:    Socket for the operation
 * Return: ??
 */
int homa_hash(struct sock *sk)
{
	return 0;
}

/**
 * homa_unhash() - Not needed for Homa.
 * @sk:    Socket for the operation
 */
void homa_unhash(struct sock *sk)
{
}

/**
 * homa_softirq() - This function is invoked at SoftIRQ level to handle
 * incoming packets.
 * @skb:   The incoming packet.
 * Return: Always 0
 */
int homa_softirq(struct sk_buff *skb)
{
	struct sk_buff *packets, *other_pkts, *next;
	struct sk_buff **prev_link, **other_link;
	IF_NO_STRIP(struct homa *homa = homa_from_skb(skb));
	struct homa_common_hdr *h;
	int header_offset;
#ifndef __STRIP__ /* See strip.py */
	u64 start;

	start = homa_clock();
	per_cpu(homa_offload_core, raw_smp_processor_id()).last_active = start;
#endif /* See strip.py */
	INC_METRIC(softirq_calls, 1);

	/* skb may actually contain many distinct packets, linked through
	 * skb_shinfo(skb)->frag_list by the Homa GRO mechanism. Make a
	 * pass through the list to process all of the short packets,
	 * leaving the longer packets in the list. Also, perform various
	 * prep/cleanup/error checking functions.
	 */
	tt_record("homa_softirq starting");
	skb->next = skb_shinfo(skb)->frag_list;
	skb_shinfo(skb)->frag_list = NULL;
	packets = skb;
	prev_link = &packets;
	for (skb = packets; skb; skb = next) {
		next = skb->next;

		/* Make the header available at skb->data, even if the packet
		 * is fragmented. One complication: it's possible that the IP
		 * header hasn't yet been removed (this happens for GRO packets
		 * on the frag_list, since they aren't handled explicitly by IP.
		 */
		if (!homa_make_header_avl(skb)) {
#ifndef __STRIP__ /* See strip.py */
			if (homa->verbose)
				pr_notice("Homa can't handle fragmented packet (no space for header); discarding\n");
#endif /* See strip.py */
			UNIT_LOG("", "pskb discard");
			goto discard;
		}
		header_offset = skb_transport_header(skb) - skb->data;
		if (header_offset)
			__skb_pull(skb, header_offset);

		/* Reject packets that are too short or have bogus types. */
		h = (struct homa_common_hdr *)skb->data;
		if (unlikely(skb->len < sizeof(struct homa_common_hdr) ||
			     h->type < DATA || h->type > MAX_OP ||
			     skb->len < header_lengths[h->type - DATA])) {
#ifndef __STRIP__ /* See strip.py */
			const struct in6_addr saddr =
					skb_canonical_ipv6_saddr(skb);
			if (homa->verbose)
				pr_warn("Homa %s packet from %s too short: %d bytes\n",
					homa_symbol_for_type(h->type),
					homa_print_ipv6_addr(&saddr),
					skb->len - header_offset);
#endif /* See strip.py */
			INC_METRIC(short_packets, 1);
			goto discard;
		}

#ifndef __UPSTREAM__ /* See strip.py */
		/* Check for FREEZE here, rather than in homa_incoming.c, so
		 * it will work even if the RPC and/or socket are unknown.
		 */
		if (unlikely(h->type == FREEZE)) {
			if (!atomic_read(&tt_frozen)) {
				homa_rpc_log_active_tt(homa_from_skb(skb), 0);
				tt_record4("Freezing because of request on port %d from 0x%x:%d, id %d",
					   ntohs(h->dport),
					   tt_addr(skb_canonical_ipv6_saddr(skb)),
					   ntohs(h->sport),
					   homa_local_id(h->sender_id));
				tt_freeze();
			}
			goto discard;
		}
#endif /* See strip.py */

		/* Process the packet now if it is a control packet or
		 * if it contains an entire short message.
		 */
		if (h->type != DATA || ntohl(((struct homa_data_hdr *)h)
				->message_length) < 1400) {
			UNIT_LOG("; ", "homa_softirq shortcut type 0x%x",
				 h->type);
			*prev_link = skb->next;
			skb->next = NULL;
			homa_dispatch_pkts(skb);
		} else {
			prev_link = &skb->next;
		}
		continue;

discard:
		*prev_link = skb->next;
		kfree_skb(skb);
	}

	/* Now process the longer packets. Each iteration of this loop
	 * collects all of the packets for a particular RPC and dispatches
	 * them (batching the packets for an RPC allows more efficient
	 * generation of grants).
	 */
	while (packets) {
		struct in6_addr saddr, saddr2;
		struct homa_common_hdr *h2;
		struct sk_buff *skb2;

		skb = packets;
		prev_link = &skb->next;
		saddr = skb_canonical_ipv6_saddr(skb);
		other_pkts = NULL;
		other_link = &other_pkts;
		h = (struct homa_common_hdr *)skb->data;
		for (skb2 = skb->next; skb2; skb2 = next) {
			next = skb2->next;
			h2 = (struct homa_common_hdr *)skb2->data;
			if (h2->sender_id == h->sender_id) {
				saddr2 = skb_canonical_ipv6_saddr(skb2);
				if (ipv6_addr_equal(&saddr, &saddr2)) {
					*prev_link = skb2;
					prev_link = &skb2->next;
					continue;
				}
			}
			*other_link = skb2;
			other_link = &skb2->next;
		}
		*prev_link = NULL;
		*other_link = NULL;
#ifdef __UNIT_TEST__
		UNIT_LOG("; ", "id %lld, offsets", homa_local_id(h->sender_id));
		for (skb2 = packets; skb2; skb2 = skb2->next) {
			struct homa_data_hdr *h3 = (struct homa_data_hdr *)
					skb2->data;
			UNIT_LOG("", " %d", ntohl(h3->seg.offset));
		}
#endif /* __UNIT_TEST__ */
		homa_dispatch_pkts(packets);
		packets = other_pkts;
	}

#ifndef __STRIP__ /* See strip.py */
	atomic_dec(&per_cpu(homa_offload_core, raw_smp_processor_id()).softirq_backlog);
#endif /* See strip.py */
	INC_METRIC(softirq_cycles, homa_clock() - start);
	return 0;
}

/**
 * homa_err_handler_v4() - Invoked by IP to handle an incoming error
 * packet, such as ICMP UNREACHABLE.
 * @skb:   The incoming packet.
 * @info:  Information about the error that occurred?
 *
 * Return: zero, or a negative errno if the error couldn't be handled here.
 */
int homa_err_handler_v4(struct sk_buff *skb, u32 info)
{
	const struct icmphdr *icmp = icmp_hdr(skb);
	struct homa *homa = homa_from_skb(skb);
	struct in6_addr daddr;
	int type = icmp->type;
	int code = icmp->code;
	struct iphdr *iph;
	int error = 0;
	int port = 0;

	iph = (struct iphdr *)(skb->data);
	ipv6_addr_set_v4mapped(iph->daddr, &daddr);
	if (type == ICMP_DEST_UNREACH && code == ICMP_PORT_UNREACH) {
		struct homa_common_hdr *h = (struct homa_common_hdr *)(skb->data
				+ iph->ihl * 4);

		port = ntohs(h->dport);
		error = -ENOTCONN;
	} else if (type == ICMP_DEST_UNREACH) {
		if (code == ICMP_PROT_UNREACH)
			error = -EPROTONOSUPPORT;
		else
			error = -EHOSTUNREACH;
	} else {
		pr_notice("%s invoked with info %x, ICMP type %d, ICMP code %d\n",
			  __func__, info, type, code);
	}
	if (error != 0)
		homa_abort_rpcs(homa, &daddr, port, error);
	return 0;
}

/**
 * homa_err_handler_v6() - Invoked by IP to handle an incoming error
 * packet, such as ICMP UNREACHABLE.
 * @skb:    The incoming packet.
 * @opt:    Not used.
 * @type:   Type of ICMP packet.
 * @code:   Additional information about the error.
 * @offset: Not used.
 * @info:   Information about the error that occurred?
 *
 * Return: zero, or a negative errno if the error couldn't be handled here.
 */
int homa_err_handler_v6(struct sk_buff *skb, struct inet6_skb_parm *opt,
			u8 type,  u8 code,  int offset,  __be32 info)
{
	const struct ipv6hdr *iph = (const struct ipv6hdr *)skb->data;
	struct homa *homa = homa_from_skb(skb);
	int error = 0;
	int port = 0;

	if (type == ICMPV6_DEST_UNREACH && code == ICMPV6_PORT_UNREACH) {
		const struct homa_common_hdr *h;

		h = (struct homa_common_hdr *)(skb->data + sizeof(*iph));
		port = ntohs(h->dport);
		error = -ENOTCONN;
	} else if (type == ICMPV6_DEST_UNREACH && code == ICMPV6_ADDR_UNREACH) {
		error = -EHOSTUNREACH;
	} else if (type == ICMPV6_PARAMPROB && code == ICMPV6_UNK_NEXTHDR) {
		error = -EPROTONOSUPPORT;
	}
	if (error != 0)
		homa_abort_rpcs(homa, &iph->daddr, port, error);
	return 0;
}

/**
 * homa_poll() - Invoked by Linux as part of implementing select, poll,
 * epoll, etc.
 * @file:  Open file that is participating in a poll, select, etc.
 * @sock:  A Homa socket, associated with @file.
 * @wait:  This table will be registered with the socket, so that it
 *         is notified when the socket's ready state changes.
 *
 * Return: A mask of bits such as EPOLLIN, which indicate the current
 *         state of the socket.
 */
__poll_t homa_poll(struct file *file, struct socket *sock,
		   struct poll_table_struct *wait)
{
	struct homa_sock *hsk = homa_sk(sock->sk);
	__poll_t mask;

	mask = 0;
	sock_poll_wait(file, sock, wait);
	tt_record2("homa_poll found sk_wmem_alloc %d, sk_sndbuf %d",
		   refcount_read(&hsk->sock.sk_wmem_alloc),
		   hsk->sock.sk_sndbuf);
	if (homa_sock_wmem_avl(hsk))
		mask |= EPOLLOUT | EPOLLWRNORM;
	else
		set_bit(SOCK_NOSPACE, &hsk->sock.sk_socket->flags);

	if (hsk->shutdown)
		mask |= EPOLLIN;

	if (!list_empty(&hsk->ready_rpcs))
		mask |= EPOLLIN | EPOLLRDNORM;
	tt_record1("homa_poll returning mask 0x%x", (__force int)mask);
	return mask;
}

#ifndef __STRIP__ /* See strip.py */
/**
 * homa_dointvec() - This function is a wrapper around proc_dointvec. It is
 * invoked to read and write sysctl values and also update other values
 * that depend on the modified value.
 * @table:    sysctl table describing value to be read or written.
 * @write:    Nonzero means value is being written, 0 means read.
 * @buffer:   Address in user space of the input/output data.
 * @lenp:     Not exactly sure.
 * @ppos:     Not exactly sure.
 *
 * Return: 0 for success, nonzero for error.
 */
int homa_dointvec(const struct ctl_table *table, int write,
		  void *buffer, size_t *lenp, loff_t *ppos)
{
	struct homa *homa = homa_net_from_net(current->nsproxy->net_ns)->homa;
	struct ctl_table table_copy;
	int result;

	/* Generate a new ctl_table that refers to a field in the
	 * net-specific struct homa.
	 */
	table_copy = *table;
	table_copy.data = ((char *)homa) + (uintptr_t)table_copy.data;

	result = proc_dointvec(&table_copy, write, buffer, lenp, ppos);
	if (write) {
		/* Update any information that is dependent on sysctl values
		 * (don't worry about which value changed, just refresh all
		 * dependent information).
		 */
		homa_incoming_sysctl_changed(homa);

		/* For this value, only call the method when this
		 * particular value was written (don't want to increment
		 * cutoff_version otherwise).
		 */
		if (table_copy.data == &homa->unsched_cutoffs ||
		    table_copy.data == &homa->num_priorities) {
			homa_prios_changed(homa);
		}

		if (homa->next_id != 0) {
			atomic64_set(&homa->next_outgoing_id, homa->next_id);
			homa->next_id = 0;
		}

		/* Handle the special value "action" by invoking a function
		 * to print information to the log.
		 */
		if (table_copy.data == &homa->sysctl_action) {
			if (homa->sysctl_action == 2) {
				homa_rpc_log_active(homa, 0);
			} else if (homa->sysctl_action == 3) {
				tt_record("Freezing because of sysctl");
				tt_freeze();
			} else if (homa->sysctl_action == 4) {
				homa_pacer_log_throttled(homa->pacer);
			} else if (homa->sysctl_action == 5) {
				tt_printk();
			} else if (homa->sysctl_action == 6) {
				tt_record("Calling homa_rpc_log_active because of action 6");
				homa_rpc_log_active_tt(homa, 0);
				tt_record("Freezing because of action 6");
				tt_freeze();
			} else if (homa->sysctl_action == 7) {
				homa_rpc_log_active_tt(homa, 0);
				tt_record("Freezing cluster because of action 7");
				homa_freeze_peers();
				tt_record("Finished freezing cluster");
				tt_freeze();
			} else if (homa->sysctl_action == 8) {
				pr_notice("homa_total_incoming is %d\n",
					  atomic_read(&homa->grant->total_incoming));
			} else if (homa->sysctl_action == 9) {
				tt_print_file("/users/ouster/node.tt");
			} else {
				homa_rpc_log_active(homa, homa->sysctl_action);
			}
			homa->sysctl_action = 0;
		}
	}
	return result;
}

/**
 * homa_sysctl_softirq_cores() - This function is invoked to handle sysctl
 * requests for the "gen3_softirq_cores" target, which requires special
 * processing.
 * @table:    sysctl table describing value to be read or written.
 * @write:    Nonzero means value is being written, 0 means read.
 * @buffer:   Address in user space of the input/output data.
 * @lenp:     Not exactly sure.
 * @ppos:     Not exactly sure.
 *
 * Return: 0 for success, nonzero for error.
 */
int homa_sysctl_softirq_cores(const struct ctl_table *table, int write,
			      void *buffer, size_t *lenp, loff_t *ppos)
{
	struct homa_offload_core *offload_core;
	struct ctl_table table_copy;
	int max_values, *values;
	int result, i;

	max_values = (NUM_GEN3_SOFTIRQ_CORES + 1) * nr_cpu_ids;
	values = kmalloc_array(max_values, sizeof(int), GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	table_copy = *table;
	table_copy.data = values;
	if (write) {
		/* First value is core id, others are contents of its
		 * gen3_softirq_cores.
		 */
		for (i = 0; i < max_values ; i++)
			values[i] = -1;
		table_copy.maxlen = max_values;
		result = proc_dointvec(&table_copy, write, buffer, lenp, ppos);
		if (result != 0)
			goto done;
		for (i = 0; i < max_values;
				i += NUM_GEN3_SOFTIRQ_CORES + 1) {
			int j;

			if (values[i] < 0)
				break;
			offload_core = &per_cpu(homa_offload_core, values[i]);
			for (j = 0; j < NUM_GEN3_SOFTIRQ_CORES; j++)
				offload_core->gen3_softirq_cores[j] =
						values[i +  j + 1];
		}
	} else {
		/* Read: return values from all of the cores. */
		int *dst;

		table_copy.maxlen = 0;
		dst = values;
		for (i = 0; i < nr_cpu_ids; i++) {
			int j;

			*dst = i;
			dst++;
			table_copy.maxlen += sizeof(int);
			offload_core = &per_cpu(homa_offload_core, i);
			for (j = 0; j < NUM_GEN3_SOFTIRQ_CORES; j++) {
				*dst = offload_core->gen3_softirq_cores[j];
				dst++;
				table_copy.maxlen += sizeof(int);
			}
		}
		result = proc_dointvec(&table_copy, write, buffer, lenp, ppos);
	}
done:
	kfree(values);
	return result;
}
#endif /* See strip.py */

/**
 * homa_hrtimer() - This function is invoked by the hrtimer mechanism to
 * wake up the timer thread. Runs at IRQ level.
 * @timer:   The timer that triggered; not used.
 *
 * Return:   Always HRTIMER_RESTART.
 */
enum hrtimer_restart homa_hrtimer(struct hrtimer *timer)
{
	wake_up_process(timer_kthread);
	return HRTIMER_NORESTART;
}

/**
 * homa_timer_main() - Top-level function for the timer thread.
 * @transport:  Pointer to struct homa.
 *
 * Return:         Always 0.
 */
int homa_timer_main(void *transport)
{
	struct homa *homa = (struct homa *)transport;
	ktime_t tick_interval;
	u64 nsec;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
	hrtimer_init(&hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer.function = &homa_hrtimer;
#else
	hrtimer_setup(&hrtimer, homa_hrtimer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
#endif
	nsec = 1000000;                   /* 1 ms */
	tick_interval = ns_to_ktime(nsec);
	while (1) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!timer_thread_exit) {
			hrtimer_start(&hrtimer, tick_interval,
				      HRTIMER_MODE_REL);
			schedule();
		}
		__set_current_state(TASK_RUNNING);
		if (timer_thread_exit)
			break;
		homa_timer(homa);
	}
	hrtimer_cancel(&hrtimer);
	kthread_complete_and_exit(&timer_thread_done, 0);
	return 0;
}

#ifndef __UNIT_TEST__
MODULE_LICENSE("Dual BSD/GPL");
#endif /* __UNIT_TEST__ */
MODULE_AUTHOR("John Ousterhout <ouster@cs.stanford.edu>");
MODULE_DESCRIPTION("Homa transport protocol");
MODULE_VERSION("1.0");

/* Arrange for this module to be loaded automatically when a Homa socket is
 * opened. Apparently symbols don't work in the macros below, so must use
 * numeric values for IPPROTO_HOMA (146) and SOCK_DGRAM(2).
 */
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_INET, 146, 2);
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_INET6, 146, 2);
