// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright 2016-2018 NXP
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/packing.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/bitrev.h>

/**
 * calculate_box_addr - Determine physical location of byte in buffer
 * @box: Index of byte within buffer seen as a logical big-endian big number
 * @len: Size of buffer in bytes
 * @quirks: mask of QUIRK_LSW32_IS_FIRST and QUIRK_LITTLE_ENDIAN
 *
 * Function interprets the buffer as a @len byte sized big number, and returns
 * the physical offset of the @box logical octet within it. Internally, it
 * treats the big number as groups of 4 bytes. If @len is not a multiple of 4,
 * the last group may be shorter.
 *
 * @QUIRK_LSW32_IS_FIRST gives the ordering of groups of 4 octets relative to
 * each other. If set, the most significant group of 4 octets is last in the
 * buffer (and may be truncated if @len is not a multiple of 4).
 *
 * @QUIRK_LITTLE_ENDIAN gives the ordering of bytes within each group of 4.
 * If set, the most significant byte is last in the group. If @len takes the
 * form of 4k+3, the last group will only be able to represent 24 bits, and its
 * most significant octet is byte 2.
 *
 * Return: the physical offset into the buffer corresponding to the logical box.
 */
static size_t calculate_box_addr(size_t box, size_t len, u8 quirks)
{
	size_t offset_of_group, offset_in_group, this_group = box / 4;
	size_t group_size;

	if (quirks & QUIRK_LSW32_IS_FIRST)
		offset_of_group = this_group * 4;
	else
		offset_of_group = len - ((this_group + 1) * 4);

	group_size = min(4, len - offset_of_group);

	if (quirks & QUIRK_LITTLE_ENDIAN)
		offset_in_group = box - this_group * 4;
	else
		offset_in_group = group_size - (box - this_group * 4) - 1;

	return offset_of_group + offset_in_group;
}

/**
 * pack - Pack u64 number into bitfield of buffer.
 *
 * @pbuf: Pointer to a buffer holding the packed value.
 * @uval: CPU-readable unpacked value to pack.
 * @startbit: The index (in logical notation, compensated for quirks) where
 *	      the packed value starts within pbuf. Must be larger than, or
 *	      equal to, endbit.
 * @endbit: The index (in logical notation, compensated for quirks) where
 *	    the packed value ends within pbuf. Must be smaller than, or equal
 *	    to, startbit.
 * @pbuflen: The length in bytes of the packed buffer pointed to by @pbuf.
 * @quirks: A bit mask of QUIRK_LITTLE_ENDIAN, QUIRK_LSW32_IS_FIRST and
 *	    QUIRK_MSB_ON_THE_RIGHT.
 *
 * Return: 0 on success, EINVAL or ERANGE if called incorrectly. Assuming
 *	   correct usage, return code may be discarded. The @pbuf memory will
 *	   be modified on success.
 */
