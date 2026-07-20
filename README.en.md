# JournalSeed

[中文](README.md) | [English](README.en.md)

JournalSeed is a Chinese-first, single-administrator web ledger. The frontend uses Svelte 5
and the SvelteKit static adapter; the backend uses C++23, Drogon, and PostgreSQL.
Accounting writes are always stored as balanced double-entry postings, and monetary values
cross HTTP boundaries as strings to avoid JavaScript floating-point errors.

### Current delivery

This repository currently provides a runnable first end-to-end loop:

- First-run web administrator setup, Argon2id password hashing, random opaque sessions, HttpOnly/SameSite cookies, CSRF protection, and same-origin write checks.
- Single administrator with multiple ledgers; ledgers, user-defined accounts, income/expense categories, account balances, and period income/expense summaries.
- Income, expense, transfer, and zero-amount note rows; transfers appear as one neutral row in the table and are stored as two balanced postings.
- Default system columns are date, description, account, category, and amount; text and boolean custom columns are supported, and both rows and columns can be recycled and restored.
- Cursor pagination, date/amount sorting, problem-detail JSON responses, authenticated SSE, hot-reloaded Lua named functions, in-browser Lua script creation/editing, and a restricted exact-decimal runtime.
- Drogon serves the SvelteKit static build on the same origin; the compact desktop table, mobile list, detail drawer, and Chinese navigation are covered by browser acceptance tests.

The following capabilities are reserved in the database/API structure but are not claimed as completed
first-release features yet: incremental formula recomputation, persistent background workers, streaming
CSV COPY import/export, complete option/relation editors, large-data virtual scrolling, and performance
release tooling.

### Prerequisites

- macOS or Linux
- CMake 3.28+, Ninja, and a C++23 compiler
- Conan 2.29+, libsodium, and PostgreSQL client tools
- Node.js 22+ and pnpm 10+
- Podman and `podman-machine-default` for the development database only

### Quick start

If the host already uses PostgreSQL port `5432`, the development script can publish the database on
`55432`. The commands below create the persistent named volume `journalseed-postgres-data` and leave
any existing host database untouched.

```sh
cp .env.example .env
JOURNALSEED_POSTGRES_PORT=55432 ./scripts/dev/database.sh start

conan install . --lockfile=conan.lock --build=missing \
  -s build_type=Debug -s compiler.cppstd=23
cmake --preset conan-debug
cmake --build --preset conan-debug

export JOURNALSEED_DATABASE_URL='postgresql://journalseed:journalseed@127.0.0.1:55432/journalseed'
pnpm --dir frontend build
./build/Debug/backend/journalseed --host 127.0.0.1 --port 8080
```

Open <http://127.0.0.1:8080/> in a browser. The first visit shows the administrator setup page.
JournalSeed has no built-in fixed administrator; the test scripts create the administrator with the
following default credentials on first run and reuse the same credentials for later logins:

```text
Username: admin
Password: JournalSeed-Test-2026!
```

To use JSON configuration, copy `config.example.json` to `config.json`. Configuration precedence is
command-line arguments, environment variables, JSON file, and built-in defaults. Run migrations only
with:

```sh
./build/Debug/backend/journalseed --migrate-only
```

For frontend development, run:

```sh
pnpm --dir frontend dev
```

Vite serves hot reload on `127.0.0.1:5173` and proxies `/api` to `127.0.0.1:8080`.
The production build is written to the Git-ignored `frontend/build` directory and is served by Drogon
on the same port.

### Configuration

Command-line arguments:

| Argument                       | Description                                      |
| ------------------------------ | ------------------------------------------------ |
| `--host <address>`             | Listen address, default `127.0.0.1`              |
| `--port <port>`                | Listen port, default `8080`                      |
| `--config <path>`              | JSON configuration path, default `./config.json` |
| `--database-url <URL>`         | PostgreSQL connection URL                        |
| `--lua-dir <path>`             | Lua named-function directory                     |
| `--static-dir <path>`          | SvelteKit static build directory                 |
| `--background-workers <count>` | Number of background worker threads              |
| `--migrate-only`               | Run migrations and exit                          |
| `--help`                       | Show command-line help                           |

Environment variables:

| Environment variable             | Default                                                           |
| -------------------------------- | ----------------------------------------------------------------- |
| `JOURNALSEED_HOST`               | `127.0.0.1`                                                       |
| `JOURNALSEED_PORT`               | `8080`                                                            |
| `JOURNALSEED_DATABASE_URL`       | `postgresql://journalseed:journalseed@127.0.0.1:5432/journalseed` |
| `JOURNALSEED_LUA_DIR`            | `./scripts/functions`                                             |
| `JOURNALSEED_STATIC_DIR`         | `./frontend/build`                                                |
| `JOURNALSEED_BACKGROUND_WORKERS` | `2`                                                               |

### Verification

