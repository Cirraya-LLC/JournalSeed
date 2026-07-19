# ADR 0001: Modular monolith

- Status: Accepted
- Date: 2026-07-19

## Decision

JournalSeed is one deployable C++ service with explicit `domain`, `application`,
`infrastructure`, `api`, and `lua` modules. The Svelte application is built to
static assets and served by the same Drogon process in production.

HTTP controllers translate protocol types only. Application services own use
case transactions, domain code enforces money and posting invariants, and
repositories own SQL. Domain headers have no Drogon or PostgreSQL dependency.

## Consequences

One process keeps deployment and same-origin session handling simple. Module
boundaries remain testable and can be separated later without introducing
distributed transactions during the initial product stages.
