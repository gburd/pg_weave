/* pg_weave 0.2.0 -> 0.3.0
 *
 * No SQL-visible changes.  0.3.0 is the release that made the lexical channel
 * measurable: the ported upstream speedups, the benchmark harness, and the first
 * recorded numbers against tsvector + GIN (bench/RESULTS_LEXICAL.md).
 *
 * This file exists so the upgrade chain stays continuous.  An extension whose
 * version advances without an upgrade script cannot be upgraded in place, and
 * PostgreSQL will refuse `ALTER EXTENSION ... UPDATE` with "no update path".
 */

\echo Use "ALTER EXTENSION pg_weave UPDATE TO '0.3.0'" to load this file. \quit

-- Intentionally empty.
