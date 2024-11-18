// SPDX-License-Identifier: GPL-2.0
/*
 * DFS referral cache routines
 *
 * Copyright (c) 2018-2019 Paulo Alcantara <palcantara@suse.de>
 */

#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/jhash.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/nls.h>
#include <linux/workqueue.h>
#include "cifsglob.h"
#include "smb2pdu.h"
#include "smb2proto.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "cifs_unicode.h"
#include "smb2glob.h"

#include "dfs_cache.h"

#define CACHE_HTABLE_SIZE 32
#define CACHE_MAX_ENTRIES 64

#define IS_INTERLINK_SET(v) ((v) & (DFSREF_REFERRAL_SERVER | \
				    DFSREF_STORAGE_SERVER))

struct cache_dfs_tgt {
	char *name;
	struct list_head list;
};

struct cache_entry {
	struct hlist_node hlist;
	const char *path;
	int ttl;
	int srvtype;
	int flags;
	struct timespec64 etime;
	int path_consumed;
	int numtgts;
	struct list_head tlist;
	struct cache_dfs_tgt *tgthint;
	struct rcu_head rcu;
};

struct vol_info {
	char *fullpath;
	spinlock_t smb_vol_lock;
	struct smb_vol smb_vol;
	char *mntdata;
	struct list_head list;
	struct list_head rlist;
	struct kref refcnt;
};

static struct kmem_cache *cache_slab __read_mostly;
static struct workqueue_struct *dfscache_wq __read_mostly;

static int cache_ttl;
static DEFINE_SPINLOCK(cache_ttl_lock);

static struct nls_table *cache_nlsc;

/*
 * Number of entries in the cache
 */
static size_t cache_count;

static struct hlist_head cache_htable[CACHE_HTABLE_SIZE];
static DEFINE_MUTEX(list_lock);

static LIST_HEAD(vol_list);
static DEFINE_SPINLOCK(vol_list_lock);

static void refresh_cache_worker(struct work_struct *work);

static DECLARE_DELAYED_WORK(refresh_task, refresh_cache_worker);

static int get_normalized_path(const char *path, char **npath)
{
	if (!path || strlen(path) < 3 || (*path != '\\' && *path != '/'))
		return -EINVAL;

	if (*path == '\\') {
		*npath = (char *)path;
	} else {
		*npath = kstrndup(path, strlen(path), GFP_KERNEL);
		if (!*npath)
			return -ENOMEM;
		convert_delimiter(*npath, '\\');
	}
	return 0;
}

static inline void free_normalized_path(const char *path, char *npath)
{
	if (path != npath)
		kfree(npath);
}

static inline bool cache_entry_expired(const struct cache_entry *ce)
{
	struct timespec64 ts;

	ktime_get_coarse_real_ts64(&ts);
	return timespec64_compare(&ts, &ce->etime) >= 0;
}

static inline void free_tgts(struct cache_entry *ce)
{
	struct cache_dfs_tgt *t, *n;

	list_for_each_entry_safe(t, n, &ce->tlist, list) {
		list_del(&t->list);
		kfree(t->name);
		kfree(t);
	}
}

static void free_cache_entry(struct rcu_head *rcu)
{
	struct cache_entry *ce = container_of(rcu, struct cache_entry, rcu);

	kmem_cache_free(cache_slab, ce);
}

static inline void flush_cache_ent(struct cache_entry *ce)
{
	if (hlist_unhashed(&ce->hlist))
		return;

	hlist_del_init_rcu(&ce->hlist);
	kfree(ce->path);
	free_tgts(ce);
	cache_count--;
	call_rcu(&ce->rcu, free_cache_entry);
}

static void flush_cache_ents(void)
{
	int i;

	rcu_read_lock();
	for (i = 0; i < CACHE_HTABLE_SIZE; i++) {
		struct hlist_head *l = &cache_htable[i];
		struct cache_entry *ce;

		hlist_for_each_entry_rcu(ce, l, hlist)
			flush_cache_ent(ce);
	}
	rcu_read_unlock();
}

/*
 * dfs cache /proc file
 */
static int dfscache_proc_show(struct seq_file *m, void *v)
{
	int bucket;
	struct cache_entry *ce;
	struct cache_dfs_tgt *t;

	seq_puts(m, "DFS cache\n---------\n");

	mutex_lock(&list_lock);

	rcu_read_lock();
	hash_for_each_rcu(cache_htable, bucket, ce, hlist) {
		seq_printf(m,
			   "cache entry: path=%s,type=%s,ttl=%d,etime=%ld,"
			   "interlink=%s,path_consumed=%d,expired=%s\n",
			   ce->path,
			   ce->srvtype == DFS_TYPE_ROOT ? "root" : "link",
			   ce->ttl, ce->etime.tv_nsec,
			   IS_INTERLINK_SET(ce->flags) ? "yes" : "no",
			   ce->path_consumed,
			   cache_entry_expired(ce) ? "yes" : "no");

		list_for_each_entry(t, &ce->tlist, list) {
			seq_printf(m, "  %s%s\n",
				   t->name,
				   ce->tgthint == t ? " (target hint)" : "");
		}

	}
	rcu_read_unlock();

	mutex_unlock(&list_lock);
	return 0;
}

static ssize_t dfscache_proc_write(struct file *file, const char __user *buffer,
				   size_t count, loff_t *ppos)
{
	char c;
	int rc;

	rc = get_user(c, buffer);
	if (rc)
		return rc;

	if (c != '0')
		return -EINVAL;

	cifs_dbg(FYI, "clearing dfs cache");
	mutex_lock(&list_lock);
	flush_cache_ents();
	mutex_unlock(&list_lock);

	return count;
}

