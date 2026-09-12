/*-------------------------------------------------------------------------
 *
 * wvec.c
 *		The wvec vector type: I/O, typmod, casts, and distance functions.
 *
 * Task V1 in doc/PHASES.md.  This is the SQL-visible surface of the vector
 * channel; the codec is in src/vector/quantize.c and the index-side shuttle in
 * src/vector/vector.c.
 *
 * wvec is a distinct type from pgvector's `vector` deliberately.  Defining a
 * second type named `vector` would make pg_weave and pgvector mutually exclusive
 * in one database, which forecloses incremental migration -- the only kind anyone
 * actually performs.  See doc/MIGRATION.md.  The struct layout mirrors
 * pgvector's, and the operator and strategy assignment matches it exactly, so a
 * query written against pgvector keeps working after a type swap.
 *
 * Semantics deliberately copied from pgvector, because divergence here would be
 * a migration trap rather than an improvement:
 *
 *		<->		L2 distance					sqrt(sum (a-b)^2)
 *		<#>		NEGATIVE inner product		-sum(a*b)
 *		<=>		cosine distance				1 - (a.b)/(|a||b|)
 *		<+>		L1 distance					sum |a-b|
 *
 * <#> is negated so that ascending order gives the largest inner product first,
 * which is what an index ORDER BY needs.  Anyone who "fixes" the sign will
 * silently invert every MIPS query.
 *
 * Accumulation is in double even though storage is float4.  Summing 1536 float4
 * products in float4 loses several bits on a realistic embedding, and the result
 * feeds a ranking comparison where those bits decide order.  The cost is
 * nothing; the alternative is nondeterministic-looking ranking churn.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/wvec.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "weave/vector.h"

/* ---------------------------------------------------------------------------
 * Construction and validation
 * ------------------------------------------------------------------------- */

/*
 * Allocate a wvec of `dim` elements.  Callers fill x[] afterwards.
 */
static WVec *
wvec_alloc(int dim)
{
	WVec	   *v;
	Size		size = WVEC_SIZE(dim);

	v = (WVec *) palloc0(size);
	SET_VARSIZE(v, size);
	v->dim = (int16) dim;
	v->unused = 0;
	return v;
}

static void
wvec_check_dim(int dim)
{
	if (dim < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("wvec must have at least 1 dimension")));
	if (dim > WVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("wvec cannot have more than %d dimensions",
						WVEC_MAX_DIM)));
}

/*
 * Reject NaN and infinity at the boundary.
 *
 * Not pedantry: a single NaN makes every distance involving that row NaN, which
 * sorts unpredictably and quietly corrupts a top-k result set rather than
 * failing.  Catching it on input is the only place it is cheap.
 */
static void
wvec_check_element(float4 f, int index)
{
	if (isnan(f))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("NaN not allowed in wvec"),
				 errdetail("Element %d is NaN.", index + 1)));
	if (isinf(f))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("infinite value not allowed in wvec"),
				 errdetail("Element %d is infinite.", index + 1)));
}

/*
 * Both operands of a distance function must agree on dimension.  Reporting the
 * two dimensions is worth the extra format argument: "different wvec
 * dimensions" alone sends people looking in the wrong place.
 */
static void
wvec_check_match(const WVec *a, const WVec *b)
{
	if (a->dim != b->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different wvec dimensions %d and %d",
						a->dim, b->dim)));
}

