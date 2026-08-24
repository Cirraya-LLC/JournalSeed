#pragma once

#include "journalseed/application/models.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace journalseed::infrastructure {

// 写入流水时分类确实存在，但它的收支方向与金额符号相反。这是调用方给错了 categoryId，
// 属于输入问题（422 validation_error + fields.categoryId），必须和“分类根本不存在”
// （404 category_not_found）分开报；两者合并成一条 SQL 谓词的年代里，二者都只能变成 500。
//
// 它派生自 std::invalid_argument，是为了让只按基类分诊的调用方也落在 422 一侧而不是 500；
// 服务层另有一条更靠前的 catch 负责生成带 fields 的文案。异常正文只用于日志。
//
// 定义在仓储头文件而不是 application/models.h：抛出方是仓储，接收方 journal_service.h
// 本来就 include 这个头文件，不需要为一个写入路径的信号再动共享契约。
class CategoryDirectionMismatch final : public std::invalid_argument {
  public:
    // required_direction 是金额符号所要求的方向（"income" / "expense"），
    // 即分类本应具备、实际却不是的那个方向。
    explicit CategoryDirectionMismatch(std::string required_direction)
        : std::invalid_argument("category direction does not match the amount sign"),
          required_direction_(std::move(required_direction)) {}

    [[nodiscard]] const std::string &required_direction() const noexcept {
        return required_direction_;
    }

  private:
    std::string required_direction_;
};

// 转账的两端账户不在同一资产上。一行流水只能有一个 asset_id，postings_balance_guard 也要求
// 一行的分录 count(DISTINCT asset_id) = 1，所以“把 0.5 ETH 转进一个 DEFAULT 账户”无法表达。
// 这同样是调用方给错了 transferAccountId（422 validation_error + fields.transferAccountId），
// 必须在写库前拦下来：否则触发器会在 INSERT 时抛 SQL 异常，客户端只能看到 500 database_error。
//
// 与 CategoryDirectionMismatch 同理派生自 std::invalid_argument：即使调用方只按基类分诊，
// 也会落在 422 一侧而不是 500。两个 symbol 供服务层拼文案，异常正文只用于日志。
class TransferAssetMismatch final : public std::invalid_argument {
  public:
    TransferAssetMismatch(std::string account_symbol, std::string transfer_symbol)
        : std::invalid_argument("transfer accounts must be denominated in one asset"),
          account_symbol_(std::move(account_symbol)),
          transfer_symbol_(std::move(transfer_symbol)) {}

    // 转出账户（accountId）所属资产的符号。
    [[nodiscard]] const std::string &account_symbol() const noexcept { return account_symbol_; }
    // 转入账户（transferAccountId）所属资产的符号。
    [[nodiscard]] const std::string &transfer_symbol() const noexcept { return transfer_symbol_; }

  private:
    std::string account_symbol_;
    std::string transfer_symbol_;
};

// 调用方重复输入了一个已被占用的取值：同名账本、同名账户、同名同方向的分类、同一账本里
// 已在监控的钱包地址、已经标注过的地址。这五种都只靠“重新敲一遍”就能触发，属于调用方的
// 输入问题（409 duplicate_value + fields.<字段>），不是服务端故障：和 404 一样，必须给出
// 明确的 Problem 并且不在服务端留下 ERROR 日志。此前它们一路撞到唯一索引上，客户端只能
// 看到 500 database_error，服务端还会记一条 ERROR。
//
// 判定不靠捕获 SQL 异常再读 SQLSTATE，也不靠匹配报错文案——drogon 1.9.13 的 PostgreSQL
// 后端两条出错路径（postgresql_impl/PgConnection.cc:429 与 PgBatchConnection.cc:536）都只
// 构造 drogon::orm::Failure(PQerrorMessage(...))，从不构造 SqlError，所以
// drogon::orm::SqlError::sqlState() 这个访问器在 PostgreSQL 上永远拿不到；约束名同样只存在
// 于那串报错文本里。因此改由写入语句自己对准具体的唯一索引做 ON CONFLICT ... DO NOTHING：
// 撞上冲突时 RETURNING 不产生行，仓储据此抛出本异常。约束的识别交给 PostgreSQL 的索引推断，
// 既不依赖任何文本匹配，也没有“先查再插”的竞态，更不会先在服务端产生一条 SQL 异常。
class DuplicateValue final : public std::runtime_error {
  public:
    // entity 与 EntityNotFound::entity() 用同一套英文名（ledger / account / category /
    // wallet / address_label），服务层据此选中文案；field 是请求体里承载冲突取值的字段名，
    // 直接进 Problem.fields，让调用方知道是哪一格撞了。异常正文只用于日志。
    DuplicateValue(std::string entity, std::string field)
        : std::runtime_error("duplicate value for " + entity + "." + field),
          entity_(std::move(entity)),
          field_(std::move(field)) {}