static int dfscache_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dfscache_proc_show, NULL);
}

const struct file_operations dfscache_proc_fops = {
	.open		= dfscache_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= dfscache_proc_write,
};

#ifdef CONFIG_CIFS_DEBUG2
static inline void dump_tgts(const struct cache_entry *ce)
{
	struct cache_dfs_tgt *t;

	cifs_dbg(FYI, "target list:\n");
	list_for_each_entry(t, &ce->tlist, list) {
		cifs_dbg(FYI, "  %s%s\n", t->name,
			 ce->tgthint == t ? " (target hint)" : "");
	}
}

static inline void dump_ce(const struct cache_entry *ce)
{
	cifs_dbg(FYI, "cache entry: path=%s,type=%s,ttl=%d,etime=%ld,"
		 "interlink=%s,path_consumed=%d,expired=%s\n", ce->path,
		 ce->srvtype == DFS_TYPE_ROOT ? "root" : "link", ce->ttl,
		 ce->etime.tv_nsec,
		 IS_INTERLINK_SET(ce->flags) ? "yes" : "no",
		 ce->path_consumed,
		 cache_entry_expired(ce) ? "yes" : "no");
	dump_tgts(ce);
}

static inline void dump_refs(const struct dfs_info3_param *refs, int numrefs)
{
	int i;

	cifs_dbg(FYI, "DFS referrals returned by the server:\n");
	for (i = 0; i < numrefs; i++) {
		const struct dfs_info3_param *ref = &refs[i];

		cifs_dbg(FYI,
			 "\n"
			 "flags:         0x%x\n"
			 "path_consumed: %d\n"
			 "server_type:   0x%x\n"
			 "ref_flag:      0x%x\n"
			 "path_name:     %s\n"
			 "node_name:     %s\n"
			 "ttl:           %d (%dm)\n",
			 ref->flags, ref->path_consumed, ref->server_type,
			 ref->ref_flag, ref->path_name, ref->node_name,
			 ref->ttl, ref->ttl / 60);
	}
}
#else
#define dump_tgts(e)
#define dump_ce(e)
#define dump_refs(r, n)
#endif

/**
 * dfs_cache_init - Initialize DFS referral cache.
 *
 * Return zero if initialized successfully, otherwise non-zero.
 */
int dfs_cache_init(void)
{
	int rc;
	int i;

	dfscache_wq = alloc_workqueue("cifs-dfscache",
				      WQ_FREEZABLE | WQ_MEM_RECLAIM, 1);
	if (!dfscache_wq)
		return -ENOMEM;

	cache_slab = kmem_cache_create("cifs_dfs_cache",
				       sizeof(struct cache_entry), 0,
				       SLAB_HWCACHE_ALIGN, NULL);
	if (!cache_slab) {
		rc = -ENOMEM;
		goto out_destroy_wq;
	}

	for (i = 0; i < CACHE_HTABLE_SIZE; i++)
		INIT_HLIST_HEAD(&cache_htable[i]);

	cache_nlsc = load_nls_default();

	cifs_dbg(FYI, "%s: initialized DFS referral cache\n", __func__);
	return 0;

out_destroy_wq:
	destroy_workqueue(dfscache_wq);
	return rc;
}

static inline unsigned int cache_entry_hash(const void *data, int size)
{
	unsigned int h;

	h = jhash(data, size, 0);
	return h & (CACHE_HTABLE_SIZE - 1);
}

/* Check whether second path component of @path is SYSVOL or NETLOGON */
static inline bool is_sysvol_or_netlogon(const char *path)
{
	const char *s;
	char sep = path[0];

	s = strchr(path + 1, sep) + 1;
	return !strncasecmp(s, "sysvol", strlen("sysvol")) ||
		!strncasecmp(s, "netlogon", strlen("netlogon"));
}

/* Return target hint of a DFS cache entry */
static inline char *get_tgt_name(const struct cache_entry *ce)
{
	struct cache_dfs_tgt *t = ce->tgthint;

	return t ? t->name : ERR_PTR(-ENOENT);
}

/* Return expire time out of a new entry's TTL */
static inline struct timespec64 get_expire_time(int ttl)
{
	struct timespec64 ts = {
		.tv_sec = ttl,
		.tv_nsec = 0,
	};
	struct timespec64 now;

	ktime_get_coarse_real_ts64(&now);
	return timespec64_add(now, ts);
}

/* Allocate a new DFS target */
static inline struct cache_dfs_tgt *alloc_tgt(const char *name)
{
	struct cache_dfs_tgt *t;

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return ERR_PTR(-ENOMEM);
	t->name = kstrndup(name, strlen(name), GFP_KERNEL);
	if (!t->name) {
		kfree(t);
		return ERR_PTR(-ENOMEM);
	}
	INIT_LIST_HEAD(&t->list);
	return t;
}

/*
 * Copy DFS referral information to a cache entry and conditionally update
 * target hint.
 */
static int copy_ref_data(const struct dfs_info3_param *refs, int numrefs,
			 struct cache_entry *ce, const char *tgthint)
{
	int i;

	ce->ttl = refs[0].ttl;
	ce->etime = get_expire_time(ce->ttl);
	ce->srvtype = refs[0].server_type;
	ce->flags = refs[0].ref_flag;
	ce->path_consumed = refs[0].path_consumed;