/* ---------------------------------------------------------------------------
 * Text I/O
 *
 * Format is pgvector's: [1,2,3].  Whitespace tolerated around the brackets and
 * after commas.
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(wvec_in);

Datum
wvec_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int32		typmod = PG_GETARG_INT32(2);
	char	   *p = str;
	float4	   *scratch;
	int			dim = 0;
	int			cap = 16;
	WVec	   *result;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '[')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type wvec: \"%s\"", str),
				 errdetail("A wvec must start with \"[\".")));
	p++;

	scratch = (float4 *) palloc(sizeof(float4) * cap);

	while (*p != ']')
	{
		char	   *endp;
		double		d;

		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type wvec: \"%s\"", str),
					 errdetail("Unexpected end of input; expected \"]\".")));

		errno = 0;
		d = strtod(p, &endp);
		if (endp == p)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type wvec: \"%s\"", str),
					 errdetail("Could not parse a number at \"%s\".", p)));
		p = endp;

		if (dim == cap)
		{
			cap *= 2;
			if (cap > WVEC_MAX_DIM + 1)
				cap = WVEC_MAX_DIM + 1;
			scratch = (float4 *) repalloc(scratch, sizeof(float4) * cap);
		}
		if (dim >= WVEC_MAX_DIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("wvec cannot have more than %d dimensions",
							WVEC_MAX_DIM)));

		scratch[dim] = (float4) d;
		wvec_check_element(scratch[dim], dim);
		dim++;

		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == ',')
		{
			const char *q;

			p++;
			/*
			 * A comma must be followed by another element.  Without this check
			 * "[1,2,]" parses as [1,2] and is silently accepted -- the loop
			 * condition sees ']' and exits happily.  pgvector rejects it, so
			 * accepting it would be a compatibility divergence, and more to the
			 * point a trailing comma usually means a generator emitted a
			 * truncated vector and the caller wants to know.
			 */
			q = p;
			while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
				q++;
			if (*q == ']' || *q == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type wvec: \"%s\"", str),
						 errdetail("Expected a number after \",\".")));
		}
		else if (*p != ']')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type wvec: \"%s\"", str),
					 errdetail("Expected \",\" or \"]\" at \"%s\".", p)));
	}
	p++;						/* past ']' */

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type wvec: \"%s\"", str),
				 errdetail("Junk after closing \"]\".")));

	wvec_check_dim(dim);

	/* Enforce a column typmod here rather than in a separate cast: an
	 * out-of-spec literal assigned to a wvec(768) column must fail at insert,
	 * not silently produce a row the index cannot hold. */
	if (typmod > 0 && dim != typmod)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));

	result = wvec_alloc(dim);
	memcpy(result->x, scratch, sizeof(float4) * dim);
	pfree(scratch);

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(wvec_out);

Datum
wvec_out(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	for (i = 0; i < v->dim; i++)
	{
		char	   *s;

		if (i > 0)
			appendStringInfoChar(&buf, ',');
		/* float4out gives shortest-round-trip output, so out(in(x)) == x. */
		s = DatumGetCString(DirectFunctionCall1(float4out,
											   Float4GetDatum(v->x[i])));
		appendStringInfoString(&buf, s);
		pfree(s);
	}
	appendStringInfoChar(&buf, ']');

	PG_RETURN_CSTRING(buf.data);
}

/* ---------------------------------------------------------------------------
 * Binary I/O.  Layout matches pgvector's: int16 dim, int16 unused, dim float4.
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(wvec_recv);

Datum
wvec_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		typmod = PG_GETARG_INT32(2);
	WVec	   *result;
	int16		dim;
	int16		unused;
	int			i;

	dim = pq_getmsgint(buf, sizeof(int16));
	unused = pq_getmsgint(buf, sizeof(int16));

	wvec_check_dim(dim);
	if (unused != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("wvec reserved field must be zero")));
	if (typmod > 0 && dim != typmod)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));

	result = wvec_alloc(dim);
	for (i = 0; i < dim; i++)
	{
		result->x[i] = pq_getmsgfloat4(buf);
		wvec_check_element(result->x[i], i);
	}

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(wvec_send);

Datum
wvec_send(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	StringInfoData buf;
	int			i;

	pq_begintypsend(&buf);
	pq_sendint(&buf, v->dim, sizeof(int16));
	pq_sendint(&buf, v->unused, sizeof(int16));
	for (i = 0; i < v->dim; i++)
		pq_sendfloat4(&buf, v->x[i]);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* ---------------------------------------------------------------------------
 * Typmod: wvec(768)
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(wvec_typmod_in);

Datum
wvec_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier for wvec"),
				 errhint("Use wvec(dimensions), for example wvec(768).")));
	if (tl[0] < 1 || tl[0] > WVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("wvec dimensions must be between 1 and %d",
						WVEC_MAX_DIM)));

	PG_RETURN_INT32(tl[0]);
}

/*
 * Applied on an explicit cast to wvec(n).  A no-op except for enforcement:
 * silently truncating or padding would produce a vector whose distances are
 * meaningless, which is worse than an error.
 */
