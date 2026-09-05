/* pg_weave--0.3.6.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_weave" to load this file. \quit

--
-- wdoc: an analyzed full-text document (terms + term frequencies).
-- to_wdoc() records per-term token positions (wdoc format v2), which the
-- phrase-query syntax ("a b c") relies on.
--
CREATE TYPE wdoc;

CREATE FUNCTION wdoc_in(cstring)
RETURNS wdoc
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wdoc_out(wdoc)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wdoc_recv(internal)
RETURNS wdoc
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wdoc_send(wdoc)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE wdoc (
    INPUT          = wdoc_in,
    OUTPUT         = wdoc_out,
    RECEIVE        = wdoc_recv,
    SEND           = wdoc_send,
    INTERNALLENGTH = VARIABLE,
    STORAGE        = extended
);

--
-- wquery: a parsed boolean query.  Supports boolean operators, phrase
-- syntax ("a b c"), prefix (term*), fuzzy (term~k) and regex (/re/) terms.
--
CREATE TYPE wquery;

CREATE FUNCTION wquery_in(cstring)
RETURNS wquery
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wquery_out(wquery)
RETURNS cstring
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wquery_recv(internal)
RETURNS wquery
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wquery_send(wquery)
RETURNS bytea
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE wquery (
    INPUT          = wquery_in,
    OUTPUT         = wquery_out,
    RECEIVE        = wquery_recv,
    SEND           = wquery_send,
    INTERNALLENGTH = VARIABLE,
    STORAGE        = extended
);

--
-- Constructors from text.
--
CREATE FUNCTION to_wdoc(text)
RETURNS wdoc
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION to_wquery(text)
RETURNS wquery
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Analyzer that reuses an installed text search configuration (pg_ts_config):
-- the configured parser + dictionary chain (stemming, stopwords, synonyms,
-- thesaurus) is applied, rather than the built-in simple tokenizer.
CREATE FUNCTION to_wdoc(regconfig, text)
RETURNS wdoc
AS 'MODULE_PATHNAME', 'to_wdoc_byid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Build an wdoc directly from an existing tsvector (no re-analysis of source
-- text): the adoption on-ramp for a table that already materializes a tsvector
-- column.  A tsvector's lexemes are sorted+distinct with ascending positions,
-- exactly wdoc's shape; positions are kept iff every lexeme has them.
CREATE FUNCTION to_wdoc(tsvector)
RETURNS wdoc
AS 'MODULE_PATHNAME', 'to_wdoc_from_tsvector'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- to_wquery(regconfig, text): parse query text AND normalize each plain term
-- through the given text search configuration (stemming, case, stopwords), so
-- query terms match the same lexemes an index built with the same config
-- stores.  Prefix (term*), fuzzy (term~k) and regex (/re/) terms stay literal.
-- Use this (not the raw text->wquery cast) whenever the indexed wdoc was
-- built with to_wdoc(regconfig, ...).
CREATE FUNCTION to_wquery(regconfig, text)
RETURNS wquery
AS 'MODULE_PATHNAME', 'to_wquery_byid'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- Support functions.
--
CREATE FUNCTION wdoc_length(wdoc)
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- The @@@ match operator.
--
CREATE FUNCTION weave_match(wdoc, wquery)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION weave_match_commutator(wquery, wdoc)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR @@@ (
    LEFTARG    = wdoc,
    RIGHTARG   = wquery,
    PROCEDURE  = weave_match,
    COMMUTATOR = @@@,
    RESTRICT   = tsmatchsel,
    JOIN       = tsmatchjoinsel
);

CREATE OPERATOR @@@ (
    LEFTARG    = wquery,
    RIGHTARG   = wdoc,
    PROCEDURE  = weave_match_commutator,
    COMMUTATOR = @@@,
    RESTRICT   = tsmatchsel,
    JOIN       = tsmatchjoinsel
);

--
-- BM25 relevance scoring.
--
-- BM25 relevance score of a document against a query.
--   n_docs : total documents in the corpus (N)
--   avgdl  : average document length (in tokens)
--   dfs    : optional float8[] of per-query-term document frequencies, in the
--            order the query's distinct terms appear; NULL treats terms as rare
CREATE FUNCTION weave_bm25(wdoc, wquery, n_docs float8, avgdl float8,
                         dfs float8[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_bm25'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- BM25 with selectable variant and explicit k1/b, for reproducing reference
-- implementations (Lucene, Robertson/classic, ATIRE, BM25+).
CREATE FUNCTION weave_bm25_opts(wdoc, wquery,
                              n_docs float8, avgdl float8,
                              k1 float8, b float8,
                              variant text,
                              dfs float8[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_bm25_opts'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- BM25F: multi-field BM25.  Pass one wdoc per field (e.g. title, body), a
-- weight per field, and an avgdl per field.  Per-field term frequencies are
-- length-normalized per field and combined by weight before tf-saturation
-- (the Robertson/Zaragoza BM25F formulation).
CREATE FUNCTION weave_bm25f(docs wdoc[], query wquery,
                          weights float8[], n_docs float8, avgdls float8[],
                          dfs float8[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_bm25f'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

--
-- Highlighting and snippets.
--
-- Highlight query terms in the source text.
CREATE FUNCTION weave_highlight(doc text, query wquery,
                              pre text DEFAULT '<b>', post text DEFAULT '</b>')
RETURNS text
AS 'MODULE_PATHNAME', 'weave_highlight'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Best-matching window (snippet) of the source text.
CREATE FUNCTION weave_snippet(doc text, query wquery,
                            pre text DEFAULT '<b>', post text DEFAULT '</b>',
                            ellipsis text DEFAULT '...', max_tokens int DEFAULT 15)
RETURNS text
AS 'MODULE_PATHNAME', 'weave_snippet'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

--
-- tsquery migration.
--
-- Migration helper: mechanically convert a tsquery to an wquery.
-- & -> AND, | -> OR, ! -> NOT.  The phrase operator <-> degrades to AND with
-- a NOTICE (phrase search is a later stage).
CREATE FUNCTION tsquery_to_wquery(tsquery)
RETURNS wquery
AS 'MODULE_PATHNAME', 'tsquery_to_wquery'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Convenience cast so existing tsquery values flow into @@@ queries.
CREATE CAST (tsquery AS wquery)
    WITH FUNCTION tsquery_to_wquery(tsquery) AS ASSIGNMENT;

--
-- The weave index access method: an inverted index over an wdoc column that
-- answers the @@@ operator by bitmap scan and maintains corpus statistics.
-- The storage engine is segment-based: inserts flush to new segments, and a
-- size-tiered merge compacts them so query cost stays bounded.  An expression
-- index on to_wdoc(text_column) is the external-content model -- the text
-- lives in the base table and the index stores only postings.
--
CREATE FUNCTION weave_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD weave TYPE INDEX HANDLER weave_handler;

COMMENT ON ACCESS METHOD weave IS 'inverted index for full-text search with BM25 ranking';

-- Operator class: strategy 1 is @@@ (wdoc @@@ wquery).
CREATE OPERATOR CLASS wdoc_lex_ops
DEFAULT FOR TYPE wdoc USING weave AS
    OPERATOR 1 @@@ (wdoc, wquery);

--
-- Index-maintained corpus statistics, so BM25 can be scored from the values
-- the weave index keeps rather than caller-supplied guesses.
--
CREATE FUNCTION weave_index_stats(regclass,
                                OUT ndocs float8, OUT avgdl float8,
                                OUT nterms bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_index_stats'
LANGUAGE C STRICT PARALLEL SAFE;

-- Per-query-term document frequencies from the index (for weave_bm25's dfs arg).
CREATE FUNCTION weave_index_df(regclass, wquery)
RETURNS float8[]
AS 'MODULE_PATHNAME', 'weave_index_df'
LANGUAGE C STRICT PARALLEL SAFE;

-- Number of live segments in a weave index.  Useful for observing/tuning merge
-- behavior.
CREATE FUNCTION weave_index_nsegments(regclass)
RETURNS integer
AS 'MODULE_PATHNAME', 'weave_index_nsegments'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- Index-only BM25 top-k search.  Scores are computed entirely from the index
-- (postings, dictionary df/max-tf, metapage N/avgdl) with no heap access; the
-- result is the top-k (ctid, score) pairs.  Join back to the table on ctid to
-- fetch rows.  A WAND upper-bound prunes documents that cannot enter the top-k.
--
CREATE FUNCTION weave_search(index regclass, query wquery, k int DEFAULT 10,
                           OUT ctid tid, OUT score float8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'weave_search'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- Lexical anomaly detection: the top-k most anomalous documents in the index,
-- i.e. those containing globally RARE terms.  A document's anomaly score is the
-- max idf over its terms (driven by its single rarest term), idf being the same
-- rarity value BM25 uses: log(1 + (N-df+0.5)/(df+0.5)) on the GLOBAL df.  It is
-- cheap because only the LOW-df tail of the dictionary is walked -- common terms
-- are skipped before any posting is decoded, so this is not a full-corpus scan.
-- `max_df` caps which terms count as rare (only df <= max_df contribute); NULL
-- defaults to max(N/1000, 1).  Returns the heap ctid, the score, the rarest term
-- driving the doc, and that term's global df, ordered by score DESC limit k.
-- The ctids are index-resident heap pointers (like weave_search); join back to
-- the table and filter for visibility if needed.  Per-segment tombstones are
-- honored so deleted docs are not reported.
CREATE FUNCTION weave_anomalous_docs(index regclass, k int DEFAULT 100,
                                   max_df int DEFAULT NULL,
                                   OUT ctid tid, OUT score float8,
                                   OUT rarest_term text, OUT min_df int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'weave_anomalous_docs'
LANGUAGE C PARALLEL SAFE;

-- MVCC-correct count of documents matching a query, computed in bulk from the
-- weave index (visibility via the visibility map, heap probed only for
-- not-all-visible pages) without the per-tuple executor round-trips of a scan.
CREATE FUNCTION weave_count(regclass, wquery)
RETURNS bigint
AS 'MODULE_PATHNAME', 'weave_count'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- BM25 distance operator for ORDER BY.  distance = 1/(1+score), so ascending
-- distance is descending relevance.  When used against a weave index it drives
-- an index ordering scan (block-max WAND top-k) with no sort node.
--
CREATE FUNCTION weave_distance(wdoc, wquery)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_distance'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION weave_distance_commutator(wquery, wdoc)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_distance_commutator'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR <=> (
    LEFTARG    = wdoc,
    RIGHTARG   = wquery,
    PROCEDURE  = weave_distance,
    COMMUTATOR = <=>
);

CREATE OPERATOR <=> (
    LEFTARG    = wquery,
    RIGHTARG   = wdoc,
    PROCEDURE  = weave_distance_commutator,
    COMMUTATOR = <=>
);

-- Add the ORDER BY operator (strategy 2) to the weave operator class so
-- "ORDER BY col <=> query LIMIT k" uses an index ordering scan.
ALTER OPERATOR FAMILY wdoc_lex_ops USING weave ADD
    OPERATOR 2 <=> (wdoc, wquery) FOR ORDER BY pg_catalog.float_ops;

--
-- Pending-list / segment maintenance.  INSERT appends to an in-index pending
-- list; VACUUM (amvacuumcleanup) folds pending documents into the main
-- posting structure, and these functions do it on demand.
--
-- weave_merge(regclass): merge the pending list into the main segments.
CREATE FUNCTION weave_merge(regclass)
RETURNS boolean
AS 'MODULE_PATHNAME', 'weave_merge'
LANGUAGE C STRICT;

-- weave_vacuum(regclass): on-demand full compaction + truncation, reclaiming
-- the dead pages left by prior merges (shrinks the physical index file).
CREATE FUNCTION weave_vacuum(regclass)
RETURNS boolean
AS 'MODULE_PATHNAME', 'weave_vacuum'
LANGUAGE C STRICT;

-- Privilege model.  Most functions operate on values the caller already holds
-- (to_wdoc / to_wquery / weave_bm25* / weave_highlight / operators / I/O) and
-- stay executable by PUBLIC.  Two functions take an index by OID and emit its
-- internal content -- heap TIDs, scores, and (weave_anomalous_docs) indexed term
-- text -- which can widen content visibility past table-level permissions.
-- Revoke them from PUBLIC; the index owner (and superusers) keep access, and an
-- owner can grant them explicitly.  (The maintenance functions weave_merge /
-- weave_vacuum enforce ownership in C and additionally refuse to run during
-- recovery, so they need no REVOKE here.)
REVOKE ALL ON FUNCTION weave_search(regclass, wquery, int) FROM PUBLIC;
REVOKE ALL ON FUNCTION weave_anomalous_docs(regclass, int, int) FROM PUBLIC;

-- Field-targeted (weight-zone) search (v1.4.0).  Tag a sub-document with a
-- weight label A/B/C/D and concatenate labelled sub-documents so a query term
-- can restrict itself to a field zone (tsvector-style term:A).  Labels live in
-- the wdoc value only (no index format change, no REINDEX); a field-restricted
-- query is answered via the heap recheck.
CREATE FUNCTION to_wdoc(regconfig, text, "char")
RETURNS wdoc
AS 'MODULE_PATHNAME', 'to_wdoc_byid_weight'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION setwdocweight(wdoc, "char")
RETURNS wdoc
AS 'MODULE_PATHNAME', 'setwdocweight'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wdoc_concat(wdoc, wdoc)
RETURNS wdoc
AS 'MODULE_PATHNAME', 'wdoc_concat'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR || (
    LEFTARG = wdoc, RIGHTARG = wdoc, FUNCTION = wdoc_concat
);
