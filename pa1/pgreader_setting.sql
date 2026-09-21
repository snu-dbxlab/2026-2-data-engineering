LOAD 'build/release/extension/postgres_scanner/postgres_scanner.duckdb_extension';
ATTACH 'host=localhost port=[port] dbname=[dbname]' AS pg (TYPE postgres, READ_ONLY);
SET pg_direct_read=true;
SET threads = 1;
USE pg.public;
