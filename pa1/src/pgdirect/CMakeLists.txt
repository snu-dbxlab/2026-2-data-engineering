# Direct heap-file reader.  Two object libraries:
#   postgres_ext_pgdirect_c   C11: the reader itself (pgheap_*.c).  Built from pg_ondisk_format.h, it never sees a
#                             PostgreSQL header nor duckdb.hpp.
#   postgres_ext_pgdirect     C++17: the DuckDB side (direct_*.cpp), sees duckdb.hpp + pgheap.h.
# The only shared header is pgheap.h (<stdint.h> types only).
add_library(postgres_ext_pgdirect_c OBJECT
  pgheap_reader.c pgheap_support.c pgheap_err.c pgheap_varlena.c pgheap_numeric.c)
set_target_properties(postgres_ext_pgdirect_c PROPERTIES
  C_STANDARD 11 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(postgres_ext_pgdirect_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(postgres_ext_pgdirect OBJECT direct_bind.cpp direct_reader.cpp direct_column.cpp)
target_include_directories(postgres_ext_pgdirect PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# numeric -> DOUBLE must round exactly like PostgresBinaryParser::ReadDecimal; with -march=x86-64-v3 the compiler
# fuses that arithmetic into FMAs here but not in the parser (1 ulp apart), so contraction is switched off for this TU set
if(NOT MSVC)
  target_compile_options(postgres_ext_pgdirect PRIVATE -ffp-contract=off)
endif()

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:postgres_ext_pgdirect_c> $<TARGET_OBJECTS:postgres_ext_pgdirect>
    PARENT_SCOPE)
