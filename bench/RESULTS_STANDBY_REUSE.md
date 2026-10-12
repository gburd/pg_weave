# Existing-core standby fence: real retired-page reuse probe

2026-10-10. **Mechanism supported on PG17 and PG18; not an adopted retirement
protocol or production safety qualification.** No production code changed.

## Reproduce and identify

Probe/source-audit commit: `3757f61aa58eb040077b88a39463a761411a7d9a`, branch
`wt/standby-reuse`. From that checkout:

```sh
AWS_PROFILE=kiwi SCRIPT_PULL=120 JOB_MINUTES=60 \
  SCRIPT="$PWD/bench/aws/standby_probe_job.sh" \
  bench/aws/run.sh c7i.4xlarge script
```

Run: `pgweave-20261010-183552-868f`. Debian 13, c7i.4xlarge, 16 vCPU,
Intel Xeon Platinum 8488C, approximately 30 GiB visible RAM. Server logs identify
PostgreSQL **17.11** and **18.6** (PGDG Debian builds). The preliminary smoke uses
the normal PG17 install; the probe rebuilds from a disposable source copy for
each major, with `with_llvm=no`, and compiles a separate helper library. No ledger,
new WAL record, production SQL API, or modified recycle gate is installed in the
repository.

Artifacts: `bench/aws/out/pgweave-20261010-183552-868f/`, especially
`remote/standby-probe/`. The controller completed with `script: done, exit 0` and
`cleanup (exit 0)`. Its final pull recovered all 31 probe output files (build,
install, helper and clean logs, package log, both prove logs, six cluster/test
logs, eight WAL dumps, four shared libraries, and two hash lists). The four
library SHA-256 values were independently checked against the pulled binaries.
AWS subsequently reported the owned instance **terminated**; no live controller
or owned live instance remains. The takeover did not kill the controller, change
shipped inputs, or launch a second instance.

## What ran

Each major ran one four-arm probe against newly initialized physical primary and
standby clusters: **65/65 assertions**, **130/130 total**. Each arm creates a
300-row lexical index, then performs two one-row inserts. These are small
mechanism probes, not latency or scale measurements. TAP cluster settings are
its own defaults, not the harness's tuned system-cluster settings; primary
`autovacuum=off`, both `jit=off`, standby `hot_standby_feedback=off` and
`max_standby_streaming_delay=-1`.

| Arm, per major | Assertions | Observation |
|---|---:|---|
| no fence | 14 | Holder of old dictionary-root address and retained index ASL observes that exact page replaced after standby replay, before releasing ASL |
| fence transaction COMMIT | 17 | Startup waits on index AEL; reuse WAL received but not replayed until old reader ends |
| fence transaction ROLLBACK | 17 | Same exclusion despite primary fence transaction abort |
| fence subtransaction ROLLBACK TO, then parent COMMIT | 17 | Same exclusion with subtransaction acquisition/abort |

Markers in **each** `prove-{17,18}.log`:

- `1..65`, `Files=1, Tests=65`, `Result: PASS` and
  `PROBE COMPLETE: all four arms executed`.
- Four `experimental allocator BUILT AND REACHED` assertions and four
  `WITNESS` lines: eight actual candidate-site executions across the majors.
- One `NO FENCE: old descriptor holder sees actual replacement after replay`:
  **two successful negative controls**, not compile failures called catches.
- Three each of `startup waits for reader index ASL`,
  `standby received reuse WAL while replay blocked`, and
  `replay cannot reach INSERT reuse while old reader holds ASL`.
- Four each of exact reused-block WAL, completed replacement replay, fresh root,
  and fresh real `weave_count(...,'gamma') = 1` checks.

The initial PG17 smoke also passed **33 regression tests, 2 isolation tests,
35 TAP files / 1,487 assertions**, three lint gates, 49,844 codec checks, and
1,199,132 plus 1,199,868 regex-prefilter checks (zero failures). This is not a
PG18 full-suite run, nor a full suite against either experimental binary.

## Exact reuse and replay witnesses

