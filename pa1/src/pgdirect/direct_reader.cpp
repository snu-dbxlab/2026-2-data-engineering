// direct_reader.cpp — the scan loop: block range -> pages -> tuples -> DataChunk ("NextTuple" + fill shell)
#include "postgres_direct.hpp"
#include "postgres_scanner.hpp"

namespace duckdb {

PostgresDirectReader::PostgresDirectReader(PostgresConnection &con, const vector<column_t> &column_ids,
                                           const PostgresBindData &bind_data)
    : PostgresResultReader(con, column_ids, bind_data), info(*bind_data.direct), writer(bind_data) {
	if (bind_data.emit_ctid) {
		// UPDATE / DELETE / INSERT ... SELECT plan the scan with emit_ctid and use the rowid column as the ctid of the
		// row to modify; the direct reader does not produce ctid, so the statement must go through COPY
		throw NotImplementedException("Direct read: %s is scanned for a data-modifying statement, which needs ctid. "
		                              "SET pg_direct_read=false",
		                              info.qualified_name);
	}
	if (pgh_file_open(&file, info.relpath.c_str(), &err) < 0) {
		throw IOException("Direct read: %s", err.msg);
	}
	desc.natts = static_cast<int>(info.atts.size());
	desc.atts = info.atts.data();
	row.resize(info.atts.size());
	// the projected columns must all be readable; other columns of the table may be of unsupported types
	for (auto col : column_ids) {
		if (col == COLUMN_IDENTIFIER_ROW_ID) {
			continue;
		}
		auto &why = info.unsupported[col];
		if (!why.empty()) {
			pgh_file_close(file);
			throw NotImplementedException("Direct read: %s", why);
		}
	}
}

PostgresDirectReader::~PostgresDirectReader() {
	pgh_scan_close(scan);
	pgh_file_close(file);
}

void PostgresDirectReader::SetTaskRange(idx_t task_min_p, idx_t task_max_p) {
	task_min = task_min_p;
	task_max = task_max_p;
}

void PostgresDirectReader::BeginCopy(ClientContext &context, const string &sql) {
	pgh_scan_close(scan);
	scan = nullptr;
	auto blk_begin = static_cast<uint32_t>(MinValue<idx_t>(task_min, info.nblocks));
	auto blk_end = static_cast<uint32_t>(MinValue<idx_t>(task_max, info.nblocks));
	if (pgh_scan_open(&scan, file, &desc, blk_begin, blk_end, &err) < 0) {
		throw IOException("Direct read: %s", err.msg);
	}
}

PostgresReadResult PostgresDirectReader::Read(DataChunk &output) {
	if (!scan) {
		throw InternalException("PostgresDirectReader::Read without BeginCopy");
	}
	while (output.size() < STANDARD_VECTOR_SIZE) {
		int r = pgh_scan_next(scan, row.data());
		if (r < 0) {
			throw IOException("Direct read of %s: %s", info.relpath, err.msg);
		}
		if (r == 0) {
			return PostgresReadResult::FINISHED;
		}
		idx_t out_row = output.size();
		for (idx_t out_idx = 0; out_idx < output.ColumnCount(); out_idx++) {
			writer.Write(output.data[out_idx], out_row, column_ids[out_idx], row);
		}
		output.SetChildCardinality(out_row + 1);
	}
	return PostgresReadResult::HAVE_MORE_TUPLES;
}

} // namespace duckdb
