//===----------------------------------------------------------------------===//
// test/vfs_ranged_reads.cpp — direct FileSystem-level coverage of the zim://
// handle's ranged reads (#27).
//
// Why this is not a .test file: the ranged/positional read path
// (FileSystem::Read(handle, buf, n, location)) has no SQL consumer over zim://
// today. read_blob/read_text/read_csv all take the sequential overload, and the
// one DuckDB reader that does positional reads — parquet — never gets as far as
// reading, because it asks for GetLastModifiedTime first and the zim:// VFS does
// not implement it (a pre-existing gap, unrelated to #27). So the exact
// semantics this change is about — reads at an offset, at EOF, spanning the end,
// zero-length — are only reachable through the C++ FileSystem API.
//
// Driven by test/vfs_ranged_reads.sh, which compiles this against the built
// libduckdb and runs it.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using duckdb::FileFlags;
using duckdb::FileSystem;

static int failures = 0;

static void Check(bool ok, const std::string &what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what.c_str());
		failures++;
	}
}

// Reads the whole entry the way read_blob does (one sequential read of the full
// size), for use as the reference bytes every ranged read is compared against.
static std::string WholeFile(FileSystem &fs, const std::string &url) {
	auto handle = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
	const auto size = handle->GetFileSize();
	std::string out(size, '\0');
	if (size > 0) {
		const auto got = handle->Read(&out[0], size);
		Check(static_cast<duckdb::idx_t>(got) == size, "whole-file read returned the full size");
	}
	return out;
}

// True if the callable threw — the positional read contract is "out of range is
// an error", and this change must not have relaxed that.
template <class F>
static bool Throws(F &&f) {
	try {
		f();
	} catch (...) {
		return true;
	}
	return false;
}

static void TestEntry(FileSystem &fs, const std::string &url, const std::string &label) {
	const std::string expected = WholeFile(fs, url);
	const auto size = expected.size();
	fprintf(stderr, "  %s: %zu bytes\n", label.c_str(), size);

	auto handle = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
	Check(static_cast<size_t>(handle->GetFileSize()) == size, label + ": GetFileSize matches");

	// --- positional reads at every offset, in a size that is not a divisor of
	// the length, so windows straddle whatever internal boundary exists.
	{
		std::string got(size, '\0');
		const size_t chunk = 4093; // prime, deliberately not a power of two
		for (size_t off = 0; off < size; off += chunk) {
			const size_t n = std::min(chunk, size - off);
			handle->Read(&got[off], n, off);
		}
		Check(got == expected, label + ": ranged reads at successive offsets reassemble the entry");
	}

	// --- a single read at an arbitrary interior offset
	if (size > 100) {
		const size_t off = size / 3;
		const size_t n = size / 4;
		std::string got(n, '\0');
		handle->Read(&got[0], n, off);
		Check(got == expected.substr(off, n), label + ": single read at an interior offset");
	}

	// --- the last byte, and the last window, exactly at the end
	if (size > 0) {
		char last = '\0';
		handle->Read(&last, 1, size - 1);
		Check(last == expected[size - 1], label + ": read of the final byte");

		const size_t n = std::min<size_t>(64, size);
		std::string got(n, '\0');
		handle->Read(&got[0], n, size - n);
		Check(got == expected.substr(size - n), label + ": read of the window ending exactly at EOF");
	}

	// --- zero-length reads are not errors, anywhere in range (including at EOF)
	{
		char dummy = 'Z';
		Check(!Throws([&] { handle->Read(&dummy, 0, 0); }), label + ": zero-length read at 0 succeeds");
		Check(!Throws([&] { handle->Read(&dummy, 0, size); }), label + ": zero-length read at EOF succeeds");
		Check(dummy == 'Z', label + ": zero-length read wrote nothing");
	}

	// --- reads that leave the entry are errors, exactly as before this change
	{
		std::vector<char> buf(64, 0);
		Check(Throws([&] { handle->Read(buf.data(), 1, size); }), label + ": 1 byte at EOF is an error");
		Check(Throws([&] { handle->Read(buf.data(), 2, size ? size - 1 : 0); }),
		      label + ": a window spanning the end is an error");
		Check(Throws([&] { handle->Read(buf.data(), 1, size + 1000); }), label + ": far past EOF is an error");
		Check(Throws([&] { handle->Read(buf.data(), 8, ~duckdb::idx_t(0) - 3); }),
		      label + ": an offset that would overflow location+nr_bytes is an error");
	}

	// --- sequential reads in an awkward chunk size, crossing every boundary
	{
		auto seq = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
		std::string got;
		std::vector<char> buf(1021, 0); // prime again
		while (true) {
			const auto n = seq->Read(buf.data(), buf.size());
			if (n <= 0) {
				break;
			}
			got.append(buf.data(), static_cast<size_t>(n));
		}
		Check(got == expected, label + ": sequential 1021-byte reads reassemble the entry");
		Check(seq->Read(buf.data(), 1) == 0, label + ": a read past EOF clamps to 0 rather than erroring");
	}

	// --- Seek / SeekPosition / Reset still drive the sequential cursor
	if (size > 10) {
		auto seq = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
		seq->Seek(size - 5);
		Check(seq->SeekPosition() == size - 5, label + ": SeekPosition reports the seek");
		std::vector<char> buf(32, 0);
		const auto n = seq->Read(buf.data(), 32);
		Check(n == 5, label + ": a read after Seek clamps to what remains");
		Check(std::string(buf.data(), 5) == expected.substr(size - 5), label + ": the clamped tail is correct");
		seq->Reset();
		Check(seq->SeekPosition() == 0, label + ": Reset rewinds to 0");
		const auto m = seq->Read(buf.data(), 4);
		Check(m == 4 && std::string(buf.data(), 4) == expected.substr(0, 4), label + ": reading from the top again");
	}
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <zim.duckdb_extension> <archive.zim> [content-path ...]\n", argv[0]);
		return 2;
	}
	duckdb::DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
	duckdb::DuckDB db(nullptr, &config);
	duckdb::Connection con(db);

	auto load = con.Query("LOAD '" + std::string(argv[1]) + "'");
	if (load->HasError()) {
		fprintf(stderr, "FAIL: could not load the zim extension: %s\n", load->GetError().c_str());
		return 1;
	}

	auto &fs = FileSystem::GetFileSystem(*db.instance);
	const std::string archive = argv[2];
	for (int i = 3; i < argc; i++) {
		TestEntry(fs, "zim://" + archive + "/" + argv[i], argv[i]);
	}

	if (failures > 0) {
		fprintf(stderr, "%d assertion(s) failed\n", failures);
		return 1;
	}
	fprintf(stderr, "all ranged-read assertions passed\n");
	return 0;
}
