// SPDX-License-Identifier: BSD-2-Clause

/* This file manages homa_sock and homa_socktab objects. */

#include "homa_impl.h"
#include "homa_interest.h"
#include "homa_peer.h"
#include "homa_pool.h"

#ifndef __STRIP__ /* See strip.py */
#include "homa_grant.h"
#endif /* See strip.py */

/**
 * homa_socktab_init() - Constructor for homa_socktabs.
 * @socktab:  The object to initialize; previous contents are discarded.
 */
void homa_socktab_init(struct homa_socktab *socktab)
{
	int i;

	spin_lock_init(&socktab->write_lock);
	for (i = 0; i < HOMA_SOCKTAB_BUCKETS; i++)
		INIT_HLIST_HEAD(&socktab->buckets[i]);
}

/**
 * homa_socktab_destroy() - Destructor for homa_socktabs: deletes all
 * existing sockets.
 * @socktab:  The object to destroy.
 * @hnet:     If non-NULL, only sockets for this namespace are deleted.
 */
void homa_socktab_destroy(struct homa_socktab *socktab, struct homa_net *hnet)
{
	struct homa_socktab_scan scan;
	struct homa_sock *hsk;

	for (hsk = homa_socktab_start_scan(socktab, &scan); hsk;
			hsk = homa_socktab_next(&scan)) {
		if (hnet && hnet != hsk->hnet)
			continue;

		/* In actual use there should be no sockets left when this
		 * function is invoked, so the code below will never be
		 * invoked. However, it is useful during unit tests.
		 */
		homa_sock_shutdown(hsk);
		homa_sock_destroy(&hsk->sock);
	}
	homa_socktab_end_scan(&scan);
}

/**
 * homa_socktab_start_scan() - Begin an iteration over all of the sockets
 * in a socktab.
 * @socktab:   Socktab to scan.
 * @scan:      Will hold the current state of the scan; any existing
 *             contents are discarded. The caller must eventually pass this
 *             to homa_socktab_end_scan.
 *
 * Return:     The first socket in the table, or NULL if the table is
 *             empty. If non-NULL, a reference is held on the socket to
 *             prevent its deletion.
 *
 * Each call to homa_socktab_next will return the next socket in the table.
 * All sockets that are present in the table at the time this function is
 * invoked will eventually be returned, as long as they are not removed
 * from the table. It is safe to remove sockets from the table while the
 * scan is in progress. If a socket is removed from the table during the scan,
 * it may or may not be returned by homa_socktab_next. New entries added
 * during the scan may or may not be returned.
 */
struct homa_sock *homa_socktab_start_scan(struct homa_socktab *socktab,
					  struct homa_socktab_scan *scan)
{
	scan->socktab = socktab;
	scan->hsk = NULL;
	scan->current_bucket = -1;

	return homa_socktab_next(scan);
}

/**
 * homa_socktab_next() - Return the next socket in an iteration over a socktab.
 * @scan:      State of the scan.
 *
 * Return:     The next socket in the table, or NULL if the iteration has
 *             returned all of the sockets in the table.  If non-NULL, a
 *             reference is held on the socket to prevent its deletion.
 *             Sockets are not returned in any particular order. It's
 *             possible that the returned socket has been destroyed.
 */
struct homa_sock *homa_socktab_next(struct homa_socktab_scan *scan)
{
	struct hlist_head *bucket;
	struct hlist_node *next;

	rcu_read_lock();
	if (scan->hsk) {
		sock_put(&scan->hsk->sock);
		next = rcu_dereference(hlist_next_rcu(&scan->hsk->socktab_links));
		if (next)
			goto success;
	}
	for (scan->current_bucket++;
	     scan->current_bucket < HOMA_SOCKTAB_BUCKETS;
	     scan->current_bucket++) {
		bucket = &scan->socktab->buckets[scan->current_bucket];
		next = rcu_dereference(hlist_first_rcu(bucket));
		if (next)
			goto success;
	}
	scan->hsk = NULL;
	rcu_read_unlock();
	return NULL;

success:
	scan->hsk =  hlist_entry(next, struct homa_sock, socktab_links);
	sock_hold(&scan->hsk->sock);
	rcu_read_unlock();
	return scan->hsk;
}

/**
 * homa_socktab_end_scan() - Must be invoked on completion of each scan
 * to clean up state associated with the scan.
 * @scan:      State of the scan.
 */
void homa_socktab_end_scan(struct homa_socktab_scan *scan)
{
	if (scan->hsk) {
		sock_put(&scan->hsk->sock);
		scan->hsk = NULL;
	}
}

/**
 * homa_sock_init() - Constructor for homa_sock objects. This function
 * initializes only the parts of the socket that are owned by Homa.
 * @hsk:    Object to initialize. The Homa-specific parts must have been
 *          initialized to zeroes by the caller.
 *
 * Return: 0 for success, otherwise a negative errno.
 */
