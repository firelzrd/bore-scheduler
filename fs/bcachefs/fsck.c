// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"
#include "bkey_buf.h"
#include "btree_cache.h"
#include "btree_update.h"
#include "buckets.h"
#include "darray.h"
#include "dirent.h"
#include "error.h"
#include "fs-common.h"
#include "fsck.h"
#include "inode.h"
#include "keylist.h"
#include "recovery.h"
#include "snapshot.h"
#include "super.h"
#include "xattr.h"

#include <linux/bsearch.h>
#include <linux/dcache.h> /* struct qstr */

#define QSTR(n) { { { .len = strlen(n) } }, .name = n }

/*
 * XXX: this is handling transaction restarts without returning
 * -BCH_ERR_transaction_restart_nested, this is not how we do things anymore:
 */
static s64 bch2_count_inode_sectors(struct btree_trans *trans, u64 inum,
				    u32 snapshot)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	u64 sectors = 0;
	int ret;

	for_each_btree_key_upto(trans, iter, BTREE_ID_extents,
				SPOS(inum, 0, snapshot),
				POS(inum, U64_MAX),
				0, k, ret)
		if (bkey_extent_is_allocation(k.k))
			sectors += k.k->size;

	bch2_trans_iter_exit(trans, &iter);

	return ret ?: sectors;
}

static s64 bch2_count_subdirs(struct btree_trans *trans, u64 inum,
				    u32 snapshot)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bkey_s_c_dirent d;
	u64 subdirs = 0;
	int ret;

	for_each_btree_key_upto(trans, iter, BTREE_ID_dirents,
				SPOS(inum, 0, snapshot),
				POS(inum, U64_MAX),
				0, k, ret) {
		if (k.k->type != KEY_TYPE_dirent)
			continue;

		d = bkey_s_c_to_dirent(k);
		if (d.v->d_type == DT_DIR)
			subdirs++;
	}
	bch2_trans_iter_exit(trans, &iter);

	return ret ?: subdirs;
}

static int __snapshot_lookup_subvol(struct btree_trans *trans, u32 snapshot,
				    u32 *subvol)
{
	struct bch_snapshot s;
	int ret = bch2_bkey_get_val_typed(trans, BTREE_ID_snapshots,
					  POS(0, snapshot), 0,
					  snapshot, &s);
	if (!ret)
		*subvol = le32_to_cpu(s.subvol);
	else if (bch2_err_matches(ret, ENOENT))
		bch_err(trans->c, "snapshot %u not found", snapshot);
	return ret;

}

static int __subvol_lookup(struct btree_trans *trans, u32 subvol,
			   u32 *snapshot, u64 *inum)
{
	struct bch_subvolume s;
	int ret;

	ret = bch2_subvolume_get(trans, subvol, false, 0, &s);

	*snapshot = le32_to_cpu(s.snapshot);
	*inum = le64_to_cpu(s.inode);
	return ret;
}

static int subvol_lookup(struct btree_trans *trans, u32 subvol,
			 u32 *snapshot, u64 *inum)
{
	return lockrestart_do(trans, __subvol_lookup(trans, subvol, snapshot, inum));
}

static int lookup_first_inode(struct btree_trans *trans, u64 inode_nr,
			      struct bch_inode_unpacked *inode)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	bch2_trans_iter_init(trans, &iter, BTREE_ID_inodes,
			     POS(0, inode_nr),
			     BTREE_ITER_ALL_SNAPSHOTS);
	k = bch2_btree_iter_peek(&iter);
	ret = bkey_err(k);
	if (ret)
		goto err;

	if (!k.k || !bkey_eq(k.k->p, POS(0, inode_nr))) {
		ret = -BCH_ERR_ENOENT_inode;
		goto err;
	}

	ret = bch2_inode_unpack(k, inode);
err:
	bch_err_msg(trans->c, ret, "fetching inode %llu", inode_nr);
	bch2_trans_iter_exit(trans, &iter);
	return ret;
}

static int __lookup_inode(struct btree_trans *trans, u64 inode_nr,
			  struct bch_inode_unpacked *inode,
			  u32 *snapshot)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	k = bch2_bkey_get_iter(trans, &iter, BTREE_ID_inodes,
			       SPOS(0, inode_nr, *snapshot), 0);
	ret = bkey_err(k);
	if (ret)
		goto err;

	ret = bkey_is_inode(k.k)
		? bch2_inode_unpack(k, inode)
		: -BCH_ERR_ENOENT_inode;
	if (!ret)
		*snapshot = iter.pos.snapshot;
err:
	bch_err_msg(trans->c, ret, "fetching inode %llu:%u", inode_nr, *snapshot);
	bch2_trans_iter_exit(trans, &iter);
	return ret;
}

static int lookup_inode(struct btree_trans *trans, u64 inode_nr,
			struct bch_inode_unpacked *inode,
			u32 *snapshot)
{
	return lockrestart_do(trans, __lookup_inode(trans, inode_nr, inode, snapshot));
}

static int __lookup_dirent(struct btree_trans *trans,
			   struct bch_hash_info hash_info,
			   subvol_inum dir, struct qstr *name,
			   u64 *target, unsigned *type)
{
	struct btree_iter iter;
	struct bkey_s_c_dirent d;
	int ret;

	ret = bch2_hash_lookup(trans, &iter, bch2_dirent_hash_desc,
			       &hash_info, dir, name, 0);
	if (ret)
		return ret;

	d = bkey_s_c_to_dirent(bch2_btree_iter_peek_slot(&iter));
	*target = le64_to_cpu(d.v->d_inum);
	*type = d.v->d_type;
	bch2_trans_iter_exit(trans, &iter);
	return 0;
}

static int __write_inode(struct btree_trans *trans,
			 struct bch_inode_unpacked *inode,
			 u32 snapshot)
{
	struct bkey_inode_buf *inode_p =
		bch2_trans_kmalloc(trans, sizeof(*inode_p));

	if (IS_ERR(inode_p))
		return PTR_ERR(inode_p);

	bch2_inode_pack(inode_p, inode);
	inode_p->inode.k.p.snapshot = snapshot;

	return bch2_btree_insert_nonextent(trans, BTREE_ID_inodes,
				&inode_p->inode.k_i,
				BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE);
}

static int fsck_write_inode(struct btree_trans *trans,
			    struct bch_inode_unpacked *inode,
			    u32 snapshot)
{
	int ret = commit_do(trans, NULL, NULL,
				  BTREE_INSERT_NOFAIL|
				  BTREE_INSERT_LAZY_RW,
				  __write_inode(trans, inode, snapshot));
	if (ret)
		bch_err_fn(trans->c, ret);
	return ret;
}

static int __remove_dirent(struct btree_trans *trans, struct bpos pos)
{
	struct bch_fs *c = trans->c;
	struct btree_iter iter;
	struct bch_inode_unpacked dir_inode;
	struct bch_hash_info dir_hash_info;
	int ret;

	ret = lookup_first_inode(trans, pos.inode, &dir_inode);
	if (ret)
		goto err;

	dir_hash_info = bch2_hash_info_init(c, &dir_inode);

	bch2_trans_iter_init(trans, &iter, BTREE_ID_dirents, pos, BTREE_ITER_INTENT);

	ret = bch2_hash_delete_at(trans, bch2_dirent_hash_desc,
				  &dir_hash_info, &iter,
				  BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE);
	bch2_trans_iter_exit(trans, &iter);
err:
	bch_err_fn(c, ret);
	return ret;
}

/* Get lost+found, create if it doesn't exist: */
static int lookup_lostfound(struct btree_trans *trans, u32 subvol,
			    struct bch_inode_unpacked *lostfound)
{
	struct bch_fs *c = trans->c;
	struct bch_inode_unpacked root;
	struct bch_hash_info root_hash_info;
	struct qstr lostfound_str = QSTR("lost+found");
	subvol_inum root_inum = { .subvol = subvol };
	u64 inum = 0;
	unsigned d_type = 0;
	u32 snapshot;
	int ret;

	ret = __subvol_lookup(trans, subvol, &snapshot, &root_inum.inum);
	if (ret)
		return ret;

	ret = __lookup_inode(trans, root_inum.inum, &root, &snapshot);
	if (ret)
		return ret;

	root_hash_info = bch2_hash_info_init(c, &root);

	ret = __lookup_dirent(trans, root_hash_info, root_inum,
			    &lostfound_str, &inum, &d_type);
	if (bch2_err_matches(ret, ENOENT)) {
		bch_notice(c, "creating lost+found");
		goto create_lostfound;
	}

	bch_err_fn(c, ret);
	if (ret)
		return ret;

	if (d_type != DT_DIR) {
		bch_err(c, "error looking up lost+found: not a directory");
		return -BCH_ERR_ENOENT_not_directory;
	}

	/*
	 * The bch2_check_dirents pass has already run, dangling dirents
	 * shouldn't exist here:
	 */
	return __lookup_inode(trans, inum, lostfound, &snapshot);

create_lostfound:
	bch2_inode_init_early(c, lostfound);

	ret = bch2_create_trans(trans, root_inum, &root,
				lostfound, &lostfound_str,
				0, 0, S_IFDIR|0700, 0, NULL, NULL,
				(subvol_inum) { }, 0);
	bch_err_msg(c, ret, "creating lost+found");
	return ret;
}