	for (i = 0; i < numrefs; i++) {
		struct cache_dfs_tgt *t;

		t = alloc_tgt(refs[i].node_name);
		if (IS_ERR(t)) {
			free_tgts(ce);
			return PTR_ERR(t);
		}
		if (tgthint && !strcasecmp(t->name, tgthint)) {
			list_add(&t->list, &ce->tlist);
			tgthint = NULL;
		} else {
			list_add_tail(&t->list, &ce->tlist);
		}
		ce->numtgts++;
	}

	ce->tgthint = list_first_entry_or_null(&ce->tlist,
					       struct cache_dfs_tgt, list);

	return 0;
}

/* Allocate a new cache entry */
static struct cache_entry *alloc_cache_entry(const char *path,
					     const struct dfs_info3_param *refs,
					     int numrefs)
{
	struct cache_entry *ce;
	int rc;

	ce = kmem_cache_zalloc(cache_slab, GFP_KERNEL);
	if (!ce)
		return ERR_PTR(-ENOMEM);

	ce->path = kstrndup(path, strlen(path), GFP_KERNEL);
	if (!ce->path) {
		kmem_cache_free(cache_slab, ce);
		return ERR_PTR(-ENOMEM);
	}
	INIT_HLIST_NODE(&ce->hlist);
	INIT_LIST_HEAD(&ce->tlist);

	rc = copy_ref_data(refs, numrefs, ce, NULL);
	if (rc) {
		kfree(ce->path);
		kmem_cache_free(cache_slab, ce);
		ce = ERR_PTR(rc);
	}
	return ce;
}

static void remove_oldest_entry(void)
{
	int bucket;
	struct cache_entry *ce;
	struct cache_entry *to_del = NULL;

	rcu_read_lock();
	hash_for_each_rcu(cache_htable, bucket, ce, hlist) {
		if (!to_del || timespec64_compare(&ce->etime,
						  &to_del->etime) < 0)
			to_del = ce;
	}
	if (!to_del) {
		cifs_dbg(FYI, "%s: no entry to remove", __func__);
		goto out;
	}
	cifs_dbg(FYI, "%s: removing entry", __func__);
	dump_ce(to_del);
	flush_cache_ent(to_del);
out:
	rcu_read_unlock();
}

/* Add a new DFS cache entry */
static inline struct cache_entry *
add_cache_entry(unsigned int hash, const char *path,
		const struct dfs_info3_param *refs, int numrefs)
{
	struct cache_entry *ce;

	ce = alloc_cache_entry(path, refs, numrefs);
	if (IS_ERR(ce))
		return ce;

	hlist_add_head_rcu(&ce->hlist, &cache_htable[hash]);

	spin_lock(&cache_ttl_lock);
	if (!cache_ttl) {
		cache_ttl = ce->ttl;
		queue_delayed_work(dfscache_wq, &refresh_task, cache_ttl * HZ);
	} else {
		cache_ttl = min_t(int, cache_ttl, ce->ttl);
		mod_delayed_work(dfscache_wq, &refresh_task, cache_ttl * HZ);
	}
	spin_unlock(&cache_ttl_lock);

	return ce;
}

/*
 * Find a DFS cache entry in hash table and optionally check prefix path against
 * @path.
 * Use whole path components in the match.
 * Return ERR_PTR(-ENOENT) if the entry is not found.
 */
static struct cache_entry *lookup_cache_entry(const char *path,
					      unsigned int *hash)
{
	struct cache_entry *ce;
	unsigned int h;
	bool found = false;

	h = cache_entry_hash(path, strlen(path));

	rcu_read_lock();
	hlist_for_each_entry_rcu(ce, &cache_htable[h], hlist) {
		if (!strcasecmp(path, ce->path)) {
			found = true;
			dump_ce(ce);
			break;
		}
	}
	rcu_read_unlock();

	if (!found)
		ce = ERR_PTR(-ENOENT);
	if (hash)
		*hash = h;

	return ce;
}

static inline void destroy_slab_cache(void)
{
	rcu_barrier();
	kmem_cache_destroy(cache_slab);
}

static void __vol_release(struct vol_info *vi)
{
	kfree(vi->fullpath);
	kfree(vi->mntdata);
	cifs_cleanup_volume_info_contents(&vi->smb_vol);
	kfree(vi);
}

static void vol_release(struct kref *kref)
{
	struct vol_info *vi = container_of(kref, struct vol_info, refcnt);

	spin_lock(&vol_list_lock);
	list_del(&vi->list);
	spin_unlock(&vol_list_lock);
	__vol_release(vi);
}

static inline void free_vol_list(void)
{
	struct vol_info *vi, *nvi;

	list_for_each_entry_safe(vi, nvi, &vol_list, list) {
		list_del_init(&vi->list);
		__vol_release(vi);
	}
}

/**
 * dfs_cache_destroy - destroy DFS referral cache
 */
void dfs_cache_destroy(void)
{
	cancel_delayed_work_sync(&refresh_task);
	unload_nls(cache_nlsc);
	free_vol_list();
	flush_cache_ents();
	destroy_slab_cache();
	destroy_workqueue(dfscache_wq);

	cifs_dbg(FYI, "%s: destroyed DFS referral cache\n", __func__);
}

static inline struct cache_entry *
__update_cache_entry(const char *path, const struct dfs_info3_param *refs,
		     int numrefs)
{
	int rc;
	unsigned int h;
	struct cache_entry *ce;
	char *s, *th = NULL;

	ce = lookup_cache_entry(path, &h);
	if (IS_ERR(ce))
		return ce;

	if (ce->tgthint) {
		s = ce->tgthint->name;
		th = kstrndup(s, strlen(s), GFP_KERNEL);
		if (!th)
			return ERR_PTR(-ENOMEM);
	}

	free_tgts(ce);
	ce->numtgts = 0;

	rc = copy_ref_data(refs, numrefs, ce, th);
	kfree(th);

	if (rc)
		ce = ERR_PTR(rc);

	return ce;
}

