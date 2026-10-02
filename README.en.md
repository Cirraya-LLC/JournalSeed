# JournalSeed

[中文](README.md) | [English](README.en.md)

JournalSeed is a Chinese-first, single-administrator web ledger. The frontend uses Svelte 5
and the SvelteKit static adapter; the backend uses C++23, Drogon, and PostgreSQL.
Accounting writes are always stored as balanced double-entry postings, and monetary values
cross HTTP boundaries as strings to avoid JavaScript floating-point errors.

> **Upgrading: chain-settings key derivation changed (breaking)**
>
> `JOURNALSEED_CHAIN_SETTINGS_KEY` no longer derives its key with bare BLAKE2b. It now takes a
> 32-byte key as hex or base64, or stretches a passphrase with Argon2id. **Chain settings
> encrypted under the old scheme no longer decrypt.** After upgrading, open the Chain settings
> page and re-enter and save the TronGrid / Etherscan API keys and any custom RPC URLs once.
> Until you do, wallet synchronization stops with an explicit
> `chain_settings_secret_undecryptable` (500) rather than silently degrading to unauthenticated
> RPC calls the way it used to. See [Configuration](#configuration).

### Current delivery

This repository currently provides a runnable first end-to-end loop:

- First-run web administrator setup, Argon2id password hashing, random opaque sessions, HttpOnly/SameSite cookies, CSRF protection, and same-origin write checks.
- Single administrator with multiple ledgers; ledgers, user-defined accounts, income/expense categories, account balances, and period income/expense summaries. The summary endpoint's `from`/`to` are inclusive dates and are genuinely applied, and balances and period flows are reported per asset.
- Income, expense, transfer, and zero-amount note rows; transfers appear as one neutral row in the table and are stored as two balanced postings.
- Default system columns are date, description, account, category, and amount (the system amount column's type is the read-only `money`, which the create-column endpoint cannot request); custom columns can be text, number, date, checkbox, single-select, relation, or formula, and both rows and columns can be recycled and restored.
- Cursor pagination, date/amount sorting, problem-detail JSON responses, authenticated SSE, hot-reloaded Lua named functions, in-browser Lua script creation/editing, and a restricted exact-decimal runtime.
- Drogon serves the SvelteKit static build on the same origin; the compact desktop table, mobile list, detail drawer, and Chinese navigation are covered by browser acceptance tests.
- A complete fetch-and-persist path for built-in TRON, Ethereum, Polygon, and Solana Mainnet watch-only wallet synchronization, plus a wallet-sync page, address labels, and a chain-transaction view. It stores public addresses only and never needs or accepts private keys; provider adapters for all four chains parse transfers, internal transfers, and fees into movements normalized to each asset's decimals, and every outbound endpoint is re-checked against the SSRF rules before each call.
- Per-wallet accepted currencies: USDT / USDC presets plus any number of custom token contracts; every other asset (spam included) is not booked. Each currency can carry an exchange rate, in which case it is booked in the default asset as amount × rate on a chosen account. Only transfers after a wallet is added are recorded, and a wallet can be paused and resumed by hand — resuming catches up on the pause.

The following capabilities are reserved in the database/API structure but are not claimed as completed
first-release features yet: incremental formula recomputation, persistent background workers, streaming
CSV COPY import/export, complete option/relation editors, large-data virtual scrolling, and performance
release tooling.

**The persistence step of wallet synchronization now works.**
`PostgresRepository::record_wallet_sync` writes chain transactions, assets, the per-asset wallet
and clearing accounts, asset movements, and the generated journal rows and postings in a single
database transaction, and advances the `wallet_sync_states` checkpoint. The sync endpoint's three
counters therefore report what the pass actually wrote, the chain-transaction view returns data,
and the row-reading query fills in `JournalRow.chainSource` through `chain_movement_row_links`.
The first pass still fetches the most recent batch; later passes read the stored checkpoints and
resume incrementally.

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

Environment variables (interchangeable with the command-line arguments and JSON configuration):

| Environment variable             | Default                                                           |
| -------------------------------- | ----------------------------------------------------------------- |
| `JOURNALSEED_HOST`               | `127.0.0.1`                                                       |
| `JOURNALSEED_PORT`               | `8080`                                                            |
| `JOURNALSEED_DATABASE_URL`       | `postgresql://journalseed:journalseed@127.0.0.1:5432/journalseed` |
| `JOURNALSEED_LUA_DIR`            | `./scripts/functions`                                             |
| `JOURNALSEED_STATIC_DIR`         | `./frontend/build`                                                |
| `JOURNALSEED_BACKGROUND_WORKERS` | `2`                                                               |

The variables below are read **from the process environment only**. They are not accepted in
`config.json` or on the command line, and the admin API cannot set them.

| Environment variable             | Default                               | Purpose                                                                 |
| -------------------------------- | ------------------------------------- | ----------------------------------------------------------------------- |
| `JOURNALSEED_CHAIN_SETTINGS_KEY` | Unset                                 | Master key for the API keys and custom RPC URLs held in chain settings; see below |
| `JOURNALSEED_RPC_ALLOW_HOSTS`    | Unset                                 | Comma-separated egress-check allowlist; see below                        |
| `JOURNALSEED_ETHERSCAN_BASE_URL` | `https://api.etherscan.io`            | Etherscan V2 base URL used for Ethereum/Polygon transfer history         |
| `JOURNALSEED_SOLANA_RPC_URL`     | `https://api.mainnet-beta.solana.com` | Fallback Solana endpoint when no custom RPC URL is saved                 |
| `JOURNALSEED_MOCK_CHAIN_SYNC`    | Unset                                 | Set to `1` to sync from deterministic mock data; `JOURNALSEED_MOCK_MULTICHAIN_SYNC` is an equivalent alias |
| `JOURNALSEED_MOCK_TRON_SYNC`     | Unset                                 | Set to `1` to use mock data for TRON wallets only                        |

#### `JOURNALSEED_CHAIN_SETTINGS_KEY`

Required to save TronGrid / Etherscan API keys or custom RPC URLs. Two forms are accepted:

- A **32-byte random key**, as hex (64 characters) or base64 (URL-safe and unpadded spellings
  included), used directly as the key.
- A **passphrase** of at least 16 characters, stretched into a 32-byte key with Argon2id over a
  fixed salt. Anything shorter is rejected.

Generating a random key is preferred:

```sh
openssl rand -hex 32
```

The derived key is cached for the life of the process and pinned with `sodium_mlock` so the master
key is never paged to disk. When the variable is unset or too weak, saving chain settings fails with
`chain_settings_key_unconfigured` (500).

> **Breaking change.** Derivation moved from bare BLAKE2b to the scheme above, so **chain settings
> encrypted under the old scheme no longer decrypt**. Re-enter and save those credentials once from
> the Chain settings page after upgrading. Until then, anything that reads them — wallet
> synchronization — fails with an explicit `chain_settings_secret_undecryptable` (500) instead of
> silently falling back to unauthenticated RPC calls. Rotating the key has the same effect.

#### `JOURNALSEED_RPC_ALLOW_HOSTS`

An escape hatch for the outbound (SSRF) egress check: a comma-separated list of hostnames
(case-insensitive, surrounding whitespace ignored) that skip the "must not resolve into loopback,
private, carrier-grade NAT, link-local including the cloud metadata address, benchmark, multicast,
or reserved space" rule. It exists for nodes genuinely running on a private address or on the host,
for example `JOURNALSEED_RPC_ALLOW_HOSTS=127.0.0.1,geth.internal`.

**Why it is environment-only and deliberately not an admin API setting:** the whole point of this
switch is to let the server reach private networks. If it were writable over the web, an attacker
holding an admin session could allowlist their own target, point an RPC URL at it, and the SSRF
protection would be worth nothing. Keeping it in the process environment means changing it requires
shell access to the host, which closes that escalation path.

The egress check validates at resolution time rather than against a fixed provider allowlist,
because self-hosted deployments use whichever RPC provider they like. The residual risk is DNS
rebinding: the address resolved during the check and the address resolved when the connection is
actually made can differ. Closing that window entirely would mean connecting by pinned IP with a
separately set SNI/Host, which is out of proportion to this project, so it is knowingly accepted.

### Multi-chain wallet synchronization

The first release supports watch-only TRON, Ethereum, Polygon, and Solana Mainnet address synchronization. Adding a wallet requires only its public address; JournalSeed does not sign or broadcast transactions and never requests or stores a private key. Supported chain IDs are `tron-mainnet`, `ethereum-mainnet`, `polygon-mainnet`, and `solana-mainnet`; TRON uses Base58Check addresses, Ethereum/Polygon use `0x` EVM addresses, and Solana uses base58 addresses.

Chain settings include a TronGrid API key, an Etherscan API key, and custom Ethereum/Polygon/Solana RPC URLs. TronGrid and Etherscan API keys are optional; Ethereum and Polygon transfer history is read through Etherscan V2 account APIs, and an Etherscan API key is optional but recommended. Solana synchronization uses RPC; when no custom Solana RPC URL is configured, JournalSeed uses a public endpoint, which is rate limited, so manual acceptance and long-running deployments should use a dedicated RPC URL. Ethereum/Polygon RPC URLs are stored as encrypted chain settings for future provider extensions; the current fetch path does not read them. Saving API keys or custom RPC URLs from the Chain settings page requires the server to set `JOURNALSEED_CHAIN_SETTINGS_KEY`; that variable encrypts the stored secrets and the API never returns the secrets themselves. Wallets with automatic sync enabled are scanned by the backend about every 60 seconds and are synced when due according to the Chain settings interval; the wallet page also provides “Sync now”.

Custom RPC URLs go through the egress check both when saved and when used: a rejection while saving returns a field-scoped `validation_error` (422), and a rejection during synchronization returns `rpc_endpoint_rejected` (422). The check is re-run before each sync, because being safe when it was saved does not mean it is still safe — DNS may since have been repointed at a private address.

Amounts use each asset's own decimals: the default `DEFAULT` asset has 2 decimal places, TRX/USDT have 6, ETH has 18, Polygon's native asset POL has 18, and SOL has 9. Each chain adapter parses incoming, outgoing, self (`internal`), and fee movements and normalizes them to those decimals. Address labels can show counterparties as customer, self, exchange, merchant, contract, or other names; the label-matching query already supports a shared EVM scope, so one label could cover the same `0x` address on both Ethereum and Polygon — but the creation endpoint currently accepts only the four concrete chain codes, so every label actually stored is chain-scoped. That is an unresolved design question; the current behaviour and the two candidate fixes are recorded under `AddressLabel.scope` in `docs/openapi/journalseed.yaml`.

**Persistence behaviour.** `PostgresRepository::record_wallet_sync` writes one synchronization pass inside a single database transaction: it locks the wallet row by `public_id`, sets `wallet_sync_states` to `running`, then writes `chain_transactions` (upserted on ledger + chain + `tx_hash`, refreshing block number, block time, confirmation, and receipt status on re-sync), `assets` (keyed on ledger + chain + normalized contract address, with native assets keyed by symbol; existing assets are never rewritten), one set of accounts per asset — the wallet's own account (`accounts` plus `wallet_asset_accounts`, numbered automatically when the plain name is taken) and the two system clearing accounts, named "收入结转 · symbol" (income clearing) and "支出结转 · symbol" (expense clearing) — `chain_asset_movements` (deduplicated on ledger + `movement_key`), `journal_rows` and `postings`, and finally `chain_movement_row_links`, which binds each movement to its row. Any failed step rolls the whole pass back, and once the rows are written the posting-balance constraint — deferred to commit time by default — is forced to check immediately, so a sync can never report success and then fail at commit.

Direction mapping: `incoming` becomes income (+amount on the wallet account against the income clearing account), `outgoing` and `fee` become expenses (-amount against the expense clearing account) and differ only in wording; `internal` is a transfer between addresses the user already owns, which both wallets see when each is synced, so booking it as money would double-count it — it becomes a single zero-amount note row with no postings at all. A zero-amount movement in any other direction is still recorded as a movement but produces no journal row, because `postings` carries a `signed_amount <> 0` check.

Idempotency comes from `chain_movement_row_links`: only movements that are not yet linked produce a journal row. Re-syncing the same window therefore creates no duplicates, and a movement that is already linked is never rewritten by a later sync — edits the user makes to an automatically generated row are preserved.

**Generated rows are editable like any other.** Such a row points at the wallet's account for that asset — an ordinary account, not an internal system one, so it appears in the account list — and `PATCH /rows/{rowId}` resolves the asset back to that chain asset by the same rule and rebuilds the postings against that asset's own clearing accounts. Neither held before: editing any synchronization-generated row returned 500. The edit never touches `chain_movement_row_links`, so `chainSource` survives it and a later sync still skips the linked movement. Note that the endpoint takes the whole row rather than a delta: dropping `accountId` from an `entry` row moves it back to the default asset's unallocated account, its asset reverts to `DEFAULT`, and an amount with more than two decimal places is then rejected.

The three counters are truthful: `transactionsSeen` is the number of chain transactions this pass wrote or refreshed, `movementsCreated` is the number of asset movements newly inserted (`0` when the same window is synced again), and `rowsCreated` is the number of journal rows newly created and linked (including the zero-amount note rows for `internal`).

Checkpoints: on success `wallet_sync_states` returns to `idle`, `checkpoint_block` and `checkpoint_timestamp` only ever move forward (an empty window leaves them untouched), and `checkpoint_signature` takes the hash of the newest transaction in the window so Solana can resume from a signature rather than a block number — it is only replaced when that transaction is no older than the stored checkpoint time. The first pass still fetches the most recent batch (100 transactions for TRON and the EVM chains, 50 for Solana). Later passes read the stored checkpoints and resume with `min_timestamp` / `startblock` / `until`, paginating at most 8 extra pages. A wallet already marked `running` rejects a concurrent sync.

The on-chain origin is readable in both directions: the row-reading query follows `chain_movement_row_links` to fill in `JournalRow.chainSource`, so rows generated by synchronization carry it and ordinary manually entered rows do not — the presence of that field is a reliable test for "this row came from a wallet sync". The reverse direction, `ChainTransaction.rowId`, is populated as well.

Current boundary: signing and on-chain transfers, swaps, NFTs, staking, cross-chain bridges, and complex contract semantics are unsupported. This is a ledger synchronizer, not a wallet client.

### Verification

```sh
# C++ unit tests
ctest --preset conan-debug --output-on-failure

# Frontend type checks, unit tests, formatting check, and production build
pnpm --dir frontend check
pnpm --dir frontend test
pnpm --dir frontend format:check
pnpm --dir frontend build

# Requires a running PostgreSQL database and JournalSeed service
./scripts/test/api-smoke.sh

# For manual wallet-sync acceptance, start the service with deterministic multi-chain transaction data
# Legacy TRON-only mock remains accepted: JOURNALSEED_MOCK_TRON_SYNC=1
JOURNALSEED_MOCK_CHAIN_SYNC=1 ./build/Debug/backend/journalseed --host 127.0.0.1 --port 8080

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
- Amounts use exact decimal arithmetic and are interpreted and displayed with the asset's decimals: `DEFAULT` has 2 decimal places and TRX/USDT have 6, ETH/POL have 18, and SOL has 9; Lua formula constants should be created with `dec("0.1")`. Every returned amount is rendered as `round(value, assets.decimals)`, so all money fields for one asset — row amounts, account balances and opening balances, summary figures, chain movement amounts — carry exactly the same number of decimal places.
- **A row's asset is the asset of the account it points at**, resolved in fixed order: the account's asset first; failing that the asset the row already holds (only a note row, which has no account, gets this far); failing that the ledger's default asset. Recording a row by hand against an account wallet synchronization created therefore denominates it in that chain asset and lets the amount carry all 18 decimal places, while an `entry` row that omits the account falls back to the default asset's unallocated account and is limited to two. On write, an amount with more precision than the row's asset allows is rejected with a `validation_error` (422) carrying `fields.amount`; it is never silently rounded. `invalid_amount` is reserved for a value that is not a parseable decimal at all — `abc`, `1.`, `1.2.3`, `1e5`.
- A transfer whose two accounts sit on different assets is rejected with a `validation_error` (422) carrying `fields.transferAccountId`: a row holds one asset and all of its postings must share it, so a cross-asset transfer cannot be expressed. Accounts created through `POST /ledgers/{ledgerId}/accounts` always use the default asset, so `openingBalance` is limited to two decimal places; accounts on chain assets are created by wallet synchronization.
- Lua exposes only basic math, string, table, and controlled decimal userdata; file, network, process, `os`, `io`, `package`, and `debug` entry points are disabled, with instruction-count and wall-clock limits.
- Write requests require same-origin `Sec-Fetch-Site` when present plus the rotating `X-JournalSeed-CSRF` header; responses use problem-detail JSON, `X-Frame-Options`, nosniff, and Referrer-Policy.
- SvelteKit static HTML generates hash-based CSP at build time; the backend reads the CSP from `index.html` and adds `frame-ancestors 'none'`.

### Ledger summary

`from` and `to` on `GET /ledgers/{ledgerId}/summary` used to be documented but silently ignored.
They now genuinely apply: both are inclusive `YYYY-MM-DD` dates matched against
`journal_rows.occurred_on`, validated as real calendar dates (`2026-02-30` is rejected rather
than passed to SQL), and `from` must not be later than `to`. A failure is a
`validation_error` (422) with `from` or `to` in `fields`. An omitted or empty bound leaves that
side open.

**`from` bounds the flows only.** `income`, `expense`, and `rowCount` are flows and are bounded
on both sides. `balance` is a stock: it is the **closing balance as of `to`** — every posting
dated on or before `to`, opening balances included. Giving a balance a left bound would turn it
into a movement over the period that no longer reconciles with the balances the accounts
endpoint returns, so leaving `from` out of it is deliberate.

**The scalar fields cover the ledger's default asset only.** `balance`, `income`, and `expense`
used to sum incommensurable assets — ETH plus CNY plus SOL — and round the mixture to two
decimals, which is not a quantity of anything. They now report the ledger's default asset alone,
at that asset's own scale. Every asset is listed in `assetSummaries`, one entry each, default
asset first — an array that used to be permanently empty. `rowCount` remains a ledger-wide count
across all assets, because rows are countable even when the amounts are not. Single-asset
ledgers (those that have never synchronized a wallet) see no change; clients that read the
scalars as ledger-wide totals must move to `assetSummaries`.

### API overview

REST routes start with `/api/v1`. The contract is `docs/openapi/journalseed.yaml`, and frontend types
are generated into `frontend/src/lib/api/schema.d.ts` with `pnpm --dir frontend generate:api`.

Errors are always `application/problem+json`, and the full set of `code` values is enumerated
under `Problem.code` in the contract; `type` is always `/problems/{code}` (`database_error`
reports `/problems/database_error`, which was once mis-spelled with a hyphen as
`/problems/database-error`). Several recent changes are worth calling out: `csrf_mismatch` is
**403**, not 401, and now also covers a **missing** `X-JournalSeed-CSRF` header (which used to
return 401 `session_required` and made clients treat it as an expired session); `invalid_json`
(400) now means strictly "body missing, not parseable JSON, or carrying content after the
top-level JSON value" and no longer echoes the parser's output — a body like
`{"name":"X"} trailing` used to be accepted outright and actually created the resource. A
well-formed body that does not match the endpoint's schema, a non-UUID path parameter, invalid
`from`/`to` dates, category-direction and account-membership errors, a transfer across two
assets (`fields.transferAccountId`), and an amount exceeding its row's asset precision
(`fields.amount`) are all `validation_error` (422). A missing target is a 404 with an
`<entity>_not_found` code: `ledger_not_found`, `account_not_found`, `category_not_found`,
`column_not_found`, `row_not_found`, `job_not_found`, `wallet_not_found`,
`address_label_not_found`.

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
- `/chain-settings`
- `/ledgers/{ledgerId}/wallets`, `/wallets/{walletId}`, `/wallets/{walletId}/sync`
- `/ledgers/{ledgerId}/chain-transactions`
- `/ledgers/{ledgerId}/address-labels`, `/address-labels/{labelId}`
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
The Functions page includes an in-product custom-function tutorial covering the
`name`/`version`/`description`/`params`/`run` structure, exact-number handling with `dec()`,
preview results, save validation, and a “use tutorial example” editable Lua template.

### Known gaps

The following are confirmed and unfixed. They are not "planned phases" but the boundaries of what
currently ships, listed here so that reserved structures in the API and the migrations are not
mistaken for finished capabilities.

1. **The shared `evm` scope for address labels is unreachable.** The database and the repository both
   support `scope='evm'` (one label covering Ethereum and Polygon), and migration `0003` created
   `wallet_address_labels_evm_unique` for it, but the service layer's chain validation accepts only the
   four concrete chain codes and rejects `evm` with a 422, so the path can never be taken. The same `0x`
   address must currently be labelled separately on each chain. There are two candidate fixes — add
   `evm` to the `AddressLabel.chain` enum, or add a column recording the originating chain — and since
   they trade off differently, neither was chosen unilaterally.
2. **Request bodies above 4 MiB return a 413 with no body.** The application limit is 1 MiB, and
   exceeding it returns the documented `body_too_large` problem detail; but anything above 4 MiB is
   rejected by Drogon at the transport layer, which cannot attach a JSON body. Requests between the two
   behave as documented.
3. **The chain-settings key cannot be rotated.** `chain_settings` has no key-version column and the
   ciphertext carries no version tag, so changing `JOURNALSEED_CHAIN_SETTINGS_KEY` leaves every stored
   secret undecryptable. The current behaviour is an explicit `chain_settings_secret_undecryptable`
   (500) telling the operator to re-enter them, rather than a silent downgrade to anonymous calls. Real
   rotation needs a migration adding `key_version` plus a re-encryption pass.
4. **Incremental fetching is not wired up.** Checkpoints are written but never read — see
   "Multi-chain wallet synchronization".
5. **Some paths have no automated verification.** Real chain RPC fetches and their failure paths (only
   the offline `JOURNALSEED_MOCK_CHAIN_SYNC=1` route is covered), the race between a user edit and a
   concurrent sync of the same movement, CSV import/export, and the Lua script write paths are all
   outside the automated tests.

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
