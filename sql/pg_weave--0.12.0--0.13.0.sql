/* pg_weave 0.12.0 -> 0.13.0 */

-- Z8: `cgram`, the CORPUS character-trigram channel.
--
-- WHAT IT IS.  A second, lexical-SHAPED weft whose vocabulary is the byte
-- trigrams of a raw text column, with docid posting lists: it answers UNANCHORED
-- CROSS-TOKEN substring search, the `LIKE '%tion refu%'` shape that no
-- token-oriented index can serve, because the match spans a token boundary and
-- is anchored to nothing.  See include/weave/cgram.h for the on-disk shape and
-- why it must not be confused with the VOCABULARY trigram weft in
-- src/pages/trgm_page.c, which maps trigrams to dictionary TERM ORDINALS.
--
-- WHAT IT COSTS, stated here because a migration is where a DBA meets a feature.
-- doc/specs/FUZZY_CHANNEL.md sect. 6 commits in writing that WITH `cgram` ON,
-- pg_weave DOES NOT CLAIM TO BE SMALLER THAN pg_trgm.  The measured corpus
-- trigram population is 58.1 (trigram, document) pairs per document, i.e. 58
-- million postings at a million rows; a pg_trgm GIN over the same column is
-- 72 MB.  Comparable bytes, and the honest side-by-side is
-- bench/RESULTS_CGRAM.md.  The channel is off unless you ask for it by adding a
-- gram_ops column, which is what "opt-in" means here -- not that the capability
-- is optional (AGENTS.md says it is required), but that the BYTES are.

-- The operator class.  STORAGE text and no support procedures: the weave AM has
-- amsupport = 0 and every opclass in it exists to NAME A CHANNEL, which is what
-- weave_index_layout() reads (the discriminator is the opclass, not the column
-- type -- a text column is not by itself a cgram column).
--
-- It is `gram_ops` and not `text_weave_ops` because the family answers "which
-- channel", not "which type", and text is exactly the type a future second text
-- channel would also want.  It is also the name include/weave/am.h's
-- WeaveIndexLayout block comment already uses as its worked example.
CREATE OPERATOR CLASS gram_ops
    FOR TYPE text USING weave AS
    STORAGE text;

-- The two operator procedures.
--
-- The VALUE is core's: these call textlike() and texticlike(), the same C
-- functions `LIKE` and `ILIKE` call.  That is what makes the parity gate in
-- sql/cgram.sql meaningful -- it asserts the index arm returns EXACTLY the ids a
-- forced sequential scan's LIKE returns, and a hand-written matcher here would be
-- testing our matcher against our matcher.
--
-- IMMUTABLE and PARALLEL SAFE for the same reason LIKE is: a pure function of two
-- byte strings under the database's own case rules.
CREATE FUNCTION weave_cgram_like(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'weave_cgram_like'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION weave_cgram_ilike(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'weave_cgram_ilike'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

-- WHY DEDICATED OPERATORS AND NOT `~~` / `~~*` THEMSELVES.  Putting core's LIKE
-- operator into the family would make EVERY LIKE on the column a candidate for
-- this index, including anchored ones the planner already has btree-oriented
-- special-case machinery for, and including patterns whose cost this AM's
-- estimator does not model.  The measurement says the win is narrow (selective
-- cross-token patterns: 4-23 ms against 155 ms) and the cost is broad (index
-- bytes), so the surface is deliberately opt-in at the QUERY level too: a query
-- asks for this channel by name.  `col @~ pattern` means exactly
-- `col LIKE pattern`, and `@~*` exactly `col ILIKE pattern`.
--
-- No COMMUTATOR: the pattern side is not indexable and `'pat' @~ col` is not a
-- form anyone writes.  No NEGATOR: `NOT (col @~ p)` selects nearly everything and
-- an index path for it would be a pessimization the planner cannot see.
CREATE OPERATOR @~ (
    LEFTARG    = text,
    RIGHTARG   = text,
    PROCEDURE  = weave_cgram_like,
    RESTRICT   = contsel,
    JOIN       = contjoinsel
);

CREATE OPERATOR @~* (
    LEFTARG    = text,
    RIGHTARG   = text,
    PROCEDURE  = weave_cgram_ilike,
    RESTRICT   = contsel,
    JOIN       = contjoinsel
);

-- Strategies 1 and 2 of gram_ops.  They are NOT the same numbers as
-- wdoc_lex_ops's @@@ (1) and <=> (2), and they do not have to be: sk_strategy is
-- resolved against the INDEX COLUMN's operator family, so the scan disambiguates
-- by attribute.  weave_rescan() checks the column's WeaveWeftKind before it reads
-- a scan key's argument, for the reason its ORDER BY dispatch already documents:
-- reading a text datum as a wquery is not a type error, it is a walk through
-- garbage.
--
-- They must be 1 and 2 rather than, say, 11 and 12 because ALTER OPERATOR FAMILY
-- validates the strategy number against the access method's amstrategies (3).
ALTER OPERATOR FAMILY gram_ops USING weave ADD
    OPERATOR 1 @~ (text, text),
    OPERATOR 2 @~* (text, text);

COMMENT ON OPERATOR @~ (text, text) IS
    'LIKE, answerable from a weave index''s corpus character-trigram weft';
COMMENT ON OPERATOR @~* (text, text) IS
    'ILIKE, answerable from a weave index''s corpus character-trigram weft (ASCII patterns only; a non-ASCII pattern falls back)';

-- weave_channel_stats() gains cgram_scan.
--
-- WHY DROP + CREATE rather than CREATE OR REPLACE, repeated from the 0.10.0 ->
-- 0.11.0 script because it is the kind of thing that gets "simplified" once a
-- release: a function whose whole result is OUT parameters has those parameters
-- AS ITS RETURN TYPE, and PostgreSQL refuses to change an existing function's
-- return type -- so adding a column cannot be done in place.  Nothing in the
-- extension depends on it (no view, no default, no other function), so the drop
-- is safe; a user who has built a view over it sees the dependency error here
-- rather than a silently changed result shape, which is the right failure.
--
-- cgram_scan counts the times the CORPUS-TRIGRAM ROUTE ACTUALLY SERVED a `@~` /
-- `@~*` restriction in this backend -- once per scan, so a test can assert
-- exactly 1.  A ZERO after such a query is not a failure: it means the pattern
-- FELL BACK because it has no literal run of three or more bytes (`'%ab%'`, a
-- pattern of nothing but wildcards) or because it was case-insensitive over
-- non-ASCII bytes, where our byte-wise ASCII fold cannot be trusted against
-- ILIKE's encoding-aware one.  A fallback is a correct slow answer.
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
        OUT surf_cache_bytes bigint,
        OUT cgram_scan bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_channel_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_channel_stats() IS
    'which mechanism inside a weave index served this backend''s query leaves';
