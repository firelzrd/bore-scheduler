#ifndef __BPF_KFUNCS__
#define __BPF_KFUNCS__

struct bpf_sock_addr_kern;

/* Description
 *  Initializes an skb-type dynptr
 * Returns
 *  Error code
 */
extern int bpf_dynptr_from_skb(struct __sk_buff *skb, __u64 flags,
    struct bpf_dynptr *ptr__uninit) __ksym;

/* Description
 *  Initializes an xdp-type dynptr
 * Returns
 *  Error code
 */
extern int bpf_dynptr_from_xdp(struct xdp_md *xdp, __u64 flags,
			       struct bpf_dynptr *ptr__uninit) __ksym;

/* Description
 *  Obtain a read-only pointer to the dynptr's data
 * Returns
 *  Either a direct pointer to the dynptr data or a pointer to the user-provided
 *  buffer if unable to obtain a direct pointer
 */
extern void *bpf_dynptr_slice(const struct bpf_dynptr *ptr, __u32 offset,
			      void *buffer, __u32 buffer__szk) __ksym;

/* Description
 *  Obtain a read-write pointer to the dynptr's data
 * Returns
 *  Either a direct pointer to the dynptr data or a pointer to the user-provided
 *  buffer if unable to obtain a direct pointer
 */
extern void *bpf_dynptr_slice_rdwr(const struct bpf_dynptr *ptr, __u32 offset,
			      void *buffer, __u32 buffer__szk) __ksym;

extern int bpf_dynptr_adjust(const struct bpf_dynptr *ptr, __u32 start, __u32 end) __ksym;
extern bool bpf_dynptr_is_null(const struct bpf_dynptr *ptr) __ksym;
extern bool bpf_dynptr_is_rdonly(const struct bpf_dynptr *ptr) __ksym;
extern __u32 bpf_dynptr_size(const struct bpf_dynptr *ptr) __ksym;
extern int bpf_dynptr_clone(const struct bpf_dynptr *ptr, struct bpf_dynptr *clone__init) __ksym;

/* Description
 *  Modify the address of a AF_UNIX sockaddr.
 * Returns__bpf_kfunc
 *  -EINVAL if the address size is too big or, 0 if the sockaddr was successfully modified.
 */
extern int bpf_sock_addr_set_sun_path(struct bpf_sock_addr_kern *sa_kern,
				      const __u8 *sun_path, __u32 sun_path__sz) __ksym;

void *bpf_cast_to_kern_ctx(void *) __ksym;

void *bpf_rdonly_cast(void *obj, __u32 btf_id) __ksym;

#endif
