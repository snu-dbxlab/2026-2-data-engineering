// direct_bind.cpp — bind step of the direct reader: catalog lookups for the file path and the physical
// column layout, LOCK TABLE, file size.  Given to students.
#include "postgres_direct.hpp"
#include "postgres_scanner.hpp"
#include "storage/postgres_transaction.hpp"
#include "postgres_result.hpp"

namespace duckdb {

static void CopyName(char *dst, size_t cap, const string &s) {
	snprintf(dst, cap, "%s", s.c_str());
}

void PostgresDirectBind(ClientContext &context, PostgresTransaction &transaction, PostgresBindData &bind_data) {
	auto info = make_shared_ptr<PostgresDirectInfo>();
	auto schema_lit = PostgresUtils::WriteLiteral(bind_data.schema_name);
	auto table_lit = PostgresUtils::WriteLiteral(bind_data.table_name);
	auto qualified = PostgresUtils::WriteIdentifier(bind_data.schema_name) + "." +
	                 PostgresUtils::WriteIdentifier(bind_data.table_name);

	// 1. the relation: kind, file paths, data directory
	auto rel = transaction.Query(StringUtil::Format(R"(
SELECT c.oid, c.relkind, c.relpersistence, c.relfilenode, pg_relation_filepath(c.oid),
       current_setting('data_directory'), c.relhassubclass
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = %s AND c.relname = %s)",
	                                                schema_lit, table_lit));
	if (rel->Count() != 1) {
		throw BinderException("Direct read: table %s not found in pg_class", qualified);
	}
	auto oid = rel->GetInt64(0, 0);
	auto relkind = rel->GetString(0, 1);
	auto persistence = rel->GetString(0, 2);
	if (relkind != "r" && relkind != "m") {
		throw NotImplementedException("Direct read: %s is not a plain heap table (relkind '%s'); SET pg_direct_read=false",
		                              qualified, relkind);
	}
	if (persistence == "t") {
		throw NotImplementedException("Direct read: %s is a temporary table", qualified);
	}
	if (rel->GetBool(0, 6)) {
		// SELECT on an inheritance parent returns the children's rows too; we only see the parent's file
		throw NotImplementedException("Direct read: %s has child tables (inheritance); their rows live in other files. "
		                              "SET pg_direct_read=false to use COPY",
		                              qualified);
	}
	if (persistence == "u") {
		// CHECKPOINT does not write the buffers of unlogged relations (only a shutdown checkpoint or eviction does),
		// so the file on disk may be zero pages while the table has rows: reading it would be silently wrong
		throw NotImplementedException("Direct read: %s is an UNLOGGED table; its pages are not flushed by CHECKPOINT. "
		                              "SET pg_direct_read=false to use COPY",
		                              qualified);
	}

	// 2. hold the table for the whole transaction: no VACUUM FULL / TRUNCATE / CLUSTER (relfilenode changes) and
	//    no writers while we read the files.  Plain SELECT only takes ACCESS SHARE.  LOCK TABLE is not allowed on
	//    materialized views; a REFRESH during the scan would then surface as a read error, never as wrong rows.
	info->lockable = relkind == "r";
	if (info->lockable) {
		transaction.Query(StringUtil::Format("LOCK TABLE %s IN SHARE MODE", qualified));
	}
	info->qualified_name = qualified;
	info->oid = static_cast<uint32_t>(oid);
	info->relfilenode = static_cast<uint32_t>(rel->GetInt64(0, 3));
	string datadir = rel->GetString(0, 5);
	info->relpath = datadir + "/" + rel->GetString(0, 4);