PG_FUNCTION_INFO_V1(wvec_enforce_typmod);

Datum
wvec_enforce_typmod(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	int32		typmod = PG_GETARG_INT32(1);

	if (typmod > 0 && v->dim != typmod)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, v->dim)));

	PG_RETURN_POINTER(v);
}

/* ---------------------------------------------------------------------------
 * Casts
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(wvec_from_float4_array);

Datum
wvec_from_float4_array(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	WVec	   *result;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int			i;

	if (ARR_NDIM(arr) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("array must be one-dimensional")));

	deconstruct_array(arr, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT,
					  &elems, &nulls, &n);

	for (i = 0; i < n; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("array must not contain NULLs")));
	}

	wvec_check_dim(n);
	if (typmod > 0 && n != typmod)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, n)));

	result = wvec_alloc(n);
	for (i = 0; i < n; i++)
	{
		result->x[i] = DatumGetFloat4(elems[i]);
		wvec_check_element(result->x[i], i);
	}

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(wvec_to_float4_array);

Datum
wvec_to_float4_array(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	Datum	   *d = (Datum *) palloc(sizeof(Datum) * v->dim);
	int			i;

	for (i = 0; i < v->dim; i++)
		d[i] = Float4GetDatum(v->x[i]);

	PG_RETURN_ARRAYTYPE_P(construct_array(d, v->dim, FLOAT4OID,
										  sizeof(float4), true, TYPALIGN_INT));
}

/* ---------------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(wvec_dims);

Datum
wvec_dims(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);

	PG_RETURN_INT32((int32) v->dim);
}

PG_FUNCTION_INFO_V1(wvec_norm);

Datum
wvec_norm(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	double		s = 0.0;
	int			i;

	for (i = 0; i < v->dim; i++)
		s += (double) v->x[i] * (double) v->x[i];

	PG_RETURN_FLOAT8(sqrt(s));
}

/*
 * Unit-normalize.  Returns NULL for a zero vector rather than a vector of NaNs:
 * a zero vector has no direction, and propagating NaN would corrupt every
 * subsequent distance silently.  This is the same decision weave_encode() makes
 * when it refuses to encode a zero vector.
 */
PG_FUNCTION_INFO_V1(wvec_l2_normalize);

Datum
wvec_l2_normalize(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	WVec	   *r;
	double		s = 0.0;
	int			i;

	for (i = 0; i < v->dim; i++)
		s += (double) v->x[i] * (double) v->x[i];
	s = sqrt(s);

	if (s <= 0.0)
		PG_RETURN_NULL();

	r = wvec_alloc(v->dim);
	for (i = 0; i < v->dim; i++)
		r->x[i] = (float4) ((double) v->x[i] / s);

	PG_RETURN_POINTER(r);
}

/* ---------------------------------------------------------------------------
 * Distance functions
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(wvec_l2_distance);

Datum
wvec_l2_distance(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	double		s = 0.0;
	int			i;

	wvec_check_match(a, b);
	for (i = 0; i < a->dim; i++)
	{
		double		d = (double) a->x[i] - (double) b->x[i];

		s += d * d;
	}

	PG_RETURN_FLOAT8(sqrt(s));
}

/*
 * Squared L2.  Order-equivalent to L2 and cheaper, so the index uses it
 * internally; exposed because a caller doing their own reranking wants the same
 * function the index used.
 */
PG_FUNCTION_INFO_V1(wvec_l2_squared_distance);

Datum
wvec_l2_squared_distance(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	double		s = 0.0;
	int			i;

	wvec_check_match(a, b);
	for (i = 0; i < a->dim; i++)
	{
		double		d = (double) a->x[i] - (double) b->x[i];

		s += d * d;
	}

	PG_RETURN_FLOAT8(s);
}

PG_FUNCTION_INFO_V1(wvec_inner_product);

Datum
wvec_inner_product(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	double		s = 0.0;
	int			i;

	wvec_check_match(a, b);
	for (i = 0; i < a->dim; i++)
		s += (double) a->x[i] * (double) b->x[i];

	PG_RETURN_FLOAT8(s);
}

