# libzim overlay port (patched)

This is a **temporary** vcpkg overlay port: the upstream registry `libzim` port,
plus one extra patch that adds a public reader API libzim doesn't ship yet.

## What the extra patch does

`stream-reader-api.patch` adds `zim::IRandomAccessReader` (a `getSize()` +
`readAt(dest, offset, size)` interface) and `Archive` constructors that take a
`std::shared_ptr<IRandomAccessReader>`. This lets the extension open a ZIM from
arbitrary storage — specifically a remote HTTP/S3 object read with byte-range
requests — so a query fetches only the bytes it touches instead of downloading
the whole archive.

Limitations of the reader path (by design): single-part archives only, and
full-text (Xapian) search is unavailable (the Xapian index is opened via a
direct file descriptor, which a custom reader can't provide — search returns no
results rather than failing). All other access works normally.

## How this overlay is built

`vcpkg.json` / `portfile.cmake` are copies of the upstream registry port for
libzim **9.7.0** (carrying its existing `cross-builds.diff`, `dllexport.diff`,
`subdirs.diff`), with `stream-reader-api.patch` appended to the `PATCHES` list
and `port-version` bumped to `1`. The repo's top-level `vcpkg.json` references
this directory via `vcpkg-configuration.overlay-ports: ["./vcpkg_ports", ...]`.

## Regenerating `stream-reader-api.patch`

The patch is `git diff` of a libzim fork branched from the `9.7.0` tag:

```sh
git clone --branch 9.7.0 https://github.com/openzim/libzim.git
cd libzim && git checkout -b stream-reader-api
# ... apply the change (new include/zim/irandomaccessreader.h, StreamFileReader
#     in file_reader.{h,cpp}, FileImpl reader ctor + guards, Archive ctors,
#     meson wiring) ...
git commit -am "Add IRandomAccessReader"
git diff 9.7.0 HEAD > stream-reader-api.patch
```

It must apply cleanly with `git apply -p1` **after** the three upstream diffs
(vcpkg applies `PATCHES` in listed order). The patch touches only
`include/zim/{archive.h,irandomaccessreader.h,meson.build}` and
`src/{archive.cpp,file_reader.*,fileimpl.*,irandomaccessreader.cpp,meson.build}`,
none of which the upstream diffs touch, so there is no overlap.

## Removing this overlay

Once a libzim release includes the `IRandomAccessReader` API (or an equivalent
upstream reader hook), delete this directory and drop `./vcpkg_ports` from the
top-level `vcpkg.json` `overlay-ports`. The registry port (at a new enough
baseline) then provides everything.
