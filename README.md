# JournalSeed

[中文](README.md) | [English](README.en.md)

JournalSeed 是一个中文优先、单管理员的网页账本。前端使用 Svelte 5 与 SvelteKit
静态适配器，后端使用 C++23、Drogon 和 PostgreSQL；账务写入始终落成平衡的复式分录，
金额在 HTTP 边界使用字符串传输，避免 JavaScript 浮点误差。

> **升级须知：链配置密钥派生方式已变更（破坏性）**
>
> `JOURNALSEED_CHAIN_SETTINGS_KEY` 的派生方式已从裸 BLAKE2b 改为「32 字节
> 十六进制/Base64 密钥，或对口令做 Argon2id」。**用旧方案加密保存的链配置无法再解密**：
> 升级后请到链设置页面把 TronGrid / Etherscan API Key 和自定义 RPC URL 重新填写并保存一次。
> 在此之前，钱包同步会明确返回 `chain_settings_secret_undecryptable`（500）并中止，
> 而不是像以前那样静默退化成不带凭据的匿名 RPC 调用。详见[配置](#配置)。

### 当前交付

当前仓库已经打通可运行的首版闭环：

- 首次网页设置管理员、Argon2id 密码哈希、随机不透明会话、HttpOnly/SameSite Cookie、CSRF 和同源写请求校验。
- 单管理员、多账本；支持账本、用户自定义账户、收入/支出分类、账户余额和期间收入/支出汇总。汇总接口的 `from`/`to` 是闭区间日期并且真正生效，按资产分别给出余额与期间收支。
- 收入、支出、转账和零金额备注；转账在表格中显示为中性一行，数据库中保存两条平衡分录。
- 默认系统列为日期、说明、账户、分类、金额（系统金额列的类型是只读的 `money`，不能通过建列接口创建）；自定义列可选文本、数字、日期、复选框、单选、关联、公式七种类型，列与流水支持回收和恢复。
- 游标分页、日期/金额排序、问题详情 JSON、认证 SSE、Lua 命名函数热加载、网页新增/编辑函数脚本和受限精确十进制运行时。
- Drogon 同源托管 SvelteKit 静态产物；桌面紧凑表格、手机列表、详情抽屉和中文导航已覆盖浏览器验收。
- 内置 TRON、Ethereum、Polygon 和 Solana Mainnet watch-only 钱包同步的完整取数与落库链路、钱包同步页面、地址标签和链交易视图。只保存公开地址，不需要也不接受私钥；四条链的 provider 适配器会把转账、内部转账和手续费解析成按资产精度归一化的流水项，出网目标每次调用前都要过 SSRF 校验。
- 每个钱包可设置接受的货币：预设 USDT / USDC，也可多选、添加任意代币合约；其余币种（包括垃圾币）一律不入账。可为每种货币填写汇率，按“数量 × 汇率”以本位币记入指定账户。只记录添加钱包之后的交易，钱包可手动暂停 / 恢复同步，恢复后从暂停处补记。

以下能力已经在数据库/API 结构中预留，但尚未作为首版完成项宣称：公式增量重算、
持久化后台 worker、CSV COPY 流式导入/导出、完整选项/关联编辑器、大数据虚拟滚动和
性能发布工具。

**钱包同步的落库环节已经打通**：`PostgresRepository::record_wallet_sync` 在同一个数据库
事务里写入链交易、资产、每个资产的钱包账户与结转账户、资产流水，以及自动生成的流水行和
分录，并推进 `wallet_sync_states` 的检查点。同步接口返回的三个计数是真实写入量，链交易
视图也能读到数据；流水读取查询也会通过 `chain_movement_row_links` 回填
`JournalRow.chainSource`。首次同步仍拉最近一批交易；之后会读取检查点做增量续拉。

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
| `--help`                      | 显示命令行帮助                          |

环境变量（可与命令行参数、JSON 配置互换）：

| 环境变量                         | 默认值                                                            |
| -------------------------------- | ----------------------------------------------------------------- |
| `JOURNALSEED_HOST`               | `127.0.0.1`                                                       |
| `JOURNALSEED_PORT`               | `8080`                                                            |
| `JOURNALSEED_DATABASE_URL`       | `postgresql://journalseed:journalseed@127.0.0.1:5432/journalseed` |
| `JOURNALSEED_LUA_DIR`            | `./scripts/functions`                                             |
| `JOURNALSEED_STATIC_DIR`         | `./frontend/build`                                                |
| `JOURNALSEED_BACKGROUND_WORKERS` | `2`                                                               |

以下变量**只从进程环境读取**：`config.json` 和命令行参数都不支持，管理 API 也无法改写。

| 环境变量                          | 默认值                              | 说明                                                             |
| --------------------------------- | ----------------------------------- | ---------------------------------------------------------------- |
| `JOURNALSEED_CHAIN_SETTINGS_KEY`  | 未设置                              | 加密链配置中 API Key 与自定义 RPC URL 的主密钥，见下              |
| `JOURNALSEED_RPC_ALLOW_HOSTS`     | 未设置                              | 出网校验白名单，逗号分隔的主机名，见下                            |
| `JOURNALSEED_ETHERSCAN_BASE_URL`  | `https://api.etherscan.io`          | Ethereum/Polygon 转账历史使用的 Etherscan V2 基地址               |
| `JOURNALSEED_BLOCKSCOUT_ETHEREUM_URL` / `JOURNALSEED_BLOCKSCOUT_POLYGON_URL` | `https://eth.blockscout.com` / `https://polygon.blockscout.com` | 未配置 Etherscan API Key 时使用的免 Key Blockscout 基地址 |
| `JOURNALSEED_SOLANA_RPC_URL`      | `https://api.mainnet-beta.solana.com` | 未保存自定义 Solana RPC URL 时使用的默认 endpoint               |
| `JOURNALSEED_MOCK_CHAIN_SYNC`     | 未设置                              | 设为 `1` 用确定性 mock 数据同步；`JOURNALSEED_MOCK_MULTICHAIN_SYNC` 为等价别名 |
| `JOURNALSEED_MOCK_TRON_SYNC`      | 未设置                              | 设为 `1` 只对 TRON 钱包使用 mock 数据                             |

#### `JOURNALSEED_CHAIN_SETTINGS_KEY`

保存 TronGrid / Etherscan API Key 或自定义 RPC URL 时必需。接受两种形式：

- **32 字节随机密钥**，写成十六进制（64 个字符）或 Base64（含 URL-safe 与省略 padding 的写法），直接当作密钥使用；
- **口令**，至少 16 个字符，用固定 salt 的 Argon2id 派生出 32 字节密钥。少于 16 个字符会被拒绝。

推荐直接生成随机密钥：

```sh
openssl rand -hex 32
```

派生结果在进程内缓存并用 `sodium_mlock` 钉在内存中，避免主密钥被换页到磁盘。
变量未设置或强度不足时，保存链配置会返回 `chain_settings_key_unconfigured`（500）。

> **破坏性变更。** 派生方式已从裸 BLAKE2b 改为上述方案，因此**用旧方案加密的链配置
> 无法再解密**。升级后请在链设置页面把这些凭据重新填写并保存一次。在此之前，读取这些
> 凭据的操作（钱包同步）会明确返回 `chain_settings_secret_undecryptable`（500），
> 而不是静默退化成不带凭据的匿名 RPC 调用。轮换密钥同理。

#### `JOURNALSEED_RPC_ALLOW_HOSTS`

出网（SSRF）校验的逃生舱：逗号分隔的主机名列表（大小写不敏感，允许前后空格），
列表中的主机跳过「解析结果不得落在回环、内网、运营商级 NAT、链路本地（含云元数据
地址）、基准测试段、组播与保留段」的检查。用于节点确实部署在内网或本机的场景，例如
`JOURNALSEED_RPC_ALLOW_HOSTS=127.0.0.1,geth.internal`。

**为什么只认进程环境、不做成管理 API 配置项**：这条开关的作用恰恰是允许服务端向内网
发起请求。如果它能通过网页写入，那么拿到管理员会话的攻击者只要先把自己的目标主机加进
白名单，再把 RPC URL 指过去，整套 SSRF 防护就形同虚设。放在进程环境里，改动它需要
宿主机的 shell 权限，这条提权路径也就被切断了。

出网校验采用「解析时校验」而不是固定白名单，因为自建部署会接各种自选 RPC 供应商。
残余风险是 DNS 重绑定：校验时解析出的地址与实际建连时解析出的地址可能不同。彻底关闭
这个窗口需要按 IP 建连并单独设置 SNI/Host，代价与本项目定位不相称，因此明确接受。

### 多链钱包同步

首版支持 TRON、Ethereum、Polygon 和 Solana Mainnet 的 watch-only 地址同步。添加钱包只需公开地址，JournalSeed 不支持签名或转账，也不会请求或保存私钥。支持的链标识为 `tron-mainnet`、`ethereum-mainnet`、`polygon-mainnet` 和 `solana-mainnet`；TRON 使用 Base58Check 地址，Ethereum/Polygon 使用 `0x` EVM 地址，Solana 使用 base58 地址。

链设置包含 TronGrid API Key、Etherscan API Key 以及 Ethereum/Polygon/Solana 自定义 RPC URL。TronGrid 和 Etherscan API Key 都是可选项；Ethereum 和 Polygon 转账历史使用 Etherscan V2 account APIs 查询；未配置 Etherscan API Key 时自动改用该链的公共 Blockscout（免 Key，但每个 IP 约每 30 分钟只有 10 次请求，频繁手动同步会被限流），配置免费的 Etherscan Key 即可解除限制。只接受代币、不接受原生币的钱包每次同步只发 1 次请求；同步失败的钱包会等一个同步间隔再自动重试。Solana 同步使用 RPC，未配置自定义 Solana RPC URL 时使用公共 endpoint，public RPC 容易触发限流，手动验收或长期运行建议配置自己的 RPC URL；Ethereum/Polygon RPC URL 作为加密链配置保留，便于后续 provider 扩展，当前取数路径并不读取它们。要在网页链设置中保存 API Key 或自定义 RPC URL，服务端必须设置 `JOURNALSEED_CHAIN_SETTINGS_KEY`，该变量仅用于加密保存的密钥/URL，接口不会返回密钥本身。启用自动同步的钱包由后端约每 60 秒扫描一次，并按链设置里的同步间隔判断是否到期；钱包页面也提供“立即同步”。

保存和使用自定义 RPC URL 时都要过出网校验：保存时不通过会返回带字段说明的 `validation_error`（422），同步时不通过会返回 `rpc_endpoint_rejected`（422）。同步前会重跑一次校验——保存时安全不代表现在仍然安全，DNS 可能已经改指内网。

金额使用资产自己的 decimals：默认本位资产 `DEFAULT` 为 2 位小数，TRX/USDT 为 6 位，ETH 为 18 位，Polygon 原生资产 POL 为 18 位，SOL 为 9 位。各链适配器会把转入、转出、自转（internal）和手续费（fee）解析成按上述精度归一化的流水项。地址标签可将对手方地址显示为客户、自有地址、交易所、商户、合约或其他名称；标签匹配查询已支持 EVM 共享 scope，同一个 `0x` 地址可以在 Ethereum 和 Polygon 上共用一条标签，但创建接口目前只接受四个具体链标识，因此实际存下来的标签全部按链隔离——这是一个未决设计问题，`docs/openapi/journalseed.yaml` 的 `AddressLabel.scope` 下记录了现状和两种候选方案。

**落库行为。** `PostgresRepository::record_wallet_sync` 把一次同步的结果放进同一个数据库事务：先按 `public_id` 锁住钱包行并把 `wallet_sync_states` 置为 `running`，再依次写入 `chain_transactions`（按「账本 + 链 + `tx_hash`」upsert，重新同步时刷新区块高度、区块时间、确认与成功状态）、`assets`（按「账本 + 链 + 归一化合约地址」，原生资产按符号归一，已存在的资产不改写）、每个资产一套账户——钱包自己的账户（`accounts` 加 `wallet_asset_accounts`，重名时自动追加编号）和「收入结转 · 符号」「支出结转 · 符号」两个系统结转账户、`chain_asset_movements`（按「账本 + `movement_key`」去重）、`journal_rows` 与 `postings`，最后写 `chain_movement_row_links` 把流水项和流水行绑定。任何一步失败整次同步回滚；流水行写完后还会把默认延迟到提交时才检查的分录平衡约束提前为立即检查，避免同步已经报告成功、事务却在提交时才失败。

方向映射：`incoming` 记为收入（钱包账户 +金额，对手方为收入结转账户），`outgoing` 和 `fee` 记为支出（钱包账户 -金额，对手方为支出结转账户），后两者只有说明文案不同；`internal` 是自有地址之间的转账，两侧钱包各自同步时都会看到它，记成金额会重复计入，因此只生成一行金额为 `0` 的备注（`note`）行、不写任何分录。其他方向上金额为 `0` 的流水项只写入资产流水、不生成流水行，因为 `postings` 上有 `signed_amount <> 0` 约束。

幂等性由 `chain_movement_row_links` 保证：只有还没有链接的资产流水项才会生成流水行。因此重复同步同一窗口不会产生重复数据，已经链接过的流水项也不会被后续同步改写——用户对自动生成的流水行做的编辑会被保留。

**同步生成的流水行可以照常编辑。** 这类流水行指向的是钱包在该资产上的账户（是普通账户，不是内部系统账户，因此会出现在账户列表里），`PATCH /rows/{rowId}` 按同一套规则把资产解析回该链上资产，分录也按该资产自己的结转账户重建；这两点以前都不成立，编辑任何同步生成的行都会返回 500。编辑不触碰 `chain_movement_row_links`，所以 `chainSource` 在编辑后依然在，后续同步也仍然跳过这条已链接的流水项。要注意编辑接口收的是整行而不是增量：`entry` 行如果把 `accountId` 去掉，它会落回本位资产的未分配账户，资产随之变回 `DEFAULT`，超过两位小数的金额会被拒绝。

三个计数是真实值：`transactionsSeen` 是本次写入或刷新的链交易条数，`movementsCreated` 是本次新插入的资产流水条数（重复同步同一窗口为 `0`），`rowsCreated` 是本次新生成并链接的流水行数（含 `internal` 的零金额备注行）。

检查点：同步成功后 `wallet_sync_states` 置回 `idle`，`checkpoint_block` 与 `checkpoint_timestamp` 只增不减（空窗口保持原值），`checkpoint_signature` 取本窗口最新一笔交易的哈希，供 Solana 按签名而不是区块高度续拉，且只有该交易的时间不早于已存检查点时间时才替换。首次同步仍拉最近一批（TRON 与 EVM 各 100 条、Solana 50 条）；之后取数会读这些检查点，按 `min_timestamp` / `startblock` / `until` 续拉，最多再翻 8 页。同一钱包在 `running` 时会拒绝并发同步。

链上来源双向可读：流水读取查询会顺着 `chain_movement_row_links` 回填 `JournalRow.chainSource`，所以同步生成的流水行带有该字段，手工录入的普通流水行没有——字段是否存在可以直接当作「这行来自钱包同步」的判据；反向的 `ChainTransaction.rowId` 同样有值。

当前边界：不支持签名或链上转账、swap、NFT、staking、跨链桥或复杂合约语义；它是账本同步器，不是钱包客户端。

### 验证

```sh
# C++ 单元测试
ctest --preset conan-debug --output-on-failure

# 前端类型检查、单元测试、格式检查和生产构建
pnpm --dir frontend check
pnpm --dir frontend test
pnpm --dir frontend format:check
pnpm --dir frontend build

# 需要正在运行的 PostgreSQL 和 JournalSeed 服务
./scripts/test/api-smoke.sh

# 手动验收钱包同步时，以确定性多链交易数据启动服务
# 兼容旧 TRON-only mock：JOURNALSEED_MOCK_TRON_SYNC=1
JOURNALSEED_MOCK_CHAIN_SYNC=1 ./build/Debug/backend/journalseed --host 127.0.0.1 --port 8080

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
- 金额采用精确十进制，并按资产的 decimals 解释和显示：`DEFAULT` 为 2 位小数，TRX/USDT 为 6 位，ETH/POL 为 18 位，SOL 为 9 位；Lua 公式常量应通过 `dec("0.1")` 创建。返回值一律按 `round(value, assets.decimals)` 渲染，同一种资产的所有金额字段（流水金额、账户余额与期初余额、汇总数字、链上流水项金额）小数位完全一致。
- **一行流水的资产由它所指的账户决定**，解析顺序固定：账户的资产优先；没有账户时用这一行原有的资产（只有备注行会走到这一档）；再没有才用账本的本位资产。因此在钱包同步建出来的链上资产账户上手工记一笔，这一行就属于那个资产，金额可以带满 18 位小数；`entry` 行不填账户则落到本位资产的未分配账户，只能保留两位小数。写入时小数位超出该行资产的精度，返回带 `fields.amount` 的 `validation_error`（422），不会被静默取整；只有根本解析不出十进制数（`abc`、`1.`、`1.2.3`、`1e5`）才是 `invalid_amount`。
- 转账的两个账户不在同一资产上，返回带 `fields.transferAccountId` 的 `validation_error`（422）：一行流水只有一个资产、它的分录必须同资产，跨资产转账无法表达。`POST /ledgers/{ledgerId}/accounts` 建出的账户固定使用本位资产，`openingBalance` 因此限定两位小数（链上资产的账户由钱包同步创建）。
- Lua 只开放基础数学、字符串、表和受控十进制 userdata；文件、网络、进程、`os`、`io`、`package`、`debug` 等入口被禁用，并有指令数/墙钟上限。
- 写请求要求同源 `Sec-Fetch-Site`（存在时）和旋转的 `X-JournalSeed-CSRF`；响应使用问题详情格式、`X-Frame-Options`、nosniff 和 Referrer-Policy。
- SvelteKit 静态 HTML 在构建时生成哈希 CSP，后端读取 `index.html` 中的 CSP 并补充 `frame-ancestors 'none'`。

### 账本汇总

`GET /ledgers/{ledgerId}/summary` 的 `from`/`to` 曾经只写在文档里、实际被忽略，现在已经真正
生效：两端都是闭区间的 `YYYY-MM-DD` 日期，作用在 `journal_rows.occurred_on` 上，并且按公历
严格校验（`2026-02-30` 会被拒绝而不是丢进 SQL），`from` 不能晚于 `to`；不合法时返回带
`fields.from` / `fields.to` 的 `validation_error`（422）。缺省或空串表示该侧不设限。

**`from` 只约束流量。** `income`、`expense`、`rowCount` 是流量，两端都受限；`balance` 是存量，
含义是**截至 `to` 的期末余额**——包含期初余额在内、日期不晚于 `to` 的全部分录。给余额加左端点
会把它变成一段期间的变动额，与账户接口返回的余额对不上，因此这是刻意保留的设计。

**标量字段只统计账本的本位资产。** `balance`、`income`、`expense` 过去是把 ETH、CNY、SOL 这些
不可通约的资产加在一起再取两位小数，那不是任何东西的数量；现在它们只表示账本本位资产的数字，
按该资产自己的精度渲染。其余资产在 `assetSummaries` 中逐项给出（每种资产一条、本位资产排在第一条，
该数组过去恒为空）。`rowCount` 仍然是跨资产的账本级计数——条数在不同资产之间是可以相加的。
单资产账本（从未同步过钱包）的取值没有变化；把标量当成全账本合计来读的客户端需要改用
`assetSummaries`。

### API 概览

REST 接口以 `/api/v1` 开头，契约文件位于 `docs/openapi/journalseed.yaml`，前端类型由
`pnpm --dir frontend generate:api` 生成到 `frontend/src/lib/api/schema.d.ts`。

错误一律是 `application/problem+json`，`code` 的完整取值列在契约的 `Problem.code` 下，
`type` 恒为 `/problems/{code}`（`database_error` 对应 `/problems/database_error`，曾经错写成
带连字符的 `/problems/database-error`）。近期有几处变化值得注意：`csrf_mismatch` 是 **403**
而不是 401，并且**缺失** `X-JournalSeed-CSRF` 头也归入它（以前返回 401 `session_required`，
会被前端误判成会话过期）；`invalid_json`（400）现在严格表示「请求体缺失、不是可解析的 JSON，
或者顶层 JSON 值之后还有多余内容」，且不再回显解析器输出——`{"name":"X"} trailing` 这样的
请求体以前会被照单全收并真的创建出对象。语法合法但不符合接口字段定义的请求体、非 UUID 的
路径参数、非法的 `from`/`to`、分类方向与账户归属错误、跨资产转账（带 `fields.transferAccountId`）、
以及超出该行资产精度的金额（带 `fields.amount`），都归入 `validation_error`（422）。找不到
目标对象统一返回 404 与 `<entity>_not_found`：`ledger_not_found`、`account_not_found`、
`category_not_found`、`column_not_found`、`row_not_found`、`job_not_found`、
`wallet_not_found`、`address_label_not_found`。

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
- `/functions`、`/functions/{name}`、`/functions/{name}/invoke`
- `/jobs`、`/jobs/{jobId}/cancel`
- `/chain-settings`
- `/ledgers/{ledgerId}/wallets`、`/wallets/{walletId}`、`/wallets/{walletId}/sync`
- `/ledgers/{ledgerId}/chain-transactions`
- `/ledgers/{ledgerId}/address-labels`、`/address-labels/{labelId}`
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

注册表通过轮询热加载目录；也可以在“函数”页面新增或编辑 Lua 脚本。保存时会先校验并重新载入全部脚本，全部验证成功后才会替换当前快照，失败时继续使用上一份有效快照并回滚文件。
函数页面内置“自定义函数教程”，说明 `name`、`version`、`description`、`params`、`run` 的结构、`dec()` 精确数字写法、预览返回值和保存校验规则，并提供“用教程示例新建”的可编辑 Lua 模板。

### 已知缺口

以下问题已经确认存在但尚未修复。它们不是“计划中的后续阶段”，而是当前实现的边界，
列在这里是为了不让接口和迁移里的预留结构被误读成已完成的能力。

1. **地址标记的 `evm` 共享作用域不可达。** 数据库和仓储层都支持 `scope='evm'`（一个标记
   同时覆盖 Ethereum 与 Polygon），迁移 `0003` 也为此建了 `wallet_address_labels_evm_unique`
   索引，但服务层的链校验只接受四个具体链代码，`evm` 会被 422 拒绝，所以这条路径永远走不到。
   同一个 `0x` 地址在两条链上目前必须分别标记。可选的修复方向有两个——把 `evm` 加进
   `AddressLabel.chain` 枚举，或者新增一列记录标记的来源链——因为涉及取舍，没有擅自决定。
2. **超过 4 MiB 的请求体返回不带响应体的 413。** 应用层上限是 1 MiB，超过时返回规范的
   `body_too_large` 问题详情；但 4 MiB 以上会被 Drogon 在传输层直接拒绝，那一层无法附带
   JSON 响应体。介于两者之间的请求行为符合文档。
3. **链配置密钥不支持轮换。** `chain_settings` 没有密钥版本列，密文本身也不带版本标记，
   因此更换 `JOURNALSEED_CHAIN_SETTINGS_KEY` 会让已保存的密钥全部无法解密。目前的处理是
   明确返回 `chain_settings_secret_undecryptable`（500）提示重新录入，而不是静默退化成匿名
   调用；真正的轮换需要一次带 `key_version` 的迁移，外加重新加密流程。
4. **增量取数尚未接上。** 检查点只写不读，详见“多链钱包同步”一节。
5. **部分路径没有自动化验证。** 真实链上 RPC 的取数与失败路径（当前只有
   `JOURNALSEED_MOCK_CHAIN_SYNC=1` 的离线覆盖）、用户编辑与同步并发写同一笔流水的竞态、
   CSV 导入导出、Lua 脚本写入路径，均未纳入自动化测试。

### 后续阶段

以下工作保留在计划阶段，接口和迁移中的预留表不代表功能已经完成：

1. 公式列的依赖声明、循环检测、增量/全量 revision 切换、错误单元格和后台恢复。
2. 选项/关联/公式列的完整编辑体验、服务端组合筛选、虚拟滚动和批量单元格读取。
3. CSV 预览映射、COPY 分块导入、流式导出、取消和错误报告。
4. `journal_bench seed/load/recalc`、50 万行压测、PGO/LTO 调优和可复现发布报告。

依赖版本由根目录 `conan.lock` 和前端 `pnpm-lock.yaml` 固定。每次提交前必须核对
README 与当前代码、启动命令、功能边界和验证方式一致，并保持迁移、OpenAPI、测试和锁文件同步。
