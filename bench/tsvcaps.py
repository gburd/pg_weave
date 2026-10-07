#!/usr/bin/env python3
"""bench/tsvcaps.py -- corpus helper for bench/tsvcaps_job.sh (M7 step 1).

STDLIB ONLY, for the same reason as bench/prepdata.py: it runs on a bare
Debian instance.

  tsvcaps.py wiki URL OUT [--limit N] [--chunk W --chunk-out OUT2] [--cache DIR]
  tsvcaps.py agree RUN_A RUN_B [--groups QID_GROUP.tsv] [--label L]

Downloads one enwiki `pages-articles-multistream<k>.xml-p<a>p<b>.bz2` part
(cached under --cache), keeps main-namespace non-redirect pages, strips the
wikitext with regular expressions, and writes `id<TAB>text` lines to OUT.
With --chunk W --chunk-out OUT2 each article is ALSO cut into consecutive
W-word windows written to OUT2 (row id = pageid*10000 + window), so the same
text is measured whole and chunked from one pass over the dump.

The strip is regex-based and deliberately crude (templates, tables, refs,
tags, link syntax, emphasis quotes, headings).  What it leaves is prose plus
some markup debris, which is fine for this measurement: the question is how
many tokens a document has and how often a lexeme repeats, not the quality
of the article text.  It is NOT a general-purpose wikitext renderer.
"""

import argparse
import bz2
import os
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET

RE_COMMENT = re.compile(r"<!--.*?-->", re.S)
RE_REF = re.compile(r"<ref[^>/]*/>|<ref[^>]*>.*?</ref>", re.S | re.I)
RE_TEMPLATE = re.compile(r"\{\{[^{}]*\}\}")
RE_TABLE = re.compile(r"\{\|[^{}]*?\|\}", re.S)
RE_FILE = re.compile(r"\[\[(?:File|Image|Category):[^\[\]]*\]\]", re.I)
RE_LINK = re.compile(r"\[\[(?:[^\[\]|]*\|)?([^\[\]]*)\]\]")
RE_EXT = re.compile(r"\[https?://[^\s\]]*\s?([^\]]*)\]")
RE_TAG = re.compile(r"<[^>]{0,200}>")
RE_QUOTES = re.compile(r"'{2,}")
RE_HEAD = re.compile(r"^=+\s*(.*?)\s*=+\s*$", re.M)
RE_WS = re.compile(r"\s+")
RE_CTRL = re.compile(r"[\x00-\x1f\x7f]")


