# ADR 0003: Exact money and double-entry postings

- Status: Accepted
- Date: 2026-07-19

## Decision

The API transports all numeric values as strings. Money has exactly two decimal
places and is parsed into a checked signed 128-bit minor-unit representation in
the domain. PostgreSQL stores it as `NUMERIC(38,2)`.

Every non-zero entry creates exactly two postings whose signed values sum to
zero. A zero-value note creates none. Transfers create a negative source and a
positive destination posting and are excluded from income and expense totals.

## Consequences

Floating-point values never enter accounting calculations. The database uses a
deferred constraint trigger as a second line of invariant enforcement.
