# A1 scan-vs-merge page-recycle race: deterministic reproduction

`repro.sh` deterministically reproduces (on unfixed code) and verifies the fix
for the A1 hazard: a scan snapshots the segment directory, then walks segment
pages under only per-page SHARE locks, while a concurrent merge frees those
pages and an insert recycles them -> stale read.

It uses a test-only pause hook compiled in **only** under `-DPG_FTS_TEST_HOOKS`
(absent from production builds). The hook makes a scan briefly take the advisory
lock named by the `pg_weave.test_pause_advisory_key` GUC right after snapshotting
the metapage, so a blocker session can stall the scan in the vulnerable window.

## Build the test-hooks prefix (Nix)

```nix
# /tmp/join.nix
let flake = builtins.getFlake (toString /path/to/pg_weave);
    pkgs = flake.inputs.nixpkgs.legacyPackages.x86_64-linux;
    testso = flake.packages.x86_64-linux.default.overrideAttrs (o: {
      makeFlags = (o.makeFlags or []) ++ [ "COPT=-DPG_FTS_TEST_HOOKS" ];
    });
in pkgs.symlinkJoin { name = "pg-with-ftstest"; paths = [ testso pkgs.postgresql ]; }
```

```sh
nix build --impure --expr 'import /tmp/join.nix' -o /tmp/join
JOIN=/tmp/join bash test/a1_recycle/repro.sh
```

Non-Nix: build with `make COPT=-DPG_FTS_TEST_HOOKS`, install into a prefix that
also has the matching PostgreSQL, and point `JOIN` at it.

## Result

- **Unfixed code:** reader returns 0 (or a wrong count) -> `FAIL`.
- **Fixed code:** reader returns 500 -> `PASS`. The metapage `generation` counter
  (bumped on every segment-directory change) is re-checked at the end of the
  scan; a change triggers a bounded restart from a fresh snapshot.

Run out-of-band (like the fuzz teeth builds); the standard CI gates build
without `-DPG_FTS_TEST_HOOKS`. `t/005_concurrency.pl` is the in-tree
probabilistic hammer for the same hazard.