static int __reattach_inode(struct btree_trans *trans,
			  struct bch_inode_unpacked *inode,
			  u32 inode_snapshot)
{
	struct bch_hash_info dir_hash;
	struct bch_inode_unpacked lostfound;
	char name_buf[20];
	struct qstr name;
	u64 dir_offset = 0;
	u32 subvol;
	int ret;

	ret = __snapshot_lookup_subvol(trans, inode_snapshot, &subvol);
	if (ret)
		return ret;

	ret = lookup_lostfound(trans, subvol, &lostfound);
	if (ret)
		return ret;

	if (S_ISDIR(inode->bi_mode)) {
		lostfound.bi_nlink++;

		ret = __write_inode(trans, &lostfound, U32_MAX);
		if (ret)
			return ret;
	}

	dir_hash = bch2_hash_info_init(trans->c, &lostfound);

	snprintf(name_buf, sizeof(name_buf), "%llu", inode->bi_inum);
	name = (struct qstr) QSTR(name_buf);

	ret = bch2_dirent_create(trans,
				 (subvol_inum) {
					.subvol = subvol,
					.inum = lostfound.bi_inum,
				 },
				 &dir_hash,
				 inode_d_type(inode),
				 &name, inode->bi_inum, &dir_offset,
				 BCH_HASH_SET_MUST_CREATE);
	if (ret)
		return ret;

	inode->bi_dir		= lostfound.bi_inum;
	inode->bi_dir_offset	= dir_offset;

	return __write_inode(trans, inode, inode_snapshot);
}

static int reattach_inode(struct btree_trans *trans,
			  struct bch_inode_unpacked *inode,
			  u32 inode_snapshot)
{
	int ret = commit_do(trans, NULL, NULL,
				  BTREE_INSERT_LAZY_RW|
				  BTREE_INSERT_NOFAIL,
			__reattach_inode(trans, inode, inode_snapshot));
	bch_err_msg(trans->c, ret, "reattaching inode %llu", inode->bi_inum);
	return ret;
}

static int remove_backpointer(struct btree_trans *trans,
			      struct bch_inode_unpacked *inode)
{
	struct btree_iter iter;
	struct bkey_s_c_dirent d;
	int ret;

	d = bch2_bkey_get_iter_typed(trans, &iter, BTREE_ID_dirents,
				     POS(inode->bi_dir, inode->bi_dir_offset), 0,
				     dirent);
	ret =   bkey_err(d) ?:
		__remove_dirent(trans, d.k->p);
	bch2_trans_iter_exit(trans, &iter);
	return ret;
}

struct snapshots_seen_entry {
	u32				id;
	u32				equiv;
};

struct snapshots_seen {
	struct bpos			pos;
	DARRAY(struct snapshots_seen_entry) ids;
};

static inline void snapshots_seen_exit(struct snapshots_seen *s)
{
	darray_exit(&s->ids);
}

static inline void snapshots_seen_init(struct snapshots_seen *s)
{
	memset(s, 0, sizeof(*s));
}

static int snapshots_seen_add_inorder(struct bch_fs *c, struct snapshots_seen *s, u32 id)
{
	struct snapshots_seen_entry *i, n = {
		.id	= id,
		.equiv	= bch2_snapshot_equiv(c, id),
	};
	int ret = 0;

	darray_for_each(s->ids, i) {
		if (i->id == id)
			return 0;
		if (i->id > id)
			break;
	}

	ret = darray_insert_item(&s->ids, i - s->ids.data, n);
	if (ret)
		bch_err(c, "error reallocating snapshots_seen table (size %zu)",
			s->ids.size);
	return ret;
}

static int snapshots_seen_update(struct bch_fs *c, struct snapshots_seen *s,
				 enum btree_id btree_id, struct bpos pos)
{
	struct snapshots_seen_entry *i, n = {
		.id	= pos.snapshot,
		.equiv	= bch2_snapshot_equiv(c, pos.snapshot),
	};
	int ret = 0;

	if (!bkey_eq(s->pos, pos))
		s->ids.nr = 0;

	s->pos = pos;
	s->pos.snapshot = n.equiv;

	darray_for_each(s->ids, i) {
		if (i->id == n.id)
			return 0;

		/*
		 * We currently don't rigorously track for snapshot cleanup
		 * needing to be run, so it shouldn't be a fsck error yet:
		 */
		if (i->equiv == n.equiv) {
			bch_err(c, "snapshot deletion did not finish:\n"
				"  duplicate keys in btree %s at %llu:%llu snapshots %u, %u (equiv %u)\n",
				bch2_btree_id_str(btree_id),
				pos.inode, pos.offset,
				i->id, n.id, n.equiv);
			set_bit(BCH_FS_NEED_DELETE_DEAD_SNAPSHOTS, &c->flags);
			return bch2_run_explicit_recovery_pass(c, BCH_RECOVERY_PASS_delete_dead_snapshots);
		}
	}

	ret = darray_push(&s->ids, n);
	if (ret)
		bch_err(c, "error reallocating snapshots_seen table (size %zu)",
			s->ids.size);
	return ret;
}

/**
 * key_visible_in_snapshot - returns true if @id is a descendent of @ancestor,
 * and @ancestor hasn't been overwritten in @seen
 *
 * @c:		filesystem handle
 * @seen:	list of snapshot ids already seen at current position
 * @id:		descendent snapshot id
 * @ancestor:	ancestor snapshot id
 *
 * Returns:	whether key in @ancestor snapshot is visible in @id snapshot
 */
static bool key_visible_in_snapshot(struct bch_fs *c, struct snapshots_seen *seen,
				    u32 id, u32 ancestor)
{
	ssize_t i;

	EBUG_ON(id > ancestor);
	EBUG_ON(!bch2_snapshot_is_equiv(c, id));
	EBUG_ON(!bch2_snapshot_is_equiv(c, ancestor));

	/* @ancestor should be the snapshot most recently added to @seen */
	EBUG_ON(ancestor != seen->pos.snapshot);
	EBUG_ON(ancestor != seen->ids.data[seen->ids.nr - 1].equiv);

	if (id == ancestor)
		return true;

	if (!bch2_snapshot_is_ancestor(c, id, ancestor))
		return false;

	/*
	 * We know that @id is a descendant of @ancestor, we're checking if
	 * we've seen a key that overwrote @ancestor - i.e. also a descendent of
	 * @ascestor and with @id as a descendent.
	 *
	 * But we already know that we're scanning IDs between @id and @ancestor
	 * numerically, since snapshot ID lists are kept sorted, so if we find
	 * an id that's an ancestor of @id we're done:
	 */

	for (i = seen->ids.nr - 2;
	     i >= 0 && seen->ids.data[i].equiv >= id;
	     --i)
		if (bch2_snapshot_is_ancestor(c, id, seen->ids.data[i].equiv))
			return false;

	return true;
}

/**
 * ref_visible - given a key with snapshot id @src that points to a key with
 * snapshot id @dst, test whether there is some snapshot in which @dst is
 * visible.
 *
 * @c:		filesystem handle
 * @s:		list of snapshot IDs already seen at @src
 * @src:	snapshot ID of src key
 * @dst:	snapshot ID of dst key
 * Returns:	true if there is some snapshot in which @dst is visible
 *
 * Assumes we're visiting @src keys in natural key order
 */
static bool ref_visible(struct bch_fs *c, struct snapshots_seen *s,
			u32 src, u32 dst)
{
	return dst <= src
		? key_visible_in_snapshot(c, s, dst, src)
		: bch2_snapshot_is_ancestor(c, src, dst);
}

static int ref_visible2(struct bch_fs *c,
			u32 src, struct snapshots_seen *src_seen,
			u32 dst, struct snapshots_seen *dst_seen)
{
	src = bch2_snapshot_equiv(c, src);
	dst = bch2_snapshot_equiv(c, dst);

	if (dst > src) {
		swap(dst, src);
		swap(dst_seen, src_seen);
	}
	return key_visible_in_snapshot(c, src_seen, dst, src);
}

#define for_each_visible_inode(_c, _s, _w, _snapshot, _i)				\
	for (_i = (_w)->inodes.data; _i < (_w)->inodes.data + (_w)->inodes.nr &&	\
	     (_i)->snapshot <= (_snapshot); _i++)					\
		if (key_visible_in_snapshot(_c, _s, _i->snapshot, _snapshot))

struct inode_walker_entry {
	struct bch_inode_unpacked inode;
	u32			snapshot;
	bool			seen_this_pos;
	u64			count;
};

struct inode_walker {
	bool				first_this_inode;
	bool				recalculate_sums;
	struct bpos			last_pos;

	DARRAY(struct inode_walker_entry) inodes;
};

static void inode_walker_exit(struct inode_walker *w)
{
	darray_exit(&w->inodes);
}

static struct inode_walker inode_walker_init(void)
{
	return (struct inode_walker) { 0, };
}

static int add_inode(struct bch_fs *c, struct inode_walker *w,
		     struct bkey_s_c inode)
{
	struct bch_inode_unpacked u;

	BUG_ON(bch2_inode_unpack(inode, &u));

	return darray_push(&w->inodes, ((struct inode_walker_entry) {
		.inode		= u,
		.snapshot	= bch2_snapshot_equiv(c, inode.k->p.snapshot),
	}));
}

static int get_inodes_all_snapshots(struct btree_trans *trans,
				    struct inode_walker *w, u64 inum)
{
	struct bch_fs *c = trans->c;
	struct btree_iter iter;
	struct bkey_s_c k;
	u32 restart_count = trans->restart_count;
	int ret;

	w->recalculate_sums = false;
	w->inodes.nr = 0;

	for_each_btree_key(trans, iter, BTREE_ID_inodes, POS(0, inum),
			   BTREE_ITER_ALL_SNAPSHOTS, k, ret) {
		if (k.k->p.offset != inum)
			break;

		if (bkey_is_inode(k.k))
			add_inode(c, w, k);
	}
	bch2_trans_iter_exit(trans, &iter);

	if (ret)
		return ret;

	w->first_this_inode = true;

	return trans_was_restarted(trans, restart_count);
}