/* Update an expired cache entry by getting a new DFS referral from server */
static struct cache_entry *
update_cache_entry(const unsigned int xid, struct cifs_ses *ses,
		   const struct nls_table *nls_codepage, int remap,
		   const char *path, struct cache_entry *ce)
{
	int rc;
	struct dfs_info3_param *refs = NULL;
	int numrefs = 0;

	cifs_dbg(FYI, "%s: update expired cache entry\n", __func__);
	/*
	 * Check if caller provided enough parameters to update an expired
	 * entry.
	 */
	if (!ses || !ses->server || !ses->server->ops->get_dfs_refer)
		return ERR_PTR(-ETIME);
	if (unlikely(!nls_codepage))
		return ERR_PTR(-ETIME);

	cifs_dbg(FYI, "%s: DFS referral request for %s\n", __func__, path);

	rc = ses->server->ops->get_dfs_refer(xid, ses, path, &refs, &numrefs,
					     nls_codepage, remap);
	if (rc)
		ce = ERR_PTR(rc);
	else
		ce = __update_cache_entry(path, refs, numrefs);

	dump_refs(refs, numrefs);
	free_dfs_info_array(refs, numrefs);

	return ce;
}

/*
 * Find, create or update a DFS cache entry.
 *
 * If the entry wasn't found, it will create a new one. Or if it was found but
 * expired, then it will update the entry accordingly.
 *
 * For interlinks, __cifs_dfs_mount() and expand_dfs_referral() are supposed to
 * handle them properly.
 */
static struct cache_entry *
do_dfs_cache_find(const unsigned int xid, struct cifs_ses *ses,
		  const struct nls_table *nls_codepage, int remap,
		  const char *path, bool noreq)
{
	int rc;
	unsigned int h;
	struct cache_entry *ce;
	struct dfs_info3_param *nrefs;
	int numnrefs;

	cifs_dbg(FYI, "%s: search path: %s\n", __func__, path);

	ce = lookup_cache_entry(path, &h);
	if (IS_ERR(ce)) {
		cifs_dbg(FYI, "%s: cache miss\n", __func__);
		/*
		 * If @noreq is set, no requests will be sent to the server for
		 * either updating or getting a new DFS referral.
		 */
		if (noreq)
			return ce;
		/*
		 * No cache entry was found, so check for valid parameters that
		 * will be required to get a new DFS referral and then create a
		 * new cache entry.
		 */
		if (!ses || !ses->server || !ses->server->ops->get_dfs_refer) {
			ce = ERR_PTR(-EOPNOTSUPP);
			return ce;
		}
		if (unlikely(!nls_codepage)) {
			ce = ERR_PTR(-EINVAL);
			return ce;
		}

		nrefs = NULL;
		numnrefs = 0;

		cifs_dbg(FYI, "%s: DFS referral request for %s\n", __func__,
			 path);

		rc = ses->server->ops->get_dfs_refer(xid, ses, path, &nrefs,
						     &numnrefs, nls_codepage,
						     remap);
		if (rc) {
			ce = ERR_PTR(rc);
			return ce;
		}

		dump_refs(nrefs, numnrefs);

		cifs_dbg(FYI, "%s: new cache entry\n", __func__);

		if (cache_count >= CACHE_MAX_ENTRIES) {
			cifs_dbg(FYI, "%s: reached max cache size (%d)",
				 __func__, CACHE_MAX_ENTRIES);
			remove_oldest_entry();
		}
		ce = add_cache_entry(h, path, nrefs, numnrefs);
		free_dfs_info_array(nrefs, numnrefs);

		if (IS_ERR(ce))
			return ce;

		cache_count++;
	}

	dump_ce(ce);

	/* Just return the found cache entry in case @noreq is set */
	if (noreq)
		return ce;

	if (cache_entry_expired(ce)) {
		cifs_dbg(FYI, "%s: expired cache entry\n", __func__);
		ce = update_cache_entry(xid, ses, nls_codepage, remap, path,
					ce);
		if (IS_ERR(ce)) {
			cifs_dbg(FYI, "%s: failed to update expired entry\n",
				 __func__);
		}
	}
	return ce;
}

/* Set up a new DFS referral from a given cache entry */
static int setup_ref(const char *path, const struct cache_entry *ce,
		     struct dfs_info3_param *ref, const char *tgt)
{
	int rc;

	cifs_dbg(FYI, "%s: set up new ref\n", __func__);

	memset(ref, 0, sizeof(*ref));

	ref->path_name = kstrndup(path, strlen(path), GFP_KERNEL);
	if (!ref->path_name)
		return -ENOMEM;

	ref->path_consumed = ce->path_consumed;

	ref->node_name = kstrndup(tgt, strlen(tgt), GFP_KERNEL);
	if (!ref->node_name) {
		rc = -ENOMEM;
		goto err_free_path;
	}

	ref->ttl = ce->ttl;
	ref->server_type = ce->srvtype;
	ref->ref_flag = ce->flags;

	return 0;

err_free_path:
	kfree(ref->path_name);
	ref->path_name = NULL;
	return rc;
}

