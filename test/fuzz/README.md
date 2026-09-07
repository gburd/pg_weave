# pg_weave fuzz / corruption harness (`test/fuzz/`)

Fuzz/corruption harness for the pg_weave functions that parse **untrusted
on-disk data** -- the exact crash surface of the last three CVE-class bugs:

| version | bug |
|---------|-----|
| v0.2.4 | MAXALIGN phrase-positions bug (detoasted doc at a non-MAXALIGN'd address) |
| v0.3.3 | pending-doc overflow (readers trust `entries[].len/off/tf/posoff`) |
| v0.3.4 | segment block-header overflow (`count`/`bytelen` from a torn page) |

**The property under test:** these functions must **never crash / overflow /
invoke UB on any input** -- they either correctly reject it or handle it
safely. Everything runs under **AddressSanitizer + UndefinedBehaviorSanitizer**,
so an overflow or UB is a hard failure. This mirrors libxtc's
"bug-injection / never-crashes-on-corrupt-input" discipline
(`xtc/docs/testing.md`): the harness plants a known bug and proves the fuzzer
*catches* it (the "teeth" checks below).

## How to run

```sh
# from the repo root; needs clang with ASan+UBSan (default CC=clang)
bash test/fuzz/run.sh          # exit 0 = all clean
CC=clang bash test/fuzz/run.sh # if clang is not the default CC
```

`run.sh` is self-contained: it compiles the three fuzzers directly (no CMake
needed) with `-fsanitize=address,undefined`, runs them, and then runs the
"teeth" builds and asserts they abort. **Exit 0 = every fuzzer clean AND every
planted bug caught.** A `CMakeLists.txt` is also provided (mirrors
`test/hegel/CMakeLists.txt`) for `ctest` integration.

### For the CI-wiring agent

Invoke exactly:

```sh
bash test/fuzz/run.sh   # expected exit 0
```

Requires a `clang` on `PATH` (e.g. `nix shell nixpkgs#clang --command bash test/fuzz/run.sh`).
No hegel server, no cmocka, no network. The fuzzers are deterministic (fixed
PRNG seeds), so a failure is reproducible. Each fuzzer runs a few hundred
thousand iterations and finishes in a few seconds under ASan.

## The three fuzzers

Deterministic xorshift64* PRNG with a fixed seed (reproducible), no hegel /
cmocka -- a plain loop is simpler and runs in CI with zero dependencies.

### `fuzz_for.c` -- the FOR integer codec (`weave/for.h`)
- **Write safety:** `weave_for_unpack(buf, n, out)` into a heap output of exactly
  `n` uint64 (ASan-redzoned), for `n` in {0,1,2,63,64,65,127,128} and every
  width byte 0..255 plus random payloads. The decoder must fill exactly `n` and
  never write past the output.
- **Round-trip:** `pack(vals,n)` then `unpack` reproduces `vals` for random `n`
  in [0,128] and random widths (incl. full 64-bit values); the packed length
  equals what `unpack` consumes, `weave_for_bytelen`, and the formula; random
  access agrees with the full unpack.
- Iterations: 8 * (2 + 256 + 2000) write-safety + 200,000 round-trip.
- Seed: `0xF0F0C0DEC0DEF0F0`.

### `fuzz_docvalid.c` -- the WeaveDoc validator (`weave/docvalid.h`)
Exercises `weave_doc_check` (the pure body of `weave_doc_is_valid`) with:
- **(a)** fully random buffers of random length (exact-size heap alloc so ASan
  redzones the end);
- **(b)** structured-but-corrupted WeaveDoc images: a valid header then mutated
  `nterms` / `entries[].len/off/tf/posoff` / `lexbytes` / `VARSIZE` (1-3
  mutations), plus truncated buffers and an inflated-declared-VARSIZE pass.
- Property: returns cleanly (0/1) and never reads outside the buffer.
- Iterations: 200,000 random + 200,000 structured (each with 3 sub-passes).
- Seed: `0xD0C5A11DFA57C0DE`.

### `fuzz_block.c` -- the segment block-header decode loop
Models the **inner loop of `weave_decode_term`** (`pg_weave_am.c`) as the pure
`decode_block_inner()` -- header parse + the v0.3.4 guards + three
`weave_for_unpack` calls into fixed `[128]` arrays -- over a full-BLCKSZ (8192)
page buffer. `weave_decode_term` itself needs the backend
(`ReadBuffer`/`LockBuffer`/`Page`), so it cannot be linked standalone; this is a
faithful transcription of the loop body sharing the real `weave/for.h` codec
and `WeaveBlockHdr` layout. **This is a model, not the backend function** -- see
the header comment in `fuzz_block.c` for the honest accounting.
- Default: well-formed FOR stream + corrupt `count` (mostly >128) + honest
  `bytelen`, plus a guard-#2 rejection sub-case (bytelen forced past the page).
  Proves the count clamp holds and the past-page guard rejects.
- Also fuzzes the primitive directly: `weave_for_unpack(buf, attacker_count, out[128])`.
- Iterations: 300,000 block + 100,000 primitive.
- Seed: `0xB10CC0DEB10CC0DE`.

## The extraction (one non-test change, behavior-identical)

To fuzz `weave_doc_is_valid` standalone (it lives in `pg_weave_doc.c` which
`#include`s the heavy `postgres.h`), its body was factored into a pure header:

- **`weave/docvalid.h`** (new): `weave_doc_check(base, sz, varsize)` -- a straight
  transcription of the validator with no PG dependencies (fixed-width struct
  mirror, local MAXALIGN, VARSIZE passed in by the caller). Single source of
  truth, mirroring how `weave/for.h` is shared with `test/hegel/`.
- **`pg_weave_doc.c`**: `weave_doc_is_valid` is now a thin wrapper -- it reads
  `VARSIZE` in its proper backend context and calls `weave_doc_check`. Five
  `StaticAssertStmt`s guard the mirror against layout drift.

Verified behavior-identical: `nix build .#checks.x86_64-linux.installcheck-pg17`
is **green** after the extraction (the static asserts also compile-check the
layout).

## Findings (the fuzzer earned its keep)

The harness surfaced three real corruption-triggered defects in the shipped
code. **Fix #1 was applied** (a one-line, behavior-identical hardening of the
shared codec, verified green by installcheck). **#2 and #3 are reported here for
the parent/reviewer** to fix in `pg_weave_am.c` (the decode path, owned
elsewhere); the fuzzer's "teeth" builds reproduce them on demand.