static struct inode_walker_entry *
lookup_inode_for_snapshot(struct bch_fs *c, struct inode_walker *w,
			  u32 snapshot, bool is_whiteout)
{
	struct inode_walker_entry *i;

	snapshot = bch2_snapshot_equiv(c, snapshot);

	darray_for_each(w->inodes, i)
		if (bch2_snapshot_is_ancestor(c, snapshot, i->snapshot))
			goto found;

	return NULL;
found:
	BUG_ON(snapshot > i->snapshot);

	if (snapshot != i->snapshot && !is_whiteout) {
		struct inode_walker_entry new = *i;
		size_t pos;
		int ret;

		new.snapshot = snapshot;
		new.count = 0;

		bch_info(c, "have key for inode %llu:%u but have inode in ancestor snapshot %u",
			 w->last_pos.inode, snapshot, i->snapshot);

		while (i > w->inodes.data && i[-1].snapshot > snapshot)
			--i;

		pos = i - w->inodes.data;
		ret = darray_insert_item(&w->inodes, pos, new);
		if (ret)
			return ERR_PTR(ret);

		i = w->inodes.data + pos;
	}

	return i;
}

static struct inode_walker_entry *walk_inode(struct btree_trans *trans,
					     struct inode_walker *w, struct bpos pos,
					     bool is_whiteout)
{
	if (w->last_pos.inode != pos.inode) {
		int ret = get_inodes_all_snapshots(trans, w, pos.inode);
		if (ret)
			return ERR_PTR(ret);
	} else if (bkey_cmp(w->last_pos, pos)) {
		struct inode_walker_entry *i;

		darray_for_each(w->inodes, i)
			i->seen_this_pos = false;

	}

	w->last_pos = pos;

	return lookup_inode_for_snapshot(trans->c, w, pos.snapshot, is_whiteout);
}

static int __get_visible_inodes(struct btree_trans *trans,
				struct inode_walker *w,
				struct snapshots_seen *s,
				u64 inum)
{
	struct bch_fs *c = trans->c;
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	w->inodes.nr = 0;

	for_each_btree_key_norestart(trans, iter, BTREE_ID_inodes, POS(0, inum),
			   BTREE_ITER_ALL_SNAPSHOTS, k, ret) {
		u32 equiv = bch2_snapshot_equiv(c, k.k->p.snapshot);

		if (k.k->p.offset != inum)
			break;

		if (!ref_visible(c, s, s->pos.snapshot, equiv))
			continue;

		if (bkey_is_inode(k.k))
			add_inode(c, w, k);

		if (equiv >= s->pos.snapshot)
			break;
	}
	bch2_trans_iter_exit(trans, &iter);

	return ret;
}

static int check_key_has_snapshot(struct btree_trans *trans,
				  struct btree_iter *iter,
				  struct bkey_s_c k)
{
	struct bch_fs *c = trans->c;
	struct printbuf buf = PRINTBUF;
	int ret = 0;

	if (mustfix_fsck_err_on(!bch2_snapshot_equiv(c, k.k->p.snapshot), c,
				bkey_in_missing_snapshot,
				"key in missing snapshot: %s",
				(bch2_bkey_val_to_text(&buf, c, k), buf.buf)))
		ret = bch2_btree_delete_at(trans, iter,
					    BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE) ?: 1;
fsck_err:
	printbuf_exit(&buf);
	return ret;
}

static int hash_redo_key(struct btree_trans *trans,
			 const struct bch_hash_desc desc,
			 struct bch_hash_info *hash_info,
			 struct btree_iter *k_iter, struct bkey_s_c k)
{
	struct bkey_i *delete;
	struct bkey_i *tmp;

	delete = bch2_trans_kmalloc(trans, sizeof(*delete));
	if (IS_ERR(delete))
		return PTR_ERR(delete);

	tmp = bch2_bkey_make_mut_noupdate(trans, k);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	bkey_init(&delete->k);
	delete->k.p = k_iter->pos;
	return  bch2_btree_iter_traverse(k_iter) ?:
		bch2_trans_update(trans, k_iter, delete, 0) ?:
		bch2_hash_set_snapshot(trans, desc, hash_info,
				       (subvol_inum) { 0, k.k->p.inode },
				       k.k->p.snapshot, tmp,
				       BCH_HASH_SET_MUST_CREATE,
				       BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE) ?:
		bch2_trans_commit(trans, NULL, NULL,
				  BTREE_INSERT_NOFAIL|
				  BTREE_INSERT_LAZY_RW);
}

static int hash_check_key(struct btree_trans *trans,
			  const struct bch_hash_desc desc,
			  struct bch_hash_info *hash_info,
			  struct btree_iter *k_iter, struct bkey_s_c hash_k)
{
	struct bch_fs *c = trans->c;
	struct btree_iter iter = { NULL };
	struct printbuf buf = PRINTBUF;
	struct bkey_s_c k;
	u64 hash;
	int ret = 0;

	if (hash_k.k->type != desc.key_type)
		return 0;

	hash = desc.hash_bkey(hash_info, hash_k);

	if (likely(hash == hash_k.k->p.offset))
		return 0;

	if (hash_k.k->p.offset < hash)
		goto bad_hash;

	for_each_btree_key_norestart(trans, iter, desc.btree_id,
				     SPOS(hash_k.k->p.inode, hash, hash_k.k->p.snapshot),
				     BTREE_ITER_SLOTS, k, ret) {
		if (bkey_eq(k.k->p, hash_k.k->p))
			break;

		if (fsck_err_on(k.k->type == desc.key_type &&
				!desc.cmp_bkey(k, hash_k), c,
				hash_table_key_duplicate,
				"duplicate hash table keys:\n%s",
				(printbuf_reset(&buf),
				 bch2_bkey_val_to_text(&buf, c, hash_k),
				 buf.buf))) {
			ret = bch2_hash_delete_at(trans, desc, hash_info, k_iter, 0) ?: 1;
			break;
		}

		if (bkey_deleted(k.k)) {
			bch2_trans_iter_exit(trans, &iter);
			goto bad_hash;
		}
	}
out:
	bch2_trans_iter_exit(trans, &iter);
	printbuf_exit(&buf);
	return ret;
bad_hash:
	if (fsck_err(c, hash_table_key_wrong_offset,
		     "hash table key at wrong offset: btree %s inode %llu offset %llu, hashed to %llu\n%s",
		     bch2_btree_id_str(desc.btree_id), hash_k.k->p.inode, hash_k.k->p.offset, hash,
		     (printbuf_reset(&buf),
		      bch2_bkey_val_to_text(&buf, c, hash_k), buf.buf))) {
		ret = hash_redo_key(trans, desc, hash_info, k_iter, hash_k);
		bch_err_fn(c, ret);
		if (ret)
			return ret;
		ret = -BCH_ERR_transaction_restart_nested;
	}
fsck_err:
	goto out;
}

static int check_inode(struct btree_trans *trans,
		       struct btree_iter *iter,
		       struct bkey_s_c k,
		       struct bch_inode_unpacked *prev,
		       struct snapshots_seen *s,
		       bool full)
{
	struct bch_fs *c = trans->c;
	struct bch_inode_unpacked u;
	bool do_update = false;
	int ret;

	ret = check_key_has_snapshot(trans, iter, k);
	if (ret < 0)
		goto err;
	if (ret)
		return 0;

	ret = snapshots_seen_update(c, s, iter->btree_id, k.k->p);
	if (ret)
		goto err;

	if (!bkey_is_inode(k.k))
		return 0;

	BUG_ON(bch2_inode_unpack(k, &u));

	if (!full &&
	    !(u.bi_flags & (BCH_INODE_i_size_dirty|
			    BCH_INODE_i_sectors_dirty|
			    BCH_INODE_unlinked)))
		return 0;

	if (prev->bi_inum != u.bi_inum)
		*prev = u;

	if (fsck_err_on(prev->bi_hash_seed	!= u.bi_hash_seed ||
			inode_d_type(prev)	!= inode_d_type(&u),
			c, inode_snapshot_mismatch,
			"inodes in different snapshots don't match")) {
		bch_err(c, "repair not implemented yet");
		return -EINVAL;
	}

	if ((u.bi_flags & (BCH_INODE_i_size_dirty|BCH_INODE_unlinked)) &&
	    bch2_key_has_snapshot_overwrites(trans, BTREE_ID_inodes, k.k->p)) {
		struct bpos new_min_pos;

		ret = bch2_propagate_key_to_snapshot_leaves(trans, iter->btree_id, k, &new_min_pos);
		if (ret)
			goto err;

		u.bi_flags &= ~BCH_INODE_i_size_dirty|BCH_INODE_unlinked;

		ret = __write_inode(trans, &u, iter->pos.snapshot);
		bch_err_msg(c, ret, "in fsck updating inode");
		if (ret)
			return ret;

		if (!bpos_eq(new_min_pos, POS_MIN))
			bch2_btree_iter_set_pos(iter, bpos_predecessor(new_min_pos));
		return 0;
	}

	if (u.bi_flags & BCH_INODE_unlinked &&
	    (!c->sb.clean ||
	     fsck_err(c, inode_unlinked_but_clean,
		      "filesystem marked clean, but inode %llu unlinked",
		      u.bi_inum))) {
		bch2_trans_unlock(trans);
		bch2_fs_lazy_rw(c);

		ret = bch2_inode_rm_snapshot(trans, u.bi_inum, iter->pos.snapshot);
		bch_err_msg(c, ret, "in fsck deleting inode");
		return ret;
	}

	if (u.bi_flags & BCH_INODE_i_size_dirty &&
	    (!c->sb.clean ||
	     fsck_err(c, inode_i_size_dirty_but_clean,
		      "filesystem marked clean, but inode %llu has i_size dirty",
		      u.bi_inum))) {
		bch_verbose(c, "truncating inode %llu", u.bi_inum);

		bch2_trans_unlock(trans);
		bch2_fs_lazy_rw(c);

		/*
		 * XXX: need to truncate partial blocks too here - or ideally
		 * just switch units to bytes and that issue goes away
		 */
		ret = bch2_btree_delete_range_trans(trans, BTREE_ID_extents,
				SPOS(u.bi_inum, round_up(u.bi_size, block_bytes(c)) >> 9,
				     iter->pos.snapshot),
				POS(u.bi_inum, U64_MAX),
				0, NULL);
		bch_err_msg(c, ret, "in fsck truncating inode");
		if (ret)
			return ret;

		/*
		 * We truncated without our normal sector accounting hook, just
		 * make sure we recalculate it:
		 */
		u.bi_flags |= BCH_INODE_i_sectors_dirty;

		u.bi_flags &= ~BCH_INODE_i_size_dirty;
		do_update = true;
	}

	if (u.bi_flags & BCH_INODE_i_sectors_dirty &&
	    (!c->sb.clean ||
	     fsck_err(c, inode_i_sectors_dirty_but_clean,
		      "filesystem marked clean, but inode %llu has i_sectors dirty",
		      u.bi_inum))) {
		s64 sectors;

		bch_verbose(c, "recounting sectors for inode %llu",
			    u.bi_inum);

		sectors = bch2_count_inode_sectors(trans, u.bi_inum, iter->pos.snapshot);
		if (sectors < 0) {
			bch_err_msg(c, sectors, "in fsck recounting inode sectors");
			return sectors;
		}

		u.bi_sectors = sectors;
		u.bi_flags &= ~BCH_INODE_i_sectors_dirty;
		do_update = true;
	}

	if (u.bi_flags & BCH_INODE_backptr_untrusted) {
		u.bi_dir = 0;
		u.bi_dir_offset = 0;
		u.bi_flags &= ~BCH_INODE_backptr_untrusted;
		do_update = true;
	}

	if (do_update) {
		ret = __write_inode(trans, &u, iter->pos.snapshot);
		bch_err_msg(c, ret, "in fsck updating inode");
		if (ret)
			return ret;
	}
err:
fsck_err:
	bch_err_fn(c, ret);
	return ret;
}