/* Return target list of a DFS cache entry */
static int get_tgt_list(const struct cache_entry *ce,
			struct dfs_cache_tgt_list *tl)
{
	int rc;
	struct list_head *head = &tl->tl_list;
	struct cache_dfs_tgt *t;
	struct dfs_cache_tgt_iterator *it, *nit;

	memset(tl, 0, sizeof(*tl));
	INIT_LIST_HEAD(head);

	list_for_each_entry(t, &ce->tlist, list) {
		it = kzalloc(sizeof(*it), GFP_KERNEL);
		if (!it) {
			rc = -ENOMEM;
			goto err_free_it;
		}

		it->it_name = kstrndup(t->name, strlen(t->name),
				       GFP_KERNEL);
		if (!it->it_name) {
			kfree(it);
			rc = -ENOMEM;
			goto err_free_it;
		}

		if (ce->tgthint == t)
			list_add(&it->it_list, head);
		else
			list_add_tail(&it->it_list, head);
	}
	tl->tl_numtgts = ce->numtgts;

	return 0;

err_free_it:
	list_for_each_entry_safe(it, nit, head, it_list) {
		kfree(it->it_name);
		kfree(it);
	}
	return rc;
}

/**
 * dfs_cache_find - find a DFS cache entry
 *
 * If it doesn't find the cache entry, then it will get a DFS referral
 * for @path and create a new entry.
 *
 * In case the cache entry exists but expired, it will get a DFS referral
 * for @path and then update the respective cache entry.
 *
 * These parameters are passed down to the get_dfs_refer() call if it
 * needs to be issued:
 * @xid: syscall xid
 * @ses: smb session to issue the request on
 * @nls_codepage: charset conversion
 * @remap: path character remapping type
 * @path: path to lookup in DFS referral cache.
 *
 * @ref: when non-NULL, store single DFS referral result in it.
 * @tgt_list: when non-NULL, store complete DFS target list in it.
 *
 * Return zero if the target was found, otherwise non-zero.
 */
int dfs_cache_find(const unsigned int xid, struct cifs_ses *ses,
		   const struct nls_table *nls_codepage, int remap,
		   const char *path, struct dfs_info3_param *ref,
		   struct dfs_cache_tgt_list *tgt_list)
{
	int rc;
	char *npath;
	struct cache_entry *ce;

	rc = get_normalized_path(path, &npath);
	if (rc)
		return rc;

	mutex_lock(&list_lock);
	ce = do_dfs_cache_find(xid, ses, nls_codepage, remap, npath, false);
	if (!IS_ERR(ce)) {
		if (ref)
			rc = setup_ref(path, ce, ref, get_tgt_name(ce));
		else
			rc = 0;
		if (!rc && tgt_list)
			rc = get_tgt_list(ce, tgt_list);
	} else {
		rc = PTR_ERR(ce);
	}
	mutex_unlock(&list_lock);
	free_normalized_path(path, npath);
	return rc;
}

/**
 * dfs_cache_noreq_find - find a DFS cache entry without sending any requests to
 * the currently connected server.
 *
 * NOTE: This function will neither update a cache entry in case it was
 * expired, nor create a new cache entry if @path hasn't been found. It heavily
 * relies on an existing cache entry.
 *
 * @path: path to lookup in the DFS referral cache.
 * @ref: when non-NULL, store single DFS referral result in it.
 * @tgt_list: when non-NULL, store complete DFS target list in it.
 *
 * Return 0 if successful.
 * Return -ENOENT if the entry was not found.
 * Return non-zero for other errors.
 */
int dfs_cache_noreq_find(const char *path, struct dfs_info3_param *ref,
			 struct dfs_cache_tgt_list *tgt_list)
{
	int rc;
	char *npath;
	struct cache_entry *ce;

	rc = get_normalized_path(path, &npath);
	if (rc)
		return rc;

	mutex_lock(&list_lock);
	ce = do_dfs_cache_find(0, NULL, NULL, 0, npath, true);
	if (IS_ERR(ce)) {
		rc = PTR_ERR(ce);
		goto out;
	}

	if (ref)
		rc = setup_ref(path, ce, ref, get_tgt_name(ce));
	else
		rc = 0;
	if (!rc && tgt_list)
		rc = get_tgt_list(ce, tgt_list);
out:
	mutex_unlock(&list_lock);
	free_normalized_path(path, npath);
	return rc;
}

/**
 * dfs_cache_update_tgthint - update target hint of a DFS cache entry
 *
 * If it doesn't find the cache entry, then it will get a DFS referral for @path
 * and create a new entry.
 *
 * In case the cache entry exists but expired, it will get a DFS referral
 * for @path and then update the respective cache entry.
 *
 * @xid: syscall id
 * @ses: smb session
 * @nls_codepage: charset conversion
 * @remap: type of character remapping for paths
 * @path: path to lookup in DFS referral cache.
 * @it: DFS target iterator
 *
 * Return zero if the target hint was updated successfully, otherwise non-zero.
 */
int dfs_cache_update_tgthint(const unsigned int xid, struct cifs_ses *ses,
			     const struct nls_table *nls_codepage, int remap,
			     const char *path,
			     const struct dfs_cache_tgt_iterator *it)
{
	int rc;
	char *npath;
	struct cache_entry *ce;
	struct cache_dfs_tgt *t;

	rc = get_normalized_path(path, &npath);
	if (rc)
		return rc;

	cifs_dbg(FYI, "%s: path: %s\n", __func__, npath);

	mutex_lock(&list_lock);
	ce = do_dfs_cache_find(xid, ses, nls_codepage, remap, npath, false);
	if (IS_ERR(ce)) {
		rc = PTR_ERR(ce);
		goto out;
	}

	rc = 0;

	t = ce->tgthint;

	if (likely(!strcasecmp(it->it_name, t->name)))
		goto out;

	list_for_each_entry(t, &ce->tlist, list) {
		if (!strcasecmp(t->name, it->it_name)) {
			ce->tgthint = t;
			cifs_dbg(FYI, "%s: new target hint: %s\n", __func__,
				 it->it_name);
			break;
		}
	}

out:
	mutex_unlock(&list_lock);
	free_normalized_path(path, npath);
	return rc;
}

