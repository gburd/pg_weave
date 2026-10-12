# G40: HOT-successor TID positive control

`g40-hot-successor.patch` is a deliberately broken, **test-only** patch. Apply it
only to a disposable EC2 source copy. It removes the root conversion in
`weave_cgram_heapscan()` and logs the physical and root offsets at that site.
The resulting physical TID reaches `weave_gettuple()` through `plainTids`.
Do not install this build on a shared or production server.

For each of PG17 and PG18, on the EC2 host:

1. Apply with `patch -p1 < test/mutations/g40-hot-successor.patch`.
2. `make clean`, build and install with the major's explicit
   `PG_CONFIG=/usr/lib/postgresql/<major>/bin/pg_config` and `with_llvm=no`.
   Pass `PG_CONFIG` to `sudo make install` explicitly too: sudo drops the
   environment. Check the **build and install exit statuses** before testing.
3. Derive `PGPORT` from `pg_lsclusters` and verify `server_version_num`.
4. Run full `make installcheck` with that port and explicit `PG_CONFIG`, saving
   stdout/stderr to a file rather than piping through a truncating command.
5. Require `hot_roots` to fail: `fallback | t` becomes `fallback | f`, while
   `all_updates_hot`, the heap oracle, `cgram` and `lexical` remain true. Require
   the three `Index Scan using hot_roots_idx` plans to remain present.
6. Require the server log to associate the `CREATE TEMP TABLE hot_actual`
   statement with `G40 mutant: block 0 physical 25 root 1` through
   `physical 36 root 12`. This proves the broken site ran on actual successors,
   not just that a mutant compiled or an unrelated test failed.
7. Reverse the patch, clean/build/install again, and run the full
   regression/isolation/TAP gate. Never regenerate expected output from a mutant.

The 2026-10-12 control caught this mutation on both majors. `cgram` also failed
on its older **pre-build** churn fixture; `hot_roots` is not claimed to be the
only detector. Its added coverage is specifically **post-build** HOT churn,
without VACUUM, through proven plain Index Scans. See `doc/GAPS.md` G40 for the
run and gate results.