noinline_for_stack
int bch2_check_inodes(struct bch_fs *c)
{
	bool full = c->opts.fsck;
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	struct bch_inode_unpacked prev = { 0 };
	struct snapshots_seen s;
	struct bkey_s_c k;
	int ret;

	snapshots_seen_init(&s);

	ret = for_each_btree_key_commit(trans, iter, BTREE_ID_inodes,
			POS_MIN,
			BTREE_ITER_PREFETCH|BTREE_ITER_ALL_SNAPSHOTS, k,
			NULL, NULL, BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL,
		check_inode(trans, &iter, k, &prev, &s, full));

	snapshots_seen_exit(&s);
	bch2_trans_put(trans);
	bch_err_fn(c, ret);
	return ret;
}

static struct bkey_s_c_dirent dirent_get_by_pos(struct btree_trans *trans,
						struct btree_iter *iter,
						struct bpos pos)
{
	return bch2_bkey_get_iter_typed(trans, iter, BTREE_ID_dirents, pos, 0, dirent);
}

static bool inode_points_to_dirent(struct bch_inode_unpacked *inode,
				   struct bkey_s_c_dirent d)
{
	return  inode->bi_dir		== d.k->p.inode &&
		inode->bi_dir_offset	== d.k->p.offset;
}

static bool dirent_points_to_inode(struct bkey_s_c_dirent d,
				   struct bch_inode_unpacked *inode)
{
	return d.v->d_type == DT_SUBVOL
		? le32_to_cpu(d.v->d_child_subvol)	== inode->bi_subvol
		: le64_to_cpu(d.v->d_inum)		== inode->bi_inum;
}

static int inode_backpointer_exists(struct btree_trans *trans,
				    struct bch_inode_unpacked *inode,
				    u32 snapshot)
{
	struct btree_iter iter;
	struct bkey_s_c_dirent d;
	int ret;

	d = dirent_get_by_pos(trans, &iter,
			SPOS(inode->bi_dir, inode->bi_dir_offset, snapshot));
	ret = bkey_err(d);
	if (ret)
		return bch2_err_matches(ret, ENOENT) ? 0 : ret;

	ret = dirent_points_to_inode(d, inode);
	bch2_trans_iter_exit(trans, &iter);
	return ret;
}

static int check_i_sectors(struct btree_trans *trans, struct inode_walker *w)
{
	struct bch_fs *c = trans->c;
	struct inode_walker_entry *i;
	u32 restart_count = trans->restart_count;
	int ret = 0;
	s64 count2;

	darray_for_each(w->inodes, i) {
		if (i->inode.bi_sectors == i->count)
			continue;

		count2 = bch2_count_inode_sectors(trans, w->last_pos.inode, i->snapshot);

		if (w->recalculate_sums)
			i->count = count2;

		if (i->count != count2) {
			bch_err(c, "fsck counted i_sectors wrong for inode %llu:%u: got %llu should be %llu",
				w->last_pos.inode, i->snapshot, i->count, count2);
			return -BCH_ERR_internal_fsck_err;
		}

		if (fsck_err_on(!(i->inode.bi_flags & BCH_INODE_i_sectors_dirty),
				c, inode_i_sectors_wrong,
				"inode %llu:%u has incorrect i_sectors: got %llu, should be %llu",
				w->last_pos.inode, i->snapshot,
				i->inode.bi_sectors, i->count)) {
			i->inode.bi_sectors = i->count;
			ret = fsck_write_inode(trans, &i->inode, i->snapshot);
			if (ret)
				break;
		}
	}
fsck_err:
	bch_err_fn(c, ret);
	return ret ?: trans_was_restarted(trans, restart_count);
}

struct extent_end {
	u32			snapshot;
	u64			offset;
	struct snapshots_seen	seen;
};

struct extent_ends {
	struct bpos			last_pos;
	DARRAY(struct extent_end)	e;
};

static void extent_ends_reset(struct extent_ends *extent_ends)
{
	struct extent_end *i;

	darray_for_each(extent_ends->e, i)
		snapshots_seen_exit(&i->seen);

	extent_ends->e.nr = 0;
}

static void extent_ends_exit(struct extent_ends *extent_ends)
{
	extent_ends_reset(extent_ends);
	darray_exit(&extent_ends->e);
}

static void extent_ends_init(struct extent_ends *extent_ends)
{
	memset(extent_ends, 0, sizeof(*extent_ends));
}

static int extent_ends_at(struct bch_fs *c,
			  struct extent_ends *extent_ends,
			  struct snapshots_seen *seen,
			  struct bkey_s_c k)
{
	struct extent_end *i, n = (struct extent_end) {
		.offset		= k.k->p.offset,
		.snapshot	= k.k->p.snapshot,
		.seen		= *seen,
	};

	n.seen.ids.data = kmemdup(seen->ids.data,
			      sizeof(seen->ids.data[0]) * seen->ids.size,
			      GFP_KERNEL);
	if (!n.seen.ids.data)
		return -BCH_ERR_ENOMEM_fsck_extent_ends_at;

	darray_for_each(extent_ends->e, i) {
		if (i->snapshot == k.k->p.snapshot) {
			snapshots_seen_exit(&i->seen);
			*i = n;
			return 0;
		}

		if (i->snapshot >= k.k->p.snapshot)
			break;
	}

	return darray_insert_item(&extent_ends->e, i - extent_ends->e.data, n);
}

static int overlapping_extents_found(struct btree_trans *trans,
				     enum btree_id btree,
				     struct bpos pos1, struct snapshots_seen *pos1_seen,
				     struct bkey pos2,
				     bool *fixed,
				     struct extent_end *extent_end)
{
	struct bch_fs *c = trans->c;
	struct printbuf buf = PRINTBUF;
	struct btree_iter iter1, iter2 = { NULL };
	struct bkey_s_c k1, k2;
	int ret;

	BUG_ON(bkey_le(pos1, bkey_start_pos(&pos2)));

	bch2_trans_iter_init(trans, &iter1, btree, pos1,
			     BTREE_ITER_ALL_SNAPSHOTS|
			     BTREE_ITER_NOT_EXTENTS);
	k1 = bch2_btree_iter_peek_upto(&iter1, POS(pos1.inode, U64_MAX));
	ret = bkey_err(k1);
	if (ret)
		goto err;

	prt_str(&buf, "\n  ");
	bch2_bkey_val_to_text(&buf, c, k1);

	if (!bpos_eq(pos1, k1.k->p)) {
		prt_str(&buf, "\n  wanted\n  ");
		bch2_bpos_to_text(&buf, pos1);
		prt_str(&buf, "\n  ");
		bch2_bkey_to_text(&buf, &pos2);

		bch_err(c, "%s: error finding first overlapping extent when repairing, got%s",
			__func__, buf.buf);
		ret = -BCH_ERR_internal_fsck_err;
		goto err;
	}

	bch2_trans_copy_iter(&iter2, &iter1);

	while (1) {
		bch2_btree_iter_advance(&iter2);

		k2 = bch2_btree_iter_peek_upto(&iter2, POS(pos1.inode, U64_MAX));
		ret = bkey_err(k2);
		if (ret)
			goto err;

		if (bpos_ge(k2.k->p, pos2.p))
			break;
	}

	prt_str(&buf, "\n  ");
	bch2_bkey_val_to_text(&buf, c, k2);

	if (bpos_gt(k2.k->p, pos2.p) ||
	    pos2.size != k2.k->size) {
		bch_err(c, "%s: error finding seconding overlapping extent when repairing%s",
			__func__, buf.buf);
		ret = -BCH_ERR_internal_fsck_err;
		goto err;
	}

	prt_printf(&buf, "\n  overwriting %s extent",
		   pos1.snapshot >= pos2.p.snapshot ? "first" : "second");