All eight targets were block **3 in distinct indexes**. The header transitions
were live dictionary `2:0` -> retired dictionary `2:256` -> live pending-v10
`36:0` (kind:FREED mask). The old root ceased to be the current root before the
optional fence. Every INSERT passed the unchanged `weave_page_recyclable()` gate.
The experimental override changes candidate selection only, to ensure the real
retired page is consumed instead of relying on an FSM choice.

| Major | Arm | Retired page LSN | Replacement page LSN |
|---|---|---|---|
| 17 | none | 0/3058BF0 | 0/305CED8 |
| 17 | commit | 0/3084688 | 0/3084B28 |
| 17 | abort | 0/30AFB70 | 0/30AFED8 |
| 17 | subabort | 0/30D37E8 | 0/30D3B78 |
| 18 | none | 0/305BD58 | 0/305DBD0 |
| 18 | commit | 0/308B4D8 | 0/308B840 |
| 18 | abort | 0/30B0860 | 0/30B0BC8 |
| 18 | subabort | 0/30D4500 | 0/30D4890 |

The eight `pg{17,18}/wal-*.txt` dumps each contain a Generic record referencing
index block 3 (full-page image) and metapage block 0. The no-fence dumps separate
the heap INSERT's block 4 from the index replacement's block 3. Primary/standby
header kind, flag and LSN agree after replay. Page LSN is a WAL record end
position, not an incarnation token or the record-start address printed by
pg_waldump. No arbitrary old-payload parser is invoked to manufacture a crash.

## Interpretation and adoption choices

An ordinary core-generated **index relation AEL** can drain an ASL-protected
standby descriptor holder before later GenericXLog replacement, even when reuse
occurs in a different primary transaction after fence commit/abort/subabort.
The no-fence arm demonstrates the interleaving the fence excludes. The helper
unlocks AEL early on the primary; ordinary core replay supplies the exclusion,
not a held primary lock, hot-standby feedback, or an extension redo callback.

This supports the candidate in `doc/specs/PAGE_RETIREMENT.md` §9: preserve primary
horizons, require durable fence coverage of each exact retired incarnation, and
fence at a suitable caller boundary rather than on every allocating INSERT.
It does **not** establish that the present allocator is standby-safe.

Before adoption, the maintainer still needs to accept/review:

1. **Blocking and starvation cost.** Conditional AEL may fail indefinitely under
   continuous primary readers/writers. UNFENCED pages then remain ineligible and
   allocation extends. Choose caller boundaries, retry policy and the acceptable
   space/replay-delay cost; none has been measured. No bounded-space guarantee.
2. **Durable coverage protocol.** Approve UNFENCED/FENCED (or a proven equivalent)
   for exact incarnations, ordering after unlink and before destructive reuse,
   including crash recovery, basebackup without historical WAL, and promotion.
   The accepted striped representation alone does not settle this addition.
3. **Reader and cache lifetime.** The source audit finds enclosing index ASL for
   AM/custom/SQL-reader page accesses, but this probe holds a helper descriptor,
   not a suspended lexical/vector/fuzzy/bitmap parser. Real-reader tests are owed.
   The doclen address directory in `rd_amcache` uses uint32 generation equality;
   generation ABA remains possible. Make page-address state scan-local or expire
   it at the enclosing ASL scope, and audit/expire copied SuRF state too. Nothing
   in this experiment implements or mutation-tests that correction.
4. **Primary self-readers.** AEL is self-compatible. A backend can retain its own
   suspended cursor or merge input; preserve primary FullXid horizons and input
   protection rather than generalizing an AEL bypass. That case remains untested.

## What this does not establish

One small run per major; no second scale, A/A latency series, cost claim or
production qualification. Only waiting was exercised, not timeout cancellation.
Abort/subabort applies to the **separate fence transaction**, not an aborted
merge or durable coverage write; the subabort arm commits its parent before the
later INSERT, so it does not isolate early subtransaction release while the
parent remains open. WAL dumps cover the INSERT interval, not the earlier AEL
record; source tracing plus the observed same-index replay wait supports the
lock mechanism. Crash/checkpoint/basebackup/promotion, repeated locally-held
AEL, primary self-cursors, generation-wrap mutation, parallel/independent writer
integration and conditional-fence starvation remain open. No parser crash,
wrong-answer demonstration, or proof for all actual AM readers is claimed.

