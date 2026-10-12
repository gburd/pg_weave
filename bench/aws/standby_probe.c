/* EXPERIMENT ONLY: ordinary core relation locks and page-header witnesses.
 * No raw WAL, no mutation, no arbitrary page parser after stale-root capture.
 * Build as a separate .so; never part of pg_weave's installed SQL API.
 */
#include "postgres.h"
#include "fmgr.h"
#include "access/genam.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "weave/am.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(probe_root);
PG_FUNCTION_INFO_V1(probe_page);
PG_FUNCTION_INFO_V1(probe_fence);

Datum
probe_root(PG_FUNCTION_ARGS)
{
	Relation r = index_open(PG_GETARG_OID(0), AccessShareLock);
	Buffer b = ReadBuffer(r, 0);
	BlockNumber root;
	LockBuffer(b, BUFFER_LOCK_SHARE);
	root = WeavePageGetMeta(BufferGetPage(b))->segs[0].dictstart;
	UnlockReleaseBuffer(b);
	/* Retain ASL until transaction end, including while client is idle. */
	index_close(r, NoLock);
	PG_RETURN_INT64(root);
}

Datum
probe_page(PG_FUNCTION_ARGS)
{
	Relation r = index_open(PG_GETARG_OID(0), AccessShareLock);
	Buffer b = ReadBuffer(r, (BlockNumber) PG_GETARG_INT64(1));
	Page p;
	char *s;
	LockBuffer(b, BUFFER_LOCK_SHARE);
	p = BufferGetPage(b);
	s = psprintf("%u:%u:%X/%X", (unsigned) WeavePageGetKind(p),
				 (unsigned) (WeavePageGetOpaque(p)->flags & WEAVE_FREED),
				 (unsigned) (PageGetLSN(p) >> 32), (unsigned) PageGetLSN(p));
	UnlockReleaseBuffer(b);
	index_close(r, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(s));
}

Datum
probe_fence(PG_FUNCTION_ARGS)
{
	Relation r = index_open(PG_GETARG_OID(0), NoLock);
	bool ok = ConditionalLockRelation(r, AccessExclusiveLock);
	/* Deliberate early unlock: replay retains the lock until xact completion. */
	if (ok)
		UnlockRelation(r, AccessExclusiveLock);
	index_close(r, NoLock);
	PG_RETURN_BOOL(ok);
}

/* Primary-only contention witness; ordinary relation lock, retained to xact end. */
PG_FUNCTION_INFO_V1(probe_hold);
Datum
probe_hold(PG_FUNCTION_ARGS)
{
	int mode = PG_GETARG_INT32(1);
	Relation r;

	if (mode != AccessShareLock && mode != RowExclusiveLock)
		elog(ERROR, "probe_hold supports ASL or RowExclusiveLock only");
	r = index_open(PG_GETARG_OID(0), mode);
	index_close(r, NoLock);
	PG_RETURN_VOID();
}
