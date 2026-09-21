// direct_column.cpp — on-disk datum -> DuckDB vector slot, one case per type ("ReadColumn").
// The on-disk representation is native-endian (the COPY wire format that postgres_binary_parser.cpp decodes is
// big-endian); the epoch shifts and the numeric arithmetic mirror that parser.
#include "postgres_direct.hpp"
#include "postgres_scanner.hpp"
#include "postgres_conversion.hpp"
#include "postgres_type_oids.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include <cstring>

namespace duckdb {

template <class T>
static inline T LoadNative(const uint8_t *p) {
	T v;
	memcpy(&v, p, sizeof(T));
	return v;
}

PostgresDirectWriter::PostgresDirectWriter(const PostgresBindData &bind_data_p)
    : bind_data(bind_data_p), info(*bind_data_p.direct), config(bind_data_p.type_config) {
}

//===--------------------------------------------------------------------===//
// bind-time support check (must agree with WriteColumn below)
//===--------------------------------------------------------------------===//
bool PostgresDirectWriter::IsSupported(const LogicalType &type, const PostgresType &pg_type, const pgh_att &att,
                                       string &reason) {
	if (pg_type.info == PostgresTypeAnnotation::CAST_TO_VARCHAR) {
		reason = "the column is read as VARCHAR through a server-side cast (out of project scope)";
		return false;
	}
	LogicalTypeId expected;
	switch (att.atttypid) {
	case BOOLOID:
		expected = LogicalTypeId::BOOLEAN;
		break;
	case INT2OID:
		expected = LogicalTypeId::SMALLINT;
		break;
	case INT4OID:
		expected = LogicalTypeId::INTEGER;
		break;
	case INT8OID:
		expected = LogicalTypeId::BIGINT;
		break;
	case FLOAT4OID:
		expected = LogicalTypeId::FLOAT;
		break;
	case FLOAT8OID:
		expected = LogicalTypeId::DOUBLE;
		break;
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID:
		expected = LogicalTypeId::VARCHAR;
		break;
	case DATEOID:
		expected = LogicalTypeId::DATE;
		break;
	case TIMESTAMPOID:
		expected = LogicalTypeId::TIMESTAMP;
		break;
	case TIMESTAMPTZOID:
		expected = LogicalTypeId::TIMESTAMP_TZ;
		break;
	case NUMERICOID:
		if (type.id() == LogicalTypeId::DOUBLE && pg_type.info == PostgresTypeAnnotation::NUMERIC_AS_DOUBLE) {
			return true;
		}
		if (type.id() != LogicalTypeId::DECIMAL) {
			reason = StringUtil::Format("numeric is bound as %s", type.ToString());
			return false;
		}
		// the DuckDB DECIMAL must carry exactly PostgreSQL's (precision, scale); upstream's typmod arithmetic turns a
		// negative scale (numeric(6,-2), PostgreSQL 15+) into a bogus type, and writing digits under a wrong scale
		// would be silently wrong rather than an error
		if (att.atttypmod >= 4) {
			int32_t pg_scale = (((att.atttypmod - 4) & 0x7ff) ^ 1024) - 1024;
			int32_t pg_width = ((att.atttypmod - 4) >> 16) & 0xffff;
			if (pg_scale < 0 || pg_scale != int32_t(DecimalType::GetScale(type)) ||
			    pg_width != int32_t(DecimalType::GetWidth(type))) {
				reason = StringUtil::Format("numeric(%d,%d) is bound as %s", pg_width, pg_scale, type.ToString());
				return false;
			}
		}
		return true;
	default:
		reason = StringUtil::Format("type %s (out of project scope)", att.typname);
		return false;
	}
	if (type.id() != expected) {
		reason = StringUtil::Format("%s is bound as %s", att.typname, type.ToString());
		return false;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// one output column of one row
//===--------------------------------------------------------------------===//
void PostgresDirectWriter::Write(Vector &out, idx_t row, column_t col, const vector<pgh_datum> &datums) {
	if (col == COLUMN_IDENTIFIER_ROW_ID) {
		// DuckDB asks for the rowid column when it needs no data column (e.g. count(*));
		// the direct reader does not produce ctid, so the column is NULL
		FlatVector::SetNull(out, row, true);
		return;
	}
	auto &d = datums[col];
	if (d.isnull) {
		FlatVector::SetNull(out, row, true);
		return;
	}
	WriteColumn(out, row, d, bind_data.types[col], bind_data.postgres_types[col], info.atts[col]);
}

const uint8_t *PostgresDirectWriter::GetVarlena(const pgh_datum &d, int32_t &len) {
	const uint8_t *data;
	if (pgh_varlena_get(&d, &data, &len, &err) < 0) {
		throw IOException("Direct read: %s", err.msg);
	}
	return data;
}

// PostgresBinaryParser::ReadDecimal, fed from the decoded on-disk header (digits are native int16).
// Compiled with -ffp-contract=off (see CMakeLists.txt) so the double result is bit-identical to the COPY path.
template <class T, class OP = DecimalConversionInteger>
static PostgresDecimal<T> DecimalFromNumeric(const pgh_numeric &n) {
	switch (n.special) {
	case PGH_NUM_NAN:
		return PostgresDecimal<T>(PostgresDecimalKind::NOT_A_NUMBER);
	case PGH_NUM_PINF:
		return PostgresDecimal<T>(PostgresDecimalKind::POSITIVE_INFINITY);
	case PGH_NUM_NINF:
		return PostgresDecimal<T>(PostgresDecimalKind::NEGATIVE_INFINITY);
	default:
		break;
	}
	PostgresDecimalConfig cfg;
	cfg.ndigits = static_cast<uint16_t>(n.ndigits);
	cfg.weight = n.weight;
	cfg.scale = static_cast<uint16_t>(n.dscale);
	cfg.is_negative = n.negative != 0;
	cfg.sign = n.negative ? NUMERIC_NEG : NUMERIC_POS;
	int32_t next = 0;
	auto digit = [&]() -> uint16_t {
		uint16_t v = LoadNative<uint16_t>(n.digits + 2 * next);
		next++;
		return v;
	};
	auto scale_POWER = OP::GetPowerOfTen(cfg.scale);
	if (cfg.ndigits == 0) {
		return PostgresDecimal<T>(static_cast<T>(0));
	}
	T integral_part = 0, fractional_part = 0;
	if (cfg.weight >= 0) {
		integral_part = digit();
		for (auto i = 1; i <= cfg.weight; i++) {
			integral_part *= NBASE;
			if (i < cfg.ndigits) {
				integral_part += digit();
			}
		}
		integral_part *= scale_POWER;
	}
	if (cfg.ndigits > cfg.weight + 1) {
		auto fractional_power = (cfg.ndigits - cfg.weight - 1) * DEC_DIGITS;
		auto fractional_power_correction = fractional_power - cfg.scale;
		fractional_part = 0;
		for (int32_t i = MaxValue<int32_t>(0, cfg.weight + 1); i < cfg.ndigits; i++) {
			if (i + 1 < cfg.ndigits) {
				fractional_part *= NBASE;
				fractional_part += digit();
			} else {
				T final_base = NBASE;
				T final_digit = digit();
				if (fractional_power_correction >= 0) {
					T compensation = OP::GetPowerOfTen(fractional_power_correction);
					final_base /= compensation;
					final_digit /= compensation;
				} else {
					T compensation = OP::GetPowerOfTen(-fractional_power_correction);
					final_base *= compensation;
					final_digit *= compensation;
				}
				fractional_part *= final_base;
				fractional_part += final_digit;
			}
		}
	}
	auto base_res = OP::Finalize(cfg, integral_part + fractional_part);
	auto val = (cfg.is_negative ? -base_res : base_res);
	return PostgresDecimal<T>(val);
}

static inline date_t DateFromDisk(const uint8_t *p) {
	auto jd = LoadNative<uint32_t>(p);
	if (jd == POSTGRES_DATE_INF) {
		return date_t::infinity();
	}
	if (jd == POSTGRES_DATE_NINF) {
		return date_t::ninfinity();
	}
	return date_t(static_cast<int32_t>(jd) + POSTGRES_EPOCH_JDATE - DUCKDB_EPOCH_DATE);
}

static inline timestamp_t TimestampFromDisk(const uint8_t *p) {
	auto usec = LoadNative<uint64_t>(p);
	if (usec == POSTGRES_INFINITY) {
		return timestamp_t::infinity();
	}
	if (usec == POSTGRES_NINFINITY) {
		return timestamp_t::ninfinity();
	}
	int64_t timestamp_usec;
	if (!TryAddOperator::Operation(static_cast<int64_t>(usec), static_cast<int64_t>(POSTGRES_EPOCH_TS - DUCKDB_EPOCH_TS),
	                               timestamp_usec)) {
		throw ConversionException("timestamp out of range");
	}
	auto timestamp = timestamp_t(timestamp_usec);
	if (!timestamp.IsFinite()) {
		throw ConversionException("timestamp out of range");
	}
	return timestamp;
}

//===--------------------------------------------------------------------===//
// per-type writers
//===--------------------------------------------------------------------===//
void PostgresDirectWriter::WriteString(Vector &out, idx_t row, const pgh_datum &d, const PostgresType &pg_type) {
	int32_t len;
	auto str = reinterpret_cast<const char *>(GetVarlena(d, len));
	if (pg_type.info == PostgresTypeAnnotation::FIXED_LENGTH_CHAR) {
		// char(n): the padding is stripped, as the binary reader does
		while (len > 0 && str[len - 1] == ' ') {
			len--;
		}
	}
	FlatVector::GetDataMutable<string_t>(out)[row] = StringVector::AddStringOrBlob(out, str, len);
}

void PostgresDirectWriter::WriteDecimal(Vector &out, idx_t row, const LogicalType &type, const pgh_numeric &num) {
	PostgresDecimalKind kind;
	switch (type.InternalType()) {
	case PhysicalType::INT16: {
		auto dec = DecimalFromNumeric<int16_t>(num);
		kind = dec.kind;
		FlatVector::GetDataMutable<int16_t>(out)[row] = dec.value;
		break;
	}
	case PhysicalType::INT32: {
		auto dec = DecimalFromNumeric<int32_t>(num);
		kind = dec.kind;
		FlatVector::GetDataMutable<int32_t>(out)[row] = dec.value;
		break;
	}
	case PhysicalType::INT64: {
		auto dec = DecimalFromNumeric<int64_t>(num);
		kind = dec.kind;
		FlatVector::GetDataMutable<int64_t>(out)[row] = dec.value;
		break;
	}
	case PhysicalType::INT128: {
		auto dec = DecimalFromNumeric<hugeint_t, DecimalConversionHugeint>(num);
		kind = dec.kind;
		FlatVector::GetDataMutable<hugeint_t>(out)[row] = dec.value;
		break;
	}
	default:
		throw InvalidInputException("Unsupported decimal storage type");
	}
	if (kind != PostgresDecimalKind::ORDINARY) {
		if (!config.numeric_nan_as_null) {
			throw InvalidInputException("Unsupported NUMERIC value: %s", kind == PostgresDecimalKind::NOT_A_NUMBER ? "NaN"
			                                                            : kind == PostgresDecimalKind::POSITIVE_INFINITY
			                                                                ? "Infinity"
			                                                                : "-Infinity");
		}
		FlatVector::SetNull(out, row, true);
	}
}

void PostgresDirectWriter::WriteColumn(Vector &out, idx_t row, const pgh_datum &d, const LogicalType &type,
                                       const PostgresType &pg_type, const pgh_att &att) {
	const uint8_t *p = d.ptr;
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		FlatVector::GetDataMutable<bool>(out)[row] = p[0] != 0;
		break;
	case LogicalTypeId::SMALLINT:
		FlatVector::GetDataMutable<int16_t>(out)[row] = LoadNative<int16_t>(p);
		break;
	case LogicalTypeId::INTEGER:
		FlatVector::GetDataMutable<int32_t>(out)[row] = LoadNative<int32_t>(p);
		break;
	case LogicalTypeId::BIGINT:
		FlatVector::GetDataMutable<int64_t>(out)[row] = LoadNative<int64_t>(p);
		break;
	case LogicalTypeId::FLOAT:
		FlatVector::GetDataMutable<float>(out)[row] = LoadNative<float>(p);
		break;
	case LogicalTypeId::DOUBLE: {
		if (pg_type.info == PostgresTypeAnnotation::NUMERIC_AS_DOUBLE) {
			int32_t len;
			auto data = GetVarlena(d, len);
			pgh_numeric num;
			if (pgh_decode_numeric(data, len, &num, &err) < 0) {
				throw IOException("Direct read: %s", err.msg);
			}
			auto dec = DecimalFromNumeric<double, DecimalConversionDouble>(num);
			double v;
			switch (dec.kind) {
			case PostgresDecimalKind::ORDINARY:
				v = dec.value;
				break;
			case PostgresDecimalKind::NOT_A_NUMBER:
				v = std::numeric_limits<double>::quiet_NaN();
				break;
			case PostgresDecimalKind::POSITIVE_INFINITY:
				v = std::numeric_limits<double>::infinity();
				break;
			default:
				v = -std::numeric_limits<double>::infinity();
				break;
			}
			FlatVector::GetDataMutable<double>(out)[row] = v;
			break;
		}
		FlatVector::GetDataMutable<double>(out)[row] = LoadNative<double>(p);
		break;
	}
	case LogicalTypeId::DECIMAL: {
		int32_t len;
		auto data = GetVarlena(d, len);
		pgh_numeric num;
		if (pgh_decode_numeric(data, len, &num, &err) < 0) {
			throw IOException("Direct read: %s", err.msg);
		}
		WriteDecimal(out, row, type, num);
		break;
	}
	case LogicalTypeId::VARCHAR:
		WriteString(out, row, d, pg_type);
		break;
	case LogicalTypeId::DATE:
		FlatVector::GetDataMutable<date_t>(out)[row] = DateFromDisk(p);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP:
		FlatVector::GetDataMutable<timestamp_t>(out)[row] = TimestampFromDisk(p);
		break;
	default:
		throw InternalException("Direct read: unsupported type %s (column %s)", type.ToString(), att.attname);
	}
}

} // namespace duckdb
