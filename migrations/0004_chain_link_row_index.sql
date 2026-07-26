-- Index chain_movement_row_links by row_id so the ledger listing's chain LATERAL is a lookup.

-- The listing joins this table by row_id, but its only index is the movement_id primary key,
-- so every listed journal row cost a full scan of the link table: on 60k rows / 20k links a
-- single 101-row page read 18005 buffers in 58.0 ms, against 1135 buffers / 14.0 ms for the
-- same ledger with no chain data at all. With this index the same page reads 1340 buffers in
-- 14.7 ms, i.e. chain data stops being a per-page tax.
--
-- UNIQUE rather than a plain index, because the 1:1 relation it asserts is already true by
-- construction: record_wallet_sync is the sole writer of this table, and its single INSERT
-- takes movement_id and row_id from one `target` CTE row whose row_id comes from a per-row
-- nextval(pg_get_serial_sequence('journal_rows','id')). One fresh sequence value per movement
-- means no two movements can name the same journal row, and identity values are never reused,
-- so no later movement can reach an old row either. Making it UNIQUE turns that invariant into
-- something the database enforces, and lets the planner treat the LATERAL as a single-row
-- lookup instead of guessing at the fan-out.
CREATE UNIQUE INDEX chain_movement_row_links_row_unique
    ON chain_movement_row_links (row_id);