/*
 * The <#> operator: NEGATIVE inner product, so ASC order returns the largest
 * inner product first.  pgvector's convention.  Inverting this sign silently
 * reverses every MIPS query, so it is called out in the file header too.
 */
PG_FUNCTION_INFO_V1(wvec_negative_inner_product);

Datum
wvec_negative_inner_product(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	double		s = 0.0;
	int			i;

	wvec_check_match(a, b);
	for (i = 0; i < a->dim; i++)
		s += (double) a->x[i] * (double) b->x[i];

	PG_RETURN_FLOAT8(-s);
}

/*
 * Cosine distance = 1 - cos(theta).
 *
 * A zero-norm operand has no direction, so cosine is undefined.  pgvector
 * returns NaN there; we do the same for compatibility, which is the whole reason
 * for this type's existence -- diverging on an edge case is a migration trap even
 * when our behaviour would be nicer.  Documented in doc/MIGRATION.md.
 */
PG_FUNCTION_INFO_V1(wvec_cosine_distance);

Datum
wvec_cosine_distance(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	double		dot = 0.0;
	double		na = 0.0;
	double		nb = 0.0;
	double		denom;
	double		cosine;
	int			i;

	wvec_check_match(a, b);
	for (i = 0; i < a->dim; i++)
	{
		double		x = a->x[i];
		double		y = b->x[i];

		dot += x * y;
		na += x * x;
		nb += y * y;
	}

	denom = sqrt(na) * sqrt(nb);
	if (denom == 0.0)
		PG_RETURN_FLOAT8(get_float8_nan());

	cosine = dot / denom;

	/* Clamp: accumulated rounding can push an identical pair to 1+1e-16, and a
	 * negative distance is more confusing than a zero one. */
	if (cosine > 1.0)
		cosine = 1.0;
	else if (cosine < -1.0)
		cosine = -1.0;

	PG_RETURN_FLOAT8(1.0 - cosine);
}

PG_FUNCTION_INFO_V1(wvec_l1_distance);

Datum
wvec_l1_distance(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	double		s = 0.0;
	int			i;

	wvec_check_match(a, b);
	for (i = 0; i < a->dim; i++)
		s += fabs((double) a->x[i] - (double) b->x[i]);

	PG_RETURN_FLOAT8(s);
}

/* ---------------------------------------------------------------------------
 * Arithmetic, and equality/ordering
 *
 * Comparison operators exist so wvec can be used with DISTINCT, GROUP BY, and a
 * btree index -- not because lexicographic order on a vector means anything.
 * Ordering is by dimension first, then element-wise, which is arbitrary but
 * total and stable.
 * ------------------------------------------------------------------------- */

static int
wvec_cmp_internal(const WVec *a, const WVec *b)
{
	int			n = Min(a->dim, b->dim);
	int			i;

	for (i = 0; i < n; i++)
	{
		if (a->x[i] < b->x[i])
			return -1;
		if (a->x[i] > b->x[i])
			return 1;
	}
	if (a->dim < b->dim)
		return -1;
	if (a->dim > b->dim)
		return 1;
	return 0;
}

PG_FUNCTION_INFO_V1(wvec_cmp);
PG_FUNCTION_INFO_V1(wvec_eq);
PG_FUNCTION_INFO_V1(wvec_ne);
PG_FUNCTION_INFO_V1(wvec_lt);
PG_FUNCTION_INFO_V1(wvec_le);
PG_FUNCTION_INFO_V1(wvec_gt);
PG_FUNCTION_INFO_V1(wvec_ge);

Datum
wvec_cmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)));
}

Datum
wvec_eq(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)) == 0);
}

Datum
wvec_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)) != 0);
}

Datum
wvec_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)) < 0);
}

Datum
wvec_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)) <= 0);
}

Datum
wvec_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)) > 0);
}

Datum
wvec_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(wvec_cmp_internal(PG_GETARG_WVEC(0), PG_GETARG_WVEC(1)) >= 0);
}

PG_FUNCTION_INFO_V1(wvec_add);
PG_FUNCTION_INFO_V1(wvec_sub);
PG_FUNCTION_INFO_V1(wvec_mul);

Datum
wvec_add(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	WVec	   *r;
	int			i;

	wvec_check_match(a, b);
	r = wvec_alloc(a->dim);
	for (i = 0; i < a->dim; i++)
	{
		r->x[i] = a->x[i] + b->x[i];
		wvec_check_element(r->x[i], i);
	}
	PG_RETURN_POINTER(r);
}

