# ADR 0004: Lua sandbox and revisioned formulas

- Status: Accepted
- Date: 2026-07-19

## Decision

Named Lua functions are loaded into a restricted Lua 5.4 state. Files, network,
processes, modules, operating-system functions, and debug APIs are absent. A
watcher validates a complete candidate registry before atomically publishing it.

Formula declarations list their dependencies. Full calculations write a new
materialized revision and activate it in one transaction. Incremental jobs use
the same dependency graph and persistent `SKIP LOCKED` work queue.

## Consequences

Readers never see a partially calculated full revision. A failed reload leaves
the last valid registry available, and formula failures are represented as cell
state rather than transaction loss.
