#!/usr/bin/env python3
"""Generate test/oracle/test_headings.zim.

Two cases that test.zim cannot cover, kept in a SEPARATE archive on purpose:
test.zim is referenced by ten test files with hard entry-count assertions, and
its per-entry digests are pinned by zim_get_text_golden.test, so adding entries
to it would ripple through all of them for no gain.

  1. A/Nested  -- an article with h1/h2/h2/h3. test.zim's articles carry a
     single <h1>, so any table-of-contents assertion against them has exactly
     one entry and would pass an implementation with broken level arithmetic or
     ordering. Reported by the duckeye session, whose -T check needs this.

  2. A/BadUtf8 -- a text/* entry whose bytes are NOT valid UTF-8 (Latin-1 0xE9).
     This is the VARCHAR-vs-BLOB boundary: an entry that is declared text but
     cannot be decoded. test.zim's binary entry (image/png) is refused on
     mimetype before decoding is ever attempted, so it does not reach this path.
     test_zh.zim covers non-ASCII that IS valid UTF-8; this covers the other side.

Requires: pip install libzim  (same C++ core as the extension links).
Run from repo root:  python3 test/oracle/make_headings_fixture.py
Keep in sync with the expected values in test/sql/zim_headings_utf8.test.
"""

from libzim.writer import Creator, Item, StringProvider, Hint


class RawItem(Item):
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


# Deliberately nested: one h1, two sibling h2s, an h3 under the second h2.
# A reader that flattens levels, drops siblings, or misorders them produces a
# different tree from this and a single-heading fixture would not notice.
NESTED = (
    "<html><body>"
    "<h1>Roots</h1><p>Top.</p>"
    "<h2>Alpha</h2><p>First branch.</p>"
    "<h2>Beta</h2><p>Second branch.</p>"
    "<h3>Beta One</h3><p>Leaf under Beta.</p>"
    "</body></html>"
)

# 0xE9 is 'e' acute in Latin-1 and an invalid standalone byte in UTF-8.
# Declared text/plain so it is NOT refused on mimetype the way a binary entry is.
BAD_UTF8 = b"caf\xe9 na\xefve"


def main(out="test/oracle/test_headings.zim"):
    with Creator(out).config_indexing(True, "eng") as c:
        c.add_item(RawItem("A/Nested", "Nested", NESTED))
        c.add_item(RawItem("A/BadUtf8", "BadUtf8", BAD_UTF8, mime="text/plain"))
        c.set_mainpath("A/Nested")
        c.add_metadata("Title", "Headings and invalid UTF-8")
        c.add_metadata("Language", "eng")
        c.add_metadata("Creator", "oracle")
        c.add_metadata("Date", "2026-09-11")
        c.add_metadata("Description", "nested headings + a text entry that is not valid UTF-8")
    print("wrote", out)


if __name__ == "__main__":
    import sys

    main(*(sys.argv[1:2] or ["test/oracle/test_headings.zim"]))
