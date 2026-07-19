# ADR 0002: PostgreSQL and typed cells

- Status: Accepted
- Date: 2026-07-19

## Decision

PostgreSQL 18 is the only persistence engine. Public API identifiers use UUIDv7;
internal joins use identity BIGINT values. Custom values are stored in typed,
hash-partitioned tables rather than row JSON.

Each typed table has a row/column primary key and a column/value/row index.
Listing queries select at most 250 row identifiers through a keyset cursor and
then batch-load their cells.

## Consequences

Filters and ordering remain indexable at 500,000 rows and 100 populated columns.
Adding a value type requires a migration and repository implementation, which is
an intentional tradeoff for predictable query plans.
