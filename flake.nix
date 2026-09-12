{
  description = "pg_weave — one index for BM25 text, vector similarity, fuzzy and regex search (weave index access method)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # PostgreSQL majors this extension supports and nixpkgs packages.
        # PG 17 and 18 are the released, packaged targets; 19/20 devel are
        # exercised in CI and on the buildfarm hosts, not from nixpkgs.
        pgVersions = {
          pg17 = pkgs.postgresql_17;
          pg18 = pkgs.postgresql_18 or pkgs.postgresql_17;
        };

        # nixpkgs' postgresql is built WITHOUT --enable-tap-tests and does not
        # ship the PostgreSQL::Test perl harness, so `make installcheck` silently
        # skips the t/*.pl TAP tests. This override rebuilds PostgreSQL with TAP
        # enabled (IPC::Run at configure time) and installs the harness, so the
        # tap-* checks below actually run t/*.pl. (--enable-tap-tests is a
        # configure flag, so this is a full PG rebuild; it is only pulled in by
        # the tap-* checks, not the normal build/installcheck.)
        pgTapFor = pg:
          pg.overrideAttrs (o: {
            configureFlags = (o.configureFlags or [ ]) ++ [ "--enable-tap-tests" ];
            nativeBuildInputs = (o.nativeBuildInputs or [ ])
              ++ [ pkgs.perl pkgs.perlPackages.IPCRun ];
            postInstall = (o.postInstall or "") + ''
              for d in "$NIX_BUILD_TOP"/*/src/test/perl; do
                if [ -d "$d/PostgreSQL" ]; then
                  mkdir -p "$out/lib/perl5"
                  cp -r "$d/PostgreSQL" "$out/lib/perl5/"
                fi
              done
            '';
          });

        # Build pg_weave against a given postgresql package via its bundled PGXS.
        # In nixpkgs the pg_config binary lives in the `.pg_config` output.
        buildFor = postgresql:
          pkgs.stdenv.mkDerivation {
            pname = "pg_weave";
            # Read from the control file rather than hardcoded, so a version bump
            # is one edit instead of two that can silently disagree.
            version = builtins.head (builtins.match
              ".*default_version = '([^']+)'.*"
              (builtins.readFile ./pg_weave.control));
            src = ./.;

            nativeBuildInputs = [ postgresql.pg_config pkgs.clang ];
            buildInputs = [ postgresql ];

            # PGXS honours PG_CONFIG; point it at this postgresql. nixpkgs builds
            # PostgreSQL with clang, so PGXS defaults CC=clang — provide it.
            makeFlags = [ "PG_CONFIG=${postgresql.pg_config}/bin/pg_config" ];

            # Install into $out so nixpkgs' postgresql .withPackages can pick it up.
            # PGXS emits pg_weave.so (Linux) or pg_weave.dylib (macOS); glob covers both.
            installPhase = ''
              runHook preInstall
              install -D -m 755 -t $out/lib pg_weave.so 2>/dev/null || \
                install -D -m 755 -t $out/lib pg_weave.dylib
              install -D -m 644 -t $out/share/postgresql/extension pg_weave.control
              # Glob, never enumerate.  A hardcoded filename here silently drops
              # every newly added install or upgrade script: the build stays green,
              # `make install` outside Nix is correct, and only the regression suite
              # notices -- with "no update path from version X to Y", which points at
              # the SQL rather than at this line.
              install -D -m 644 -t $out/share/postgresql/extension sql/pg_weave--*.sql
              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "One index for BM25 text, vector similarity, fuzzy and regex search (weave index AM)";
              homepage = "https://codeberg.org/gregburd/pg_weave";
              license = licenses.postgresql;
              platforms = postgresql.meta.platforms;
            };
          };

        packages = builtins.mapAttrs (_: pg: buildFor pg) pgVersions;

        # A postgresql with pg_weave installed, for running the extension's own
        # regression/isolation/TAP suite the way `make installcheck` expects.
        pgWith = pg: pg.withPackages (_: [ (buildFor pg) ]);

        # `make installcheck` needs a running server + the test harness perl
        # module IPC::Run (for TAP) and the isolation tester (bundled with PG).
        checkFor = pg:
          pkgs.stdenv.mkDerivation {
            name = "pg_weave-installcheck-${pg.version}";
            src = ./.;
            nativeBuildInputs = [ (pgWith pg) pg.pg_config pkgs.perl pkgs.perlPackages.IPCRun ];
            dontInstall = true;
            buildPhase = ''
              export PGDATA=$TMPDIR/pgdata
              export PGHOST=$TMPDIR
              export PGPORT=5432
              initdb -U postgres --no-locale --encoding=UTF8 >/dev/null
              pg_ctl -D "$PGDATA" -o "-k $TMPDIR -c listen_addresses=localhost" -w start
              # REGRESS + ISOLATION run via PGXS installcheck against the running server.
              make installcheck \
                PG_CONFIG=${pg.pg_config}/bin/pg_config \
                PGHOST=$TMPDIR PGUSER=postgres PGPORT=$PGPORT \
                || { cat regression.diffs 2>/dev/null; cat output_iso/regression.diffs 2>/dev/null; exit 1; }
              pg_ctl -D "$PGDATA" -w stop
              touch $out
            '';
          };

        # TAP check: runs the t/*.pl tests (crash recovery, replication,
        # corruption, encodings, concurrency, segment-cap, vacuum-reclaim,
        # doclen sidecar), which the plain checkFor SKIPS because stock
        # nixpkgs postgresql lacks --enable-tap-tests + the perl harness. Uses
        # the pgTapFor override (TAP enabled, harness installed) with pg_weave
        # installed, and points PERL5LIB at the harness so PostgreSQL::Test
        # resolves. Each t/*.pl inits + tears down its own cluster.
        tapCheckFor = pg:
          let
            pgt = pgTapFor pg;
            pgWeave = buildFor pgt;
            # One prefix (symlinks, no rebuild) that has pgt's server binaries +
            # perl harness AND pg_weave's extension files. initdb/postgres run
            # from their real store paths (rpaths intact -- unlike a cp of the
            # prefix, which breaks them). A wrapped pg_config reports THIS prefix
            # so PostgreSQL::Test inits clusters that can CREATE EXTENSION pg_weave.
            pgtJoin = pkgs.symlinkJoin {
              name = "pg_weave-tap-prefix-${pg.version}";
              paths = [ pgWeave pgt ];
            };
            pgConfigWrapped = pkgs.writeShellScriptBin "pg_config" ''
              exec ${pgt.pg_config}/bin/pg_config "$@" | sed "s#${pgt}#${pgtJoin}#g"
            '';
          in pkgs.stdenv.mkDerivation {
            name = "pg_weave-tap-${pg.version}";
            src = ./.;
            nativeBuildInputs =
              [ pgtJoin pgConfigWrapped pkgs.perl pkgs.perlPackages.IPCRun ];
            dontInstall = true;
            buildPhase = ''
              # pgConfigWrapped (reporting the joined prefix) must win over the
              # join's own symlinked pg_config, so it goes first on PATH.
              export PATH=${pgConfigWrapped}/bin:${pgtJoin}/bin:$PATH
              export PERL5LIB=${pgt}/lib/perl5''${PERL5LIB:+:$PERL5LIB}
              # Run the t/*.pl TAP tests via prove against the TAP-enabled server.
              # PROVE_TESTS selects the pg_weave-behavior tests that run cleanly
              # in the nix build sandbox (corruption, multi-encoding,
              # concurrency, segment cap, vacuum reclaim, doclen sidecar,
              # format-v6 upgrade, chandesc corruption, and the v7 fuzzy
              # weft's crash-recovery + corruption pair). The replication test
              # (t/002) and the original crash-recovery test (t/001) use an
              # older PostgreSQL::Test idiom that the nixpkgs-shipped harness
              # rejects under the sandbox; they are gated in CI (real PG,
              # matching harness), not here. t/012 is a crash-recovery test that
              # DOES run here: it is written in t/011's idiom, because gate 7
              # ("a new weft that survives no crash test is a data-loss risk")
              # is not met by a test that only the CI runner ever executes.
              # This is what makes `nix flake check` actually exercise TAP
              # instead of silently skipping it.
              make installcheck REGRESS= ISOLATION= \
                PROVE_TESTS='t/003_corruption.pl t/004_encodings.pl t/005_concurrency.pl t/006_concurrent_extend.pl t/007_segment_cap.pl t/008_vacuum_reclaim.pl t/009_doclen_sidecar.pl t/010_format_v6_upgrade.pl t/011_chandesc_corruption.pl t/012_surf_crash_recovery.pl t/013_surf_corruption.pl t/014_merge_durability.pl' \
                PG_CONFIG=${pgConfigWrapped}/bin/pg_config \
                || { echo '--- TAP logs ---'; cat tmp_check/log/*.log tmp_check/log/regress_log_* 2>/dev/null; exit 1; }
              touch $out
            '';
          };
      in
      {
        packages = packages // {
          default = packages.pg17;
        };

        # `nix flake check` builds every PG target and runs its installcheck.
        checks = packages // {
          installcheck-pg17 = checkFor pgVersions.pg17;
          installcheck-pg18 = checkFor pgVersions.pg18;
          # TAP checks run t/*.pl (which the plain installcheck SKIPS: stock
          # nixpkgs PG is built without --enable-tap-tests and ships no perl
          # harness). Each rebuilds PostgreSQL with --enable-tap-tests (a
          # one-time ~20-min compile per major, then cached), installs the perl
          # harness, and runs the pg_weave-behavior TAP tests against it.
          tap-pg17 = tapCheckFor pgVersions.pg17;
          tap-pg18 = tapCheckFor pgVersions.pg18;
        };

        devShells.default = pkgs.mkShell {
          name = "pg_weave-dev";
          # PG 17 by default; `PG_CONFIG=$(pg18-config) make` to switch.
          packages = [
            pgVersions.pg17
            pgVersions.pg17.pg_config
            pkgs.gcc
            pkgs.gnumake
            pkgs.perl
            pkgs.perlPackages.IPCRun # TAP tests (t/*.pl)
            pkgs.clang-tools # clang-format / clangd for editing the C
          ];
          shellHook = ''
            echo "pg_weave dev shell — PostgreSQL ${pgVersions.pg17.version} on PATH"
            echo "  make PG_CONFIG=\$(command -v pg_config)      # build"
            echo "  nix flake check                              # build+test all PG majors"
          '';
        };

        formatter = pkgs.nixpkgs-fmt;
      });
}