int homa_sock_init(struct homa_sock *hsk)
{
	struct homa_pool *buffer_pool;
	struct homa_socktab *socktab;
	struct homa_sock *other;
	struct homa_net *hnet;
	struct homa *homa;
	int starting_port;
	int result = 0;
	int i;

	hnet = (struct homa_net *)net_generic(sock_net(&hsk->sock),
					      homa_net_id);
	homa = hnet->homa;
	socktab = homa->socktab;

	/* Initialize fields outside the Homa part. */
	hsk->sock.sk_sndbuf = homa->wmem_max;
	sock_set_flag(&hsk->inet.sk, SOCK_RCU_FREE);
#ifndef __STRIP__ /* See strip.py */
	if (homa->hijack_tcp)
		hsk->sock.sk_protocol = IPPROTO_TCP;
#endif /* See strip.py */

	/* Do things requiring memory allocation before locking the socket,
	 * so that GFP_ATOMIC is not needed.
	 */
	buffer_pool = homa_pool_alloc(hsk);
	if (IS_ERR(buffer_pool))
		return PTR_ERR(buffer_pool);

	/* Initialize Homa-specific fields. */
	hsk->homa = homa;
	hsk->hnet = hnet;
	hsk->buffer_pool = buffer_pool;

	/* Pick a default port. Must keep the socktab locked from now
	 * until the new socket is added to the socktab, to ensure that
	 * no other socket chooses the same port.
	 */
	spin_lock_bh(&socktab->write_lock);
	starting_port = hnet->prev_default_port;
	while (1) {
		hnet->prev_default_port++;
		if (hnet->prev_default_port < HOMA_MIN_DEFAULT_PORT)
			hnet->prev_default_port = HOMA_MIN_DEFAULT_PORT;
		other = homa_sock_find(hnet, hnet->prev_default_port);
		if (!other)
			break;
		sock_put(&other->sock);
		if (hnet->prev_default_port == starting_port) {
			spin_unlock_bh(&socktab->write_lock);
			hsk->shutdown = true;
			hsk->homa = NULL;
			result = -EADDRNOTAVAIL;
			goto error;
		}
	}
	hsk->port = hnet->prev_default_port;
	hsk->inet.inet_num = hsk->port;
	hsk->inet.inet_sport = htons(hsk->port);

	hsk->is_server = false;
	hsk->shutdown = false;
	// Hard-coded for now,will see how this works or not
	hsk->in_kernel = true;
	hsk->ip_header_length = (hsk->inet.sk.sk_family == AF_INET) ?
				sizeof(struct iphdr) : sizeof(struct ipv6hdr);
	spin_lock_init(&hsk->lock);
	atomic_set(&hsk->protect_count, 0);
	INIT_LIST_HEAD(&hsk->active_rpcs);
	INIT_LIST_HEAD(&hsk->dead_rpcs);
	hsk->dead_skbs = 0;
	INIT_LIST_HEAD(&hsk->waiting_for_bufs);
	INIT_LIST_HEAD(&hsk->ready_rpcs);
	INIT_LIST_HEAD(&hsk->interests);
	for (i = 0; i < HOMA_CLIENT_RPC_BUCKETS; i++) {
		struct homa_rpc_bucket *bucket = &hsk->client_rpc_buckets[i];

		spin_lock_init(&bucket->lock);
		bucket->id = i;
		INIT_HLIST_HEAD(&bucket->rpcs);
	}
	for (i = 0; i < HOMA_SERVER_RPC_BUCKETS; i++) {
		struct homa_rpc_bucket *bucket = &hsk->server_rpc_buckets[i];

		spin_lock_init(&bucket->lock);
		bucket->id = i + 1000000;
		INIT_HLIST_HEAD(&bucket->rpcs);
	}
	hlist_add_head_rcu(&hsk->socktab_links,
			   &socktab->buckets[homa_socktab_bucket(hnet,
								 hsk->port)]);
	hsk->connected = false;
	memset(&(hsk->target_addr), 0, sizeof(hsk->target_addr));
	spin_unlock_bh(&socktab->write_lock);
	return result;

error:
	homa_pool_free(buffer_pool);
	return result;
}

/*
 * homa_sock_unlink() - Unlinks a socket from its socktab and does
 * related cleanups. Once this method returns, the socket will not be
 * discoverable through the socktab.
 * @hsk:  Socket to unlink.
 */
void homa_sock_unlink(struct homa_sock *hsk)
{
	struct homa_socktab *socktab = hsk->homa->socktab;

	spin_lock_bh(&socktab->write_lock);
	hlist_del_rcu(&hsk->socktab_links);
	spin_unlock_bh(&socktab->write_lock);
}

