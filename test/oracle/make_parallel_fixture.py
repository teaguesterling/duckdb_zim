#!/usr/bin/env python3
"""Generate test/oracle/test_parallel.zim — a many-entry fixture for the parallel
scan stress test (issue #19).

read_zim's ParallelScan dispenses cluster-order morsels of 4096 content entries to
DuckDB threads, which then read the SAME pooled libzim Archive concurrently. To
actually exercise that (multiple in-flight morsels -> concurrent reads), the
archive needs well over 4096 content entries. This writes N tiny HTML articles so
test/sql/zim_parallel.test can scan it with many threads and assert a deterministic
count -- and, under a THREADSAN=1 build, surface any data race in the shared read.

Requires: pip install libzim.  Run from repo root.
"""

from libzim.writer import Creator, Item, StringProvider, Hint

N = 12000  # ~3 morsels of 4096 -> concurrent reads across threads


class HtmlItem(Item):
    def __init__(self, path, title, content):
        super().__init__()
        self._p, self._t, self._c = path, title, content

    def get_path(self):
        return self._p

    def get_title(self):
        return self._t

    def get_mimetype(self):
        return "text/html"

    def get_contentprovider(self):
        return StringProvider(self._c)

    def get_hints(self):
        return {Hint.FRONT_ARTICLE: True}


def main(out="test/oracle/test_parallel.zim"):
    with Creator(out).config_indexing(False, "eng") as c:
        for i in range(N):
            c.add_item(HtmlItem(f"A/{i:06d}", f"art{i:06d}", f"<html><body><p>article {i}</p></body></html>"))
        c.set_mainpath("A/000000")
        c.add_metadata("Title", "parallel scan fixture")
        c.add_metadata("Language", "eng")
    print(f"wrote {out} ({N} entries)")


if __name__ == "__main__":
    import sys

    main(*(sys.argv[1:2] or ["test/oracle/test_parallel.zim"]))