int pack(void *pbuf, u64 uval, size_t startbit, size_t endbit, size_t pbuflen,
	 u8 quirks)
{
	/* Logical byte indices corresponding to the
	 * start and end of the field.
	 */
	int plogical_first_u8, plogical_last_u8, box;
	/* width of the field to access in the pbuf */
	u64 value_width;

	/* startbit is expected to be larger than endbit, and both are
	 * expected to be within the logically addressable range of the buffer.
	 */
	if (unlikely(startbit < endbit || startbit >= BITS_PER_BYTE * pbuflen))
		/* Invalid function call */
		return -EINVAL;

	value_width = startbit - endbit + 1;
	if (unlikely(value_width > 64))
		return -ERANGE;

	/* Check if "uval" fits in "value_width" bits.
	 * If value_width is 64, the check will fail, but any
	 * 64-bit uval will surely fit.
	 */
	if (unlikely(value_width < 64 && uval >= (1ull << value_width)))
		/* Cannot store "uval" inside "value_width" bits.
		 * Truncating "uval" is most certainly not desirable,
		 * so simply erroring out is appropriate.
		 */
		return -ERANGE;

	/* Iterate through an idealistic view of the pbuf as an u64 with
	 * no quirks, u8 by u8 (aligned at u8 boundaries), from high to low
	 * logical bit significance. "box" denotes the current logical u8.
	 */
	plogical_first_u8 = startbit / BITS_PER_BYTE;
	plogical_last_u8  = endbit / BITS_PER_BYTE;

	for (box = plogical_first_u8; box >= plogical_last_u8; box--) {
		/* Bit indices into the currently accessed 8-bit box */
		size_t box_start_bit, box_end_bit, box_addr;
		u8  box_mask;
		/* Corresponding bits from the unpacked u64 parameter */
		size_t proj_start_bit, proj_end_bit;
		u64 proj_mask;
		u64 pval;

		/* This u8 may need to be accessed in its entirety
		 * (from bit 7 to bit 0), or not, depending on the
		 * input arguments startbit and endbit.
		 */
		if (box == plogical_first_u8)
			box_start_bit = startbit % BITS_PER_BYTE;
		else
			box_start_bit = 7;
		if (box == plogical_last_u8)
			box_end_bit = endbit % BITS_PER_BYTE;
		else
			box_end_bit = 0;

		/* We have determined the box bit start and end.
		 * Now we calculate where this (masked) u8 box would fit
		 * in the unpacked (CPU-readable) u64 - the u8 box's
		 * projection onto the unpacked u64. Though the
		 * box is u8, the projection is u64 because it may fall
		 * anywhere within the unpacked u64.
		 */
		proj_start_bit = ((box * BITS_PER_BYTE) + box_start_bit) - endbit;
		proj_end_bit = ((box * BITS_PER_BYTE) + box_end_bit) - endbit;
		proj_mask = GENMASK_ULL(proj_start_bit, proj_end_bit);
		box_mask = GENMASK(box_start_bit, box_end_bit);

		/* Determine the offset of the u8 box inside the pbuf,
		 * adjusted for quirks. The adjusted box_addr will be used for
		 * effective addressing inside the pbuf (so it's not
		 * logical any longer).
		 */
		box_addr = calculate_box_addr(box, pbuflen, quirks);

		/* Write to pbuf, read from uval */
		pval = uval & proj_mask;
		pval >>= proj_end_bit;
		pval <<= box_end_bit;

		if (quirks & QUIRK_MSB_ON_THE_RIGHT) {
			pval = bitrev8(pval);
			box_mask = bitrev8(box_mask);
		}

		((u8 *)pbuf)[box_addr] &= ~box_mask;
		((u8 *)pbuf)[box_addr] |= pval;
	}
	return 0;
}
EXPORT_SYMBOL(pack);

/**
 * unpack - Unpack u64 number from packed buffer.
 *
 * @pbuf: Pointer to a buffer holding the packed value.
 * @uval: Pointer to an u64 holding the unpacked value.
 * @startbit: The index (in logical notation, compensated for quirks) where
 *	      the packed value starts within pbuf. Must be larger than, or
 *	      equal to, endbit.
 * @endbit: The index (in logical notation, compensated for quirks) where
 *	    the packed value ends within pbuf. Must be smaller than, or equal
 *	    to, startbit.
 * @pbuflen: The length in bytes of the packed buffer pointed to by @pbuf.
 * @quirks: A bit mask of QUIRK_LITTLE_ENDIAN, QUIRK_LSW32_IS_FIRST and
 *	    QUIRK_MSB_ON_THE_RIGHT.
 *
 * Return: 0 on success, EINVAL or ERANGE if called incorrectly. Assuming
 *	   correct usage, return code may be discarded. The @uval will be
 *	   modified on success.
 */
