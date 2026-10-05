# Document list (v12) at scale: DELETE / VACUUM / INSERT cycles vs the heap

**Gaps:** G77, G78, G80, G81 (`doc/GAPS.md`). **Hard rule 12:** this work touches
tombstones, merge and VACUUM, so local green is not evidence. **Script:**
`bench/aws/doclist_scale.sh` (`SCRIPT=bench/aws/doclist_scale.sh bench/aws/run.sh
c7i.4xlarge script`). **Run:** `pgweave-20261005-072827-c05b`, c7i.4xlarge,
Debian 13, PG 17, branch `wt/doclist` at 5ef0531. The run's smoke (regression 23/23,
isolation, all 31 TAP files) was green before any number was taken.

## What ran

1,000,000 rows. 1 in 97 has a NULL `wdoc` (NULL documents, G77), 1 in 89 is
`to_wdoc('')` (zero terms, G78/G80), 1 in 7 has a NULL vector. Index:
`(body, emb, price int8_docval_ops)`. Then four cycles, each:

1. `DELETE` 2 % of rows, a slice that includes NULL, zero-term and ordinary rows;
2. `VACUUM` (bulkdelete tombstones them; the freed ctids are recyclable);
3. `INSERT` 16,666 rows into the freed space. Each is a NULL document, a zero-term
   document or a fresh one, with `price = 999` and the vector `[5000,...]`, values that
   are wrong if they inherit a dead row's docvalue or lane. Check (`pending`).
4. `VACUUM` again, which flushes them into a new bolt. Check (`flushed`).

Then `weave_merge()` and a final check. Each check compares the index scan and the
bitmap scan with the heap on four restrictions: `price < 3` (docvalues, where G77/G80
show up), `!common` and `x3 & !w7` (NOT universe, G78), and `price < 3 AND !x5`. Each
comparison is exact, via the count plus an md5 of the sorted id list. It also compares
a vector `ORDER BY ... LIMIT 50` with and without `price < 5`, and runs
`weave_check(deep)`.

## Result

**Every set comparison matched the heap: 40 of 40** (4 restrictions x 10 phases: build,
4 x pending, 4 x flushed, merged), on the index scan AND the bitmap scan, by count and
md5 of the sorted id list. **`weave_check(deep)` was clean in all 10 phases.** Every
bolt carried a COMPLETE document list in every phase.

| phase | bolts | ndocs (BM25 N) | ndeleted | lists |
|---|---:|---:|---:|---|
| build | 1 | 989,691 | 0 | 1 of 1 COMPLETE |
| cycle1-pending | 1 | 981,008 | 19,794 | 1 of 1 |
| cycle1-flushed | 2 | 981,008 | 19,794 | 2 of 2 |
| cycle2-pending | 2 | 972,102 | 39,811 | 2 of 2 |
| cycle2-flushed | 3 | 972,102 | 39,811 | 3 of 3 |
| cycle3-pending | 3 | 962,975 | 60,049 | 3 of 3 |
| cycle3-flushed | 4 | 962,975 | 60,049 | 4 of 4 |
| cycle4-pending | 4 | 953,623 | 80,512 | 4 of 4 |
| cycle4-flushed | 5 | 953,623 | 80,512 | 5 of 5 |
| merged | 1 | 953,623 | 0 | 1 of 1 |

`ndocs` at build is 989,691 = 1,000,000 minus the 10,309 NULL documents (1 in 97): a
NULL document is not a BM25 document, as specified. Each cycle's inserted NULL
documents likewise do not move it.

**The job's exit status was 1, from the vector comparison alone** (`DONE fails=10`: 10
of 20 vector comparisons printed `distances differ`, and 0 of 40 set comparisons
failed). See below.

**Not measured here:** time. Each `weave_check(deep)` took about 6.5 minutes at 1M rows,
which is a cost in its own right (it walks every chain and recomputes the docset
coverage per bolt), and nothing else in this run was timed. One scale only (hard rule
11): the 1M-row result has not been reproduced at a second scale.

## The vector comparison, and why its FAIL lines are not failures

The harness compares the vector ORDER BY by the exact SEQUENCE of distances, and it
printed `FAIL ... distances differ` 11 times (the unrestricted ORDER BY in all 10
phases, and the `price < 5` one once). In 8 of the 11 (`remote/vecdiff.log`) the 50
distances are the SAME MULTISET as the heap's, with 1 to 3 adjacent pairs swapped and
the largest inversion 0.083 in distance. In the other 3 (cycle4-pending, cycle4-flushed,
merged: the same boundary row each time), the index's 50th row has distance 34.6987
where the heap's 50th has 34.641. That is a gap of 0.058 at the cutoff, inside the
inversion range seen elsewhere: recall 49/50 at k = 50. That is the documented ANN behaviour: the index orders by QUANTIZED
reconstructions and keeps `xs_recheckorderby` false (`src/am/amscan.c`, "WHAT THE
DISTANCE IS"; `sql/vecorderby.sql` records the divergence). The harness was too strict,
not the index. The property this gate exists for holds, and the harness's shape is
what proves it: the distances it prints are recomputed by `emb <-> q` from the rows the
INDEX returned, so a recycled ctid ranked at a dead row's distance (G79/G80) would
appear in the index list at its OWN true distance, about 8,660 for the inserted
`[5000,...]` vectors. No such distance appears in any of the 20 index lists. What is
left is ANN ordering among live rows, a property of the quantized vector channel and
not of the document list. **Loss recorded:** one boundary row at k = 50 in three phases.
