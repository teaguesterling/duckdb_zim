//===----------------------------------------------------------------------===//
// zim_writer.cpp — the ONLY translation unit that includes <zim/writer/*>.
//===----------------------------------------------------------------------===//
#include "zim_writer.hpp"

#include <zim/writer/creator.h>
#include <zim/writer/item.h>
#include <zim/zim.h>

#include <stdexcept>

namespace duckdb {
namespace zim_ext {

namespace {

// NOTE: libzim 9.7.0's zim::Compression enum (zim/zim.h) has only two live
// enumerators -- None = 1 and Zstd = 5. The "intermediate values" that used
// to name LZMA/other compressors are documented in that header as "no longer
// supported" and were removed as named enumerators. There is no
// zim::Compression::Lzma to reference in this libzim version, unlike what the
// brief assumed. "lzma" is therefore rejected with a clear error rather than
// left as a dangling reference to a nonexistent enumerator.
zim::Compression ParseCompression(const std::string &name) {
	if (name == "zstd") {
		return zim::Compression::Zstd;
	}
	if (name == "none") {
		return zim::Compression::None;
	}
	if (name == "lzma") {
		throw std::runtime_error("compression 'lzma' is not supported by this libzim build (9.7.0 only "
		                         "exposes 'zstd' and 'none'); expected zstd or none");
	}
	throw std::runtime_error("unknown compression '" + name + "'; expected zstd or none");
}

} // namespace

struct ZimWriter::Impl {
	zim::writer::Creator creator;
	ZimWriterConfig config;
};

ZimWriter::ZimWriter(const std::string &out_path, const ZimWriterConfig &config) : impl(new Impl()) {
	impl->config = config;
	impl->creator.configVerbose(false);
	impl->creator.configCompression(ParseCompression(config.compression));
	impl->creator.configNbWorkers(config.workers);
	if (config.cluster_size > 0) {
		impl->creator.configClusterSize(static_cast<zim::size_type>(config.cluster_size));
	}
	if (config.index) {
		impl->creator.configIndexing(true, config.index_language);
	}
	impl->creator.startZimCreation(out_path);
}

ZimWriter::~ZimWriter() = default;

void ZimWriter::AddItem(const ZimWriteEntry &entry) {
	zim::writer::Hints hints;
	if (entry.has_front_article) {
		hints[zim::writer::FRONT_ARTICLE] = entry.front_article ? 1 : 0;
	}
	if (entry.has_compress) {
		hints[zim::writer::COMPRESS] = entry.compress ? 1 : 0;
	}
	if (entry.is_redirect) {
		impl->creator.addRedirection(entry.path, entry.title, entry.redirect_path, hints);
		return;
	}
	// StringItem::create copies `content` into storage libzim owns, which is what
	// makes the pull/push mismatch safe (see the header comment).
	impl->creator.addItem(
	    zim::writer::StringItem::create(entry.path, entry.mimetype, entry.title, hints, entry.content));
}

bool ZimWriterHasFulltextIndexing() {
#ifdef LIBZIM_WITH_XAPIAN
	return true;
#else
	return false;
#endif
}

void ZimWriter::Finish() {
	for (auto &kv : impl->config.metadata) {
		impl->creator.addMetadata(kv.first, kv.second);
	}
	if (!impl->config.illustration.empty()) {
		impl->creator.addIllustration(48, impl->config.illustration);
	}
	if (!impl->config.main_path.empty()) {
		impl->creator.setMainPath(impl->config.main_path);
	}
	impl->creator.finishZimCreation();
}

} // namespace zim_ext
} // namespace duckdb