/**
 * homa_sock_shutdown() - Disable a socket so that it can no longer
 * be used for either sending or receiving messages. Any system calls
 * currently waiting to send or receive messages will be aborted. This
 * function will terminate any existing use of the socket, but it does
 * not free up socket resources: that happens in homa_sock_destroy.
 * @hsk:       Socket to shut down.
 */
void homa_sock_shutdown(struct homa_sock *hsk)
{
	struct homa_interest *interest;
	struct homa_rpc *rpc;

	tt_record1("Starting shutdown for socket %d", hsk->port);
	homa_sock_lock(hsk);
	if (hsk->shutdown || !hsk->homa) {
		homa_sock_unlock(hsk);
		return;
	}

	/* The order of cleanup is very important, because there could be
	 * active operations that hold RPC locks but not the socket lock.
	 * 1. Set @shutdown; this ensures that no new RPCs will be created for
	 *    this socket (though some creations might already be in progress).
	 * 2. Remove the socket from its socktab: this ensures that
	 *    incoming packets for the socket will be dropped.
	 * 3. Go through all of the RPCs and delete them; this will
	 *    synchronize with any operations in progress.
	 * 4. Perform other socket cleanup: at this point we know that
	 *    there will be no concurrent activities on individual RPCs.
	 * 5. Don't delete the buffer pool until after all of the RPCs
	 *    have been reaped.
	 * See "Homa Locking Strategy" in homa_impl.h for additional information
	 * about locking.
	 */
	hsk->shutdown = true;
	homa_sock_unlink(hsk);
	homa_sock_unlock(hsk);

	rcu_read_lock();
	list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
		homa_rpc_lock(rpc);
		homa_rpc_end(rpc);
		homa_rpc_unlock(rpc);
	}
	rcu_read_unlock();

	homa_sock_lock(hsk);
	while (!list_empty(&hsk->interests)) {
		interest = list_first_entry(&hsk->interests,
					    struct homa_interest, links);
		list_del_init(&interest->links);
		atomic_set_release(&interest->ready, 1);
		wake_up(&interest->wait_queue);
	}
	homa_sock_unlock(hsk);
	tt_record1("Finished shutdown for socket %d", hsk->port);
}

/**
 * homa_sock_destroy() - Release all of the internal resources associated
 * with a socket; is invoked at time when that is safe (i.e., all references
 * on the socket have been dropped).
 * @hsk:       Socket to destroy.
 */
void homa_sock_destroy(struct sock *sk)
{
	struct homa_sock *hsk = homa_sk(sk);
	IF_NO_STRIP(int i = 0);

	if (!hsk->homa)
		return;

	tt_record1("Starting to destroy socket %d", hsk->port);
	while (!list_empty(&hsk->dead_rpcs)) {
		homa_rpc_reap(hsk, true);
#ifndef __STRIP__ /* See strip.py */
		i++;
		if (i == 5) {
			tt_record("Freezing because reap seems hung");
			tt_freeze();
		}
#endif /* See strip.py */
	}

	WARN_ON_ONCE(refcount_read(&hsk->sock.sk_wmem_alloc) != 1);
#ifdef __UNIT_TEST__
	{
		u64 tx_memory = refcount_read(&hsk->sock.sk_wmem_alloc);

		if (tx_memory != 1)
			FAIL(" sk_wmem_alloc %llu after shutdown for port %d",
			     tx_memory, hsk->port);
	}
#endif /* __UNIT_TEST__ */

	if (hsk->buffer_pool) {
		homa_pool_free(hsk->buffer_pool);
		hsk->buffer_pool = NULL;
	}
	tt_record1("Finished destroying socket %d", hsk->port);
}

/**
 * homa_sock_bind() - Associates a server port with a socket; if there
 * was a previous server port assignment for @hsk, it is abandoned.
 * @hnet:      Network namespace with which port is associated.
 * @hsk:       Homa socket.
 * @port:      Desired server port for @hsk. If 0, then this call
 *             becomes a no-op: the socket will continue to use
 *             its randomly assigned client port.
 *
 * Return:  0 for success, otherwise a negative errno.
 */
int homa_sock_bind(struct homa_net *hnet, struct homa_sock *hsk,
		   u16 port)
{
	struct homa_socktab *socktab = hnet->homa->socktab;
	struct homa_sock *owner;
	int result = 0;

	if (port == 0)
		return result;
	if (port >= HOMA_MIN_DEFAULT_PORT)
		return -EINVAL;
	homa_sock_lock(hsk);
	spin_lock_bh(&socktab->write_lock);
	if (hsk->shutdown) {
		result = -ESHUTDOWN;
		goto done;
	}

	owner = homa_sock_find(hnet, port);
	if (owner) {
		sock_put(&owner->sock);
		if (owner != hsk)
			result = -EADDRINUSE;
		goto done;
	}
	hlist_del_rcu(&hsk->socktab_links);
	hsk->port = port;
	hsk->inet.inet_num = port;
	hsk->inet.inet_sport = htons(hsk->port);
	hlist_add_head_rcu(&hsk->socktab_links,
			   &socktab->buckets[homa_socktab_bucket(hnet, port)]);
	hsk->is_server = true;
done:
	spin_unlock_bh(&socktab->write_lock);
	homa_sock_unlock(hsk);
	return result;
}

