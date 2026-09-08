#!/usr/bin/env bash
#
# bench/compete/engines/common.sh -- shared provisioning for every engine host.
#
# Sourced by each engine script.  Everything here is a lesson from a predecessor
# run; see bench/compete/STRATEGY.md for the citations.
#
set -euo pipefail

NVME=${NVME:-/nvme}
PGPORT=${PGPORT:-55432}
PGDATA=$NVME/pgdata
OUTDIR=$HOME/compete/out
export PGHOST=$NVME/sock
export PGPORT
export PGDATABASE=${PGDATABASE:-bench}
DSN="postgresql://?host=$PGHOST&port=$PGPORT&dbname=$PGDATABASE"

mkdir -p "$OUTDIR"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# Instance-store NVMe, not EBS.  Every prior competitive run used local NVMe
# explicitly so the storage path is real; pg_tre's stress harness additionally
# RAID-0s all instance-store devices because the build-time temp-disk wall only
# shows up at multi-GB/s.
# ---------------------------------------------------------------------------
setup_nvme() {
    mountpoint -q "$NVME" && { say "nvme already mounted"; return; }
    sudo dnf install -y -q mdadm >/dev/null 2>&1 || true
    mapfile -t devs < <(lsblk -dno NAME,MODEL | awk '/Instance Storage|NVMe Instance/{print "/dev/"$1}')
    if [ ${#devs[@]} -eq 0 ]; then
        mapfile -t devs < <(lsblk -dno NAME | grep -E '^nvme[1-9]' | sed 's|^|/dev/|')
    fi
    sudo mkdir -p "$NVME"
    if [ ${#devs[@]} -eq 0 ]; then
        say "WARNING: no instance-store device found; using the root EBS volume."
        say "         Storage-path numbers from this host are NOT comparable to"
        say "         prior runs and the result must say so."
        sudo chown "$USER" "$NVME"; return
    fi
    if [ ${#devs[@]} -eq 1 ]; then
        sudo mkfs.xfs -f -q "${devs[0]}" && sudo mount -o noatime,nodiscard "${devs[0]}" "$NVME"
    else
        sudo mdadm --create /dev/md0 --level=0 --raid-devices=${#devs[@]} "${devs[@]}" --force
        sudo mkfs.xfs -f -q /dev/md0 && sudo mount -o noatime,nodiscard /dev/md0 "$NVME"
    fi
    sudo chown "$USER" "$NVME"
    say "nvme: ${devs[*]} -> $NVME"
}

tune_os() {
    # THP never / governor performance / no NUMA balancing: pg_tre's stress
    # provisioner sets exactly these, and leaving them at defaults adds variance
    # that swamps the differences being measured.
    echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null 2>&1 || true
    sudo sh -c 'for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
                  [ -w $c ] && echo performance > $c; done' 2>/dev/null || true
    sudo sysctl -qw vm.swappiness=1 kernel.numa_balancing=0 >/dev/null 2>&1 || true
}

install_pg_source() {
    # Build PostgreSQL from source, like every prior competitive run, so all
    # engines link against a byte-identical server.  A distro package differing
    # between hosts is another uncontrolled variable.
    local ver=${PGVER:-17.6}
    command -v "$NVME/pg/bin/pg_config" >/dev/null && { say "pg already built"; return; }
    say "installing build deps"
    sudo dnf groupinstall -y -q "Development Tools" >/dev/null 2>&1 || true
    sudo dnf install -y -q readline-devel zlib-devel libicu-devel openssl-devel \
        bison flex perl-IPC-Run perl-Test-Simple git wget xz python3 >/dev/null 2>&1
    say "building postgresql $ver"
    cd "$NVME"
    wget -q "https://ftp.postgresql.org/pub/source/v$ver/postgresql-$ver.tar.bz2"
    tar xf "postgresql-$ver.tar.bz2"
    cd "postgresql-$ver"
    ./configure --prefix="$NVME/pg" --without-icu --enable-tap-tests \
        CFLAGS="-O2" >/dev/null
    make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null
    cd contrib && make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null
    say "postgresql installed at $NVME/pg"
}

start_pg() {
    export PATH="$NVME/pg/bin:$PATH"
    mkdir -p "$PGHOST"
    [ -d "$PGDATA" ] || initdb -D "$PGDATA" -U "$USER" --no-sync -A trust >/dev/null
    local memkb sb
    memkb=$(awk '/MemTotal/{print $2}' /proc/meminfo)
    sb=$((memkb / 1024 / 1024 * 40 / 100))
    cat > "$PGDATA/conf.d.conf" <<EOF
listen_addresses = '*'
unix_socket_directories = '$PGHOST'
port = $PGPORT
shared_buffers = ${sb}GB
maintenance_work_mem = 8GB
work_mem = 256MB
effective_cache_size = $((memkb / 1024 / 1024 * 75 / 100))GB
max_parallel_maintenance_workers = 8
max_parallel_workers = 16
max_parallel_workers_per_gather = 4
max_worker_processes = 32
checkpoint_timeout = 60min
max_wal_size = 32GB
random_page_cost = 1.1
jit = off
autovacuum = off
fsync = off
EOF
    grep -q "conf.d.conf" "$PGDATA/postgresql.conf" || \
        echo "include = 'conf.d.conf'" >> "$PGDATA/postgresql.conf"
    [ -n "${PRELOAD:-}" ] && \
        echo "shared_preload_libraries = '$PRELOAD'" >> "$PGDATA/conf.d.conf"
    pg_ctl -D "$PGDATA" -l "$NVME/pg.log" -w start >/dev/null 2>&1 || \
        pg_ctl -D "$PGDATA" -l "$NVME/pg.log" -w restart >/dev/null
    createdb "$PGDATABASE" 2>/dev/null || true
    say "postgres up on $PGHOST:$PGPORT"
}

record_hostinfo() {
    {
        echo "engine=${ENGINE:-?}"
        lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Core|Socket|Flags)' | cut -c1-200
        free -g | head -2
        df -h "$NVME" | tail -1
        echo "kernel=$(uname -r)"
        "$NVME/pg/bin/pg_config" --version 2>/dev/null || true
    } > "$OUTDIR/hostinfo.txt"
}

# ---------------------------------------------------------------------------
# Load the shared corpus.  The checksum assertion is the whole point.
# ---------------------------------------------------------------------------
load_corpus() {
    local corpus=$1
    export PATH="$NVME/pg/bin:$PATH"
    bash "$HOME/compete/corpus/build.sh" "$corpus" | tee "$OUTDIR/corpus.txt"

    # A generated corpus is deterministic, so its sha256 is a cross-host
    # assertion: if two engine hosts disagree here they are not benchmarking the
    # same data, and every comparison between them is void.
    local sha
    sha=$(cat "$NVME/corpus.sha256")
    echo "corpus_sha256=$sha" >> "$OUTDIR/hostinfo.txt"

    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" <<SQL
DROP TABLE IF EXISTS docs CASCADE;
CREATE TABLE docs (id bigint PRIMARY KEY, content text NOT NULL);
SQL
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" \
        -c "\\copy docs(id,content) FROM '$NVME/corpus.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" <<'SQL'
-- Frequency bands chosen from the ACTUAL data by absolute document frequency.
-- percent_rank is the wrong tool on a heavy-tailed vocabulary: most terms tie at
-- a very low df so the rank jumps, and a window like "pr between 0.10 and 0.12"
-- can select nothing -- which it did, leaving a benchmark measuring an empty
-- query that trivially returned 0 rows.
DROP TABLE IF EXISTS bands;
CREATE TABLE bands (band text PRIMARY KEY, term text NOT NULL, df bigint NOT NULL);
-- Corpus-agnostic band selection.  This filtered on `w LIKE 'word%'`, which is the
-- synthetic corpus's token shape and yields nothing on a real one -- so a wiki run
-- would either pick arbitrary bands or trip the strictly-increasing-df assertion
-- below.  Filter on token SHAPE instead: alphanumeric, 4..30 characters, which
-- excludes punctuation fragments, single letters and the pathological long tokens
-- that appear in scraped text, and works unchanged on both corpora.
CREATE TEMP TABLE freq AS
  SELECT w AS term, count(*) AS df
    FROM docs, unnest(string_to_array(lower(content),' ')) w
   WHERE w ~ '^[a-z0-9]{4,30}$'
   GROUP BY w;
-- Targets are FRACTIONS of the corpus, not absolute counts.  With absolute
-- targets, a 200k-row corpus whose most frequent term has df 39,360 assigned
-- both "mid" (nearest 25,000) and "common" (the maximum) to the SAME term -- so
-- two of the three latency bands measured an identical query and the comparison
-- silently lost a dimension.  At 2M rows these fractions reproduce the pg_fts
-- bands closely (rare 10,875 / mid 24,097 / common 734,896).
INSERT INTO bands
  SELECT 'rare', term, df FROM freq
   ORDER BY abs(df - (SELECT count(*)*0.005 FROM docs)), term LIMIT 1;
INSERT INTO bands
  SELECT 'mid', term, df FROM freq
   WHERE term <> (SELECT term FROM bands WHERE band='rare')
   ORDER BY abs(df - (SELECT count(*)*0.02 FROM docs)), term LIMIT 1;
INSERT INTO bands
  SELECT 'common', term, df FROM freq
   WHERE term NOT IN (SELECT term FROM bands)
   ORDER BY df DESC, term LIMIT 1;
-- Assert three DISTINCT bands with strictly increasing df.  A collision or an
-- empty band means the corpus is too small or too flat for the requested bands,
-- and benchmarking it anyway produces numbers that look fine and mean nothing.
DO $$
DECLARE n int; nd int; r bigint; m bigint; c bigint;
BEGIN
  SELECT count(*), count(DISTINCT term) INTO n, nd FROM bands WHERE df > 0;
  IF n <> 3 OR nd <> 3 THEN
    RAISE EXCEPTION 'band selection produced % rows / % distinct terms, need 3/3', n, nd;
  END IF;
  SELECT df INTO r FROM bands WHERE band='rare';
  SELECT df INTO m FROM bands WHERE band='mid';
  SELECT df INTO c FROM bands WHERE band='common';
  IF NOT (r < m AND m < c) THEN
    RAISE EXCEPTION 'bands not strictly increasing in df: rare=% mid=% common=%', r, m, c;
  END IF;
END $$;
SELECT band, term, df FROM bands ORDER BY df;
SQL
    say "corpus loaded, sha256=$sha"
}

# The fingerprint the analyzer compares across hosts.
#
# Hashes the per-row md5s rather than the concatenated corpus.  The obvious form,
# md5(string_agg(content, ...)), materialises the ENTIRE corpus as one datum: at
# 2M x 120 words that is 2.6 GB, which exceeds MaxAllocSize and failed a whole
# three-host run with "out of memory" during the measure phase -- after 22 minutes
# of provisioning, loading and indexing. Per-row md5 keeps the aggregate at 32
# bytes per row (64 MB at 2M rows, 320 MB at 10M), and the ORDER BY id makes it
# just as sensitive to any difference in content or row set.  md5 over the ordered
# content, so any difference in what was indexed is caught mechanically instead
# of being inferred from divergent match counts a month later.
FINGERPRINT_SQL="SELECT md5(string_agg(h, '' ORDER BY id)) FROM (SELECT id, md5(content) AS h FROM docs) t"

band() {
    # PATH must be exported here too: this runs in a fresh shell per verb, and
    # the first attempt failed with "psql: command not found" only AFTER the
    # index had been built, wasting the whole provisioning phase.
    export PATH="$NVME/pg/bin:$PATH"
    psql -X -q -t -A -d "$PGDATABASE" -c "SELECT term FROM bands WHERE band='$1'"
}

timed_index() {
    local ddl=$1
    export PATH="$NVME/pg/bin:$PATH"
    local t0 t1
    t0=$(date +%s.%N)
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" -c "$ddl"
    t1=$(date +%s.%N)
    echo "$(echo "$t1 - $t0" | bc)" > "$OUTDIR/build.txt"
    say "index built in $(cat "$OUTDIR/build.txt")s"
}

prewarm() {
    export PATH="$NVME/pg/bin:$PATH"
    psql -X -q -d "$PGDATABASE" -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm" >/dev/null 2>&1 || true
    for r in "$@"; do
        psql -X -q -t -A -d "$PGDATABASE" -c "SELECT pg_prewarm('$r')" >/dev/null 2>&1 || true
    done
    psql -X -q -t -A -d "$PGDATABASE" -c "SELECT count(*) FROM docs" >/dev/null
}

run_measure() {
    local spec=$1 samples=${2:-200} warmup=${3:-10} run=${4:-adhoc}
    export PATH="$NVME/pg/bin:$PATH"
    python3 "$HOME/compete/lib/wbench.py" measure --dsn "$PGDATABASE" \
        --spec "$spec" --out "$OUTDIR/raw.jsonl" \
        --samples "$samples" --warmup "$warmup" --run "$run"
}

run_gate() {
    local spec=$1
    export PATH="$NVME/pg/bin:$PATH"
    python3 "$HOME/compete/lib/wbench.py" gate --dsn "$PGDATABASE" \
        --spec "$spec" --out "$OUTDIR/gates.jsonl"
}
