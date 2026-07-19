#!/usr/bin/env python3
"""Generate test/oracle/test_counter_malformed.zim.

The libzim writer auto-generates the "Counter" metadata (well-formed) and refuses
a manual override, so a malformed Counter can't be written directly. Instead we
build the archive UNCOMPRESSED (so the metadata string is stored verbatim in the
file), then byte-patch the Counter value to a same-length malformed string. This
lets test/sql/zim_metadata.test assert that ZimArchive::Counter() skips malformed
segments (no '=', non-numeric value) and still parses the valid ones (issue #19).

Requires: pip install libzim.  Run from repo root.
"""

from libzim.writer import Creator, Item, StringProvider, Hint, Compression
from libzim.reader import Archive


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


def main(out="test/oracle/test_counter_malformed.zim"):
    # Compression.none -> the Counter metadata string is stored verbatim, so we
    # can byte-patch it below.
    with Creator(out).config_compression(Compression.none) as c:
        c.add_item(HtmlItem("A/a", "a", "<html><body>a</body></html>"))
        c.add_item(HtmlItem("A/b", "b", "<html><body>b</body></html>"))
        c.add_item(HtmlItem("style.css", "s", "body{}", mime="text/css"))
        c.set_mainpath("A/a")
        c.add_metadata("Title", "counter malformed fixture")
        c.add_metadata("Language", "eng")

    raw = open(out, "rb").read()
    original = Archive(out).get_metadata("Counter").decode()  # e.g. "text/css=1;text/html=2"
    orig_b = original.encode()
    L = len(orig_b)

    # A same-length malformed replacement: one valid segment, one with no '=',
    # one with a non-numeric value, padded with empty (skipped) segments.
    base = b"kept/x=42;noeq;bad/y=z"
    if len(base) > L:
        raise SystemExit(f"original Counter '{original}' ({L}B) too short for the malformed payload ({len(base)}B)")
    malformed = base + b";" * (L - len(base))
    assert len(malformed) == L

    idx = raw.find(orig_b)
    if idx < 0:
        raise SystemExit(f"Counter string '{original}' not found verbatim (was the archive compressed?)")
    patched = raw[:idx] + malformed + raw[idx + L :]
    open(out, "wb").write(patched)

    check = Archive(out).get_metadata("Counter").decode()
    print(f"original Counter: {original!r}")
    print(f"patched  Counter: {check!r}")
    assert check == malformed.decode()
    print("wrote", out)


if __name__ == "__main__":
    import sys

    main(*(sys.argv[1:2] or ["test/oracle/test_counter_malformed.zim"]))
