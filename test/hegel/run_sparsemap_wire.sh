#!/usr/bin/env bash
#
# test/hegel/run_sparsemap_wire.sh -- cross-version sparsemap wire compatibility.
#
# pg_weave stores sparsemap blobs on disk (per-segment livedocs tombstones and
# trigram term-ordinal postings), so re-vendoring the library carries a hazard an
# ordinary dependency upgrade does not: EXISTING INDEXES hold bytes written by the
# OLD version.  If the new version reads them differently that is silent corruption
# of live data, and it presents as a relevance bug rather than an error.
#
# Two programs, bytes through a file, deliberately mirroring reality: the old
# library writes, a separate binary built against the new library reads.  One
# process including both headers fights the library's type namespacing (types,
# enums and macros are not covered by SPARSEMAP_PREFIX) for no benefit, and models
# "bytes written months ago" less faithfully.
#
# Needs the sparsemap repository for the old sources.  Override with
# SPARSEMAP_REPO / OLD_TAG.
#
set -euo pipefail
cd "$(dirname "$0")/../.."

REPO=${SPARSEMAP_REPO:-$HOME/ws/sparsemap}
OLD_TAG=${OLD_TAG:-v5.4.0}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ -d "$REPO/.git" ] || { echo "no sparsemap repo at $REPO (set SPARSEMAP_REPO)" >&2; exit 1; }

echo "old = $OLD_TAG from $REPO"
git -C "$REPO" show "$OLD_TAG:sm.h" > "$WORK/sm_old.h"
git -C "$REPO" show "$OLD_TAG:sm.c" > "$WORK/sm_old.c"
sed -i -e 's|#include "sm\.h"|#include "sm_old.h"|' "$WORK/sm_old.c"

cp test/hegel/sm_wire_corpus.h test/hegel/sm_wire_writer.c test/hegel/sm_wire_reader.c "$WORK/"
cp src/util/sparsemap.c "$WORK/sm_new.c"

CF="-O2 -Wall -Wno-unused-function -I $WORK -I include"

echo "building writer (old) and reader (new)"
gcc $CF -o "$WORK/writer" "$WORK/sm_wire_writer.c" "$WORK/sm_old.c" 2>&1 | head -20
gcc $CF -o "$WORK/reader" "$WORK/sm_wire_reader.c" "$WORK/sm_new.c" 2>&1 | head -20

"$WORK/writer" "$WORK/blobs.bin"
"$WORK/reader" "$WORK/blobs.bin"
