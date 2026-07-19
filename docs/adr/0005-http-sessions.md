# ADR 0005: Opaque same-origin sessions

- Status: Accepted
- Date: 2026-07-19

## Decision

Passwords use libsodium Argon2id. A login creates a random opaque token; only a
keyed hash is stored in PostgreSQL. The raw token is sent in an HttpOnly,
SameSite=Strict cookie. The session response contains a separate CSRF token,
which same-origin writes repeat in `X-JournalSeed-CSRF`.

The API and production frontend share one origin. CORS is not enabled.

## Consequences

Session revocation is immediate and a database leak does not reveal active raw
tokens. Frontend code never reads the authentication cookie.
