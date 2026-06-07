#!/usr/bin/env python3
"""Generate test/oracle/test.zim — the fixture the sqllogictests assert against.

Requires: pip install libzim  (same C++ core as the extension links).
Run from repo root:  python3 test/oracle/make_fixture.py
Keep this in sync with the expected values in test/sql/read_zim.test.
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


def main(out="test/oracle/test.zim"):
    with Creator(out).config_indexing(True, "eng") as c:
        c.add_item(HtmlItem("A/Photosynthesis", "Photosynthesis",
                            "<html><body><h1>Photosynthesis</h1><p>Plants convert light.</p>"
                            "<a href='./Chlorophyll'>cl</a></body></html>"))
        c.add_item(HtmlItem("A/Chlorophyll", "Chlorophyll",
                            "<html><body><h1>Chlorophyll</h1><p>Green pigment in plants.</p></body></html>"))
        c.add_item(HtmlItem("A/Calcium", "Calcium",
                            "<html><body><h1>Calcium</h1></body></html>"))
        c.add_item(HtmlItem("I/logo.png", "logo", b"\x89PNG\r\n\x1a\n fakepng", mime="image/png"))
        c.add_item(HtmlItem("style.css", "style", "body{color:green}", mime="text/css"))
        c.add_redirection("A/Photosynth", "Photosynth (redirect)", "A/Photosynthesis", {})
        c.set_mainpath("A/Photosynthesis")
        c.add_metadata("Title", "Test Botany ZIM")
        c.add_metadata("Language", "eng")
        c.add_metadata("Creator", "oracle")
        c.add_metadata("Date", "2026-06-06")
        c.add_metadata("Description", "tiny test archive")
    print("wrote", out)


if __name__ == "__main__":
    import sys
    main(*(sys.argv[1:2] or ["test/oracle/test.zim"]))