/**
 * homa_sock_find() - Returns the socket associated with a given port.
 * @hnet:       Network namespace where the socket will be used.
 * @port:       The port of interest.
 * Return:      The socket that owns @port, or NULL if none. If non-NULL
 *              then this method has taken a reference on the socket and
 *              the caller must call sock_put to release it.
 */
struct homa_sock *homa_sock_find(struct homa_net *hnet, u16 port)
{
	int bucket = homa_socktab_bucket(hnet, port);
	struct homa_sock *result = NULL;
	struct homa_sock *hsk;

	rcu_read_lock();
	hlist_for_each_entry_rcu(hsk, &hnet->homa->socktab->buckets[bucket],
				 socktab_links) {
		if (hsk->port == port && hsk->hnet == hnet) {
			result = hsk;
			sock_hold(&hsk->sock);
			break;
		}
	}
	rcu_read_unlock();
	return result;
}

#ifndef __STRIP__ /* See strip.py */
/**
 * homa_sock_lock_slow() - This function implements the slow path for
 * acquiring a socketC lock. It is invoked when a socket lock isn't immediately
 * available. It waits for the lock, but also records statistics about
 * the waiting time.
 * @hsk:    socket to  lock.
 */
void homa_sock_lock_slow(struct homa_sock *hsk)
	__acquires(&hsk->lock)
{
	u64 start = homa_clock();

	tt_record("beginning wait for socket lock");
	spin_lock_bh(&hsk->lock);
	tt_record("ending wait for socket lock");
	INC_METRIC(socket_lock_misses, 1);
	INC_METRIC(socket_lock_miss_cycles, homa_clock() - start);
}

/**
 * homa_bucket_lock_slow() - This function implements the slow path for
 * locking a bucket in one of the hash tables of RPCs. It is invoked when a
 * lock isn't immediately available. It waits for the lock, but also records
 * statistics about the waiting time.
 * @bucket:    The hash table bucket to lock.
 * @id:        Id of the RPC on whose behalf the bucket is being locked.
 *             Used only for metrics.
 */
void homa_bucket_lock_slow(struct homa_rpc_bucket *bucket, u64 id)
	__acquires(rpc_bucket_lock)
{
	u64 start = homa_clock();

	tt_record2("beginning wait for rpc lock, id %d, (bucket %d)",
		   id, bucket->id);
	spin_lock_bh(&bucket->lock);
	tt_record2("ending wait for bucket lock, id %d, (bucket %d)",
		   id, bucket->id);
	if (homa_is_client(id)) {
		INC_METRIC(client_lock_misses, 1);
		INC_METRIC(client_lock_miss_cycles, homa_clock() - start);
	} else {
		INC_METRIC(server_lock_misses, 1);
		INC_METRIC(server_lock_miss_cycles, homa_clock() - start);
	}
}
#endif /* See strip.py */

/**
 * homa_sock_wait_wmem() - Block the thread until @hsk's usage of tx
 * packet memory drops below the socket's limit.
 * @hsk:          Socket of interest.
 * @nonblocking:  If there's not enough memory, return -EWOLDBLOCK instead
 *                of blocking.
 * Return: 0 for success, otherwise a negative errno.
 */
int homa_sock_wait_wmem(struct homa_sock *hsk, int nonblocking)
{
	long timeo = hsk->sock.sk_sndtimeo;
	int result;

	if (nonblocking)
		timeo = 0;
	set_bit(SOCK_NOSPACE, &hsk->sock.sk_socket->flags);
	tt_record2("homa_sock_wait_wmem waiting on port %d, wmem %d",
		   hsk->port, refcount_read(&hsk->sock.sk_wmem_alloc));
	result = wait_event_interruptible_timeout(*sk_sleep(&hsk->sock),
				homa_sock_wmem_avl(hsk) || hsk->shutdown,
				timeo);
	tt_record4("homa_sock_wait_wmem woke up on port %d with result %d, wmem %d, signal pending %d",
		   hsk->port, result, refcount_read(&hsk->sock.sk_wmem_alloc),
		   signal_pending(current));
	if (signal_pending(current))
		return -EINTR;
	if (result == 0)
		return -EWOULDBLOCK;
	return 0;
}