## Real AM scan follow-up — launch blocked, 2026-10-10

**SUPERSEDED status, 2026-10-11:** corrected run `pgweave-20261011-010049-1ff4`
completed both majors; see the final section. This blocked-launch history remains.

**Unmeasured.** The helper-only results above remain unchanged. The continuation
implements a runnable real-reader probe in the same four bench harness files;
it has not produced PG17 or PG18 results. Launch
`pgweave-20261010-195152-ea8b`, source commit `3da9e4b`, requested one
`c7i.4xlarge` and failed at `RunInstances` with `VcpuLimitExceeded`, before any
instance existed. A subsequent regional inventory summed to **1,152 allocated
vCPUs**, matching the quota in the refusal. A smaller instance would not fit
that inventory either. No other workloads were modified. The controller exited
1 and completed its cleanup; no test, build, or experimental site ran remotely.

The new disposable patch pauses `weave_collect_matches()` immediately after its
actual `weave_read_meta()` directory capture, at the existing test-hook window.
A NOTICE identifies backend PID, index OID, captured root and generation; a
TAP-owned file controls release, with `CHECK_FOR_INTERRUPTS()` while paused.
Production C remains unchanged in the repository. The patch does not enable
unrelated test hooks, bypass primary horizons, or alter the existing exact-target
allocator override/recycle gate.

The TAP adaptation is intended to establish these **still unexecuted** checks:

- Two-term `weave_count()` reaches the real capture marker and holds index ASL.
- No fence: the exact captured DICT block is replaced with live PENDING-v10 and
  replayed while the scan remains paused; cancel it before parsing stale payload.
- Fence WAIT: reuse WAL is received but not replayed; release the scan, check its
  count is 300, then check replacement replay and a fresh query.
- Fence CANCEL: while waiting, set supported `max_standby_streaming_delay=1s`;
  require the recovery-conflict error, no pause-release marker, and the core
  lock-conflict counter. This is not a measured one-second latency guarantee.
- Primary conditional AEL fails under another backend's ASL or RowExclusiveLock
  and succeeds after release. A real primary cursor can acquire its own AEL but
  its retained snapshot must still prevent exact-target reuse until it ends.

Patch anchor application to an isolated source copy and `git diff --check`
passed locally; these are **not** compilation or runtime evidence. The follow-up
also preserves the patched C sources beside build logs and binary hashes in the
remote artifacts. Rerun the reproduction command above from the follow-up
harness commit once kiwi capacity is available, using `nohup`, `SCRIPT_PULL=120`,
and no inherited hotdog expiry. PG17/18 outcomes, final artifact/hash inspection,
and termination verification remain required. No production qualification,
bounded progress/space claim, ledger, implementation plan or spec change follows
from this blocked launch.

Capacity rechecked at 2026-10-10 19:55 UTC: still 1,152 allocated vCPUs, so the
conditional smaller `c7i.2xlarge` attempt (8 vCPUs) was not launched. Launch
retries stopped. Independent checks found no controller, no pending/running
`Project=pg_weave` instance, no instance bearing the failed run's Name tag, and
no remaining failed-run key/group. No other instance or quota was changed.
The committed harness remains unexecuted on both majors; the refusal is solely
an infrastructure/setup outcome, not a failed or successful test.

### First real-scan execution and harness correction

Run `pgweave-20261010-223238-fdab` at `dc47574`, one Debian c7i.2xlarge,
passed the initial PG17 smoke (35 TAP files / 1,487 assertions). The experimental
PG17 binary built and reached all three real scan captures and exact allocator
sites. No-fence replacement/cancel and WAIT release/count=300 passed. The probe
then **failed**, timing out after 59 passing assertions, with no TAP plan; PG18
was not reached. This is not a green probe.