int unpack(const void *pbuf, u64 *uval, size_t startbit, size_t endbit,
	   size_t pbuflen, u8 quirks)
{
	/* Logical byte indices corresponding to the
	 * start and end of the field.
	 */
	int plogical_first_u8, plogical_last_u8, box;
	/* width of the field to access in the pbuf */
	u64 value_width;

	/* startbit is expected to be larger than endbit, and both are
	 * expected to be within the logically addressable range of the buffer.
	 */
	if (unlikely(startbit < endbit || startbit >= BITS_PER_BYTE * pbuflen))
		/* Invalid function call */
		return -EINVAL;

	value_width = startbit - endbit + 1;
	if (unlikely(value_width > 64))
		return -ERANGE;

	/* Initialize parameter */
	*uval = 0;

	/* Iterate through an idealistic view of the pbuf as an u64 with
	 * no quirks, u8 by u8 (aligned at u8 boundaries), from high to low
	 * logical bit significance. "box" denotes the current logical u8.
	 */
	plogical_first_u8 = startbit / BITS_PER_BYTE;
	plogical_last_u8  = endbit / BITS_PER_BYTE;

	for (box = plogical_first_u8; box >= plogical_last_u8; box--) {
		/* Bit indices into the currently accessed 8-bit box */
		size_t box_start_bit, box_end_bit, box_addr;
		u8  box_mask;
		/* Corresponding bits from the unpacked u64 parameter */
		size_t proj_start_bit, proj_end_bit;
		u64 proj_mask;
		u64 pval;

		/* This u8 may need to be accessed in its entirety
		 * (from bit 7 to bit 0), or not, depending on the
		 * input arguments startbit and endbit.
		 */
		if (box == plogical_first_u8)
			box_start_bit = startbit % BITS_PER_BYTE;
		else
			box_start_bit = 7;
		if (box == plogical_last_u8)
			box_end_bit = endbit % BITS_PER_BYTE;
		else
			box_end_bit = 0;

		/* We have determined the box bit start and end.
		 * Now we calculate where this (masked) u8 box would fit
		 * in the unpacked (CPU-readable) u64 - the u8 box's
		 * projection onto the unpacked u64. Though the
		 * box is u8, the projection is u64 because it may fall
		 * anywhere within the unpacked u64.
		 */
		proj_start_bit = ((box * BITS_PER_BYTE) + box_start_bit) - endbit;
		proj_end_bit = ((box * BITS_PER_BYTE) + box_end_bit) - endbit;
		proj_mask = GENMASK_ULL(proj_start_bit, proj_end_bit);
		box_mask = GENMASK(box_start_bit, box_end_bit);

		/* Determine the offset of the u8 box inside the pbuf,
		 * adjusted for quirks. The adjusted box_addr will be used for
		 * effective addressing inside the pbuf (so it's not
		 * logical any longer).
		 */
		box_addr = calculate_box_addr(box, pbuflen, quirks);

		/* Read from pbuf, write to uval */
		pval = ((u8 *)pbuf)[box_addr];

		if (quirks & QUIRK_MSB_ON_THE_RIGHT)
			pval = bitrev8(pval);

		pval &= box_mask;

		pval >>= box_end_bit;
		pval <<= proj_end_bit;
		*uval &= ~proj_mask;
		*uval |= pval;
	}
	return 0;
}
EXPORT_SYMBOL(unpack);

/**
 * packing - Convert numbers (currently u64) between a packed and an unpacked
 *	     format. Unpacked means laid out in memory in the CPU's native
 *	     understanding of integers, while packed means anything else that
 *	     requires translation.
 *
 * @pbuf: Pointer to a buffer holding the packed value.
 * @uval: Pointer to an u64 holding the unpacked value.
 * @startbit: The index (in logical notation, compensated for quirks) where
 *	      the packed value starts within pbuf. Must be larger than, or
 *	      equal to, endbit.
 * @endbit: The index (in logical notation, compensated for quirks) where
 *	    the packed value ends within pbuf. Must be smaller than, or equal
 *	    to, startbit.
 * @pbuflen: The length in bytes of the packed buffer pointed to by @pbuf.
 * @op: If PACK, then uval will be treated as const pointer and copied (packed)
 *	into pbuf, between startbit and endbit.
 *	If UNPACK, then pbuf will be treated as const pointer and the logical
 *	value between startbit and endbit will be copied (unpacked) to uval.
 * @quirks: A bit mask of QUIRK_LITTLE_ENDIAN, QUIRK_LSW32_IS_FIRST and
 *	    QUIRK_MSB_ON_THE_RIGHT.
 *
 * Note: this is deprecated, prefer to use pack() or unpack() in new code.
 *
 * Return: 0 on success, EINVAL or ERANGE if called incorrectly. Assuming
 *	   correct usage, return code may be discarded.
 *	   If op is PACK, pbuf is modified.
 *	   If op is UNPACK, uval is modified.
 */
int packing(void *pbuf, u64 *uval, int startbit, int endbit, size_t pbuflen,
	    enum packing_op op, u8 quirks)
{
	if (op == PACK)
		return pack(pbuf, *uval, startbit, endbit, pbuflen, quirks);

	return unpack(pbuf, uval, startbit, endbit, pbuflen, quirks);
}
EXPORT_SYMBOL(packing);

MODULE_DESCRIPTION("Generic bitfield packing and unpacking");
