#pragma once

#include "journalseed/application/models.h"
#include "journalseed/infrastructure/postgres_repository.h"
#include "journalseed/lua/function_registry.h"

#include <drogon/utils/coroutine.h>

#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace journalseed::application {

template <typename T>
using ServiceResult = std::expected<T, Problem>;

// EntityNotFound 现在定义在 application/models.h（上面已 include），基础设施层要抛出它，
// 让仓储反过来 include 应用服务头文件是分层倒置。两者同属 journalseed::application，
// 所以引用 application::EntityNotFound 的代码不需要任何改动。
using ::journalseed::application::EntityNotFound;

class JournalService final {
  public:
    JournalService(std::shared_ptr<infrastructure::PostgresRepository> repository,
                   std::shared_ptr<lua::FunctionRegistry> functions);

    [[nodiscard]] drogon::Task<ServiceResult<SetupStatus>> setup_status() const;
    [[nodiscard]] drogon::Task<ServiceResult<SessionEnvelope>>
    setup(SetupRequest input) const;
    [[nodiscard]] drogon::Task<ServiceResult<SessionEnvelope>>
    login(LoginRequest input) const;
    [[nodiscard]] drogon::Task<ServiceResult<AuthContext>>
    authenticate(std::string_view cookie_token,
                 std::optional<std::string_view> csrf_token = std::nullopt) const;
    [[nodiscard]] drogon::Task<ServiceResult<SessionView>>
    current_session(std::string_view cookie_token) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    logout(std::string_view cookie_token) const;

    [[nodiscard]] drogon::Task<ServiceResult<std::vector<LedgerView>>> ledgers() const;
    [[nodiscard]] drogon::Task<ServiceResult<LedgerView>>
    create_ledger(CreateLedgerRequest input) const;
    [[nodiscard]] drogon::Task<ServiceResult<LedgerSummaryView>>
    summary(std::string_view ledger_id,
            std::optional<std::string> from = std::nullopt,
            std::optional<std::string> to = std::nullopt) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::vector<AccountView>>>
    accounts(std::string_view ledger_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<AccountView>>
    create_account(std::string_view ledger_id, AccountInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::vector<CategoryView>>>
    categories(std::string_view ledger_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<CategoryView>>
    create_category(std::string_view ledger_id, CategoryInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::vector<ColumnView>>>
    columns(std::string_view ledger_id, bool recycled) const;
    [[nodiscard]] drogon::Task<ServiceResult<ColumnView>>
    create_column(std::string_view ledger_id, ColumnInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<ColumnView>>
    update_column(std::string_view column_id, ColumnPatch patch) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    recycle_column(std::string_view column_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    restore_column(std::string_view column_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<RowPage>>
    rows(std::string_view ledger_id,
         std::uint16_t limit,
         std::string_view sort,
         bool recycled,
         std::optional<std::string_view> cursor) const;
    [[nodiscard]] drogon::Task<ServiceResult<JournalRowView>>
    create_row(std::string_view ledger_id, std::int64_t user_id, RowInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<JournalRowView>>
    update_row(std::string_view row_id, std::int64_t user_id, RowInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    recycle_row(std::string_view row_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    restore_row(std::string_view row_id) const;
    [[nodiscard]] std::vector<LuaFunctionView> functions() const;
    [[nodiscard]] ServiceResult<LuaFunctionView> create_function(LuaFunctionInput input) const;
    [[nodiscard]] ServiceResult<LuaFunctionView> update_function(std::string_view name, LuaFunctionInput input) const;
    [[nodiscard]] ServiceResult<lua::LuaValue>
    invoke_function(std::string_view name, const lua::LuaValue::Object &input) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::vector<JobView>>> jobs() const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    cancel_job(std::string_view job_id) const;

    [[nodiscard]] drogon::Task<ServiceResult<ChainSettingsView>> chain_settings() const;
    [[nodiscard]] drogon::Task<ServiceResult<ChainSettingsView>>
    update_chain_settings(ChainSettingsPatch patch) const;

    [[nodiscard]] drogon::Task<ServiceResult<std::vector<WalletView>>>
    wallets(std::string_view ledger_id) const;
    // 单个钱包读取：不存在时返回 404 wallet_not_found，供调用方在触发副作用
    //（例如 wallet.sync.started 事件）之前确认目标存在。
    [[nodiscard]] drogon::Task<ServiceResult<WalletView>>
    wallet(std::string_view wallet_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<WalletView>>
    create_wallet(std::string_view ledger_id, WalletInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<WalletView>>
    update_wallet(std::string_view wallet_id, WalletPatch patch) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    delete_wallet(std::string_view wallet_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<SyncResultView>>
    sync_wallet(std::string_view wallet_id, std::int64_t user_id) const;

    [[nodiscard]] drogon::Task<ServiceResult<std::vector<AddressLabelView>>>
    address_labels(std::string_view ledger_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<AddressLabelView>>
    create_address_label(std::string_view ledger_id, AddressLabelInput input) const;
    [[nodiscard]] drogon::Task<ServiceResult<AddressLabelView>>
    update_address_label(std::string_view label_id, AddressLabelPatch patch) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    delete_address_label(std::string_view label_id) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::vector<ChainTransactionView>>>
    chain_transactions(std::string_view ledger_id, std::uint16_t limit) const;

  private:
    [[nodiscard]] static Problem problem(std::uint16_t status,
                                         std::string code,
                                         std::string title,
                                         std::string detail);
    [[nodiscard]] static std::string random_token();
    [[nodiscard]] static std::string hash_token(std::string_view token);
    [[nodiscard]] static std::string encode_cursor(const CursorData &cursor);
    [[nodiscard]] static std::expected<CursorData, Problem>
    decode_cursor(std::string_view cursor);
    [[nodiscard]] static std::expected<RowInput, Problem> validate_row(RowInput input);
    // 金额小数位超限的统一 422：请求校验阶段（超过 18 位存储精度）与仓储抛出的
    // std::invalid_argument（超过资产小数位）共用同一个 Problem，只有后者写日志。
    [[nodiscard]] static Problem amount_scale_problem();
    [[nodiscard]] static Problem amount_scale_problem(const std::exception &exception);
    // 分类方向与金额符号冲突时的 422：由仓储的 CategoryDirectionMismatch 触发，
    // create_row 与 update_row 共用，两条路径不可能给出不同的响应。
    [[nodiscard]] static Problem
    category_direction_problem(std::string_view required_direction);
    // 转账两端账户资产不一致时的 422：由仓储的 TransferAssetMismatch 触发，
    // create_row 与 update_row 共用，两条路径不可能给出不同的响应。
    [[nodiscard]] static Problem transfer_asset_problem(std::string_view account_symbol,
                                                        std::string_view transfer_symbol);

    std::shared_ptr<infrastructure::PostgresRepository> repository_;
    std::shared_ptr<lua::FunctionRegistry> functions_;
};

}  // namespace journalseed::application