def die(msg):
    print("tsvcaps.py: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


def fetch(url, cache):
    os.makedirs(cache, exist_ok=True)
    dest = os.path.join(cache, url.rsplit("/", 1)[-1])
    if os.path.exists(dest):
        return dest
    tmp = dest + ".part"
    print("downloading %s" % url, file=sys.stderr)
    req = urllib.request.Request(url, headers={"User-Agent": "pg_weave-bench/1 (tsvcaps)"})
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
    os.replace(tmp, dest)
    return dest


def strip_wikitext(t):
    t = RE_COMMENT.sub(" ", t)
    t = RE_REF.sub(" ", t)
    # templates and tables nest; peel innermost-first until nothing changes
    for _ in range(20):
        n = RE_TEMPLATE.sub(" ", t)
        n = RE_TABLE.sub(" ", n)
        if n == t:
            break
        t = n
    t = RE_FILE.sub(" ", t)
    t = RE_LINK.sub(r"\1", t)
    t = RE_EXT.sub(r"\1", t)
    t = RE_TAG.sub(" ", t)
    t = RE_QUOTES.sub("", t)
    t = RE_HEAD.sub(r"\1", t)
    t = RE_CTRL.sub(" ", t)
    return RE_WS.sub(" ", t).strip()


def pages(path):
    """Yield (pageid, wikitext) for ns=0 non-redirect pages."""
    with bz2.BZ2File(path) as f:   # multistream: BZ2File reads all streams
        ctx = ET.iterparse(f, events=("end",))
        for _, el in ctx:
            tag = el.tag.rsplit("}", 1)[-1]
            if tag != "page":
                continue
            ns = pid = text = None
            redirect = False
            for c in el:
                ct = c.tag.rsplit("}", 1)[-1]
                if ct == "ns":
                    ns = c.text
                elif ct == "id":
                    pid = c.text
                elif ct == "redirect":
                    redirect = True
                elif ct == "revision":
                    for r in c:
                        if r.tag.rsplit("}", 1)[-1] == "text":
                            text = r.text or ""
            el.clear()
            if ns == "0" and not redirect and pid and text:
                yield int(pid), text


def cmd_wiki(a):
    path = fetch(a.url, a.cache)
    n = nchunk = 0
    cf = open(a.chunk_out + ".part", "w", encoding="utf-8", newline="\n") if a.chunk else None
    with open(a.out + ".part", "w", encoding="utf-8", newline="\n") as out:
        for pid, wt in pages(path):
            body = strip_wikitext(wt)
            if not body:
                continue
            n += 1
            out.write("%d\t%s\n" % (pid, body))
            if cf:
                words = body.split(" ")
                for k in range(0, len(words), a.chunk):
                    nchunk += 1
                    cf.write("%d\t%s\n" % (pid * 10000 + k // a.chunk,
                                           " ".join(words[k:k + a.chunk])))
            if a.limit and n >= a.limit:
                break
    os.replace(a.out + ".part", a.out)
    if cf:
        cf.close()
        os.replace(a.chunk_out + ".part", a.chunk_out)
    print("articles=%d chunks=%d -> %s" % (n, nchunk, a.out), file=sys.stderr)


def read_run(path):
    """run.tsv (qid, docid, rank) -> {qid: [docid in rank order]}."""
    runs = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            q, d, r = line.rstrip("\n").split("\t")
            runs.setdefault(q, []).append((int(r), d))
    return {q: [d for _, d in sorted(v)] for q, v in runs.items()}


def cmd_agree(a):
    """Rank agreement between two runs over the same queries; needs no qrels.

    ov10 / ov100: |top-k(A) & top-k(B)| / |top-k(A)|, a query whose A list is
    empty is skipped.  same10: fraction of queries whose top-10 is the SAME
    LIST IN THE SAME ORDER.  A query present in A and absent from B counts as
    zero overlap (B returned nothing), never as skipped."""
    ra, rb = read_run(a.run_a), read_run(a.run_b)
    groups = {}
    if a.groups:
        with open(a.groups, encoding="utf-8") as f:
            for line in f:
                q, g = line.rstrip("\n").split("\t")
                groups.setdefault(q, []).append(g)   # a query may be in several
    acc = {}
    for q, la in ra.items():
        lb = rb.get(q, [])
        for g in ["all"] + groups.get(q, []):
            s = acc.setdefault(g, [0, 0.0, 0.0, 0])
            s[0] += 1
            s[1] += len(set(la[:10]) & set(lb[:10])) / len(la[:10])
            s[2] += len(set(la[:100]) & set(lb[:100])) / len(la[:100])
            s[3] += la[:10] == lb[:10]
    for g in sorted(acc, key=lambda x: (x != "all", x)):
        n, o10, o100, same = acc[g]
        print("%s\t%s\t%d\t%.4f\t%.4f\t%.4f"
              % (a.label, g, n, o10 / n, o100 / n, same / n))


def main():
    p = argparse.ArgumentParser()
    sp = p.add_subparsers(dest="cmd", required=True)
    w = sp.add_parser("wiki")
    w.add_argument("url")
    w.add_argument("out")
    w.add_argument("--limit", type=int, default=0)
    w.add_argument("--chunk", type=int, default=0)
    w.add_argument("--chunk-out", default="")
    w.add_argument("--cache", default="/scratch/tsvcaps/_cache")
    g = sp.add_parser("agree")
    g.add_argument("run_a")
    g.add_argument("run_b")
    g.add_argument("--groups", default="")
    g.add_argument("--label", default="agree")
    a = p.parse_args()
    if a.cmd == "agree":
        cmd_agree(a)
    if a.cmd == "wiki":
        if a.chunk and not a.chunk_out:
            die("--chunk needs --chunk-out")
        if a.chunk and a.chunk < 10:
            die("--chunk must be >= 10 words (window index is packed into the row id)")
        cmd_wiki(a)


if __name__ == "__main__":
    main()