	// 3. physical column layout.  The table must never have been ALTERed: no dropped columns, no attmissingval
	// (an ALTERed table fails in pgh_tuple_deform with "tuple has N attributes but catalog has M")
	auto att = transaction.Query(StringUtil::Format(R"(
SELECT a.attnum, a.attname, a.atttypid, t.typname, a.attlen, a.attalign, a.atttypmod
FROM pg_attribute a
LEFT JOIN pg_type t ON t.oid = a.atttypid
WHERE a.attrelid = %lld AND a.attnum > 0 AND NOT a.attisdropped
ORDER BY a.attnum)",
	                                                (long long)oid));
	if (att->Count() != bind_data.names.size()) {
		throw InternalException("Direct read: bound %d columns but pg_attribute has %d attributes",
		                        (int)bind_data.names.size(), (int)att->Count());
	}
	for (idx_t r = 0; r < att->Count(); r++) {
		pgh_att a;
		memset(&a, 0, sizeof(a));
		CopyName(a.attname, sizeof(a.attname), att->GetString(r, 1));
		a.atttypid = static_cast<uint32_t>(att->GetInt64(r, 2));
		CopyName(a.typname, sizeof(a.typname), att->IsNull(r, 3) ? "-" : att->GetString(r, 3));
		a.attlen = static_cast<int16_t>(att->GetInt64(r, 4));
		a.attalign = att->GetString(r, 5)[0];
		a.atttypmod = static_cast<int32_t>(att->GetInt64(r, 6));

		if (bind_data.names[r] != string(a.attname)) {
			throw InternalException("Direct read: column %d (%s) does not match the bound column list", (int)r + 1,
			                        a.attname);
		}
		auto &type = bind_data.types[r];
		auto &pg_type = bind_data.postgres_types[r];
		string reason;
		string unsupported;
		if (!PostgresDirectWriter::IsSupported(type, pg_type, a, reason)) {
			// remembered, not thrown: the error fires only when a query actually reads this column
			unsupported = StringUtil::Format("column \"%s\" (%s -> %s) cannot be read directly: %s. SET pg_direct_read=false to use COPY",
			                                 a.attname, a.typname, type.ToString(), reason);
		}
		info->atts.push_back(a);
		info->unsupported.push_back(std::move(unsupported));
	}

	// 4. the file itself: real block count (relpages is stale)
	pgh_err e;
	pgh_file *f = nullptr;
	if (pgh_file_open(&f, info->relpath.c_str(), &e) < 0) {
		throw IOException("Direct read: cannot open heap file of %s: %s (DuckDB must run on the PostgreSQL host with read "
		                  "access to the data directory). SET pg_direct_read=false if you want to read the table "
		                  "through COPY.",
		                  qualified, e.msg);
	}
	info->nblocks = pgh_file_nblocks(f);
	pgh_file_close(f);

	bind_data.use_direct_read = true;
	bind_data.direct = std::move(info);
}

void PostgresDirectCheckAtScan(ClientContext &context, PostgresTransaction &transaction,
                               const PostgresBindData &bind_data) {
	auto &info = *bind_data.direct;
	if (info.lockable) {
		transaction.Query(StringUtil::Format("LOCK TABLE %s IN SHARE MODE", info.qualified_name));
	}
	auto res = transaction.Query(StringUtil::Format("SELECT relfilenode FROM pg_class WHERE oid = %u", info.oid));
	if (res->Count() != 1 || static_cast<uint32_t>(res->GetInt64(0, 0)) != info.relfilenode) {
		throw InvalidInputException("Direct read: %s was rewritten (VACUUM FULL/CLUSTER/TRUNCATE) or dropped since the "
		                            "query was bound; run the query again",
		                            info.qualified_name);
	}
	pgh_err e;
	pgh_file *f = nullptr;
	if (pgh_file_open(&f, info.relpath.c_str(), &e) < 0) {
		throw IOException("Direct read: %s", e.msg);
	}
	auto nblocks = pgh_file_nblocks(f);
	pgh_file_close(f);
	if (nblocks != info.nblocks) {
		throw InvalidInputException("Direct read: %s grew from %u to %u blocks since the query was bound; run the query again",
		                            info.qualified_name, info.nblocks, nblocks);
	}
}

} // namespace duckdb