	if (fsck_err(c, extent_overlapping,
		     "overlapping extents%s", buf.buf)) {
		struct btree_iter *old_iter = &iter1;
		struct disk_reservation res = { 0 };

		if (pos1.snapshot < pos2.p.snapshot) {
			old_iter = &iter2;
			swap(k1, k2);
		}

		trans->extra_journal_res += bch2_bkey_sectors_compressed(k2);

		ret =   bch2_trans_update_extent_overwrite(trans, old_iter,
				BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE,
				k1, k2) ?:
			bch2_trans_commit(trans, &res, NULL,
				BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL);
		bch2_disk_reservation_put(c, &res);

		if (ret)
			goto err;

		*fixed = true;

		if (pos1.snapshot == pos2.p.snapshot) {
			/*
			 * We overwrote the first extent, and did the overwrite
			 * in the same snapshot:
			 */
			extent_end->offset = bkey_start_offset(&pos2);
		} else if (pos1.snapshot > pos2.p.snapshot) {
			/*
			 * We overwrote the first extent in pos2's snapshot:
			 */
			ret = snapshots_seen_add_inorder(c, pos1_seen, pos2.p.snapshot);
		} else {
			/*
			 * We overwrote the second extent - restart
			 * check_extent() from the top:
			 */
			ret = -BCH_ERR_transaction_restart_nested;
		}
	}
fsck_err:
err:
	bch2_trans_iter_exit(trans, &iter2);
	bch2_trans_iter_exit(trans, &iter1);
	printbuf_exit(&buf);
	return ret;
}

static int check_overlapping_extents(struct btree_trans *trans,
			      struct snapshots_seen *seen,
			      struct extent_ends *extent_ends,
			      struct bkey_s_c k,
			      u32 equiv,
			      struct btree_iter *iter,
			      bool *fixed)
{
	struct bch_fs *c = trans->c;
	struct extent_end *i;
	int ret = 0;

	/* transaction restart, running again */
	if (bpos_eq(extent_ends->last_pos, k.k->p))
		return 0;

	if (extent_ends->last_pos.inode != k.k->p.inode)
		extent_ends_reset(extent_ends);

	darray_for_each(extent_ends->e, i) {
		if (i->offset <= bkey_start_offset(k.k))
			continue;

		if (!ref_visible2(c,
				  k.k->p.snapshot, seen,
				  i->snapshot, &i->seen))
			continue;

		ret = overlapping_extents_found(trans, iter->btree_id,
						SPOS(iter->pos.inode,
						     i->offset,
						     i->snapshot),
						&i->seen,
						*k.k, fixed, i);
		if (ret)
			goto err;
	}

	ret = extent_ends_at(c, extent_ends, seen, k);
	if (ret)
		goto err;

	extent_ends->last_pos = k.k->p;
err:
	return ret;
}

static int check_extent_overbig(struct btree_trans *trans, struct btree_iter *iter,
				struct bkey_s_c k)
{
	struct bch_fs *c = trans->c;
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	struct bch_extent_crc_unpacked crc;
	const union bch_extent_entry *i;
	unsigned encoded_extent_max_sectors = c->opts.encoded_extent_max >> 9;

	bkey_for_each_crc(k.k, ptrs, crc, i)
		if (crc_is_encoded(crc) &&
		    crc.uncompressed_size > encoded_extent_max_sectors) {
			struct printbuf buf = PRINTBUF;

			bch2_bkey_val_to_text(&buf, c, k);
			bch_err(c, "overbig encoded extent, please report this:\n  %s", buf.buf);
			printbuf_exit(&buf);
		}

	return 0;
}

static int check_extent(struct btree_trans *trans, struct btree_iter *iter,
			struct bkey_s_c k,
			struct inode_walker *inode,
			struct snapshots_seen *s,
			struct extent_ends *extent_ends)
{
	struct bch_fs *c = trans->c;
	struct inode_walker_entry *i;
	struct printbuf buf = PRINTBUF;
	struct bpos equiv = k.k->p;
	int ret = 0;

	equiv.snapshot = bch2_snapshot_equiv(c, k.k->p.snapshot);

	ret = check_key_has_snapshot(trans, iter, k);
	if (ret) {
		ret = ret < 0 ? ret : 0;
		goto out;
	}

	if (inode->last_pos.inode != k.k->p.inode) {
		ret = check_i_sectors(trans, inode);
		if (ret)
			goto err;
	}

	i = walk_inode(trans, inode, equiv, k.k->type == KEY_TYPE_whiteout);
	ret = PTR_ERR_OR_ZERO(i);
	if (ret)
		goto err;

	ret = snapshots_seen_update(c, s, iter->btree_id, k.k->p);
	if (ret)
		goto err;

	if (k.k->type != KEY_TYPE_whiteout) {
		if (fsck_err_on(!i, c, extent_in_missing_inode,
				"extent in missing inode:\n  %s",
				(printbuf_reset(&buf),
				 bch2_bkey_val_to_text(&buf, c, k), buf.buf)))
			goto delete;

		if (fsck_err_on(i &&
				!S_ISREG(i->inode.bi_mode) &&
				!S_ISLNK(i->inode.bi_mode),
				c, extent_in_non_reg_inode,
				"extent in non regular inode mode %o:\n  %s",
				i->inode.bi_mode,
				(printbuf_reset(&buf),
				 bch2_bkey_val_to_text(&buf, c, k), buf.buf)))
			goto delete;

		ret = check_overlapping_extents(trans, s, extent_ends, k,
						equiv.snapshot, iter,
						&inode->recalculate_sums);
		if (ret)
			goto err;
	}

	/*
	 * Check inodes in reverse order, from oldest snapshots to newest,
	 * starting from the inode that matches this extent's snapshot. If we
	 * didn't have one, iterate over all inodes:
	 */
	if (!i)
		i = inode->inodes.data + inode->inodes.nr - 1;

	for (;
	     inode->inodes.data && i >= inode->inodes.data;
	     --i) {
		if (i->snapshot > equiv.snapshot ||
		    !key_visible_in_snapshot(c, s, i->snapshot, equiv.snapshot))
			continue;

		if (k.k->type != KEY_TYPE_whiteout) {
			if (fsck_err_on(!(i->inode.bi_flags & BCH_INODE_i_size_dirty) &&
					k.k->p.offset > round_up(i->inode.bi_size, block_bytes(c)) >> 9 &&
					!bkey_extent_is_reservation(k),
					c, extent_past_end_of_inode,
					"extent type past end of inode %llu:%u, i_size %llu\n  %s",
					i->inode.bi_inum, i->snapshot, i->inode.bi_size,
					(bch2_bkey_val_to_text(&buf, c, k), buf.buf))) {
				struct btree_iter iter2;

				bch2_trans_copy_iter(&iter2, iter);
				bch2_btree_iter_set_snapshot(&iter2, i->snapshot);
				ret =   bch2_btree_iter_traverse(&iter2) ?:
					bch2_btree_delete_at(trans, &iter2,
						BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE);
				bch2_trans_iter_exit(trans, &iter2);
				if (ret)
					goto err;

				iter->k.type = KEY_TYPE_whiteout;
			}

			if (bkey_extent_is_allocation(k.k))
				i->count += k.k->size;
		}

		i->seen_this_pos = true;
	}
out:
err:
fsck_err:
	printbuf_exit(&buf);
	bch_err_fn(c, ret);
	return ret;
delete:
	ret = bch2_btree_delete_at(trans, iter, BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE);
	goto out;
}

/*
 * Walk extents: verify that extents have a corresponding S_ISREG inode, and
 * that i_size an i_sectors are consistent
 */
int bch2_check_extents(struct bch_fs *c)
{
	struct inode_walker w = inode_walker_init();
	struct snapshots_seen s;
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	struct bkey_s_c k;
	struct extent_ends extent_ends;
	struct disk_reservation res = { 0 };
	int ret = 0;

	snapshots_seen_init(&s);
	extent_ends_init(&extent_ends);

	ret = for_each_btree_key_commit(trans, iter, BTREE_ID_extents,
			POS(BCACHEFS_ROOT_INO, 0),
			BTREE_ITER_PREFETCH|BTREE_ITER_ALL_SNAPSHOTS, k,
			&res, NULL,
			BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL, ({
		bch2_disk_reservation_put(c, &res);
		check_extent(trans, &iter, k, &w, &s, &extent_ends) ?:
		check_extent_overbig(trans, &iter, k);
	})) ?:
	check_i_sectors(trans, &w);

	bch2_disk_reservation_put(c, &res);
	extent_ends_exit(&extent_ends);
	inode_walker_exit(&w);
	snapshots_seen_exit(&s);
	bch2_trans_put(trans);

	bch_err_fn(c, ret);
	return ret;
}

int bch2_check_indirect_extents(struct bch_fs *c)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	struct bkey_s_c k;
	struct disk_reservation res = { 0 };
	int ret = 0;

	ret = for_each_btree_key_commit(trans, iter, BTREE_ID_reflink,
			POS_MIN,
			BTREE_ITER_PREFETCH, k,
			&res, NULL,
			BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL, ({
		bch2_disk_reservation_put(c, &res);
		check_extent_overbig(trans, &iter, k);
	}));

	bch2_disk_reservation_put(c, &res);
	bch2_trans_put(trans);

	bch_err_fn(c, ret);
	return ret;
}

static int check_subdir_count(struct btree_trans *trans, struct inode_walker *w)
{
	struct bch_fs *c = trans->c;
	struct inode_walker_entry *i;
	u32 restart_count = trans->restart_count;
	int ret = 0;
	s64 count2;

	darray_for_each(w->inodes, i) {
		if (i->inode.bi_nlink == i->count)
			continue;

		count2 = bch2_count_subdirs(trans, w->last_pos.inode, i->snapshot);
		if (count2 < 0)
			return count2;

		if (i->count != count2) {
			bch_err(c, "fsck counted subdirectories wrong: got %llu should be %llu",
				i->count, count2);
			i->count = count2;
			if (i->inode.bi_nlink == i->count)
				continue;
		}

		if (fsck_err_on(i->inode.bi_nlink != i->count,
				c, inode_dir_wrong_nlink,
				"directory %llu:%u with wrong i_nlink: got %u, should be %llu",
				w->last_pos.inode, i->snapshot, i->inode.bi_nlink, i->count)) {
			i->inode.bi_nlink = i->count;
			ret = fsck_write_inode(trans, &i->inode, i->snapshot);
			if (ret)
				break;
		}
	}
fsck_err:
	bch_err_fn(c, ret);
	return ret ?: trans_was_restarted(trans, restart_count);
}