/**
 * dfs_cache_noreq_update_tgthint - update target hint of a DFS cache entry
 * without sending any requests to the currently connected server.
 *
 * NOTE: This function will neither update a cache entry in case it was
 * expired, nor create a new cache entry if @path hasn't been found. It heavily
 * relies on an existing cache entry.
 *
 * @path: path to lookup in DFS referral cache.
 * @it: target iterator which contains the target hint to update the cache
 * entry with.
 *
 * Return zero if the target hint was updated successfully, otherwise non-zero.
 */
int dfs_cache_noreq_update_tgthint(const char *path,
				   const struct dfs_cache_tgt_iterator *it)
{
	int rc;
	char *npath;
	struct cache_entry *ce;
	struct cache_dfs_tgt *t;

	if (!it)
		return -EINVAL;

	rc = get_normalized_path(path, &npath);
	if (rc)
		return rc;

	cifs_dbg(FYI, "%s: path: %s\n", __func__, npath);

	mutex_lock(&list_lock);

	ce = do_dfs_cache_find(0, NULL, NULL, 0, npath, true);
	if (IS_ERR(ce)) {
		rc = PTR_ERR(ce);
		goto out;
	}

	rc = 0;

	t = ce->tgthint;

	if (unlikely(!strcasecmp(it->it_name, t->name)))
		goto out;

	list_for_each_entry(t, &ce->tlist, list) {
		if (!strcasecmp(t->name, it->it_name)) {
			ce->tgthint = t;
			cifs_dbg(FYI, "%s: new target hint: %s\n", __func__,
				 it->it_name);
			break;
		}
	}

out:
	mutex_unlock(&list_lock);
	free_normalized_path(path, npath);
	return rc;
}

/**
 * dfs_cache_get_tgt_referral - returns a DFS referral (@ref) from a given
 * target iterator (@it).
 *
 * @path: path to lookup in DFS referral cache.
 * @it: DFS target iterator.
 * @ref: DFS referral pointer to set up the gathered information.
 *
 * Return zero if the DFS referral was set up correctly, otherwise non-zero.
 */
int dfs_cache_get_tgt_referral(const char *path,
			       const struct dfs_cache_tgt_iterator *it,
			       struct dfs_info3_param *ref)
{
	int rc;
	char *npath;
	struct cache_entry *ce;
	unsigned int h;

	if (!it || !ref)
		return -EINVAL;

	rc = get_normalized_path(path, &npath);
	if (rc)
		return rc;

	cifs_dbg(FYI, "%s: path: %s\n", __func__, npath);

	mutex_lock(&list_lock);

	ce = lookup_cache_entry(npath, &h);
	if (IS_ERR(ce)) {
		rc = PTR_ERR(ce);
		goto out;
	}

	cifs_dbg(FYI, "%s: target name: %s\n", __func__, it->it_name);

	rc = setup_ref(path, ce, ref, it->it_name);

out:
	mutex_unlock(&list_lock);
	free_normalized_path(path, npath);
	return rc;
}

static int dup_vol(struct smb_vol *vol, struct smb_vol *new)
{
	memcpy(new, vol, sizeof(*new));

	if (vol->username) {
		new->username = kstrndup(vol->username, strlen(vol->username),
					 GFP_KERNEL);
		if (!new->username)
			return -ENOMEM;
	}
	if (vol->password) {
		new->password = kstrndup(vol->password, strlen(vol->password),
					 GFP_KERNEL);
		if (!new->password)
			goto err_free_username;
	}
	if (vol->UNC) {
		cifs_dbg(FYI, "%s: vol->UNC: %s\n", __func__, vol->UNC);
		new->UNC = kstrndup(vol->UNC, strlen(vol->UNC), GFP_KERNEL);
		if (!new->UNC)
			goto err_free_password;
	}
	if (vol->domainname) {
		new->domainname = kstrndup(vol->domainname,
					   strlen(vol->domainname), GFP_KERNEL);
		if (!new->domainname)
			goto err_free_unc;
	}
	if (vol->iocharset) {
		new->iocharset = kstrndup(vol->iocharset,
					  strlen(vol->iocharset), GFP_KERNEL);
		if (!new->iocharset)
			goto err_free_domainname;
	}
	if (vol->prepath) {
		cifs_dbg(FYI, "%s: vol->prepath: %s\n", __func__, vol->prepath);
		new->prepath = kstrndup(vol->prepath, strlen(vol->prepath),
					GFP_KERNEL);
		if (!new->prepath)
			goto err_free_iocharset;
	}

	return 0;

err_free_iocharset:
	kfree(new->iocharset);
err_free_domainname:
	kfree(new->domainname);
err_free_unc:
	kfree(new->UNC);
err_free_password:
	kzfree(new->password);
err_free_username:
	kfree(new->username);
	kfree(new);
	return -ENOMEM;
}

/**
 * dfs_cache_add_vol - add a cifs volume during mount() that will be handled by
 * DFS cache refresh worker.
 *
 * @mntdata: mount data.
 * @vol: cifs volume.
 * @fullpath: origin full path.
 *
 * Return zero if volume was set up correctly, otherwise non-zero.
 */
