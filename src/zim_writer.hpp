//===----------------------------------------------------------------------===//
// zim_writer.hpp
//
// Thin wrapper over libzim's writer, mirroring zim_access.hpp on the read side.
// All zim::writer contact for the extension is confined to zim_writer.cpp; the
// DuckDB binding layer (copy_to_zim.cpp) consumes only the plain structs below
// and never touches a zim:: type directly.
//
// Ownership note that drives the whole design: libzim's Creator is a PULL
// interface -- Item::getContentProvider() is called on libzim's own worker
// threads, long after addItem() returns. DuckDB's sink is PUSH and recycles its
// DataChunk vectors the moment it returns. So AddItem() must COPY content into
// storage libzim owns; zim::writer::StringItem does exactly that.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace duckdb {
namespace zim_ext {

// One entry to write. Field names mirror ZimEntry on the read side so the
// round trip reads as an identity.
struct ZimWriteEntry {
	std::string path;
	std::string title;
	std::string mimetype;
	std::string content;
	bool is_redirect = false;
	std::string redirect_path;
	// Hints are tri-state: absent means "let libzim decide from the mimetype".
	bool has_front_article = false;
	bool front_article = false;
	bool has_compress = false;
	bool compress = false;
};

// Archive-level settings, all optional. Defaults match libzim's own.
struct ZimWriterConfig {
	std::string compression = "zstd"; // zstd | lzma | none
	uint64_t cluster_size = 0;        // 0 = libzim default
	uint32_t workers = 4;
	bool index = false;
	std::string index_language;
	std::string main_path;
	std::map<std::string, std::string> metadata;
	std::string illustration; // raw 48x48 PNG bytes; empty = none
};

// RAII wrapper around zim::writer::Creator.
//
// Construction starts the archive (the output file is created immediately).
// Finish() completes it. If the object is destroyed WITHOUT a successful
// Finish(), the partial output is left on disk -- deleting it is the caller's
// job, because only the caller knows the path policy. See copy_to_zim.cpp.
class ZimWriter {
public:
	ZimWriter(const std::string &out_path, const ZimWriterConfig &config);
	~ZimWriter();
	ZimWriter(const ZimWriter &) = delete;
	ZimWriter &operator=(const ZimWriter &) = delete;

	// Throws on a duplicate path (libzim zim::InvalidEntry). The caller detects
	// duplicates first so the error names the SQL problem, not a libzim internal.
	void AddItem(const ZimWriteEntry &entry);

	// Applies main_path and metadata, then finalizes. After this returns the
	// archive on disk is complete and valid.
	void Finish();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace zim_ext
} // namespace duckdb