    [[nodiscard]] const std::string &entity() const noexcept { return entity_; }
    [[nodiscard]] const std::string &field() const noexcept { return field_; }

  private:
    std::string entity_;
    std::string field_;
};

struct UserRecord {
    std::int64_t id;
    std::string publicId;
    std::string username;
    std::string passwordHash;
};

struct SessionRecord {
    std::int64_t userId;
    std::string userPublicId;
    std::string username;
    std::string expiresAt;
};

struct SetupRecord {
    std::string userPublicId;
    std::string username;
    std::string expiresAt;
};

struct EncryptedSecretRecord {
    std::optional<std::string> ciphertextHex;
    std::optional<std::string> nonceHex;
};

struct ChainSettingsSecretRecord {
    EncryptedSecretRecord tronGridApiKey;
    EncryptedSecretRecord etherscanApiKey;
    EncryptedSecretRecord ethereumRpcUrl;
    EncryptedSecretRecord polygonRpcUrl;
    EncryptedSecretRecord solanaRpcUrl;
    std::int32_t syncIntervalMinutes{30};
};

struct ChainSettingsSecretPatch {
    std::optional<EncryptedSecretRecord> tronGridApiKey;
    std::optional<EncryptedSecretRecord> etherscanApiKey;
    std::optional<EncryptedSecretRecord> ethereumRpcUrl;
    std::optional<EncryptedSecretRecord> polygonRpcUrl;
    std::optional<EncryptedSecretRecord> solanaRpcUrl;
};

struct ChainAssetMovementInput {
    std::string movementKey;
    std::string direction;
    std::string assetSymbol;
    std::string assetName;
    std::int16_t assetDecimals;
    bool isNative{false};
    std::optional<std::string> contractAddress;
    std::optional<std::string> contractAddressNormalized;
    std::string amount;
    std::optional<std::string> fromAddress;
    std::optional<std::string> fromAddressNormalized;
    std::optional<std::string> toAddress;
    std::optional<std::string> toAddressNormalized;
};

struct ChainTransactionInput {
    std::string txHash;
    std::optional<std::int64_t> blockNumber;
    std::optional<std::int64_t> blockTimestampMs;
    bool confirmed{true};
    bool success{true};
    std::string rawJson;
    std::vector<ChainAssetMovementInput> movements;
};

// Resume cursor written by record_wallet_sync. An empty cursor means first sync:
// adapters keep the "latest N" window. Subsequent passes pass these fields so
// fetch can continue from the last persisted height / time / signature.
struct ChainFetchCursor {
    std::optional<std::int64_t> block;
    std::optional<std::int64_t> timestampMs;
    std::optional<std::string> signature;
};

// An asset together with the scale every amount denominated in it must fit and is
// rendered at. `symbol` only feeds user-facing wording (e.g. the cross-asset transfer
// rejection), never a lookup key.
struct AssetScaleRecord {
    std::int64_t id;
    std::int16_t decimals;
    std::string symbol;
};

struct SyncWriteStats {
    std::int64_t transactionsSeen{0};
    std::int64_t movementsCreated{0};
    std::int64_t rowsCreated{0};
};

class PostgresRepository final {
  public:
    explicit PostgresRepository(drogon::orm::DbClientPtr client);