Datum
wvec_sub(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	WVec	   *r;
	int			i;

	wvec_check_match(a, b);
	r = wvec_alloc(a->dim);
	for (i = 0; i < a->dim; i++)
	{
		r->x[i] = a->x[i] - b->x[i];
		wvec_check_element(r->x[i], i);
	}
	PG_RETURN_POINTER(r);
}

Datum
wvec_mul(PG_FUNCTION_ARGS)
{
	WVec	   *a = PG_GETARG_WVEC(0);
	WVec	   *b = PG_GETARG_WVEC(1);
	WVec	   *r;
	int			i;

	wvec_check_match(a, b);
	r = wvec_alloc(a->dim);
	for (i = 0; i < a->dim; i++)
	{
		r->x[i] = a->x[i] * b->x[i];
		wvec_check_element(r->x[i], i);
	}
	PG_RETURN_POINTER(r);
}

/* ---------------------------------------------------------------------------
 * Codec introspection
 *
 * Exposes the quantizer so its behaviour is observable from SQL before the index
 * side exists.  This is how the codec gets exercised against real embeddings
 * without waiting for tasks V7-V9, and it is what weave_check() will later use to
 * report calibration staleness.
 * ------------------------------------------------------------------------- */

static void *
wvec_palloc(size_t sz)
{
	return palloc(sz);
}

static void
wvec_pfree(void *p)
{
	pfree(p);
}

/*
 * Round-trip a vector through the quantizer and return the reconstruction.
 *
 * The point is to make quantization error inspectable:
 *
 *		SELECT wvec_l2_distance(v, weave_quantize_roundtrip(v, 4)) / wvec_norm(v)
 *		  FROM embeddings;
 *
 * gives the per-row relative error distribution for a real corpus, which is the
 * number that actually predicts recall.  Cheaper to answer this way than by
 * building an index.
 */
PG_FUNCTION_INFO_V1(weave_quantize_roundtrip);

Datum
weave_quantize_roundtrip(PG_FUNCTION_ARGS)
{
	WVec	   *v = PG_GETARG_WVEC(0);
	int32		bits = PG_GETARG_INT32(1);
	WeaveQuantizer q;
	WVec	   *r;
	uint8	   *code;
	float		norm;
	float		scale;

	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bits must be between %d and %d",
						WEAVE_BITS_MIN, WEAVE_BITS_MAX),
				 errhint("Wider codes have lower quantization error at "
						 "proportionally larger size; %d is the smallest and %d the "
						 "widest supported.",
						 WEAVE_BITS_MIN, WEAVE_BITS_MAX)));

	if (weave_quantizer_init(&q, v->dim, bits, NULL,
							 wvec_palloc, wvec_pfree) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot build a quantizer for %d dimensions at %d bits",
						v->dim, bits)));

	code = (uint8 *) palloc(q.codebytes);
	if (weave_encode(&q, v->x, code, &norm, &scale) != 0)
	{
		weave_quantizer_free(&q, wvec_pfree);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("cannot quantize a zero wvec"),
				 errdetail("A zero vector has no direction, so its "
						   "renormalization scale is undefined.")));
	}

	r = wvec_alloc(v->dim);
	weave_decode(&q, code, scale, r->x);
	weave_quantizer_free(&q, wvec_pfree);
	pfree(code);

	PG_RETURN_POINTER(r);
}

/*
 * Bytes one vector occupies at a given code width, excluding the per-block
 * overhead.  Lets someone size an index before building one.
 */
PG_FUNCTION_INFO_V1(weave_quantize_size);

Datum
weave_quantize_size(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	int32		bits = PG_GETARG_INT32(1);

	if (dim < 1 || dim > WVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions must be between 1 and %d", WVEC_MAX_DIM)));
	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bits must be between %d and %d",
						WEAVE_BITS_MIN, WEAVE_BITS_MAX)));

	/* codes, plus the per-vector scale and norm from WeaveVecLane */
	PG_RETURN_INT32((dim * bits + 7) / 8 + (int32) sizeof(WeaveVecLane));
}