```sh
# C++ unit tests, currently 17 tests
ctest --test-dir build/server-check --output-on-failure

# Frontend type checks, unit tests, formatting check, and production build
pnpm --dir frontend check
pnpm --dir frontend test
pnpm --dir frontend format:check
pnpm --dir frontend build

# Requires a running PostgreSQL database and JournalSeed service
./scripts/test/api-smoke.sh

# Requires the running service; default base URL is 127.0.0.1:8080
pnpm --dir frontend exec playwright install chromium
pnpm --dir frontend test:e2e
```

Manual Playwright CLI acceptance settings live in `.playwright/cli.config.json`; screenshots and logs
are written to the Git-ignored `output/playwright/` directory. E2E and API smoke tests can override
`JOURNALSEED_BASE_URL`, `JOURNALSEED_TEST_USERNAME`, and `JOURNALSEED_TEST_PASSWORD`.

### Project layout

```text
backend/                 C++ domain, application services, Drogon API, Lua, and PostgreSQL repositories
frontend/                SvelteKit pages, API client, Vitest, and Playwright
migrations/              PostgreSQL migrations with SHA-256 checksums
scripts/functions/       Lua named functions; each file returns a function-definition table
scripts/dev/             Podman machine and PostgreSQL development scripts
scripts/test/            Repeatable API smoke acceptance test
docs/openapi/            HTTP contract and generated TypeScript type input
docs/adr/                Decision records for the modular monolith, typed cells, double-entry accounting, Lua, and sessions
```

Backend controllers only translate protocols; application services own transaction boundaries, domain
modules own money/posting rules, and repositories encapsulate SQL. The migration runner uses a PostgreSQL
advisory lock, a single-connection transaction, `schema_migrations`, and script checksums so concurrent
process starts run migrations once.

### Data and security constraints

- External identifiers use PostgreSQL `uuidv7()`, while internal joins use `BIGINT`; typed cells are hash-partitioned by column.
- Amounts are fixed to two decimal places and use exact decimal arithmetic; Lua formula constants should be created with `dec("0.1")`.
- Lua exposes only basic math, string, table, and controlled decimal userdata; file, network, process, `os`, `io`, `package`, and `debug` entry points are disabled, with instruction-count and wall-clock limits.
- Write requests require same-origin `Sec-Fetch-Site` when present plus the rotating `X-JournalSeed-CSRF` header; responses use problem-detail JSON, `X-Frame-Options`, nosniff, and Referrer-Policy.
- SvelteKit static HTML generates hash-based CSP at build time; the backend reads the CSP from `index.html` and adds `frame-ancestors 'none'`.

### API overview

REST routes start with `/api/v1`. The contract is `docs/openapi/journalseed.yaml`, and frontend types
are generated into `frontend/src/lib/api/schema.d.ts` with `pnpm --dir frontend generate:api`.

Current main routes:

- `/setup/status`, `/setup`
- `/auth/session`, `/auth/login`, `/auth/logout`
- `/ledgers`, `/ledgers/{ledgerId}/summary`
- `/ledgers/{ledgerId}/accounts`
- `/ledgers/{ledgerId}/categories`
- `/ledgers/{ledgerId}/columns`
- `/ledgers/{ledgerId}/rows`
- `/rows/{rowId}`, `/rows/{rowId}/restore`
- `/columns/{columnId}`, `/columns/{columnId}/restore`
- `/functions`, `/functions/{name}`, `/functions/{name}/invoke`
- `/jobs`, `/jobs/{jobId}/cancel`
- `/events`

### Lua named functions

Each `scripts/functions/*.lua` file returns a function-definition table, for example:

```lua
return {
  name = "quick_expense",
  version = "1.0.0",
  description = "把正数金额转换为支出，并补齐常用说明。",
  params = {
    { name = "amount", type = "number", label = "支出金额", required = true },
    { name = "description", type = "text", label = "说明", required = true }
  },
  run = function(params)
    local amount = dec(params.amount)
    return { amount = tostring(-amount), description = params.description }
  end
}
```

The registry hot-reloads the directory by polling. Lua scripts can also be created or edited from the
Functions page. Saving validates and reloads the full script set first; a new snapshot replaces the
current registry only after all scripts validate successfully, and validation failures keep the previous
valid snapshot active while rolling the file change back.

### Roadmap

The following work remains in the planned phases. Reserved tables in migrations and reserved API shapes
do not mean these features are complete yet:

1. Formula-column dependency declarations, cycle detection, incremental/full revision switching, error cells, and background recovery.
2. Complete editing UX for option/relation/formula columns, server-side combined filters, virtual scrolling, and batched cell reads.
3. CSV preview mapping, chunked COPY import, streaming export, cancellation, and error reports.
4. `journal_bench seed/load/recalc`, 500k-row pressure tests, PGO/LTO tuning, and reproducible release reports.

Dependency versions are pinned by the root `conan.lock` and frontend `pnpm-lock.yaml`. Before every
commit, README content must be checked against the current code, startup commands, feature boundaries,
and verification steps; migrations, OpenAPI, tests, and lockfiles must stay in sync.