Root cause in the CANCEL setup: it changed `max_standby_streaming_delay` from
`-1` to `1s` after startup entered its relation-lock wait. PG17's
`ResolveRecoveryConflictWithLock()` selects its deadline and arms the timeout
before sleeping; changing the GUC did not arm a timer for this existing wait.
The server logged reload, but the query stayed paused until the 120-second
harness timeout. Configure the finite budget before the conflict instead: the
corrected arm restarts the standby with `10s` and checks SHOW before starting the
scan. Ten seconds leaves time to assert received-but-not-replayed reuse WAL;
this is a test budget, not a cancellation-latency guarantee. No assertion is
removed, and the outer timeout is unchanged. The failed run's artifacts were
pulled and its instance independently confirmed terminated.

### Corrected rerun blocked; finite outcome at 22:50 UTC

Correction commit `6d4d629` was committed before requesting a fresh full-archive
run, `pgweave-20261010-224653-f58c`, again one `c7i.2xlarge`. AWS refused
`RunInstances` with `VcpuLimitExceeded` (quota 1,152); no instance was created and
no corrected test ran. **Launch retries stopped.** Independent checks found the
first real-run host terminated, no instance for the refused rerun, neither
controller alive, no live owned instance, and no refused-run key/security group.
This second refusal is setup failure, not product or test evidence.

The first real-run final pull contains patched sources, PG17 build/install/helper
logs, probe/test/server logs, both shared libraries and all three WAL dumps. Both
library hashes match the recorded SHA-256 values. Every dump has a Generic
record referencing index block 3 and metapage block 0. Recorded PG17 targets:

| Arm | Retired DICT LSN | Live PENDING-v10 LSN | Outcome |
|---|---|---|---|
| none | 0/3058C48 | 0/305CF30 | replacement observed with actual scan ASL still held; user cancel, no release marker |
| wait | 0/30846D8 | 0/3084C68 | received, not replayed while paused; released real scan returned 300; replay completed |
| cancel | 0/30AFCB8 | 0/30B0038 | received, not replayed while paused; harness timed out before cancellation |

All three captured roots were block 3 in separate indexes, with marker PIDs
17444/17501/17554. These actual-site observations narrow the earlier helper-only
limitation for **one lexical count path on PG17**; they do not establish every
reader path. The successful no-fence and WAIT arms are partial evidence inside
an overall failed probe, not a passing suite. **PG18 and the primary conditional
lock/self-cursor controls were not reached.** Finite-delay cancellation remains
unverified, as does the corrected setup. There is no second-scale result,
latency/progress guarantee, production qualification, or implementation ledger.

### Final checkpoint: assertion accounting and evidence paths

**SUPERSEDED status, 2026-10-11:** the NOT RUN/cancellation-unverified statements
below describe `fdab` only. Corrected run `1ff4` validated the finite probe on both
majors; it does not retroactively make `fdab` pass.

For **`pgweave-20261010-223238-fdab` only** (not the older helper experiment):

| Component | Executed assertions | Status |
|---|---:|---|
| Initial unpatched PG17 smoke TAP | 1,487 across 35 files | PASS |
| Experimental PG17 no-fence arm | 1–21 (21) | All emitted `ok`; arm completed |
| Experimental PG17 WAIT arm | 22–43 (22) | All emitted `ok`; arm completed |
| Experimental PG17 CANCEL arm | 44–59 (16) | All emitted `ok`; then harness timeout before cancellation assertions |
| Experimental PG17 complete probe | 59 total, zero `not ok` | **FAIL**, exit 25, no plan / no `done_testing()` |
| Experimental PG18 probe | 0 | **NOT RUN** |
| Primary conditional-lock and self-cursor controls | 0 | **NOT RUN** |
| Corrected harness `6d4d629` | 0 | **NOT RUN**, rerun launch quota-refused |

The CANCEL timeout is a **harness ordering failure**, not evidence of a fence
or product failure. No complete recovery-conflict cancellation validation may
be claimed by the designer/spec from this run. In particular, user-requested
cancellation of the no-fence query is not recovery-conflict cancellation.
Production retirement ledger remains **unimplemented**.

Artifacts are preserved under
`bench/aws/out/pgweave-20261010-223238-fdab/remote/standby-probe/`:

