#!/usr/bin/env python3
"""Generate test/oracle/test_zh.zim — a tiny Chinese-language fixture.

Its only job is to exercise the search-open path for a language Xapian cannot
stem (Chinese). Upstream libzim printed "No stemming for language 'zh'" to
std::cout when opening such an archive's Searcher; our libzim overlay patch
drops that print (issue #21). test/no_stdout_pollution.sh asserts searching this
archive emits nothing on stdout but the query result.

Requires: pip install libzim  (same C++ core the extension links).
Run from repo root:  python3 test/oracle/make_zh_fixture.py
"""

from libzim.writer import Creator, Item, StringProvider, Hint


class HtmlItem(Item):
    def __init__(self, path, title, content, mime="text/html"):
        super().__init__()
        self._p, self._t, self._c, self._m = path, title, content, mime

    def get_path(self):
        return self._p

    def get_title(self):
        return self._t

    def get_mimetype(self):
        return self._m

    def get_contentprovider(self):
        return StringProvider(self._c)

    def get_hints(self):
        return {Hint.FRONT_ARTICLE: self._m == "text/html"}


def main(out="test/oracle/test_zh.zim"):
    # config_indexing(True, "zho"): build a Xapian fulltext index tagged Chinese,
    # the language Xapian has no stemmer for -- exactly what triggers the upstream
    # stdout print on the read side.
    with Creator(out).config_indexing(True, "zho") as c:
        c.add_item(
            HtmlItem(
                "A/矩阵",
                "矩阵",
                "<html><body><h1>矩阵</h1><p>矩阵是一个按照长方阵列排列的复数或实数集合。</p></body></html>",
            )
        )
        c.add_item(
            HtmlItem(
                "A/三角矩阵",
                "三角矩阵",
                "<html><body><h1>三角矩阵</h1><p>三角矩阵是一种特殊的方块矩阵。</p></body></html>",
            )
        )
        c.add_redirection("A/矩陣", "矩陣 (redirect)", "A/矩阵", {})
        c.set_mainpath("A/矩阵")
        c.add_metadata("Title", "测试数学 ZIM")
        c.add_metadata("Language", "zho")
        c.add_metadata("Creator", "oracle")
        c.add_metadata("Date", "2026-07-18")
        c.add_metadata("Description", "tiny Chinese-language test archive (#21)")
    print("wrote", out)


if __name__ == "__main__":
    import sys

    main(*(sys.argv[1:2] or ["test/oracle/test_zh.zim"]))