static int check_dirent_target(struct btree_trans *trans,
			       struct btree_iter *iter,
			       struct bkey_s_c_dirent d,
			       struct bch_inode_unpacked *target,
			       u32 target_snapshot)
{
	struct bch_fs *c = trans->c;
	struct bkey_i_dirent *n;
	bool backpointer_exists = true;
	struct printbuf buf = PRINTBUF;
	int ret = 0;

	if (!target->bi_dir &&
	    !target->bi_dir_offset) {
		target->bi_dir		= d.k->p.inode;
		target->bi_dir_offset	= d.k->p.offset;

		ret = __write_inode(trans, target, target_snapshot);
		if (ret)
			goto err;
	}

	if (!inode_points_to_dirent(target, d)) {
		ret = inode_backpointer_exists(trans, target, d.k->p.snapshot);
		if (ret < 0)
			goto err;

		backpointer_exists = ret;
		ret = 0;

		if (fsck_err_on(S_ISDIR(target->bi_mode) && backpointer_exists,
				c, inode_dir_multiple_links,
				"directory %llu with multiple links",
				target->bi_inum)) {
			ret = __remove_dirent(trans, d.k->p);
			goto out;
		}

		if (fsck_err_on(backpointer_exists && !target->bi_nlink,
				c, inode_multiple_links_but_nlink_0,
				"inode %llu type %s has multiple links but i_nlink 0",
				target->bi_inum, bch2_d_types[d.v->d_type])) {
			target->bi_nlink++;
			target->bi_flags &= ~BCH_INODE_unlinked;

			ret = __write_inode(trans, target, target_snapshot);
			if (ret)
				goto err;
		}

		if (fsck_err_on(!backpointer_exists,
				c, inode_wrong_backpointer,
				"inode %llu:%u has wrong backpointer:\n"
				"got       %llu:%llu\n"
				"should be %llu:%llu",
				target->bi_inum, target_snapshot,
				target->bi_dir,
				target->bi_dir_offset,
				d.k->p.inode,
				d.k->p.offset)) {
			target->bi_dir		= d.k->p.inode;
			target->bi_dir_offset	= d.k->p.offset;

			ret = __write_inode(trans, target, target_snapshot);
			if (ret)
				goto err;
		}
	}

	if (fsck_err_on(d.v->d_type != inode_d_type(target),
			c, dirent_d_type_wrong,
			"incorrect d_type: got %s, should be %s:\n%s",
			bch2_d_type_str(d.v->d_type),
			bch2_d_type_str(inode_d_type(target)),
			(printbuf_reset(&buf),
			 bch2_bkey_val_to_text(&buf, c, d.s_c), buf.buf))) {
		n = bch2_trans_kmalloc(trans, bkey_bytes(d.k));
		ret = PTR_ERR_OR_ZERO(n);
		if (ret)
			goto err;

		bkey_reassemble(&n->k_i, d.s_c);
		n->v.d_type = inode_d_type(target);

		ret = bch2_trans_update(trans, iter, &n->k_i, 0);
		if (ret)
			goto err;

		d = dirent_i_to_s_c(n);
	}

	if (d.v->d_type == DT_SUBVOL &&
	    target->bi_parent_subvol != le32_to_cpu(d.v->d_parent_subvol) &&
	    (c->sb.version < bcachefs_metadata_version_subvol_dirent ||
	     fsck_err(c, dirent_d_parent_subvol_wrong,
		      "dirent has wrong d_parent_subvol field: got %u, should be %u",
		      le32_to_cpu(d.v->d_parent_subvol),
		      target->bi_parent_subvol))) {
		n = bch2_trans_kmalloc(trans, bkey_bytes(d.k));
		ret = PTR_ERR_OR_ZERO(n);
		if (ret)
			goto err;

		bkey_reassemble(&n->k_i, d.s_c);
		n->v.d_parent_subvol = cpu_to_le32(target->bi_parent_subvol);

		ret = bch2_trans_update(trans, iter, &n->k_i, 0);
		if (ret)
			goto err;

		d = dirent_i_to_s_c(n);
	}
out:
err:
fsck_err:
	printbuf_exit(&buf);
	bch_err_fn(c, ret);
	return ret;
}

static int check_dirent(struct btree_trans *trans, struct btree_iter *iter,
			struct bkey_s_c k,
			struct bch_hash_info *hash_info,
			struct inode_walker *dir,
			struct inode_walker *target,
			struct snapshots_seen *s)
{
	struct bch_fs *c = trans->c;
	struct bkey_s_c_dirent d;
	struct inode_walker_entry *i;
	struct printbuf buf = PRINTBUF;
	struct bpos equiv;
	int ret = 0;

	ret = check_key_has_snapshot(trans, iter, k);
	if (ret) {
		ret = ret < 0 ? ret : 0;
		goto out;
	}

	equiv = k.k->p;
	equiv.snapshot = bch2_snapshot_equiv(c, k.k->p.snapshot);

	ret = snapshots_seen_update(c, s, iter->btree_id, k.k->p);
	if (ret)
		goto err;

	if (k.k->type == KEY_TYPE_whiteout)
		goto out;

	if (dir->last_pos.inode != k.k->p.inode) {
		ret = check_subdir_count(trans, dir);
		if (ret)
			goto err;
	}

	BUG_ON(!iter->path->should_be_locked);

	i = walk_inode(trans, dir, equiv, k.k->type == KEY_TYPE_whiteout);
	ret = PTR_ERR_OR_ZERO(i);
	if (ret < 0)
		goto err;

	if (dir->first_this_inode && dir->inodes.nr)
		*hash_info = bch2_hash_info_init(c, &dir->inodes.data[0].inode);
	dir->first_this_inode = false;

	if (fsck_err_on(!i, c, dirent_in_missing_dir_inode,
			"dirent in nonexisting directory:\n%s",
			(printbuf_reset(&buf),
			 bch2_bkey_val_to_text(&buf, c, k), buf.buf))) {
		ret = bch2_btree_delete_at(trans, iter,
				BTREE_UPDATE_INTERNAL_SNAPSHOT_NODE);
		goto out;
	}

	if (!i)
		goto out;

	if (fsck_err_on(!S_ISDIR(i->inode.bi_mode),
			c, dirent_in_non_dir_inode,
			"dirent in non directory inode type %s:\n%s",
			bch2_d_type_str(inode_d_type(&i->inode)),
			(printbuf_reset(&buf),
			 bch2_bkey_val_to_text(&buf, c, k), buf.buf))) {
		ret = bch2_btree_delete_at(trans, iter, 0);
		goto out;
	}

	ret = hash_check_key(trans, bch2_dirent_hash_desc, hash_info, iter, k);
	if (ret < 0)
		goto err;
	if (ret) {
		/* dirent has been deleted */
		ret = 0;
		goto out;
	}

	if (k.k->type != KEY_TYPE_dirent)
		goto out;

	d = bkey_s_c_to_dirent(k);

	if (d.v->d_type == DT_SUBVOL) {
		struct bch_inode_unpacked subvol_root;
		u32 target_subvol = le32_to_cpu(d.v->d_child_subvol);
		u32 target_snapshot;
		u64 target_inum;

		ret = __subvol_lookup(trans, target_subvol,
				      &target_snapshot, &target_inum);
		if (ret && !bch2_err_matches(ret, ENOENT))
			goto err;

		if (fsck_err_on(ret, c, dirent_to_missing_subvol,
				"dirent points to missing subvolume %u",
				le32_to_cpu(d.v->d_child_subvol))) {
			ret = __remove_dirent(trans, d.k->p);
			goto err;
		}

		ret = __lookup_inode(trans, target_inum,
				   &subvol_root, &target_snapshot);
		if (ret && !bch2_err_matches(ret, ENOENT))
			goto err;

		if (fsck_err_on(ret, c, subvol_to_missing_root,
				"subvolume %u points to missing subvolume root %llu",
				target_subvol,
				target_inum)) {
			bch_err(c, "repair not implemented yet");
			ret = -EINVAL;
			goto err;
		}

		if (fsck_err_on(subvol_root.bi_subvol != target_subvol,
				c, subvol_root_wrong_bi_subvol,
				"subvol root %llu has wrong bi_subvol field: got %u, should be %u",
				target_inum,
				subvol_root.bi_subvol, target_subvol)) {
			subvol_root.bi_subvol = target_subvol;
			ret = __write_inode(trans, &subvol_root, target_snapshot);
			if (ret)
				goto err;
		}

		ret = check_dirent_target(trans, iter, d, &subvol_root,
					  target_snapshot);
		if (ret)
			goto err;
	} else {
		ret = __get_visible_inodes(trans, target, s, le64_to_cpu(d.v->d_inum));
		if (ret)
			goto err;

		if (fsck_err_on(!target->inodes.nr,
				c, dirent_to_missing_inode,
				"dirent points to missing inode: (equiv %u)\n%s",
				equiv.snapshot,
				(printbuf_reset(&buf),
				 bch2_bkey_val_to_text(&buf, c, k),
				 buf.buf))) {
			ret = __remove_dirent(trans, d.k->p);
			if (ret)
				goto err;
		}

		darray_for_each(target->inodes, i) {
			ret = check_dirent_target(trans, iter, d,
						  &i->inode, i->snapshot);
			if (ret)
				goto err;
		}
	}

	if (d.v->d_type == DT_DIR)
		for_each_visible_inode(c, s, dir, equiv.snapshot, i)
			i->count++;

out:
err:
fsck_err:
	printbuf_exit(&buf);
	bch_err_fn(c, ret);
	return ret;
}

/*
 * Walk dirents: verify that they all have a corresponding S_ISDIR inode,
 * validate d_type
 */
