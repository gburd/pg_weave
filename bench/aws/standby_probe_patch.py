#!/usr/bin/env python3
"""Apply only to the disposable experimental build, never the source checkout."""
from pathlib import Path
p = Path('src/am/am.c')
s = p.read_text()
old = '''weave_new_buffer_internal(Relation index)
{
	Buffer		buffer;
'''
new = old + '''
	/* EXPERIMENT: force ONE real retired candidate, retaining the real gate. */
	const char *candidate = GetConfigOptionByName("pg_weave.probe_target", NULL, true);
	if (candidate != NULL && candidate[0] != '\\0' && atoi(candidate) > 0)
	{
		BlockNumber target = (BlockNumber) atoi(candidate);
		buffer = ReadBuffer(index, target);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		if (!WeavePageIsFreed(BufferGetPage(buffer)) ||
			!weave_page_recyclable(index, BufferGetPage(buffer)))
			elog(ERROR, "PROBE refused target %u: real recycle gate", target);
		RecordUsedIndexPage(index, target);
		elog(NOTICE, "PROBE REAL REUSE target=%u xid=%u", target, GetCurrentTransactionId());
		return buffer;
	}
'''
assert s.count(old) == 1
p.write_text(s.replace(old, new))

# Reuse the real collect-matches hook's location, without enabling unrelated
# WEAVE_TEST_HOOKS. A file owned by the TAP process is an external release gate.
p = Path('src/am/amscan.c')
s = p.read_text()
old = '''\tweave_read_meta(index, &meta);

#ifdef WEAVE_TEST_HOOKS
\t/*
\t * TEST-ONLY window: after snapshotting the segment directory'''
new = '''\tweave_read_meta(index, &meta);

\t/* EXPERIMENT ONLY: real AM directory captured; no segment page read yet. */
\t{
\t\tconst char *gate = GetConfigOptionByName("pg_weave.probe_pause_file", NULL, true);
\t\tif (gate != NULL && gate[0] != '\\0')
\t\t{
\t\t\telog(NOTICE, "PROBE REAL SCAN CAPTURE pid=%d index=%u root=%u generation=%u",
\t\t\t\t MyProcPid, RelationGetRelid(index), meta.segs[0].dictstart, meta.generation);
\t\t\twhile (access(gate, F_OK) == 0)
\t\t\t{
\t\t\t\tCHECK_FOR_INTERRUPTS();
\t\t\t\tpg_usleep(10000L);
\t\t\t}
\t\t\tCHECK_FOR_INTERRUPTS();
\t\t\telog(NOTICE, "PROBE REAL SCAN RELEASE pid=%d", MyProcPid);
\t\t}
\t}

#ifdef WEAVE_TEST_HOOKS
\t/*
\t * TEST-ONLY window: after snapshotting the segment directory'''
assert s.count(old) == 1
p.write_text(s.replace(old, new))