int dfs_cache_add_vol(char *mntdata, struct smb_vol *vol, const char *fullpath)
{
	int rc;
	struct vol_info *vi;

	if (!vol || !fullpath || !mntdata)
		return -EINVAL;

	cifs_dbg(FYI, "%s: fullpath: %s\n", __func__, fullpath);

	vi = kzalloc(sizeof(*vi), GFP_KERNEL);
	if (!vi)
		return -ENOMEM;

	vi->fullpath = kstrndup(fullpath, strlen(fullpath), GFP_KERNEL);
	if (!vi->fullpath) {
		rc = -ENOMEM;
		goto err_free_vi;
	}

	rc = dup_vol(vol, &vi->smb_vol);
	if (rc)
		goto err_free_fullpath;

	vi->mntdata = mntdata;
	spin_lock_init(&vi->smb_vol_lock);
	kref_init(&vi->refcnt);

	spin_lock(&vol_list_lock);
	list_add_tail(&vi->list, &vol_list);
	spin_unlock(&vol_list_lock);

	return 0;

err_free_fullpath:
	kfree(vi->fullpath);
err_free_vi:
	kfree(vi);
	return rc;
}

/* Must be called with vol_list_lock held */
static struct vol_info *find_vol(const char *fullpath)
{
	struct vol_info *vi;

	list_for_each_entry(vi, &vol_list, list) {
		cifs_dbg(FYI, "%s: vi->fullpath: %s\n", __func__, vi->fullpath);
		if (!strcasecmp(vi->fullpath, fullpath))
			return vi;
	}
	return ERR_PTR(-ENOENT);
}

/**
 * dfs_cache_update_vol - update vol info in DFS cache after failover
 *
 * @fullpath: fullpath to look up in volume list.
 * @server: TCP ses pointer.
 *
 * Return zero if volume was updated, otherwise non-zero.
 */
int dfs_cache_update_vol(const char *fullpath, struct TCP_Server_Info *server)
{
	struct vol_info *vi;

	if (!fullpath || !server)
		return -EINVAL;

	cifs_dbg(FYI, "%s: fullpath: %s\n", __func__, fullpath);

	spin_lock(&vol_list_lock);
	vi = find_vol(fullpath);
	if (IS_ERR(vi)) {
		spin_unlock(&vol_list_lock);
		return PTR_ERR(vi);
	}
	kref_get(&vi->refcnt);
	spin_unlock(&vol_list_lock);

	cifs_dbg(FYI, "%s: updating volume info\n", __func__);
	spin_lock(&vi->smb_vol_lock);
	memcpy(&vi->smb_vol.dstaddr, &server->dstaddr,
	       sizeof(vi->smb_vol.dstaddr));
	spin_unlock(&vi->smb_vol_lock);

	kref_put(&vi->refcnt, vol_release);

	return 0;
}

/**
 * dfs_cache_del_vol - remove volume info in DFS cache during umount()
 *
 * @fullpath: fullpath to look up in volume list.
 */
void dfs_cache_del_vol(const char *fullpath)
{
	struct vol_info *vi;

	if (!fullpath || !*fullpath)
		return;

	cifs_dbg(FYI, "%s: fullpath: %s\n", __func__, fullpath);

	spin_lock(&vol_list_lock);
	vi = find_vol(fullpath);
	spin_unlock(&vol_list_lock);

	kref_put(&vi->refcnt, vol_release);
}

/* Get all tcons that are within a DFS namespace and can be refreshed */
static void get_tcons(struct TCP_Server_Info *server, struct list_head *head)
{
	struct cifs_ses *ses;
	struct cifs_tcon *tcon;

	INIT_LIST_HEAD(head);

	spin_lock(&cifs_tcp_ses_lock);
	list_for_each_entry(ses, &server->smb_ses_list, smb_ses_list) {
		list_for_each_entry(tcon, &ses->tcon_list, tcon_list) {
			if (!tcon->need_reconnect && !tcon->need_reopen_files &&
			    tcon->dfs_path) {
				tcon->tc_count++;
				list_add_tail(&tcon->ulist, head);
			}
		}
		if (ses->tcon_ipc && !ses->tcon_ipc->need_reconnect &&
		    ses->tcon_ipc->dfs_path) {
			list_add_tail(&ses->tcon_ipc->ulist, head);
		}
	}
	spin_unlock(&cifs_tcp_ses_lock);
}

static bool is_dfs_link(const char *path)
{
	char *s;

	s = strchr(path + 1, '\\');
	if (!s)
		return false;
	return !!strchr(s + 1, '\\');
}

static char *get_dfs_root(const char *path)
{
	char *s, *npath;

	s = strchr(path + 1, '\\');
	if (!s)
		return ERR_PTR(-EINVAL);

	s = strchr(s + 1, '\\');
	if (!s)
		return ERR_PTR(-EINVAL);

	npath = kstrndup(path, s - path, GFP_KERNEL);
	if (!npath)
		return ERR_PTR(-ENOMEM);

	return npath;
}

static inline void put_tcp_server(struct TCP_Server_Info *server)
{
	cifs_put_tcp_session(server, 0);
}

static struct TCP_Server_Info *get_tcp_server(struct smb_vol *vol)
{
	struct TCP_Server_Info *server;

	server = cifs_find_tcp_session(vol);
	if (IS_ERR_OR_NULL(server))
		return NULL;

