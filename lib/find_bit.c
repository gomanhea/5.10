// SPDX-License-Identifier: GPL-2.0-or-later
/* bit search implementation
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * Copyright (C) 2008 IBM Corporation
 * 'find_last_bit' is written by Rusty Russell <rusty@rustcorp.com.au>
 * (Inspired by David Howell's find_next_bit implementation)
 *
 * Rewritten by Yury Norov <yury.norov@gmail.com> to decrease
 * size and improve performance, 2015.
 */

#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/swab.h>

#if !defined(find_next_bit) || !defined(find_next_zero_bit) ||			\
	!defined(find_next_bit_le) || !defined(find_next_zero_bit_le) ||	\
	!defined(find_next_and_bit)
/*
 * This is a common helper function for find_next_bit, find_next_zero_bit, and
 * find_next_and_bit. The differences are:
 *  - The "invert" argument, which is XORed with each fetched word before
 *    searching it for one bits.
 *  - The optional "addr2", which is anded with "addr1" if present.
 */

/* IAMROOT, 2021.09.29:
 * - addr1 : 실제 수정되는 memory
 * - addr2 : addr1을 masking할 값이 저장되있는 memory
 * - invert : bit가 set되있는걸 전제로 함수가 작성되어 있는데,
 *   clear bit를 찾을때는 bit 반전을 하여 clear bit를 set bit로 변환후 찾는다.
 *
 * - little endian이면 swap으로 뒤집는게 확인된다.

 * @addr1	bitmap1 주소
 * @addr2	bitmap2 주소 (& 연산으로 bitmap1을 마스킹)
 * @nbits	검색할 비트수
 * @start	요청한 비트를 포함하여 검색
 * @invert	반전시킬 비트들(모두 0인 경우 1을 검색, 모두 1인 경우 0을 검색)
 * @le		little endian 검색 여부
 *
 * return	비트번호(based 0)를 반환, 몿잧은 경우 @size 반환
 *
 * ex) unsigned long addr[2] = {3UL, 3UL}; 
 *
 * bit0          bit63 bit64      bit127
 *    v              v v               v
 *    11000........000 11000........0000
 *    lowest -----(search)-----> highest
 *
 * _find_next_bit(bitmap1, NULL, 128,  0,  0UL, 0) ->  0번 부터 검색 -> 0
 * _find_next_bit(bitmap1, NULL, 128,  1,  0UL, 0) ->  1번 부터 검색 -> 1
 * _find_next_bit(bitmap1, NULL, 128,  2,  0UL, 0) ->  2번 부터 검색 -> 64
 * _find_next_bit(bitmap1, NULL, 128, 65,  0UL, 0) -> 65번 부터 검색 -> 65  
 * _find_next_bit(bitmap1, NULL, 128, 66,  0UL, 0) -> 66번 부터 검색 -> 128 (not found)
 *
 * _find_next_bit(bitmap1, NULL, 128,  0, ~0UL, 0) ->  0번 부터 검색 -> 2
 * _find_next_bit(bitmap1, NULL, 128,  3, ~0UL, 0) ->  1번 부터 검색 -> 3
 * _find_next_bit(bitmap1, NULL, 128, 64, ~0UL, 0) -> 64번 부터 검색 -> 66
 */
unsigned long _find_next_bit(const unsigned long *addr1,
		const unsigned long *addr2, unsigned long nbits,
		unsigned long start, unsigned long invert, unsigned long le)
{
	unsigned long tmp, mask;

	if (unlikely(start >= nbits))
		return nbits;

/* IAMROOT, 2021.09.30:
 * start bit의 long field를 구해온다.
 */
	tmp = addr1[start / BITS_PER_LONG];
	if (addr2)
		tmp &= addr2[start / BITS_PER_LONG];
	tmp ^= invert;

/* IAMROOT, 2021.09.30:
 * start bit이전의 bit들을 전부 clear 시켜버려
 * start bit 이상의 bit들만 남겨 놓는다.
 */
	/* Handle 1st word. */
	mask = BITMAP_FIRST_WORD_MASK(start);
	if (le)
		mask = swab(mask);

	tmp &= mask;

/* IAMROOT, 2021.09.30:
 * BITS_PER_LONG으로 align을 맞춘다. ex) start = 129 -> start = 128
 * 이제 start 변수는 인자로 주어졌던 값이 아니라 long값으로 align된 값이
 * 될것이다.
 */
	start = round_down(start, BITS_PER_LONG);

/* IAMROOT, 2021.09.30:
 * BITMAP_FIRST_WORD_MASK로 masking한 결과값(tmp)에 bit가 남아있다면
 * while문에 진입을 안할것이며 set bit가 없다면 다음 long field로 이동하며
 * 찾을것이다.
 */
	while (!tmp) {
		start += BITS_PER_LONG;
		if (start >= nbits)
			return nbits;

		tmp = addr1[start / BITS_PER_LONG];
		if (addr2)
			tmp &= addr2[start / BITS_PER_LONG];
		tmp ^= invert;
	}

	if (le)
		tmp = swab(tmp);

/* IAMROOT, 2021.09.30:
 * start는 long값으로 align되있으므로 __ffs를 통해 결과값(tmp)에서 bit번호만
 * 찾으면 bit index가 구해질 것이다.
 */
	return min(start + __ffs(tmp), nbits);
}
EXPORT_SYMBOL(_find_next_bit);
#endif

#ifndef find_first_bit
/*
 * Find the first set bit in a memory region.
 */
unsigned long _find_first_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx;

	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx])
			return min(idx * BITS_PER_LONG + __ffs(addr[idx]), size);
	}

	return size;
}
EXPORT_SYMBOL(_find_first_bit);
#endif

#ifndef find_first_zero_bit
/*
 * Find the first cleared bit in a memory region.
 */
unsigned long _find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx;

	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx] != ~0UL)
			return min(idx * BITS_PER_LONG + ffz(addr[idx]), size);
	}

	return size;
}
EXPORT_SYMBOL(_find_first_zero_bit);
#endif

#ifndef find_last_bit
unsigned long _find_last_bit(const unsigned long *addr, unsigned long size)
{
	if (size) {
		unsigned long val = BITMAP_LAST_WORD_MASK(size);
		unsigned long idx = (size-1) / BITS_PER_LONG;

		do {
			val &= addr[idx];
			if (val)
				return idx * BITS_PER_LONG + __fls(val);

			val = ~0ul;
		} while (idx--);
	}
	return size;
}
EXPORT_SYMBOL(_find_last_bit);
#endif

unsigned long find_next_clump8(unsigned long *clump, const unsigned long *addr,
			       unsigned long size, unsigned long offset)
{
	offset = find_next_bit(addr, size, offset);
	if (offset == size)
		return size;

	offset = round_down(offset, 8);
	*clump = bitmap_get_value8(addr, offset);

	return offset;
}
EXPORT_SYMBOL(find_next_clump8);