    [[nodiscard]] drogon::Task<bool> setup_required() const;
    [[nodiscard]] drogon::Task<SetupRecord>
    create_initial_setup(const application::SetupRequest &input,
                         const std::string &password_hash,
                         const std::string &token_hash_hex,
                         const std::string &csrf_hash_hex) const;
    [[nodiscard]] drogon::Task<std::optional<UserRecord>>
    find_user(std::string_view username) const;
    [[nodiscard]] drogon::Task<SessionRecord>
    create_session(std::int64_t user_id,
                   const std::string &token_hash_hex,
                   const std::string &csrf_hash_hex) const;
    [[nodiscard]] drogon::Task<std::optional<SessionRecord>>
    find_session(const std::string &token_hash_hex,
                 const std::optional<std::string> &csrf_hash_hex) const;
    [[nodiscard]] drogon::Task<std::optional<SessionRecord>>
    rotate_session_csrf(const std::string &token_hash_hex,
                        const std::string &csrf_hash_hex) const;
    [[nodiscard]] drogon::Task<> delete_session(const std::string &token_hash_hex) const;

    [[nodiscard]] drogon::Task<std::vector<application::LedgerView>> list_ledgers() const;
    [[nodiscard]] drogon::Task<application::LedgerView>
    create_ledger(std::string_view name) const;
    // `from` and `to` are inclusive ISO dates (YYYY-MM-DD) matched against
    // journal_rows.occurred_on; std::nullopt or a blank string leaves that side open. They
    // bound income, expense and rowCount. balance is a stock and is reported as of `to`.
    [[nodiscard]] drogon::Task<application::LedgerSummaryView>
    ledger_summary(std::string_view ledger_public_id,
                   std::optional<std::string> from,
                   std::optional<std::string> to) const;

    [[nodiscard]] drogon::Task<std::vector<application::AccountView>>
    list_accounts(std::string_view ledger_public_id) const;
    [[nodiscard]] drogon::Task<application::AccountView>
    create_account(std::string_view ledger_public_id,
                   const application::AccountInput &input) const;

    [[nodiscard]] drogon::Task<std::vector<application::CategoryView>>
    list_categories(std::string_view ledger_public_id) const;
    [[nodiscard]] drogon::Task<application::CategoryView>
    create_category(std::string_view ledger_public_id,
                    const application::CategoryInput &input) const;

    [[nodiscard]] drogon::Task<std::vector<application::ColumnView>>
    list_columns(std::string_view ledger_public_id, bool recycled) const;
    [[nodiscard]] drogon::Task<application::ColumnView>
    create_column(std::string_view ledger_public_id,
                  const application::ColumnInput &input) const;
    [[nodiscard]] drogon::Task<application::ColumnView>
    update_column(std::string_view column_public_id,
                  const application::ColumnPatch &patch) const;
    [[nodiscard]] drogon::Task<> recycle_column(std::string_view column_public_id) const;
    [[nodiscard]] drogon::Task<> restore_column(std::string_view column_public_id) const;

    [[nodiscard]] drogon::Task<application::RowPage>
    list_rows(std::string_view ledger_public_id, const application::RowQuery &query) const;
    [[nodiscard]] drogon::Task<application::JournalRowView>
    create_row(std::string_view ledger_public_id,
               std::int64_t user_id,
               const application::RowInput &input) const;
    [[nodiscard]] drogon::Task<application::JournalRowView>
    update_row(std::string_view row_public_id,
               std::int64_t user_id,
               const application::RowInput &input) const;
    [[nodiscard]] drogon::Task<> recycle_row(std::string_view row_public_id) const;
    [[nodiscard]] drogon::Task<> restore_row(std::string_view row_public_id) const;

    [[nodiscard]] drogon::Task<std::vector<application::JobView>> list_jobs() const;
    [[nodiscard]] drogon::Task<> request_job_cancel(std::string_view job_public_id) const;

    [[nodiscard]] drogon::Task<application::ChainSettingsView> chain_settings() const;
    [[nodiscard]] drogon::Task<ChainSettingsSecretRecord> chain_settings_secret() const;
    [[nodiscard]] drogon::Task<application::ChainSettingsView> update_chain_settings(
        const application::ChainSettingsPatch &patch,
        const ChainSettingsSecretPatch &secrets) const;

