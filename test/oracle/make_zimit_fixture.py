#!/usr/bin/env python3
"""Generate test/oracle/test_zimit.zim — a second fixture exercising the edge cases
test.zim doesn't:

  * zimit / browsertrix-style **full-URL content paths** (`example.com/about`,
    with dots, multiple slashes, and trailing-slash "directory" pages) rather
    than mwoffliner's `A/Foo`. Verifies read_zim and the zim:// filesystem
    address these correctly.
  * **No fulltext index** (config_indexing is not enabled), so
    `zim_info(...).has_fulltext_index` is false and `zim_search` returns no rows
    rather than erroring.

Kept separate from make_fixture.py so test.zim's verified counts don't move.
Requires: pip install libzim. Run from repo root:
    python3 test/oracle/make_zimit_fixture.py
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


def main(out="test/oracle/test_zimit.zim"):
    # NOTE: config_indexing is intentionally NOT called -> no Xapian fulltext index.
    with Creator(out) as c:
        c.add_item(
            HtmlItem(
                "example.com/",
                "Home",
                "<html><body><h1>Home</h1><p>Welcome.</p></body></html>",
            )
        )
        c.add_item(
            HtmlItem(
                "example.com/about",
                "About",
                "<html><body><h1>About</h1><p>About us.</p></body></html>",
            )
        )
        # a trailing-slash "directory" page, deep path
        c.add_item(
            HtmlItem(
                "example.com/products/widget/",
                "Widget",
                "<html><body><h1>Widget</h1><p>A fine widget.</p></body></html>",
            )
        )
        c.add_item(
            HtmlItem(
                "example.com/static/style.css",
                "style",
                "body{color:blue}",
                mime="text/css",
            )
        )
        c.set_mainpath("example.com/")
        c.add_metadata("Title", "Test Zimit ZIM")
        c.add_metadata("Language", "eng")
        # a 48px illustration (fake PNG bytes) so zim_illustration has something to return
        c.add_illustration(48, b"\x89PNG\r\n\x1a\n fake48illustration")
    print("wrote", out)


if __name__ == "__main__":
    import sys

    main(*(sys.argv[1:2] or ["test/oracle/test_zimit.zim"]))