- `experimental-amscan.c`: actual directory capture before the file-gated pause;
  `experimental-am.c`: exact candidate selection retaining the real recycle gate.
- `prove-17.log`: capture PID/index/root markers, per-site ASL assertions,
  primary retirement/replacement headers, and assertion numbers above. Assertions
  13–14 prove the no-fence replacement was already replayed while the *real AM*
  descriptor-holder retained ASL; 36–37 prove WAIT received replacement WAL but
  had not replayed it. Assertions 38–40 show real release and count 300.
- `pg17/log/regress_log_standby_probe`: timestamped sequence and IPC::Run timeout;
  `pg17/log/standby_probe_{primary,standby}.log`: SQL/replay/reload context.
- `pg17/wal-{none,wait,cancel}.txt`: exact index-block-3 Generic WAL references.
- `build-17.log`, `install-17.log`, `helper-17.log`, `hashes-17.txt`,
  `pg_weave-experimental-17.so`, `standby_probe-17.so`: build and identity evidence;
  both pulled library hashes independently verified.

Controller logs are retained locally at
`/scratch/pg_weave/standby-realscan-resume/controller.log` (executed run) and
`/scratch/pg_weave/standby-realscan-rerun/controller.log` (quota-refused rerun).
The executed host was independently observed **terminated**; the refused rerun
created no instance. Neither controller remains. No further launch is authorized
while quota-blocked; no main merge/push or production implementation was done.

## Corrected real-AM probe completed — 2026-10-11

**Finite mechanism probe PASS on PG17.11 and PG18.6, 79 assertions each (158
 total). Not production retirement qualification.** Run
`pgweave-20261011-010049-1ff4` archived commit
`f1f949cd1ea7179f40e29b7cc0334d1edfffa367`, including correction `6d4d629`.
One Debian 13 c7i.2xlarge, 8 vCPUs, about 15 GiB visible RAM, Xeon Platinum
8488C. Majors built and tested sequentially, separate binaries and clusters.

Reproduction from that commit (scratch log path is caller-selected):

```sh
nohup env -u AWS_EXPIRES AWS_PROFILE=kiwi AWS_REGION=us-east-2 \
  AWS_DEFAULT_REGION=us-east-2 SCRIPT_PULL=120 JOB_MINUTES=60 \
  SCRIPT="$PWD/bench/aws/standby_probe_job.sh" \
  bash bench/aws/run.sh c7i.2xlarge script > /scratch/pg_weave/probe-controller.log 2>&1 &
```

The corrected CANCEL arm configures `max_standby_streaming_delay=10s` before
startup reaches the conflict. No assertion or outer timeout was weakened. Both
majors now reach the recovery-conflict error, unlike `fdab`'s late-reload harness
failure. The old run remains FAIL. No corrective edits or reruns were needed
within `1ff4`.

### Exactly what passed

Each `prove-{17,18}.log` contains all 79 numbered `ok` assertions, `1..79`, the
`PROBE COMPLETE` marker, and `Result: PASS`; no `not ok` or unfinished plan.

| Assertions per major | Count | Evidence |
|---|---:|---|
| 1–21, no fence | 21 | Real AM captured old directory under index ASL; exact retired root overwritten and replayed with reader still paused/holding ASL; user cancel before payload parse |
| 22–43, WAIT | 22 | Core index AEL wait; exact replacement WAL received but not replayed; release real scan, count 300; replacement replay and fresh query succeed |
| 44–67, CANCEL | 24 | Finite budget present before conflict; real scan capture/ASL and exact replacement; received/not-replayed; recovery lock-conflict cancellation without release marker, `confl_lock` increment; replacement replay and fresh query succeed |
| 68–71, primary locks | 4 | Another backend's ASL and RowExclusiveLock each deny conditional AEL; each release permits it |
| 72–79, self-cursor | 8 | Cursor query uses weave Index Scan and retains ASL/snapshot; same backend obtains AEL; unchanged recycle gate refuses retired target with snapshot held, permits real reuse after cursor transaction ends |