int bch2_check_dirents(struct bch_fs *c)
{
	struct inode_walker dir = inode_walker_init();
	struct inode_walker target = inode_walker_init();
	struct snapshots_seen s;
	struct bch_hash_info hash_info;
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret = 0;

	snapshots_seen_init(&s);

	ret = for_each_btree_key_commit(trans, iter, BTREE_ID_dirents,
			POS(BCACHEFS_ROOT_INO, 0),
			BTREE_ITER_PREFETCH|BTREE_ITER_ALL_SNAPSHOTS,
			k,
			NULL, NULL,
			BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL,
		check_dirent(trans, &iter, k, &hash_info, &dir, &target, &s));

	bch2_trans_put(trans);
	snapshots_seen_exit(&s);
	inode_walker_exit(&dir);
	inode_walker_exit(&target);
	bch_err_fn(c, ret);
	return ret;
}

static int check_xattr(struct btree_trans *trans, struct btree_iter *iter,
		       struct bkey_s_c k,
		       struct bch_hash_info *hash_info,
		       struct inode_walker *inode)
{
	struct bch_fs *c = trans->c;
	struct inode_walker_entry *i;
	int ret;

	ret = check_key_has_snapshot(trans, iter, k);
	if (ret)
		return ret;

	i = walk_inode(trans, inode, k.k->p, k.k->type == KEY_TYPE_whiteout);
	ret = PTR_ERR_OR_ZERO(i);
	if (ret)
		return ret;

	if (inode->first_this_inode && inode->inodes.nr)
		*hash_info = bch2_hash_info_init(c, &inode->inodes.data[0].inode);
	inode->first_this_inode = false;

	if (fsck_err_on(!i, c, xattr_in_missing_inode,
			"xattr for missing inode %llu",
			k.k->p.inode))
		return bch2_btree_delete_at(trans, iter, 0);

	if (!i)
		return 0;

	ret = hash_check_key(trans, bch2_xattr_hash_desc, hash_info, iter, k);
fsck_err:
	bch_err_fn(c, ret);
	return ret;
}

/*
 * Walk xattrs: verify that they all have a corresponding inode
 */
int bch2_check_xattrs(struct bch_fs *c)
{
	struct inode_walker inode = inode_walker_init();
	struct bch_hash_info hash_info;
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret = 0;

	ret = bch2_trans_run(c,
		for_each_btree_key_commit(trans, iter, BTREE_ID_xattrs,
			POS(BCACHEFS_ROOT_INO, 0),
			BTREE_ITER_PREFETCH|BTREE_ITER_ALL_SNAPSHOTS,
			k,
			NULL, NULL,
			BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL,
		check_xattr(trans, &iter, k, &hash_info, &inode)));
	bch_err_fn(c, ret);
	return ret;
}

static int check_root_trans(struct btree_trans *trans)
{
	struct bch_fs *c = trans->c;
	struct bch_inode_unpacked root_inode;
	u32 snapshot;
	u64 inum;
	int ret;

	ret = __subvol_lookup(trans, BCACHEFS_ROOT_SUBVOL, &snapshot, &inum);
	if (ret && !bch2_err_matches(ret, ENOENT))
		return ret;

	if (mustfix_fsck_err_on(ret, c, root_subvol_missing,
				"root subvol missing")) {
		struct bkey_i_subvolume root_subvol;

		snapshot	= U32_MAX;
		inum		= BCACHEFS_ROOT_INO;

		bkey_subvolume_init(&root_subvol.k_i);
		root_subvol.k.p.offset = BCACHEFS_ROOT_SUBVOL;
		root_subvol.v.flags	= 0;
		root_subvol.v.snapshot	= cpu_to_le32(snapshot);
		root_subvol.v.inode	= cpu_to_le64(inum);
		ret = commit_do(trans, NULL, NULL,
				      BTREE_INSERT_NOFAIL|
				      BTREE_INSERT_LAZY_RW,
			bch2_btree_insert_trans(trans, BTREE_ID_subvolumes,
					    &root_subvol.k_i, 0));
		bch_err_msg(c, ret, "writing root subvol");
		if (ret)
			goto err;

	}

	ret = __lookup_inode(trans, BCACHEFS_ROOT_INO, &root_inode, &snapshot);
	if (ret && !bch2_err_matches(ret, ENOENT))
		return ret;

	if (mustfix_fsck_err_on(ret, c, root_dir_missing,
				"root directory missing") ||
	    mustfix_fsck_err_on(!S_ISDIR(root_inode.bi_mode),
				c, root_inode_not_dir,
				"root inode not a directory")) {
		bch2_inode_init(c, &root_inode, 0, 0, S_IFDIR|0755,
				0, NULL);
		root_inode.bi_inum = inum;

		ret = __write_inode(trans, &root_inode, snapshot);
		bch_err_msg(c, ret, "writing root inode");
	}
err:
fsck_err:
	return ret;
}

/* Get root directory, create if it doesn't exist: */
int bch2_check_root(struct bch_fs *c)
{
	int ret;

	ret = bch2_trans_do(c, NULL, NULL,
			     BTREE_INSERT_NOFAIL|
			     BTREE_INSERT_LAZY_RW,
		check_root_trans(trans));
	bch_err_fn(c, ret);
	return ret;
}

struct pathbuf_entry {
	u64	inum;
	u32	snapshot;
};

typedef DARRAY(struct pathbuf_entry) pathbuf;

static bool path_is_dup(pathbuf *p, u64 inum, u32 snapshot)
{
	struct pathbuf_entry *i;

	darray_for_each(*p, i)
		if (i->inum	== inum &&
		    i->snapshot	== snapshot)
			return true;

	return false;
}

static int path_down(struct bch_fs *c, pathbuf *p,
		     u64 inum, u32 snapshot)
{
	int ret = darray_push(p, ((struct pathbuf_entry) {
		.inum		= inum,
		.snapshot	= snapshot,
	}));

	if (ret)
		bch_err(c, "fsck: error allocating memory for pathbuf, size %zu",
			p->size);
	return ret;
}

/*
 * Check that a given inode is reachable from the root:
 *
 * XXX: we should also be verifying that inodes are in the right subvolumes
 */
static int check_path(struct btree_trans *trans,
		      pathbuf *p,
		      struct bch_inode_unpacked *inode,
		      u32 snapshot)
{
	struct bch_fs *c = trans->c;
	int ret = 0;

	snapshot = bch2_snapshot_equiv(c, snapshot);
	p->nr = 0;

	while (!(inode->bi_inum == BCACHEFS_ROOT_INO &&
		 inode->bi_subvol == BCACHEFS_ROOT_SUBVOL)) {
		struct btree_iter dirent_iter;
		struct bkey_s_c_dirent d;
		u32 parent_snapshot = snapshot;

		if (inode->bi_subvol) {
			u64 inum;

			ret = subvol_lookup(trans, inode->bi_parent_subvol,
					    &parent_snapshot, &inum);
			if (ret)
				break;
		}

		ret = lockrestart_do(trans,
			PTR_ERR_OR_ZERO((d = dirent_get_by_pos(trans, &dirent_iter,
					  SPOS(inode->bi_dir, inode->bi_dir_offset,
					       parent_snapshot))).k));
		if (ret && !bch2_err_matches(ret, ENOENT))
			break;

		if (!ret && !dirent_points_to_inode(d, inode)) {
			bch2_trans_iter_exit(trans, &dirent_iter);
			ret = -BCH_ERR_ENOENT_dirent_doesnt_match_inode;
		}

		if (bch2_err_matches(ret, ENOENT)) {
			if (fsck_err(c,  inode_unreachable,
				     "unreachable inode %llu:%u, type %s nlink %u backptr %llu:%llu",
				     inode->bi_inum, snapshot,
				     bch2_d_type_str(inode_d_type(inode)),
				     inode->bi_nlink,
				     inode->bi_dir,
				     inode->bi_dir_offset))
				ret = reattach_inode(trans, inode, snapshot);
			break;
		}

		bch2_trans_iter_exit(trans, &dirent_iter);

		if (!S_ISDIR(inode->bi_mode))
			break;

		ret = path_down(c, p, inode->bi_inum, snapshot);
		if (ret) {
			bch_err(c, "memory allocation failure");
			return ret;
		}

		snapshot = parent_snapshot;

		ret = lookup_inode(trans, inode->bi_dir, inode, &snapshot);
		if (ret) {
			/* Should have been caught in dirents pass */
			bch_err(c, "error looking up parent directory: %i", ret);
			break;
		}

		if (path_is_dup(p, inode->bi_inum, snapshot)) {
			struct pathbuf_entry *i;

			/* XXX print path */
			bch_err(c, "directory structure loop");

			darray_for_each(*p, i)
				pr_err("%llu:%u", i->inum, i->snapshot);
			pr_err("%llu:%u", inode->bi_inum, snapshot);

			if (!fsck_err(c, dir_loop,
				      "directory structure loop"))
				return 0;

			ret = commit_do(trans, NULL, NULL,
					      BTREE_INSERT_NOFAIL|
					      BTREE_INSERT_LAZY_RW,
					remove_backpointer(trans, inode));
			if (ret) {
				bch_err(c, "error removing dirent: %i", ret);
				break;
			}

			ret = reattach_inode(trans, inode, snapshot);
		}
	}
fsck_err:
	bch_err_fn(c, ret);
	return ret;
}

/*
 * Check for unreachable inodes, as well as loops in the directory structure:
 * After bch2_check_dirents(), if an inode backpointer doesn't exist that means it's
 * unreachable:
 */
