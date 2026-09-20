/* pg_weave 0.10.0 -> 0.11.0 */

-- Z6: regex is decided on the DICTIONARY, exactly, and one new column says so.
--
-- WHAT CHANGED UNDERNEATH.  A regex leaf (`/re/`) used to be served by the trigram
-- funnel: a literal-run scanner over the raw pattern text picked "required"
-- trigrams, the weft's ordinal sets for those trigrams were UNIONed into a candidate
-- term set, and every candidate ROW was then rechecked on the heap.  Two defects,
-- one of each kind.  Correctness: the scanner read `\d` as the literal `d`, so
-- `/ab\dcd/` demanded a trigram no matching term contains and the index returned NO
-- rows for a document containing `ab5cd` -- a false negative (G32) that
-- a recheck cannot repair, because a recheck only removes rows.  Cost: a pattern
-- with no literal run of three (`/e12[0-9]{2}/`) had nothing to funnel and fell back
-- to rechecking every document in the segment, ~5 s per query on 1M rows, whether
-- or not the index had a trigram weft.
--
-- The regex is a predicate on TERMS.  It is now compiled once per (segment, leaf)
-- with core's own engine -- pg_regcomp under REG_ADVANCED and the C collation, the
-- same call the heap predicate makes -- and run over the segment's dictionary
-- terms, so the index and `to_wdoc(...) @@@ '/re/'` agree by construction and no
-- heap recheck is needed.  When the index was built WITH (trigrams = on), the
-- pg_tre extractor's CNF of required trigrams (src/query/extract.c, which parses
-- the pattern rather than scanning it) is evaluated over the weft with
-- INTERSECTION to decide which terms the engine is asked about; the narrowing is
-- refused for every pattern where pg_tre's dialect and core's ARE could disagree
-- (`\d`, `\y`, `(?i)`, `[[:digit:]]`, ...), because a wrongly required trigram is
-- exactly the G32 failure again.
--
-- THE NEW COLUMN, AND WHY regex_trgm DID NOT CHANGE MEANING.  regex_dict counts
-- leaves SERVED BY THE DICTIONARY WALK -- once per segment per leaf, like
-- fuzzy_dict.  regex_trgm still means "the trigram weft narrowed the candidates",
-- which is what the funnel's success path meant; it is now incremented by the walk
-- when its narrowing was applied.  The two are therefore NOT alternatives: a
-- narrowable pattern on a trigrams=on index increments BOTH, and the same pattern
-- on a trigrams=off index, or a refused pattern on either, increments regex_dict
-- alone.  A regex query with regex_dict = 0 did not go through the index at all.
-- The columns fuzzy_dict/fuzzy_trgm ARE alternatives (a fuzzy term goes to the
-- automaton walk OR, if too long for it, to the funnel), so read the two pairs
-- differently.
--
-- WHY DROP + CREATE rather than CREATE OR REPLACE.  A function whose whole result is
-- OUT parameters has those parameters as its return type, and PostgreSQL refuses to
-- change the return type of an existing function -- so adding a column to
-- weave_channel_stats() cannot be done in place.  Nothing in the extension depends
-- on it (no view, no default, no other function), so the drop is safe; a user who
-- has built a view over it will see the dependency error here rather than a silently
-- changed result shape, which is the right failure.
DROP FUNCTION weave_channel_stats();

CREATE FUNCTION weave_channel_stats(
        OUT lex_term bigint,
        OUT prefix_dict bigint,
        OUT prefix_surf bigint,
        OUT fuzzy_dict bigint,
        OUT fuzzy_trgm bigint,
        OUT fuzzy_surf bigint,
        OUT regex_dict bigint,
        OUT regex_trgm bigint,
        OUT regex_surf bigint,
        OUT vector_scan bigint,
        OUT terms_expanded bigint,
        OUT dict_pages bigint,
        OUT surf_loads bigint,
        OUT surf_bytes bigint,
        OUT surf_cache_hits bigint,
        OUT surf_cache_misses bigint,
        OUT surf_cache_evicts bigint,
        OUT surf_cache_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_channel_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_channel_stats() IS
    'which mechanism inside a weave index served this backend''s query leaves';