    [[nodiscard]] drogon::Task<std::vector<application::WalletView>>
    list_wallets(std::string_view ledger_public_id) const;
    [[nodiscard]] drogon::Task<std::vector<std::string>>
    list_due_wallet_ids(std::uint16_t limit) const;
    [[nodiscard]] drogon::Task<application::WalletView>
    create_wallet(std::string_view ledger_public_id, const application::WalletInput &input,
                  std::string_view normalized_address) const;
    [[nodiscard]] drogon::Task<application::WalletView>
    update_wallet(std::string_view wallet_public_id, const application::WalletPatch &patch) const;
    [[nodiscard]] drogon::Task<> delete_wallet(std::string_view wallet_public_id) const;
    [[nodiscard]] drogon::Task<std::optional<application::WalletView>>
    wallet(std::string_view wallet_public_id) const;
    [[nodiscard]] drogon::Task<ChainFetchCursor>
    wallet_sync_cursor(std::string_view wallet_public_id) const;
    // Commits status='running' before any RPC. Returns false if another sync
    // already holds the row. Throws EntityNotFound when the wallet is missing.
    [[nodiscard]] drogon::Task<bool>
    try_begin_wallet_sync(std::string_view wallet_public_id) const;
    [[nodiscard]] drogon::Task<>
    fail_wallet_sync(std::string_view wallet_public_id, std::string_view error) const;
    [[nodiscard]] drogon::Task<SyncWriteStats> record_wallet_sync(
        std::string_view wallet_public_id, std::int64_t user_id,
        const std::vector<ChainTransactionInput> &transactions) const;
    [[nodiscard]] drogon::Task<application::JobView> create_wallet_sync_job(
        std::string_view wallet_public_id, std::string_view status, std::int64_t done,
        std::int64_t total, const std::optional<std::string> &error) const;
    [[nodiscard]] drogon::Task<std::optional<std::int64_t>> first_admin_user_id() const;

    [[nodiscard]] drogon::Task<std::vector<application::ChainTransactionView>>
    list_chain_transactions(std::string_view ledger_public_id, std::uint16_t limit) const;

    [[nodiscard]] drogon::Task<std::vector<application::AddressLabelView>>
    list_address_labels(std::string_view ledger_public_id) const;
    [[nodiscard]] drogon::Task<application::AddressLabelView>
    create_address_label(std::string_view ledger_public_id, const application::AddressLabelInput &input,
                         std::string_view normalized_address) const;
    [[nodiscard]] drogon::Task<application::AddressLabelView>
    update_address_label(std::string_view label_public_id, const application::AddressLabelPatch &patch) const;
    [[nodiscard]] drogon::Task<> delete_address_label(std::string_view label_public_id) const;

  private:
    [[nodiscard]] drogon::Task<std::int64_t>
    create_ledger_in_transaction(const drogon::orm::DbClientPtr &transaction,
                                 std::string_view name) const;
    [[nodiscard]] drogon::Task<application::JournalRowView>
    save_row(const drogon::orm::DbClientPtr &transaction,
             std::optional<std::int64_t> existing_row_id,
             std::int64_t ledger_id,
             std::int64_t user_id,
             const application::RowInput &input) const;
    // The asset a row is written against, in strict precedence: the account the row will
    // point at (`account_id`, already resolved — the ledger's `unallocated` account when the
    // caller named none), then the asset an existing row already carries, then the ledger's
    // default asset. Only a note row, which has no account at all, can reach past the first
    // step. That order is what journal_rows_link_guard demands — it insists
    // journal_rows.asset_id equals accounts.asset_id for both account_id and
    // transfer_account_id — and it carries the scale save_row validates the amount against.
    [[nodiscard]] drogon::Task<AssetScaleRecord>
    row_asset(const drogon::orm::DbClientPtr &transaction,
              std::int64_t ledger_id,
              std::optional<std::int64_t> existing_row_id,
              std::optional<std::int64_t> account_id) const;
    // The ledger's income/expense clearing account for one asset, created on first use.
    // Postings are checked account-by-account by check_row_postings_balanced
    // (a.asset_id = p.asset_id), so a row on a chain asset needs the clearing account that
    // sits on that same asset; the ledger-wide pair seeded on the default asset will not do.
    [[nodiscard]] drogon::Task<std::int64_t>
    clearing_account(const drogon::orm::DbClientPtr &transaction,
                     std::int64_t ledger_id,
                     std::int64_t asset_id,
                     bool income) const;

    drogon::orm::DbClientPtr client_;
};

}  // namespace journalseed::infrastructure
