# Phase B progress — remote Xapian range-read glass reader

Decision: private vcpkg overlay (Xapian 1.4.22).

## DONE
- Xapian patch in ~/Projects/xapian-core (baseline commit ba20235), mirrored to
  vcpkg_ports/xapian/randomaccessreader.patch (~520 lines/18 files): public
  Xapian::RandomAccessReader (read_at/get_size) + Database(reader) ctor; reader
  threaded through GlassDatabase->GlassVersion+7 tables; read_block & GlassVersion::read
  route via reader; readahead no-op; reuses single-file -3-fd handle (dummy fd);
  header in include/Makefile.mk install list.
- PROOF PASSED: Database(reader)==Database(fd) byte-identical (doccount+docids+pct)
  on extracted restarters index, all query shapes. Progs: /tmp/xq_base.cpp,
  /tmp/xq_reader.cpp, /tmp/extract_index.cpp.
- Overlay port vcpkg_ports/xapian/ wired (PATCHES + port-version 4); git apply
  --check vs fresh tarball clean.

## TODO
1. libzim adapter (extend stream-reader-api.patch): Xapian::RandomAccessReader over
   libzim reader. Index offset = FileImpl::getBlobOffset(clusterIdx,blobIdx); size =
   cluster->getBlobSize. read_at -> zimReader->readAt(buf, base+pos, len). In
   loadXapianDb reader branch: if uncompressed, Database(adapter) not temp-copy.
   Adapter must outlive Database (GlassTable holds RandomAccessReader*); XapianDb
   wrapper owns it.
2. Extension: index>cap -> range reader instead of throw.
3. Scale test: medicine 2.2GB/166MB index over snape tunnel; expect ~0.5MB/cold
   query. Range server: see [[zim-test-fixtures]] (range_server.py + ssh -L).
4. Full vcpkg rebuild (binary_sources clear).

## Iterate: xapian baseline ba20235; regenerate patch via git diff ba20235 HEAD --
<source pathspecs>. Standalone: ./configure --enable-static --disable-shared
--disable-documentation && make -j. libzim: ~/Projects/libzim branch stream-reader-api.