The self-cursor gate test uses a separate allocating backend while the cursor
backend demonstrates self-compatible AEL. It does **not** test destructive reuse
issued from the cursor's own backend, nor claim that AEL replaces primary
snapshot horizons. The primary contention controls hold ordinary relation locks
through the bench helper; they do not measure sustained workload starvation.

The initial unpatched PG17 smoke also reports 35 TAP files / **1,527 assertions**,
PASS (the suite has data-dependent assertion totals). This is not a PG18 full
suite, or a full regression suite against either experimental binary.

### Actual page and replay witnesses

All six targets are block 3 in distinct indexes. For each, captured live DICT
`2:0` becomes retired DICT `2:256`, then live PENDING-v10 `36:0`. The source patch
forces candidate selection only; `weave_page_recyclable()` remains the gate.
Each WAL dump has a Generic record naming the exact index block 3 plus metapage
block 0. This is actual target replacement, not merely a blocked SQL statement.

| Major | Arm | Capture PID | Retired LSN | Replacement LSN |
|---|---|---:|---|---|
| 17 | none | 17337 | 0/3058C48 | 0/305CF30 |
| 17 | wait | 17394 | 0/30846D8 | 0/3084C68 |
| 17 | cancel | 17459 | 0/30AFCB8 | 0/30B0038 |
| 18 | none | 17856 | 0/305BDB8 | 0/305DD20 |
| 18 | wait | 17913 | 0/308B620 | 0/308B988 |
| 18 | cancel | 17981 | 0/30B09B0 | 0/30B0D18 |

CANCEL errors explicitly say `canceling statement due to conflict with recovery`
and `User was holding a relation lock for too long.` Neither canceled arm logs
`PROBE REAL SCAN RELEASE`. WAIT does log release and returns 300 on both majors.
No-fence cancellation is deliberately user-requested, not counted as a recovery
cancellation. No unsafe stale-payload parse or manufactured backend crash.

### Preserved artifacts and lifecycle

Root: `bench/aws/out/pgweave-20261011-010049-1ff4/`; probe evidence under
`remote/standby-probe/`. It retains both `prove-*.log`, patched
`experimental-{am,amscan}.c`, per-major build/install/helper/clean logs, six WAL
dumps `pg{17,18}/wal-{none,wait,cancel}.txt`, server/test logs, four libraries and
hash lists. Hashes independently checked against the pulled files:

| Binary | SHA-256 |
|---|---|
| pg_weave-experimental-17.so | 355c3230895cf75d24fb84927c04eb51a80f5cee49218eedc6d6407b550e3bbf |
| standby_probe-17.so | c21c6e81f590374afb3107bf5e59a1d2ec87ac43d119287d197cac2e35cd7d15 |
| pg_weave-experimental-18.so | 7b7b374773450ef319ce1dd2603f0e744e20346a019eff268d989e76a2926e33 |
| standby_probe-18.so | 53d09bf7cfb715c436750ff3ebfd21897780f168997d53fba54960d424d9f68e |

Thus both pg_weave and helper binaries differ between majors. Server logs identify
17.11 and 18.6. Controller log:
`/scratch/pg_weave/standby-realscan-oct11/controller.log`. Final artifact pull
completed; controller reports script exit 0 and cleanup exit 0. Independent AWS
lookup reports the owned instance **terminated**; controller exited; no
pending/running `Project=pg_weave` instances remained at final verification.

### Limits and design handoff

Designer/lead notified: finite supported cancellation is now validated for this
probe on both majors; the earlier unvalidated status is superseded, not erased.
Only one lexical count path paused after actual directory capture, 300 initial
rows per index, one completed corrected run per major. No second scale or A/A
latency study; no cancellation-time SLA, starvation bound, bounded-space or
progress guarantee. Other AM readers, cache-generation ABA, crash/checkpoint,
basebackup/promotion, parallel writers and durable fence coverage remain open.
The successful self-cursor control supports preserving primary horizons, not
bypassing them. **Production retirement ledger remains unimplemented.** No
production C, spec, implementation-plan, main merge or push was authorized or
done in this continuation.
