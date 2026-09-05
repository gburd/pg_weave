# meson.build for pg_weave (Windows / MSVC out-of-tree build)
#
# The primary build path is the PGXS Makefile (Linux, macOS, *BSD). This
# meson recipe exists only for Windows/MSVC, where PostgreSQL itself is built
# with meson and PGXS is unavailable.
#
# PREREQUISITE: an MSVC-built PostgreSQL >= 17 built from source with the same
# Visual Studio toolchain (`meson setup ... && ninja install`). A
# MinGW/Strawberry PostgreSQL will NOT work: it ships a GCC-flavored pg_config.h
# that MSVC cannot consume, and no postgres.lib backend import library to link
# against.
#
# Build (from an "x64 Native Tools" prompt, or after vcvars64.bat):
#   meson setup build -Dpg_dir=C:/pgsql --buildtype=release
#   ninja -C build
#   ninja -C build install   # pg_weave.dll -> <pg>/lib, .control/.sql -> share/extension
#
# Note: build recipe only; the regress/isolation/TAP harness on Windows is
# separate PGXS-style plumbing, add when installcheck actually runs on Windows.

project('pg_weave', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'])

fs = import('fs')
pg_dir = get_option('pg_dir')            # e.g. C:/pgsql (MSVC-built PG >= 17)

# A meson/nixpkgs-built PostgreSQL installs headers under
# include/postgresql/server and extension files under share/postgresql/extension;
# the older/MSVC layout uses include/server and share/extension. Detect which.
if fs.is_dir(pg_dir / 'include' / 'postgresql' / 'server')
  pg_include_server = pg_dir / 'include' / 'postgresql' / 'server'
  pg_extension_dir  = pg_dir / 'share' / 'postgresql' / 'extension'
else
  pg_include_server = pg_dir / 'include' / 'server'
  pg_extension_dir  = pg_dir / 'share' / 'extension'
endif
pg_lib = pg_dir / 'lib'

cc = meson.get_compiler('c')

# The backend import library. An MSVC PG build ships postgres.lib here.
postgres_lib = cc.find_library('postgres', dirs: [pg_lib], required: true)

incdirs = include_directories(
  pg_include_server,                     # <postgres.h>, fmgr.h, ...
  pg_include_server / 'port' / 'win32',  # win32 port shims
  pg_include_server / 'port' / 'win32_msvc',
  pg_dir / 'include',
  'include',                             # weave/*.h (RELAYOUT: headers moved here)
)

# Mirrors the Makefile OBJS list. src/am/amscan.c, src/query/lev.c and
# src/pages/trgm_page.c are #included into src/am/am.c (unity build, not
# separate TUs), so they are intentionally NOT listed here.
sources = files(
  'src/query/analyze.c',
  'src/query/tsanalyze.c',
  'src/query/doc.c',
  'src/query/parse.c',
  'src/query/rank.c',
  'src/am/am.c',
  'src/am/customscan.c',
  'src/am/amaux.c',
  'src/util/migrate.c',
  'src/query/trgm.c',
  'src/util/sparsemap.c',
  'src/query/match.c',
)

# -DWIN32 + the CRT quiet macro; /wd4267 /wd4244 silence the size_t/__int64 ->
# int narrowing warnings exactly as PostgreSQL's own build suppresses them.
c_args = [
  '-DWIN32',
  '-D_CRT_SECURE_NO_WARNINGS',
  '/wd4267',
  '/wd4244',
]

pg_weave = shared_module('pg_weave',
  sources,
  include_directories: incdirs,
  dependencies: [postgres_lib],
  c_args: c_args,
  name_prefix: '',                       # -> pg_weave.dll, not libpg_weave.dll
  install: true,
  install_dir: pg_lib,
)

# Control + SQL script into the extension dir (layout auto-detected above).
install_data('pg_weave.control', install_dir: pg_extension_dir)
install_data('sql/pg_weave--0.1.0.sql', install_dir: pg_extension_dir)
