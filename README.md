# JournalSeed

[中文](#中文版本) | [English](#english-version)

---

## 中文版本

JournalSeed 是一个中文优先、单管理员的网页账本。前端使用 Svelte 5 与 SvelteKit
静态适配器，后端使用 C++23、Drogon 和 PostgreSQL；账务写入始终落成平衡的复式分录，
金额在 HTTP 边界使用字符串传输，避免 JavaScript 浮点误差。

### 当前交付

当前仓库已经打通可运行的首版闭环：

- 首次网页设置管理员、Argon2id 密码哈希、随机不透明会话、HttpOnly/SameSite Cookie、CSRF 和同源写请求校验。
- 单管理员、多账本；支持账本、用户自定义账户、收入/支出分类、账户余额和期间收入/支出汇总。
- 收入、支出、转账和零金额备注；转账在表格中显示为中性一行，数据库中保存两条平衡分录。
- 默认系统列为日期、说明、账户、分类、金额；支持文本和布尔自定义列，列与流水支持回收和恢复。
- 游标分页、日期/金额排序、问题详情 JSON、认证 SSE、Lua 命名函数热加载和受限精确十进制运行时。
- Drogon 同源托管 SvelteKit 静态产物；桌面紧凑表格、手机列表、详情抽屉和中文导航已覆盖浏览器验收。

以下能力已经在数据库/API 结构中预留，但尚未作为首版完成项宣称：公式增量重算、
持久化后台 worker、CSV COPY 流式导入/导出、完整选项/关联编辑器、大数据虚拟滚动和
性能发布工具。

### 前置条件

- macOS 或 Linux
- CMake 3.28+、Ninja、C++23 编译器
- Conan 2.29+、libsodium、PostgreSQL 客户端工具
- Node.js 22+、pnpm 10+
- Podman 和 `podman-machine-default`（仅用于开发数据库）

### 快速启动

宿主机如果已经占用 PostgreSQL `5432`，开发脚本可以使用 `55432`。下面的命令会创建
持久命名卷 `journalseed-postgres-data`，不会触碰宿主机已有数据库。

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

浏览器打开 <http://127.0.0.1:8080/>，首次访问会显示管理员设置页。JournalSeed
没有内置固定管理员；测试脚本在首次运行时默认使用下面的凭据创建管理员，之后复用同一凭据登录：

```text
用户名：admin
密码：JournalSeed-Test-2026!
```

若使用 JSON 配置，可复制 `config.example.json` 为 `config.json`。配置优先级为命令行参数、
环境变量、JSON 文件、内置默认值。迁移器可单独运行：

```sh
./build/Debug/backend/journalseed --migrate-only
```

开发前端时也可以运行：

```sh
pnpm --dir frontend dev
```

Vite 会在 `127.0.0.1:5173` 提供热更新，并把 `/api` 代理到 `127.0.0.1:8080`。
生产构建会写入被 Git 忽略的 `frontend/build`，再由 Drogon 在同一端口托管。

### 配置

命令行参数：

| 参数                          | 说明                                    |
| ----------------------------- | --------------------------------------- |
| `--host <地址>`               | 监听地址，默认 `127.0.0.1`              |
| `--port <端口>`               | 监听端口，默认 `8080`                   |
| `--config <路径>`             | JSON 配置文件路径，默认 `./config.json` |
| `--database-url <URL>`        | PostgreSQL 连接地址                     |
| `--lua-dir <路径>`            | Lua 命名函数目录                        |
| `--static-dir <路径>`         | SvelteKit 静态产物目录                  |
| `--background-workers <数量>` | 后台工作线程数量                        |
| `--migrate-only`              | 只执行迁移后退出                        |

环境变量：

| 环境变量                         | 默认值                                                            |
| -------------------------------- | ----------------------------------------------------------------- |
| `JOURNALSEED_HOST`               | `127.0.0.1`                                                       |
| `JOURNALSEED_PORT`               | `8080`                                                            |
| `JOURNALSEED_DATABASE_URL`       | `postgresql://journalseed:journalseed@127.0.0.1:5432/journalseed` |
| `JOURNALSEED_LUA_DIR`            | `./scripts/functions`                                             |
| `JOURNALSEED_STATIC_DIR`         | `./frontend/build`                                                |
| `JOURNALSEED_BACKGROUND_WORKERS` | `2`                                                               |

### 验证

```sh
# C++ 单元测试（当前 16 项）
ctest --test-dir build/server-check --output-on-failure

# 前端类型检查、单元测试、格式检查和生产构建
pnpm --dir frontend check
pnpm --dir frontend test
pnpm --dir frontend format:check
pnpm --dir frontend build

# 需要正在运行的 PostgreSQL 和 JournalSeed 服务
./scripts/test/api-smoke.sh

# 需要正在运行的服务；默认使用 127.0.0.1:8080
pnpm --dir frontend exec playwright install chromium
pnpm --dir frontend test:e2e
```

Playwright CLI 的人工验收配置位于 `.playwright/cli.config.json`，截图和日志统一写到
被忽略的 `output/playwright/`。E2E 测试和 API 冒烟测试可通过
`JOURNALSEED_BASE_URL`、`JOURNALSEED_TEST_USERNAME` 和 `JOURNALSEED_TEST_PASSWORD`
覆盖默认值。

### 工程布局

```text
backend/                 C++ 领域、应用服务、Drogon API、Lua 和 PostgreSQL 仓储
frontend/                SvelteKit 页面、API 客户端、Vitest 和 Playwright
migrations/              带 SHA-256 校验和的 PostgreSQL 迁移
scripts/functions/       Lua 命名函数（每个文件返回一个函数定义 table）
scripts/dev/             Podman machine 与 PostgreSQL 开发脚本
scripts/test/            可重复 API 冒烟验收
docs/openapi/            HTTP 契约及生成的 TypeScript 类型输入
docs/adr/                模块化单体、类型化单元格、复式账务、Lua 和会话决策记录
```

后端控制器只做协议转换，应用服务负责事务边界，领域模块负责金额/分录规则，仓储封装 SQL。
迁移器使用 PostgreSQL advisory lock、单连接事务、`schema_migrations` 和脚本校验和，
多个进程同时启动时只会执行一次迁移。

### 数据与安全约束

- 外部主键使用 PostgreSQL `uuidv7()`，内部连接使用 `BIGINT`；类型化单元格按列哈希分区。
- 金额固定两位小数并采用精确十进制；Lua 公式常量应通过 `dec("0.1")` 创建。
- Lua 只开放基础数学、字符串、表和受控十进制 userdata；文件、网络、进程、`os`、`io`、`package`、`debug` 等入口被禁用，并有指令数/墙钟上限。
- 写请求要求同源 `Sec-Fetch-Site`（存在时）和旋转的 `X-JournalSeed-CSRF`；响应使用问题详情格式、`X-Frame-Options`、nosniff 和 Referrer-Policy。
- SvelteKit 静态 HTML 在构建时生成哈希 CSP，后端读取 `index.html` 中的 CSP 并补充 `frame-ancestors 'none'`。

### API 概览

REST 接口以 `/api/v1` 开头，契约文件位于 `docs/openapi/journalseed.yaml`，前端类型由
`pnpm --dir frontend generate:api` 生成到 `frontend/src/lib/api/schema.d.ts`。

当前主要接口：

- `/setup/status`、`/setup`
- `/auth/session`、`/auth/login`、`/auth/logout`
- `/ledgers`、`/ledgers/{ledgerId}/summary`
- `/ledgers/{ledgerId}/accounts`
- `/ledgers/{ledgerId}/categories`
- `/ledgers/{ledgerId}/columns`
- `/ledgers/{ledgerId}/rows`
- `/rows/{rowId}`、`/rows/{rowId}/restore`
- `/columns/{columnId}`、`/columns/{columnId}/restore`
- `/functions`、`/functions/{name}/invoke`
- `/jobs`、`/jobs/{jobId}/cancel`
- `/events`

### Lua 命名函数

`scripts/functions/*.lua` 每个文件返回一个函数定义 table，例如：

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

注册表通过轮询热加载目录；新脚本全部验证成功后才会替换当前快照，失败时继续使用上一份有效快照。

### 后续阶段

以下工作保留在计划阶段，接口和迁移中的预留表不代表功能已经完成：

1. 公式列的依赖声明、循环检测、增量/全量 revision 切换、错误单元格和后台恢复。
2. 选项/关联/公式列的完整编辑体验、服务端组合筛选、虚拟滚动和批量单元格读取。
3. CSV 预览映射、COPY 分块导入、流式导出、取消和错误报告。
4. `journal_bench seed/load/recalc`、50 万行压测、PGO/LTO 调优和可复现发布报告。

依赖版本由根目录 `conan.lock` 和前端 `pnpm-lock.yaml` 固定。每次提交前必须核对
README 与当前代码、启动命令、功能边界和验证方式一致，并保持迁移、OpenAPI、测试和锁文件同步。

---

## English version

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
- Cursor pagination, date/amount sorting, problem-detail JSON responses, authenticated SSE, hot-reloaded Lua named functions, and a restricted exact-decimal runtime.
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
# C++ unit tests, currently 16 tests
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
- `/functions`, `/functions/{name}/invoke`
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

The registry hot-reloads the directory by polling. A new snapshot replaces the current registry only
after all scripts validate successfully; on validation failure, the previous valid snapshot remains active.

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