1. **`weave/for.h` shift-by-64 UB (FIXED).** In `weave_for_unpack`'s wide-window
   branch, `hi << (64 - shift)` is UB when `shift == 0`, reachable only with a
   **corrupt width byte > 64**. Fixed by taking `lo & mask` directly when
   `shift == 0` -- identical result for every valid width (valid width <= 64 =>
   `shift >= 1` in that branch), safe on corrupt width. This is the one-line
   change to `weave/for.h`.

2. **Signed-cast bypass of the v0.3.4 count clamp (REPORT).** In
   `weave_decode_term`: `int cnt = (int) bh->count; if (cnt > WEAVE_BLOCK_SIZE)
   cnt = WEAVE_BLOCK_SIZE;`. `bh->count` is `uint32`; a corrupt value in
   `[2^31, 2^32)` casts to a **negative int**, so the clamp is skipped and
   `weave_for_unpack(stream, negative_n, ...)` walks `pos` to a wild pointer ->
   OOB read. The clamp is one-sided. **Suggested fix:** clamp both ends, e.g.
   `if (cnt <= 0 || cnt > WEAVE_BLOCK_SIZE) { handle as corrupt/empty }`.
   Reproduce: `run.sh` builds `fuzz_block_signed` (the shipped one-sided clamp)
   and asserts ASan aborts it.

3. **Corrupt width / bytelen read past the page (REPORT).** Guard #2
   (`stream + bytelen + posbytelen <= pend`) bounds the *declared* `bytelen`,
   but `weave_for_unpack` reads `1 + (cnt*width+7)/8` bytes driven by the
   *on-disk width byte*, which is not cross-checked against `bytelen`. A corrupt
   width can make the decode read past `bytelen` and past the page.
   **Suggested fix:** validate each column's `weave_for_bytelen` against the
   remaining `bytelen` before unpacking, or bound the decode by `bytelen`.
   Reproduce: `run.sh` builds `fuzz_block_randstream` (fully-random FOR stream)
   and asserts ASan aborts it.

## Teeth (planted-bug discipline)

`run.sh` compiles `fuzz_block.c` in three "teeth" modes and asserts each
**aborts** under ASan -- if any exited 0, the harness would be toothless and the
whole run fails:

| build define | plants | proves the fuzzer catches |
|--------------|--------|---------------------------|
| `FUZZ_NO_CLAMP` | the v0.3.4 count clamp reverted | count>128 overflowing `gaps[128]` (the original v0.3.4 bug) |
| `FUZZ_SIGNED_COUNT` | the shipped one-sided clamp | finding #2 (negative-cast count -> wild read) |
| `FUZZ_RANDOM_STREAM` | a fully-random FOR stream | finding #3 (corrupt width -> read past page) |
