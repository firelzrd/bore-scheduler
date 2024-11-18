/* SPDX-License-Identifier: GPL-2.0 OR MIT */

#ifndef _CRYPTO_INTERNAL_BLAKE2S_H
#define _CRYPTO_INTERNAL_BLAKE2S_H

#include <crypto/blake2s.h>

void blake2s_compress_generic(struct blake2s_state *state,const u8 *block,
			      size_t nblocks, const u32 inc);

void blake2s_compress_arch(struct blake2s_state *state,const u8 *block,
			   size_t nblocks, const u32 inc);

static inline void blake2s_set_lastblock(struct blake2s_state *state)
{
	state->f[0] = -1;
}

#endif /* _CRYPTO_INTERNAL_BLAKE2S_H */
