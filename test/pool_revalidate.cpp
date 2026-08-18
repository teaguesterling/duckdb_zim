//===----------------------------------------------------------------------===//
// test/pool_revalidate.cpp — ArchivePool revalidation (#38).
//
// Why this is not a .test file: the whole point is replacing a .zim on disk
// *between queries of one DatabaseInstance*, and neither half of that is
// reachable from sqllogictest. There is no file-manipulation directive, and
// COPY ... (FORMAT zim) deliberately refuses to overwrite an existing archive
// ("A ZIM is written once -- remove the file first"), which is exactly the
// refusal a shelf-refresh script sidesteps by writing elsewhere and renaming.
// Restarting the DB would clear the pool and hide the bug being tested.
//
// So this drives one DuckDB instance directly: build the archives with the
// extension's own writer, swap the file underneath, and re-query. It also
// covers the case SQL cannot express at all — a zim:// FileHandle held OPEN
// across the replacement, whose pinned libzim item must keep serving the bytes
// it was opened over.
//
// Driven by test/pool_revalidate.sh, which compiles this against the built
// libduckdb and runs it.
//===----------------------------------------------------------------------===//
#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using duckdb::Connection;
using duckdb::FileFlags;
using duckdb::FileSystem;

static int failures = 0;

static void Check(bool ok, const std::string &what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what.c_str());
		failures++;
	} else {
		fprintf(stderr, "  ok: %s\n", what.c_str());
	}
}

// Runs a query and returns its single VARCHAR cell; "<error: ...>" on failure so
// a thrown query shows up as a value mismatch rather than aborting the run.
static std::string Scalar(Connection &con, const std::string &sql) {
	auto res = con.Query(sql);
	if (res->HasError()) {
		return "<error: " + res->GetError() + ">";
	}
	auto chunk = res->Fetch();
	if (!chunk || chunk->size() == 0) {
		return "<no rows>";
	}
	if (chunk->GetValue(0, 0).IsNull()) {
		return "<null>";
	}
	return chunk->GetValue(0, 0).ToString();
}

static void Exec(Connection &con, const std::string &sql, const std::string &what) {
	auto res = con.Query(sql);
	if (res->HasError()) {
		fprintf(stderr, "FAIL: %s: %s\n", what.c_str(), res->GetError().c_str());
		failures++;
	}
}

static void CheckEq(Connection &con, const std::string &sql, const std::string &expected, const std::string &what) {
	const auto got = Scalar(con, sql);
	if (got != expected) {
		fprintf(stderr, "FAIL: %s\n    query:    %s\n    expected: %s\n    got:      %s\n", what.c_str(), sql.c_str(),
		        expected.c_str(), got.c_str());
		failures++;
	} else {
		fprintf(stderr, "  ok: %s\n", what.c_str());
	}
}

// Writes one archive with the extension's own writer. `rows` is a SELECT
// producing (path, content, mimetype).
static void WriteArchive(Connection &con, const std::string &rows, const std::string &path) {
	Exec(con, "COPY (" + rows + ") TO '" + path + "' (FORMAT zim);", "writing " + path);
}

// Replaces `dst` with `src` by rename — how a new archive normally lands
// (libzim's own Creator finishes with exactly this move), and the case where the
// inode changes under the pool.
static void ReplaceByRename(const std::string &src, const std::string &dst) {
	if (std::rename(src.c_str(), dst.c_str()) != 0) {
		fprintf(stderr, "FAIL: could not rename %s -> %s\n", src.c_str(), dst.c_str());
		failures++;
	}
}

// Replaces `dst`'s bytes in place, keeping the inode — a `cat new > old` refresh.
// Only (mtime, size) move here, so this is the case dev/inode alone would miss.
static void ReplaceInPlace(const std::string &src, const std::string &dst) {
	std::ifstream in(src, std::ios::binary);
	std::ofstream out(dst, std::ios::binary | std::ios::trunc);
	out << in.rdbuf();
	out.close();
	if (!out) {
		fprintf(stderr, "FAIL: could not overwrite %s in place\n", dst.c_str());
		failures++;
	}
	std::remove(src.c_str());
}

