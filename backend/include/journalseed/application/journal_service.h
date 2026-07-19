#pragma once

#include "journalseed/application/models.h"
#include "journalseed/infrastructure/postgres_repository.h"
#include "journalseed/lua/function_registry.h"

#include <drogon/utils/coroutine.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace journalseed::application {

template <typename T>
using ServiceResult = std::expected<T, Problem>;

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
    summary(std::string_view ledger_id) const;
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
    [[nodiscard]] ServiceResult<lua::LuaValue>
    invoke_function(std::string_view name, const lua::LuaValue::Object &input) const;
    [[nodiscard]] drogon::Task<ServiceResult<std::vector<JobView>>> jobs() const;
    [[nodiscard]] drogon::Task<ServiceResult<std::monostate>>
    cancel_job(std::string_view job_id) const;

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

    std::shared_ptr<infrastructure::PostgresRepository> repository_;
    std::shared_ptr<lua::FunctionRegistry> functions_;
};

}  // namespace journalseed::application