	spin_lock(&GlobalMid_Lock);
	if (server->tcpStatus != CifsGood) {
		spin_unlock(&GlobalMid_Lock);
		put_tcp_server(server);
		return NULL;
	}
	spin_unlock(&GlobalMid_Lock);

	return server;
}

/* Find root SMB session out of a DFS link path */
static struct cifs_ses *find_root_ses(struct vol_info *vi,
				      struct cifs_tcon *tcon,
				      const char *path)
{
	char *rpath;
	int rc;
	struct dfs_info3_param ref = {0};
	char *mdata = NULL, *devname = NULL;
	struct TCP_Server_Info *server;
	struct cifs_ses *ses;
	struct smb_vol vol = {NULL};

	rpath = get_dfs_root(path);
	if (IS_ERR(rpath))
		return ERR_CAST(rpath);

	memset(&vol, 0, sizeof(vol));

	rc = dfs_cache_noreq_find(rpath, &ref, NULL);
	if (rc) {
		ses = ERR_PTR(rc);
		goto out;
	}

	mdata = cifs_compose_mount_options(vi->mntdata, rpath, &ref, &devname);
	free_dfs_info_param(&ref);

	if (IS_ERR(mdata)) {
		ses = ERR_CAST(mdata);
		mdata = NULL;
		goto out;
	}

	rc = cifs_setup_volume_info(&vol, mdata, devname, false);
	kfree(devname);

	if (rc) {
		ses = ERR_PTR(rc);
		goto out;
	}

	server = get_tcp_server(&vol);
	if (!server) {
		ses = ERR_PTR(-EHOSTDOWN);
		goto out;
	}

	ses = cifs_get_smb_ses(server, &vol);

out:
	cifs_cleanup_volume_info_contents(&vol);
	kfree(mdata);
	kfree(rpath);

	return ses;
}

/* Refresh DFS cache entry from a given tcon */
static void refresh_tcon(struct vol_info *vi, struct cifs_tcon *tcon)
{
	int rc = 0;
	unsigned int xid;
	char *path, *npath;
	unsigned int h;
	struct cache_entry *ce;
	struct dfs_info3_param *refs = NULL;
	int numrefs = 0;
	struct cifs_ses *root_ses = NULL, *ses;

	xid = get_xid();

	path = tcon->dfs_path + 1;

	rc = get_normalized_path(path, &npath);
	if (rc)
		goto out;

	mutex_lock(&list_lock);
	ce = lookup_cache_entry(npath, &h);
	mutex_unlock(&list_lock);

	if (IS_ERR(ce)) {
		rc = PTR_ERR(ce);
		goto out;
	}

	if (!cache_entry_expired(ce))
		goto out;

	/* If it's a DFS Link, then use root SMB session for refreshing it */
	if (is_dfs_link(npath)) {
		ses = root_ses = find_root_ses(vi, tcon, npath);
		if (IS_ERR(ses)) {
			rc = PTR_ERR(ses);
			root_ses = NULL;
			goto out;
		}
	} else {
		ses = tcon->ses;
	}

	if (unlikely(!ses->server->ops->get_dfs_refer)) {
		rc = -EOPNOTSUPP;
	} else {
		rc = ses->server->ops->get_dfs_refer(xid, ses, path, &refs,
						     &numrefs, cache_nlsc,
						     tcon->remap);
		if (!rc) {
			mutex_lock(&list_lock);
			ce = __update_cache_entry(npath, refs, numrefs);
			mutex_unlock(&list_lock);
			dump_refs(refs, numrefs);
			free_dfs_info_array(refs, numrefs);
			if (IS_ERR(ce))
				rc = PTR_ERR(ce);
		}
	}

out:
	if (root_ses)
		cifs_put_smb_ses(root_ses);

	free_xid(xid);
	free_normalized_path(path, npath);
}

/*
 * Worker that will refresh DFS cache based on lowest TTL value from a DFS
 * referral.
 */
static void refresh_cache_worker(struct work_struct *work)
{
	struct vol_info *vi, *nvi;
	struct TCP_Server_Info *server;
	LIST_HEAD(vols);
	LIST_HEAD(tcons);
	struct cifs_tcon *tcon, *ntcon;

	/*
	 * Find SMB volumes that are eligible (server->tcpStatus == CifsGood)
	 * for refreshing.
	 */
	spin_lock(&vol_list_lock);
	list_for_each_entry(vi, &vol_list, list) {
		server = get_tcp_server(&vi->smb_vol);
		if (!server)
			continue;

		kref_get(&vi->refcnt);
		list_add_tail(&vi->rlist, &vols);
		put_tcp_server(server);
	}
	spin_unlock(&vol_list_lock);

	/* Walk through all TCONs and refresh any expired cache entry */
	list_for_each_entry_safe(vi, nvi, &vols, rlist) {
		spin_lock(&vi->smb_vol_lock);
		server = get_tcp_server(&vi->smb_vol);
		spin_unlock(&vi->smb_vol_lock);

		if (!server)
			goto next_vol;

		get_tcons(server, &tcons);
		list_for_each_entry_safe(tcon, ntcon, &tcons, ulist) {
			refresh_tcon(vi, tcon);
			list_del_init(&tcon->ulist);
			cifs_put_tcon(tcon);
		}

		put_tcp_server(server);

next_vol:
		list_del_init(&vi->rlist);
		kref_put(&vi->refcnt, vol_release);
	}

	spin_lock(&cache_ttl_lock);
	queue_delayed_work(dfscache_wq, &refresh_task, cache_ttl * HZ);
	spin_unlock(&cache_ttl_lock);
}
