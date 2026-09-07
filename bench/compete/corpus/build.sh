#!/usr/bin/env bash
#
# bench/compete/corpus/build.sh -- build one corpus, ONCE, identically for every
# engine host.
#
# This file exists because of the single most expensive error in this project's
# lineage.  pg_fts's 5-way comparison had to be retracted because pg_weave and
# VectorChord indexed `title||body` while pg_search and pg_textsearch indexed
# `body` alone: a 48% difference in postings scanned for the common term, spotted
# only when the match counts disagreed (commit 81532f4, recorded by the author as
# "my error").  The correction's *explanation* was then also wrong and required a
# second correction.
#
# The rule that prevents it: there is exactly ONE column, named `content`,
# produced by ONE generator, and every host proves it has the same bytes before
# building an index.
#
# Usage (on an engine host):
#   corpus/build.sh <corpus-id>       # writes /nvme/corpus.tsv + .sha256
#
# Corpora are generated deterministically from a seed rather than downloaded
# where possible.  A generated corpus is reproducible years later; a HuggingFace
# shard URL is not, and pg_fts's Wikipedia loader needed `iconv -c` fixes for
# invalid UTF-8 that silently changed the data.
#
set -euo pipefail

CORPUS=${1:?usage: build.sh <corpus-id>}
DEST=${DEST:-/nvme}
mkdir -p "$DEST"

case "$CORPUS" in
  # -------------------------------------------------------------------------
  # synth-2m: 2,000,000 documents, Zipfian vocabulary, deterministic.
  #
  # Sized to match the 2.19M-article Wikipedia corpus every prior pg_fts 5-way
  # used, so latency bands stay roughly comparable, while being reproducible from
  # a seed on any host with no network dependency.
  #
  # Token shape matters and is a hard-won detail: tokens must contain NO
  # separator characters.  An earlier pg_weave corpus used `word_00042`, and
  # because the analyzer splits on non-word bytes that tokenized as TWO terms --
  # so every "single rare term" measurement was silently a two-term boolean AND,
  # visible only in the EXPLAIN Sort Key.
  # -------------------------------------------------------------------------
  synth-*)
    # Row count comes from the id itself, so the corpus name fully determines the
    # data: synth-200k, synth-2m, synth-10m.  A corpus whose size depends on an
    # environment variable is a corpus you cannot reproduce from a results file.
    case "$CORPUS" in
      *-200k) N=200000 ;;
      *-1m)   N=1000000 ;;
      *-2m)   N=2000000 ;;
      *-10m)  N=10000000 ;;
      *) echo "cannot parse row count from '$CORPUS'" >&2; exit 1 ;;
    esac
    python3 - "$N" > "$DEST/corpus.tsv" <<'PY'
import random, sys
n = int(sys.argv[1])
rng = random.Random(20260907)          # frozen: the corpus is part of the result
VOCAB = 200000
# Zipf-ish via u^3 so rare terms carry real IDF signal.  A uniform vocabulary
# makes BM25 meaningless: every term gets the same IDF and ranked retrieval
# degenerates into an arbitrary tie-break.
def tok():
    return "word%06d" % (int(VOCAB * rng.random() ** 3) + 1)
# Planted patterns with KNOWN frequencies, so the fuzzy/regex/substring axes have
# ground truth and the analyzer can assert the corpus is what it claims.
for i in range(1, n + 1):
    nw = 8 + (i % 8)
    words = [tok() for _ in range(nw)]
    if i % 1000 == 0:
        words.append("zzqrare")                       # 0.1%  high-IDF marker
    if i % 100 == 0:
        words.append("connection refused")            # 1%    multi-word phrase
    if i % 5000 == 0:
        words.append("error E-%04d" % (i % 10000))    # 0.02% char-class regex
    if i % 20 == 0:
        words.append("government")                    # 5%    fuzzy target
    body = " ".join(words)
    print("%d\t%s" % (i, body))
PY
    ;;
  *)
    echo "unknown corpus '$CORPUS'" >&2
    echo "known: synth-2m (deterministic, no network)" >&2
    exit 1
    ;;
esac

sha256sum "$DEST/corpus.tsv" | awk '{print $1}' > "$DEST/corpus.sha256"
echo "corpus=$CORPUS rows=$(wc -l < "$DEST/corpus.tsv") bytes=$(stat -c%s "$DEST/corpus.tsv")"
echo "sha256=$(cat "$DEST/corpus.sha256")"