int bch2_check_directory_structure(struct bch_fs *c)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bch_inode_unpacked u;
	pathbuf path = { 0, };
	int ret;

	for_each_btree_key(trans, iter, BTREE_ID_inodes, POS_MIN,
			   BTREE_ITER_INTENT|
			   BTREE_ITER_PREFETCH|
			   BTREE_ITER_ALL_SNAPSHOTS, k, ret) {
		if (!bkey_is_inode(k.k))
			continue;

		ret = bch2_inode_unpack(k, &u);
		if (ret) {
			/* Should have been caught earlier in fsck: */
			bch_err(c, "error unpacking inode %llu: %i", k.k->p.offset, ret);
			break;
		}

		if (u.bi_flags & BCH_INODE_unlinked)
			continue;

		ret = check_path(trans, &path, &u, iter.pos.snapshot);
		if (ret)
			break;
	}
	bch2_trans_iter_exit(trans, &iter);
	bch2_trans_put(trans);
	darray_exit(&path);
	bch_err_fn(c, ret);
	return ret;
}

struct nlink_table {
	size_t		nr;
	size_t		size;

	struct nlink {
		u64	inum;
		u32	snapshot;
		u32	count;
	}		*d;
};

static int add_nlink(struct bch_fs *c, struct nlink_table *t,
		     u64 inum, u32 snapshot)
{
	if (t->nr == t->size) {
		size_t new_size = max_t(size_t, 128UL, t->size * 2);
		void *d = kvmalloc_array(new_size, sizeof(t->d[0]), GFP_KERNEL);

		if (!d) {
			bch_err(c, "fsck: error allocating memory for nlink_table, size %zu",
				new_size);
			return -BCH_ERR_ENOMEM_fsck_add_nlink;
		}

		if (t->d)
			memcpy(d, t->d, t->size * sizeof(t->d[0]));
		kvfree(t->d);

		t->d = d;
		t->size = new_size;
	}


	t->d[t->nr++] = (struct nlink) {
		.inum		= inum,
		.snapshot	= snapshot,
	};

	return 0;
}

static int nlink_cmp(const void *_l, const void *_r)
{
	const struct nlink *l = _l;
	const struct nlink *r = _r;

	return cmp_int(l->inum, r->inum);
}

static void inc_link(struct bch_fs *c, struct snapshots_seen *s,
		     struct nlink_table *links,
		     u64 range_start, u64 range_end, u64 inum, u32 snapshot)
{
	struct nlink *link, key = {
		.inum = inum, .snapshot = U32_MAX,
	};

	if (inum < range_start || inum >= range_end)
		return;

	link = __inline_bsearch(&key, links->d, links->nr,
				sizeof(links->d[0]), nlink_cmp);
	if (!link)
		return;

	while (link > links->d && link[0].inum == link[-1].inum)
		--link;

	for (; link < links->d + links->nr && link->inum == inum; link++)
		if (ref_visible(c, s, snapshot, link->snapshot)) {
			link->count++;
			if (link->snapshot >= snapshot)
				break;
		}
}

noinline_for_stack
static int check_nlinks_find_hardlinks(struct bch_fs *c,
				       struct nlink_table *t,
				       u64 start, u64 *end)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bch_inode_unpacked u;
	int ret = 0;

	for_each_btree_key(trans, iter, BTREE_ID_inodes,
			   POS(0, start),
			   BTREE_ITER_INTENT|
			   BTREE_ITER_PREFETCH|
			   BTREE_ITER_ALL_SNAPSHOTS, k, ret) {
		if (!bkey_is_inode(k.k))
			continue;

		/* Should never fail, checked by bch2_inode_invalid: */
		BUG_ON(bch2_inode_unpack(k, &u));

		/*
		 * Backpointer and directory structure checks are sufficient for
		 * directories, since they can't have hardlinks:
		 */
		if (S_ISDIR(u.bi_mode))
			continue;

		if (!u.bi_nlink)
			continue;

		ret = add_nlink(c, t, k.k->p.offset, k.k->p.snapshot);
		if (ret) {
			*end = k.k->p.offset;
			ret = 0;
			break;
		}

	}
	bch2_trans_iter_exit(trans, &iter);
	bch2_trans_put(trans);

	if (ret)
		bch_err(c, "error in fsck: btree error %i while walking inodes", ret);

	return ret;
}

noinline_for_stack
static int check_nlinks_walk_dirents(struct bch_fs *c, struct nlink_table *links,
				     u64 range_start, u64 range_end)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct snapshots_seen s;
	struct btree_iter iter;
	struct bkey_s_c k;
	struct bkey_s_c_dirent d;
	int ret;

	snapshots_seen_init(&s);

	for_each_btree_key(trans, iter, BTREE_ID_dirents, POS_MIN,
			   BTREE_ITER_INTENT|
			   BTREE_ITER_PREFETCH|
			   BTREE_ITER_ALL_SNAPSHOTS, k, ret) {
		ret = snapshots_seen_update(c, &s, iter.btree_id, k.k->p);
		if (ret)
			break;

		switch (k.k->type) {
		case KEY_TYPE_dirent:
			d = bkey_s_c_to_dirent(k);

			if (d.v->d_type != DT_DIR &&
			    d.v->d_type != DT_SUBVOL)
				inc_link(c, &s, links, range_start, range_end,
					 le64_to_cpu(d.v->d_inum),
					 bch2_snapshot_equiv(c, d.k->p.snapshot));
			break;
		}
	}
	bch2_trans_iter_exit(trans, &iter);

	if (ret)
		bch_err(c, "error in fsck: btree error %i while walking dirents", ret);

	bch2_trans_put(trans);
	snapshots_seen_exit(&s);
	return ret;
}

static int check_nlinks_update_inode(struct btree_trans *trans, struct btree_iter *iter,
				     struct bkey_s_c k,
				     struct nlink_table *links,
				     size_t *idx, u64 range_end)
{
	struct bch_fs *c = trans->c;
	struct bch_inode_unpacked u;
	struct nlink *link = &links->d[*idx];
	int ret = 0;

	if (k.k->p.offset >= range_end)
		return 1;

	if (!bkey_is_inode(k.k))
		return 0;

	BUG_ON(bch2_inode_unpack(k, &u));

	if (S_ISDIR(u.bi_mode))
		return 0;

	if (!u.bi_nlink)
		return 0;

	while ((cmp_int(link->inum, k.k->p.offset) ?:
		cmp_int(link->snapshot, k.k->p.snapshot)) < 0) {
		BUG_ON(*idx == links->nr);
		link = &links->d[++*idx];
	}

	if (fsck_err_on(bch2_inode_nlink_get(&u) != link->count,
			c, inode_wrong_nlink,
			"inode %llu type %s has wrong i_nlink (%u, should be %u)",
			u.bi_inum, bch2_d_types[mode_to_type(u.bi_mode)],
			bch2_inode_nlink_get(&u), link->count)) {
		bch2_inode_nlink_set(&u, link->count);
		ret = __write_inode(trans, &u, k.k->p.snapshot);
	}
fsck_err:
	return ret;
}

noinline_for_stack
static int check_nlinks_update_hardlinks(struct bch_fs *c,
			       struct nlink_table *links,
			       u64 range_start, u64 range_end)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	size_t idx = 0;
	int ret = 0;

	ret = bch2_trans_run(c,
		for_each_btree_key_commit(trans, iter, BTREE_ID_inodes,
				POS(0, range_start),
				BTREE_ITER_INTENT|BTREE_ITER_PREFETCH|BTREE_ITER_ALL_SNAPSHOTS, k,
				NULL, NULL, BTREE_INSERT_LAZY_RW|BTREE_INSERT_NOFAIL,
			check_nlinks_update_inode(trans, &iter, k, links, &idx, range_end)));
	if (ret < 0) {
		bch_err(c, "error in fsck: btree error %i while walking inodes", ret);
		return ret;
	}

	return 0;
}

int bch2_check_nlinks(struct bch_fs *c)
{
	struct nlink_table links = { 0 };
	u64 this_iter_range_start, next_iter_range_start = 0;
	int ret = 0;

	do {
		this_iter_range_start = next_iter_range_start;
		next_iter_range_start = U64_MAX;

		ret = check_nlinks_find_hardlinks(c, &links,
						  this_iter_range_start,
						  &next_iter_range_start);

		ret = check_nlinks_walk_dirents(c, &links,
					  this_iter_range_start,
					  next_iter_range_start);
		if (ret)
			break;

		ret = check_nlinks_update_hardlinks(c, &links,
					 this_iter_range_start,
					 next_iter_range_start);
		if (ret)
			break;

		links.nr = 0;
	} while (next_iter_range_start != U64_MAX);

	kvfree(links.d);
	bch_err_fn(c, ret);
	return ret;
}

static int fix_reflink_p_key(struct btree_trans *trans, struct btree_iter *iter,
			     struct bkey_s_c k)
{
	struct bkey_s_c_reflink_p p;
	struct bkey_i_reflink_p *u;
	int ret;

	if (k.k->type != KEY_TYPE_reflink_p)
		return 0;

	p = bkey_s_c_to_reflink_p(k);

	if (!p.v->front_pad && !p.v->back_pad)
		return 0;

	u = bch2_trans_kmalloc(trans, sizeof(*u));
	ret = PTR_ERR_OR_ZERO(u);
	if (ret)
		return ret;

	bkey_reassemble(&u->k_i, k);
	u->v.front_pad	= 0;
	u->v.back_pad	= 0;

	return bch2_trans_update(trans, iter, &u->k_i, BTREE_TRIGGER_NORUN);
}

int bch2_fix_reflink_p(struct bch_fs *c)
{
	struct btree_iter iter;
	struct bkey_s_c k;
	int ret;

	if (c->sb.version >= bcachefs_metadata_version_reflink_p_fix)
		return 0;

	ret = bch2_trans_run(c,
		for_each_btree_key_commit(trans, iter,
				BTREE_ID_extents, POS_MIN,
				BTREE_ITER_INTENT|BTREE_ITER_PREFETCH|
				BTREE_ITER_ALL_SNAPSHOTS, k,
				NULL, NULL, BTREE_INSERT_NOFAIL|BTREE_INSERT_LAZY_RW,
			fix_reflink_p_key(trans, &iter, k)));
	bch_err_fn(c, ret);
	return ret;
}
