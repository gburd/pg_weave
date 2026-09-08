# ci/noop.pl -- a trivially passing TAP file.
#
# PGXS decides whether to run the TAP stage with `ifdef TAP_TESTS`, so overriding
# TAP_TESTS= on the command line does NOT disable it.  Pointing PROVE_TESTS here
# makes the stage a no-op for jobs that gate REGRESS + ISOLATION only, without
# editing the Makefile.
use strict;
use warnings;
use Test::More tests => 1;
ok(1, 'noop');
