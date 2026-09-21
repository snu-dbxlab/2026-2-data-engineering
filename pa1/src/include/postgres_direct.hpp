//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_direct.hpp — direct read: read PostgreSQL heap files directly instead of COPY
//
// C++ side of the direct reader.  Talks to the C reader only through pgheap.h.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "postgres_result_reader.hpp"
#include "postgres_utils.hpp"
#include "pgheap.h"

namespace duckdb {
class PostgresTransaction;
struct PostgresBindData;

//! Everything the bind step resolves for a direct read of one table; immutable during the scan
struct PostgresDirectInfo {
	string qualified_name;          // "schema"."table" as used in LOCK TABLE
	uint32_t oid = 0;               // pg_class.oid
	uint32_t relfilenode = 0;       // pg_class.relfilenode at bind time (re-checked at every scan start)
	bool lockable = true;           // false for materialized views (LOCK TABLE is not allowed on them)
	string relpath;                 // absolute path of the main fork (segment 0)
	uint32_t nblocks = 0;           // from stat() of the segment files - pg_class.relpages is stale
	vector<pgh_att> atts;           // one per column, in attnum order (the table has no dropped columns)
	vector<string> unsupported;     // parallel to atts: why the column cannot be read directly (empty = fine)
};

//! bind: resolve PostgresDirectInfo for bind_data's table through the catalog transaction
//! (takes LOCK TABLE ... IN SHARE MODE for the duration of the transaction)
void PostgresDirectBind(ClientContext &context, PostgresTransaction &transaction, PostgresBindData &bind_data);
//! scan start: re-take the lock in the executing transaction and refuse to run if the table was rewritten or
//! grew since bind (a prepared statement may be executed long after it was bound)
void PostgresDirectCheckAtScan(ClientContext &context, PostgresTransaction &transaction, const PostgresBindData &bind_data);

//! scan: one deformed row -> DuckDB vector slots
class PostgresDirectWriter {
public:
	explicit PostgresDirectWriter(const PostgresBindData &bind_data);

	//! true if a column of this type can be read directly; otherwise `reason` says why not
	static bool IsSupported(const LogicalType &type, const PostgresType &pg_type, const pgh_att &att, string &reason);

	//! output column `col` of the row: rowid -> NULL, NULL -> NULL, otherwise WriteColumn
	void Write(Vector &out, idx_t row, column_t col, const vector<pgh_datum> &datums);

	//! on-disk datum -> DuckDB vector slot, per type ("ReadColumn")
	void WriteColumn(Vector &out, idx_t row, const pgh_datum &d, const LogicalType &type, const PostgresType &pg_type,
	                 const pgh_att &att);

private:
	const uint8_t *GetVarlena(const pgh_datum &d, int32_t &len);
	void WriteString(Vector &out, idx_t row, const pgh_datum &d, const PostgresType &pg_type);
	void WriteDecimal(Vector &out, idx_t row, const LogicalType &type, const pgh_numeric &num);

private:
	const PostgresBindData &bind_data;
	const PostgresDirectInfo &info;
	const PostgresTypeConfig &config;
	pgh_err err;
};

//! The third PostgresResultReader: reads the block range [task_min, task_max) of the heap file ("NextTuple" loop)
class PostgresDirectReader : public PostgresResultReader {
public:
	PostgresDirectReader(PostgresConnection &con, const vector<column_t> &column_ids, const PostgresBindData &bind_data);
	~PostgresDirectReader() override;

	void SetTaskRange(idx_t task_min, idx_t task_max) override;
	//! `sql` is ignored: opens the scan cursor over the task range
	void BeginCopy(ClientContext &context, const string &sql) override;
	PostgresReadResult Read(DataChunk &result) override;

private:
	const PostgresDirectInfo &info;
	pgh_file *file = nullptr;
	pgh_scan *scan = nullptr;
	pgh_err err;
	pgh_tupdesc desc;
	vector<pgh_datum> row;
	idx_t task_min = 0, task_max = 0;
	PostgresDirectWriter writer;
};

class PostgresDirectScanFunction : public TableFunction {
public:
	PostgresDirectScanFunction();
};

} // namespace duckdb