// Reads a whole zim:// entry through a freshly opened handle.
static std::string ReadWhole(FileSystem &fs, const std::string &url) {
	auto handle = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
	const auto size = handle->GetFileSize();
	std::string out(size, '\0');
	if (size > 0) {
		handle->Read(&out[0], size);
	}
	return out;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <zim.duckdb_extension> <workdir>\n", argv[0]);
		return 2;
	}
	const std::string ext = argv[1];
	const std::string dir = std::string(argv[2]);
	const std::string live = dir + "/live.zim";

	duckdb::DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
	duckdb::DuckDB db(nullptr, &config);
	Connection con(db);

	auto load = con.Query("LOAD '" + ext + "'");
	if (load->HasError()) {
		fprintf(stderr, "FAIL: could not load the zim extension: %s\n", load->GetError().c_str());
		return 1;
	}
	auto &fs = FileSystem::GetFileSystem(*db.instance);

	// --- three distinct archives, built by this extension's own writer ---------
	// v2 is deliberately a different SIZE and a different entry SET from v1, so a
	// revalidation that only noticed one of the two would still be caught.
	WriteArchive(con, "SELECT 'a.txt' AS path, 'version one'::BLOB AS content, 'text/plain' AS mimetype",
	             dir + "/v1.zim");
	WriteArchive(con,
	             "SELECT 'a.txt' AS path, 'version two, replaced in place'::BLOB AS content, "
	             "'text/plain' AS mimetype "
	             "UNION ALL SELECT 'b.txt', 'brand new entry'::BLOB, 'text/plain'",
	             dir + "/v2.zim");
	WriteArchive(con, "SELECT 'a.txt' AS path, 'version three'::BLOB AS content, 'text/plain' AS mimetype",
	             dir + "/v3.zim");

	const std::string q_entries = "SELECT string_agg(path, ',' ORDER BY path) FROM read_zim('" + live + "')";
	const std::string q_text = "SELECT zim_get_text('" + live + "', 'a.txt')";
	const std::string q_blob = "SELECT content::VARCHAR FROM read_blob('zim://" + live + "/a.txt')";
	const std::string q_glob = "SELECT count(*)::VARCHAR FROM read_blob('zim://" + live + "/*.txt')";

	// --- v1 in place, and warm every consumer's pooled handle -----------------
	ReplaceByRename(dir + "/v1.zim", live);
	CheckEq(con, q_entries, "a.txt", "v1: read_zim lists a.txt");
	CheckEq(con, q_text, "version one", "v1: zim_get_text reads v1");
	CheckEq(con, q_blob, "version one", "v1: zim:// reads v1");
	CheckEq(con, q_glob, "1", "v1: zim:// glob finds 1 entry");

	// Re-querying an UNCHANGED file must keep working (and keep its warm handle).
	CheckEq(con, q_text, "version one", "v1: an unchanged file still reads v1");

	// --- replace by rename (new inode) ----------------------------------------
	ReplaceByRename(dir + "/v2.zim", live);
	CheckEq(con, q_entries, "a.txt,b.txt", "v2 (rename): read_zim sees the new entry set");
	CheckEq(con, q_text, "version two, replaced in place", "v2 (rename): zim_get_text reads v2");
	CheckEq(con, "SELECT zim_has_entry('" + live + "', 'b.txt')::VARCHAR", "true",
	        "v2 (rename): zim_has_entry sees the new entry");
	CheckEq(con, q_blob, "version two, replaced in place", "v2 (rename): zim:// reads v2");
	CheckEq(con, q_glob, "2", "v2 (rename): zim:// glob sees both entries");
	CheckEq(con, "SELECT content::VARCHAR FROM read_blob('zim://" + live + "/b.txt')", "brand new entry",
	        "v2 (rename): zim:// serves an entry that did not exist in v1");

	// --- a handle held OPEN across a replacement ------------------------------
	// The handle pins a libzim item inside the v2 archive. Replacing the file must
	// not pull that archive out from under it: the pool may only swap its own map
	// entry, leaving the displaced ZimArchive alive for its last reader. If that
	// were wrong this would read garbage or crash, not merely return stale bytes.
	{
		const std::string url = "zim://" + live + "/a.txt";
		const std::string v2_bytes = "version two, replaced in place";

		auto held = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
		Check(static_cast<size_t>(held->GetFileSize()) == v2_bytes.size(), "in-flight: handle opened over v2");

		// Read the first few bytes, so the item is genuinely mid-stream.
		std::string head(7, '\0');
		held->Read(&head[0], 7);
		Check(head == v2_bytes.substr(0, 7), "in-flight: first read returns v2 bytes");

		// ...now swap the file underneath it, in place this time (same inode, so
		// only mtime/size move) — and drop v3's own copy in the same step.
		ReplaceInPlace(dir + "/v3.zim", live);

		// The open handle keeps serving the archive it was opened over.
		std::string tail(v2_bytes.size() - 7, '\0');
		const auto n = held->Read(&tail[0], static_cast<int64_t>(tail.size()));
		Check(static_cast<size_t>(n) == tail.size(), "in-flight: sequential read after replacement is not short");
		Check(head + tail == v2_bytes, "in-flight: the held handle still reassembles v2 exactly");

		// ...including positional reads, which go through the same pinned item.
		std::string window(5, '\0');
		held->Read(&window[0], 5, 0);
		Check(window == v2_bytes.substr(0, 5), "in-flight: positional read on the held handle still returns v2");
		Check(static_cast<size_t>(held->GetFileSize()) == v2_bytes.size(),
		      "in-flight: the held handle's size is still v2's");

		// A handle opened NOW sees v3, while the old one is still open.
		Check(ReadWhole(fs, url) == "version three", "in-flight: a new handle opened alongside it reads v3");

		// SQL consumers see v3 too, with the old handle still alive.
		CheckEq(con, q_entries, "a.txt", "v3 (in-place): read_zim sees the shrunken entry set");
		CheckEq(con, "SELECT zim_has_entry('" + live + "', 'b.txt')::VARCHAR", "false",
		        "v3 (in-place): the entry that only existed in v2 is gone");
		CheckEq(con, q_text, "version three", "v3 (in-place): zim_get_text reads v3");

		// One more read after all of that, then let the handle go.
		held->Seek(0);
		std::string again(v2_bytes.size(), '\0');
		held->Read(&again[0], static_cast<int64_t>(again.size()));
		Check(again == v2_bytes, "in-flight: the held handle is still intact at the very end");
	}

	// The displaced archive is released here; nothing should be left pointing at it.
	CheckEq(con, q_text, "version three", "after the held handle closes, v3 still reads");

	// --- the motivating consumer: read_parquet over zim:// --------------------
	// This is the interaction #38 is actually about. read_parquet stats the file
	// (GetLastModifiedTime, which reports the CONTAINING .zim's mtime) and re-reads
	// when it moves. Before this fix the mtime moved but the pool kept handing back
	// the old archive, so the re-read returned the same stale bytes -- a caching
	// consumer being told "this changed" and then shown the old data.
	{
		// Probe for the parquet reader: a Catalog Error means this build has no
		// parquet extension (the .test counterpart uses `require parquet`); any other
		// error just means the probe path does not exist, which is expected.
		auto probe = con.Query("SELECT count(*) FROM read_parquet('/nonexistent-probe.parquet')");
		if (probe->HasError() && probe->GetError().find("read_parquet") != std::string::npos &&
		    probe->GetError().find("does not exist") != std::string::npos) {
			fprintf(stderr, "  skip: parquet not available in this build\n");
		} else {
			Exec(con, "COPY (SELECT i AS id FROM range(1000) t(i)) TO '" + dir + "/big.parquet' (FORMAT parquet);",
			     "writing big.parquet");
			Exec(con, "COPY (SELECT i AS id FROM range(250) t(i)) TO '" + dir + "/small.parquet' (FORMAT parquet);",
			     "writing small.parquet");
			WriteArchive(con,
			             "SELECT 'data/rows.parquet' AS path, content, "
			             "'application/vnd.apache.parquet' AS mimetype FROM read_blob('" +
			                 dir + "/big.parquet')",
			             dir + "/v4.zim");
			WriteArchive(con,
			             "SELECT 'data/rows.parquet' AS path, content, "
			             "'application/vnd.apache.parquet' AS mimetype FROM read_blob('" +
			                 dir + "/small.parquet')",
			             dir + "/v5.zim");

			const std::string q_pq =
			    "SELECT count(*)::VARCHAR FROM read_parquet('zim://" + live + "/data/rows.parquet')";
			ReplaceByRename(dir + "/v4.zim", live);
			CheckEq(con, q_pq, "1000", "parquet: read_parquet over zim:// reads the 1000-row archive");
			ReplaceByRename(dir + "/v5.zim", live);
			CheckEq(con, q_pq, "250", "parquet: after replacement it reads the 250-row archive, not the cached one");
		}
	}

	if (failures > 0) {
		fprintf(stderr, "%d assertion(s) failed\n", failures);
		return 1;
	}
	fprintf(stderr, "all pool-revalidation assertions passed\n");
	return 0;
}
