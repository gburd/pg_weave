/*-------------------------------------------------------------------------
 *
 * pack.c
 *		Bit-packing layouts for 32-vector code blocks -- scalar reference.
 *
 * Two layouts exist because the two kernel families want opposite strides:
 *
 *	 WEAVE_PACK_LANE		coordinate-major.  Code (coordinate j, lane s) sits
 *							at bit index (j * 32 + s) * bits.  A byte-LUT kernel
 *							loads 32 lanes' codes for one coordinate in one
 *							vector register and gathers from the query table.
 *
 *	 WEAVE_PACK_VECMAJOR	vector-major.  Code (coordinate j, lane s) sits at
 *							bit index (s * dim + j) * bits.  An int8 dot-product
 *							kernel (NEON SDOT/SMMLA, AVX-512 VNNI) wants one
 *							vector's coordinates contiguous.
 *
 * Which layout an existing segment used is a fact recorded in WeaveVecMeta, not
 * a runtime choice: a reader that guesses wrong does not fail, it returns wrong
 * distances.
 *
 * This is the reference implementation: a bit at a time, correct and slow.  The
 * SIMD kernels read the packed form directly rather than calling these, but they
 * must agree with them, which is what test/hegel/test_pack.c asserts.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/pack.c
 *
 *-------------------------------------------------------------------------
 */
#include "weave/quantize.h"

/*
 * Bit index of coordinate j of lane s, in units of `bits`.
 *
 * On x86 the LANE layout additionally permutes lanes by perm0 so that an AVX2
 * byte shuffle can cross the 128-bit lane boundary in one instruction.  That
 * permutation is a pure relabelling of the 32 slots and is applied by the
 * kernels, not here, so this reference stays architecture-independent and the
 * property test can compare the two.
 */
static inline int
code_index(WeavePackLayout layout, int dim, int slot, int j)
{
	if (layout == WEAVE_PACK_LANE)
		return j * WEAVE_VEC_BLOCK + slot;
	return slot * dim + j;
}

static inline void
put_bits(weave_uint8 *dst, int index, int bits, weave_uint32 value)
{
	size_t		bit = (size_t) index * bits;
	int			k;

	for (k = 0; k < bits; k++)
	{
		size_t		b = bit + k;

		if (value & (1u << k))
			dst[b >> 3] |= (weave_uint8) (1u << (b & 7));
		else
			dst[b >> 3] &= (weave_uint8) ~(1u << (b & 7));
	}
}

static inline weave_uint32
get_bits(const weave_uint8 *src, int index, int bits)
{
	size_t		bit = (size_t) index * bits;
	weave_uint32 v = 0;
	int			k;

	for (k = 0; k < bits; k++)
	{
		size_t		b = bit + k;

		if (src[b >> 3] & (weave_uint8) (1u << (b & 7)))
			v |= (1u << k);
	}
	return v;
}

void
weave_pack_lane(WeavePackLayout layout, int dim, int bits,
				weave_uint8 *block, int slot, const weave_uint8 *code)
{
	int			j;

	for (j = 0; j < dim; j++)
	{
		weave_uint32 v = get_bits(code, j, bits);

		put_bits(block, code_index(layout, dim, slot, j), bits, v);
	}
}

void
weave_unpack_lane(WeavePackLayout layout, int dim, int bits,
				  const weave_uint8 *block, int slot, weave_uint8 *code)
{
	int			j;
	int			nbytes = (dim * bits + 7) / 8;

	for (j = 0; j < nbytes; j++)
		code[j] = 0;
	for (j = 0; j < dim; j++)
	{
		weave_uint32 v = get_bits(block, code_index(layout, dim, slot, j), bits);

		put_bits(code, j, bits, v);
	}
}

/*
 * Zero one lane.  This is how vacuum retires a deleted vector: clear the lane,
 * clear its bit in WeaveVecBlockHdr.livemask, and recompute smax/minnorm over
 * the remaining live lanes.
 *
 * Recomputing smax is not optional.  smax is the input to the block bound, and a
 * bound computed from a scale that is no longer present is still a valid UPPER
 * bound (it can only be too high, never too low), so correctness survives -- but
 * it degrades pruning silently and forever.  Recompute it.
 */
void
weave_pack_zero_lane(WeavePackLayout layout, int dim, int bits,
					 weave_uint8 *block, int slot)
{
	int			j;

	for (j = 0; j < dim; j++)
		put_bits(block, code_index(layout, dim, slot, j), bits, 0);
}

/*
 * Move lane `src` to lane `dst`, coordinate by coordinate, without touching
 * any other lane's bits.  See the declaration in weave/quantize.h for why this
 * is the operation vacuum wants: it costs one lane, not the block.
 *
 * Reads all of `src` before writing any of `dst`... no, it does not need to:
 * `code_index` is injective in (slot, j) for fixed layout, so src's and dst's
 * bit ranges never overlap when dst != src, and a single left-to-right pass
 * reading-then-writing coordinate j at a time is safe.  The dst == src case is
 * a copy onto itself, which is also safe without special-casing, but the
 * early return avoids WEAVE_VEC_BLOCK-many redundant bit twiddles.
 */
void
weave_pack_move_lane(WeavePackLayout layout, int dim, int bits,
					 weave_uint8 *block, int dst, int src)
{
	int			j;

	if (dst == src)
		return;

	for (j = 0; j < dim; j++)
	{
		weave_uint32 v = get_bits(block, code_index(layout, dim, src, j), bits);

		put_bits(block, code_index(layout, dim, dst, j), bits, v);
	}
}
