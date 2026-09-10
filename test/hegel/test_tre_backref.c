/*
 * test/hegel/test_tre_backref.c -- pg_tre 1521662 regression: TRE backref
 * wrong-answer fix (upstream 2f7dcec, "Fix backtracking restart so backrefs
 * scan later start positions", regression case added in e0d2777).
 *
 * lib/tre-match-backtrack.c's end-of-string check used the CURRENT
 * backtracking position (next_c/pos) instead of the position at the START
 * of the current search attempt (next_c_start/pos_start), so the
 * backtracker gave up scanning for a match at a later start offset even
 * when a real match existed there.  That is a silent wrong answer: fewer
 * matches than a correct engine, no error, no crash.
 *
 * Compiled twice by test/hegel/run_tre_bump.sh -- once against the TRE pin
 * this repo carried before the bump (d0e0c997, buggy), once against the
 * current vendor/tre (fixed) -- against the exact upstream regression case:
 * pattern "(.{1,3})\1" against "foo".  Expected (see e0d2777):
 *
 *   whole match  = [1,3)  ("oo")
 *   group 1      = [1,2)  ("o")
 *
 * The buggy pin returns REG_NOMATCH for this pattern/input pair -- not a
 * crash, a wrong answer -- which is exactly why a fixed-output regression
 * test alone would not have caught this: there is no wrong output to
 * compare against, only a MISSING one.
 */
#include <stdio.h>
#include <string.h>
#include "tre.h"

int
main(void)
{
	regex_t preg;
	regmatch_t pmatch[4];
	int rc;

	memset(pmatch, 0, sizeof(pmatch));
	rc = tre_regcomp(&preg, "(.{1,3})\\1", REG_EXTENDED);
	if (rc != REG_OK)
	{
		fprintf(stderr, "tre_regcomp failed: %d\n", rc);
		return 2;
	}
	rc = tre_regexec(&preg, "foo", 4, pmatch, 0);
	tre_regfree(&preg);

	/*
	 * Machine-readable single line; test/hegel/run_tre_bump.sh parses this
	 * rather than re-implementing regex-match assertions in shell.
	 */
	printf("rc=%d whole_so=%d whole_eo=%d g1_so=%d g1_eo=%d\n",
		   rc,
		   (int) pmatch[0].rm_so, (int) pmatch[0].rm_eo,
		   (int) pmatch[1].rm_so, (int) pmatch[1].rm_eo);
	return 0;
}
