/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_NET_AFUNIX_H
#define __LINUX_NET_AFUNIX_H

#include <linux/socket.h>
#include <linux/un.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <net/sock.h>

void unix_inflight(struct user_struct *user, struct file *fp);
void unix_notinflight(struct user_struct *user, struct file *fp);
void unix_destruct_scm(struct sk_buff *skb);
void unix_gc(void);
void wait_for_unix_gc(void);
struct sock *unix_get_socket(struct file *filp);
struct sock *unix_peer_get(struct sock *sk);

#define UNIX_HASH_SIZE	256
#define UNIX_HASH_BITS	8

extern unsigned int unix_tot_inflight;
extern spinlock_t unix_table_lock;
extern struct hlist_head unix_socket_table[2 * UNIX_HASH_SIZE];

struct unix_address {
	refcount_t	refcnt;
	int		len;
	unsigned int	hash;
	struct sockaddr_un name[0];
};

struct unix_skb_parms {
	struct pid		*pid;		/* Skb credentials	*/
	kuid_t			uid;
	kgid_t			gid;
	struct scm_fp_list	*fp;		/* Passed files		*/
#ifdef CONFIG_SECURITY_NETWORK
	u32			secid;		/* Security ID		*/
#endif
	u32			consumed;
} __randomize_layout;

#define UNIXCB(skb)	(*(struct unix_skb_parms *)&((skb)->cb))

/* The AF_UNIX socket */
struct unix_sock {
	/* WARNING: sk has to be the first member */
	struct sock		sk;
	struct unix_address	*addr;
	struct path		path;
	struct mutex		iolock, bindlock;
	struct sock		*peer;
	struct list_head	link;
	unsigned long		inflight;
	spinlock_t		lock;
	unsigned long		gc_flags;
#define UNIX_GC_CANDIDATE	0
#define UNIX_GC_MAYBE_CYCLE	1
	struct socket_wq	peer_wq;
	wait_queue_entry_t	peer_wake;
};

static inline struct unix_sock *unix_sk(const struct sock *sk)
{
	return (struct unix_sock *)sk;
}

#define unix_state_lock(s)	spin_lock(&unix_sk(s)->lock)
#define unix_state_unlock(s)	spin_unlock(&unix_sk(s)->lock)
enum unix_socket_lock_class {
	U_LOCK_NORMAL,
	U_LOCK_SECOND,	/* for double locking, see unix_state_double_lock(). */
	U_LOCK_DIAG, /* used while dumping icons, see sk_diag_dump_icons(). */
	U_LOCK_GC_LISTENER, /* used for listening socket while determining gc
			     * candidates to close a small race window.
			     */
};

static inline void unix_state_lock_nested(struct sock *sk,
				   enum unix_socket_lock_class subclass)
{
	spin_lock_nested(&unix_sk(sk)->lock, subclass);
}

#define peer_wait peer_wq.wait

long unix_inq_len(struct sock *sk);
long unix_outq_len(struct sock *sk);

#ifdef CONFIG_SYSCTL
int unix_sysctl_register(struct net *net);
void unix_sysctl_unregister(struct net *net);
#else
static inline int unix_sysctl_register(struct net *net) { return 0; }
static inline void unix_sysctl_unregister(struct net *net) {}
#endif
#endif
