# JournalSeed

JournalSeed 是一个中文优先、单管理员的网页账本。前端使用 Svelte 5，后端使用
C++23、Drogon 和 PostgreSQL；账务写入始终落成平衡的复式分录，金额在 HTTP
边界使用字符串传输，避免 JavaScript 浮点误差。

## 当前交付

当前仓库已经打通可运行的首版闭环：

- 首次网页设置管理员、Argon2id 密码哈希、随机不透明会话、HttpOnly/SameSite Cookie、CSRF 和同源写请求校验。
- 账本、账户、收入/支出分类，以及账户余额和期间收入/支出汇总。
- 收入、支出、转账和零金额备注；转账显示为中性一行，数据库中保存两条平衡分录。
- 日期、说明、账户、分类、金额系统列和文本/布尔自定义列；列与流水支持回收和恢复。
- 游标分页、日期/金额排序、问题详情 JSON、认证 SSE、Lua 命名函数热加载和受限精确十进制运行时。
- Drogon 同源托管 SvelteKit 静态产物；桌面表格、手机列表、详情抽屉和中文导航已覆盖浏览器验收。

公式增量重算、持久化后台 worker、CSV COPY 流式导入/导出、完整选项/关联编辑器、
大数据虚拟滚动和性能发布工具已经在数据库/API 结构中预留，尚未作为首版完成项宣称。

## 前置条件

- macOS 或 Linux、CMake 3.28+、Ninja、C++23 编译器
- Conan 2.29+、libsodium、PostgreSQL 客户端工具
- Node.js 22+、pnpm 10+
- Podman 和 `podman-machine-default`（仅用于开发数据库）

## 快速启动

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
./build/Debug/backend/journalseed --host 127.0.0.1 --port 8080
```

浏览器打开 <http://127.0.0.1:8080/>，首次访问会显示管理员设置页。若使用 JSON 配置，
可复制 `config.example.json` 为 `config.json`；配置优先级为命令行参数、环境变量、
JSON 文件、内置默认值。迁移器可单独运行：

```sh
./build/Debug/backend/journalseed --migrate-only
```

开发时也可以运行 `pnpm --dir frontend dev`，Vite 在 `127.0.0.1:5173` 提供热更新，
并把 `/api` 代理到 `127.0.0.1:8080`。生产构建会写入被 Git 忽略的 `frontend/build`，
再由 Drogon 在同一端口托管。

## 验证

```sh
# C++ 单元测试（当前 16 项）
ctest --test-dir build/server-check --output-on-failure

# 前端类型检查和单元测试
pnpm --dir frontend check
pnpm --dir frontend test
pnpm --dir frontend build

# 需要正在运行的 PostgreSQL 和 JournalSeed 服务
./scripts/test/api-smoke.sh

# 需要正在运行的服务；默认使用 127.0.0.1:8080
pnpm --dir frontend exec playwright install chromium
pnpm --dir frontend test:e2e
```

Playwright CLI 的人工验收配置位于 `.playwright/cli.config.json`，截图和日志统一写到
被忽略的 `output/playwright/`。E2E 测试可通过 `JOURNALSEED_BASE_URL`、
`JOURNALSEED_TEST_USERNAME` 和 `JOURNALSEED_TEST_PASSWORD` 覆盖默认值。

## 工程布局

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

后端控制器只做协议转换，应用服务负责事务边界，领域模块负责金额/分录规则，仓储
封装 SQL。迁移器使用 PostgreSQL advisory lock、单连接事务、`schema_migrations` 和
脚本校验和，多个进程同时启动时只会执行一次迁移。

## 数据与安全约束

- 外部主键使用 PostgreSQL `uuidv7()`，内部连接使用 `BIGINT`；类型化单元格按列哈希分区。
- 金额固定两位小数并采用精确十进制；Lua 公式常量应通过 `dec("0.1")` 创建。
- Lua 只开放基础数学、字符串、表和受控十进制 userdata；文件、网络、进程、`os`、`io`、`package`、`debug` 等入口被禁用，并有指令数/墙钟上限。
- 写请求要求同源 `Sec-Fetch-Site`（存在时）和旋转的 `X-JournalSeed-CSRF`；响应使用问题详情格式、`X-Frame-Options`、nosniff 和 Referrer-Policy。
- SvelteKit 静态 HTML 在构建时生成哈希 CSP，后端不再用冲突的通用内联脚本策略覆盖它。

## 后续阶段

以下工作保留在计划阶段，接口和迁移中的预留表不代表功能已经完成：

1. 公式列的依赖声明、循环检测、增量/全量 revision 切换、错误单元格和后台恢复。
2. 选项/关联/公式列的完整编辑体验、服务端组合筛选、虚拟滚动和批量单元格读取。
3. CSV 预览映射、COPY 分块导入、流式导出、取消和错误报告。
4. `journal_bench seed/load/recalc`、50 万行压测、PGO/LTO 调优和可复现发布报告。

依赖版本由根目录 `conan.lock` 和前端 `pnpm-lock.yaml` 固定。每次提交前必须核对
README 与当前代码、启动命令、功能边界和验证方式一致，并保持迁移、OpenAPI、测试
和锁文件同步。
