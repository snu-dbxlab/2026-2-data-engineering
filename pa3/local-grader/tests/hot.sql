\set k random(1, 100)
BEGIN;
INSERT INTO append_t SELECT g, repeat('x', 100) FROM generate_series(1, 200) g;
UPDATE hot SET v = v + 1 WHERE id = :k;
COMMIT;
