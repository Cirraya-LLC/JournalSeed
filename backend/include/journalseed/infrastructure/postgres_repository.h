#pragma once

#include "journalseed/application/models.h"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace journalseed::infrastructure {

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
    [[nodiscard]] drogon::Task<application::LedgerSummaryView>
    ledger_summary(std::string_view ledger_public_id) const;

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

    drogon::orm::DbClientPtr client_;
};

}  // namespace journalseed::infrastructure
