#include "journalseed/infrastructure/postgres_repository.h"

#include "journalseed/domain/journal_entry.h"
#include "journalseed/domain/money.h"

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace journalseed::infrastructure {
namespace {

using drogon::orm::DbClientPtr;
using drogon::orm::Result;
using drogon::orm::Row;

std::optional<std::string> optional_string(const Row &row, const char *column) {
    if (row[column].isNull()) return std::nullopt;
    return row[column].as<std::string>();
}

application::AccountView account_from_row(const Row &row) {
    return application::AccountView{
        .id = row["id"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .openingBalance = row["opening_balance"].as<std::string>(),
        .balance = row["balance"].as<std::string>(),
        .archived = row["archived"].as<bool>(),
    };
}

application::CategoryView category_from_row(const Row &row) {
    return application::CategoryView{
        .id = row["id"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .direction = row["direction"].as<std::string>(),
        .archived = row["archived"].as<bool>(),
    };
}

application::ColumnView column_from_row(const Row &row) {
    application::ColumnView result{
        .id = row["id"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .type = row["type"].as<std::string>(),
        .system = optional_string(row, "system"),
        .position = row["position"].as<std::int32_t>(),
        .width = row["width"].as<std::int32_t>(),
        .decimalPlaces = row["decimal_places"].isNull()
                             ? std::nullopt
                             : std::optional(row["decimal_places"].as<std::int16_t>()),
        .formulaSource = optional_string(row, "formula_source"),
        .formulaResultType = optional_string(row, "formula_result_type"),
        .formulaDependencies = {},
        .recycled = row["recycled"].as<bool>(),
    };
    const auto dependencies = row["formula_dependencies"].as<std::string>();
    if (const auto parse_error = glz::read_json(result.formulaDependencies, dependencies); parse_error) {
        throw std::runtime_error("invalid formula dependency JSON in database");
    }
    return result;
}

application::JournalRowView journal_row_from_row(const Row &row) {
    application::JournalRowView result{
        .id = row["id"].as<std::string>(),
        .date = row["date"].as<std::string>(),
        .description = row["description"].as<std::string>(),
        .kind = row["kind"].as<std::string>(),
        .amount = row["amount"].as<std::string>(),
        .accountId = optional_string(row, "account_id"),
        .accountName = optional_string(row, "account_name"),
        .categoryId = optional_string(row, "category_id"),
        .categoryName = optional_string(row, "category_name"),
        .transferAccountId = optional_string(row, "transfer_account_id"),
        .transferAccountName = optional_string(row, "transfer_account_name"),
        .cells = {},
        .revision = row["revision"].as<std::int64_t>(),
        .createdAt = row["created_at"].as<std::string>(),
        .updatedAt = row["updated_at"].as<std::string>(),
    };
    const auto cells = row["cells"].as<std::string>();
    if (const auto parse_error = glz::read_json(result.cells, cells); parse_error) {
        throw std::runtime_error("invalid typed cell JSON in database");
    }
    return result;
}

constexpr std::string_view kColumnSelect = R"SQL(
SELECT c.public_id::text AS id,
       c.name,
       c.value_type AS type,
       c.system_key AS system,
       c.position,
       c.width,
       c.decimal_places,
       c.formula_source,
       c.formula_result_type,
       c.formula_dependencies::text AS formula_dependencies,
       (c.deleted_at IS NOT NULL) AS recycled
  FROM ledger_columns c
)SQL";

constexpr std::string_view kJournalRowSelect = R"SQL(
SELECT r.public_id::text AS id,
       r.occurred_on::text AS date,
       r.description,
       r.kind,
       r.amount::text AS amount,
       CASE WHEN a.system_code IS NULL THEN a.public_id::text END AS account_id,
       CASE WHEN a.system_code IS NULL THEN a.name END AS account_name,
       CASE WHEN cat.system_code IS NULL THEN cat.public_id::text END AS category_id,
       CASE WHEN cat.system_code IS NULL THEN cat.name END AS category_name,
       ta.public_id::text AS transfer_account_id,
       ta.name AS transfer_account_name,
       r.revision,
       to_char(r.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at,
       to_char(r.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS updated_at,
       COALESCE((
           SELECT jsonb_object_agg(values_by_column.column_public_id, values_by_column.value)
             FROM (
                 SELECT c.public_id::text AS column_public_id, to_jsonb(v.value) AS value
                   FROM text_cells v JOIN ledger_columns c ON c.id = v.column_id
                  WHERE v.row_id = r.id AND c.deleted_at IS NULL
                 UNION ALL
                 SELECT c.public_id::text, to_jsonb(v.value::text)
                   FROM number_cells v JOIN ledger_columns c ON c.id = v.column_id
                  WHERE v.row_id = r.id AND c.deleted_at IS NULL
                 UNION ALL
                 SELECT c.public_id::text, to_jsonb(v.value::text)
                   FROM date_cells v JOIN ledger_columns c ON c.id = v.column_id
                  WHERE v.row_id = r.id AND c.deleted_at IS NULL
                 UNION ALL
                 SELECT c.public_id::text, to_jsonb(v.value)
                   FROM boolean_cells v JOIN ledger_columns c ON c.id = v.column_id
                  WHERE v.row_id = r.id AND c.deleted_at IS NULL
                 UNION ALL
                 SELECT c.public_id::text, to_jsonb(o.label)
                   FROM option_cells v
                   JOIN ledger_columns c ON c.id = v.column_id
                   JOIN column_options o ON o.id = v.value
                  WHERE v.row_id = r.id AND c.deleted_at IS NULL
                 UNION ALL
                 SELECT c.public_id::text,
                        to_jsonb(COALESCE(rr.public_id::text, ra.public_id::text, rc.public_id::text))
                   FROM relation_cells v
                   JOIN ledger_columns c ON c.id = v.column_id
                   LEFT JOIN journal_rows rr ON rr.id = v.related_row_id
                   LEFT JOIN accounts ra ON ra.id = v.related_account_id
                   LEFT JOIN categories rc ON rc.id = v.related_category_id
                  WHERE v.row_id = r.id AND c.deleted_at IS NULL
             ) values_by_column
       ), '{}'::jsonb)::text AS cells
  FROM journal_rows r
  LEFT JOIN accounts a ON a.id = r.account_id
  LEFT JOIN categories cat ON cat.id = r.category_id
  LEFT JOIN accounts ta ON ta.id = r.transfer_account_id
)SQL";

drogon::Task<application::JournalRowView>
fetch_journal_row(const DbClientPtr &client, std::int64_t row_id) {
    const auto query = std::string(kJournalRowSelect) + " WHERE r.id = $1";
    const auto result = co_await client->execSqlCoro(query, row_id);
    if (result.empty()) throw std::runtime_error("journal row not found after write");
    co_return journal_row_from_row(result.front());
}

}  // namespace

PostgresRepository::PostgresRepository(drogon::orm::DbClientPtr client)
    : client_(std::move(client)) {
    if (!client_) throw std::invalid_argument("database client is required");
    client_->setTimeout(10.0);
}

drogon::Task<bool> PostgresRepository::setup_required() const {
    const auto result = co_await client_->execSqlCoro("SELECT NOT EXISTS (SELECT 1 FROM app_users) AS required");
    co_return result.front()["required"].as<bool>();
}

drogon::Task<std::int64_t>
PostgresRepository::create_ledger_in_transaction(const DbClientPtr &transaction,
                                                 std::string_view name) const {
    const auto ledger = co_await transaction->execSqlCoro(
        "INSERT INTO ledgers(name) VALUES($1) RETURNING id", std::string(name));
    const auto ledger_id = ledger.front()["id"].as<std::int64_t>();

    co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO accounts(ledger_id, name, system_code)
VALUES ($1, '未分配账户', 'unallocated'),
       ($1, '收入结转', 'income_clearing'),
       ($1, '支出结转', 'expense_clearing'),
       ($1, '期初余额', 'opening_balance')
)SQL",
        ledger_id);
    co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO categories(ledger_id, name, direction, system_code)
VALUES ($1, '未分配收入', 'income', 'unallocated_income'),
       ($1, '未分配支出', 'expense', 'unallocated_expense')
)SQL",
        ledger_id);
    co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO ledger_columns(ledger_id, system_key, name, value_type, position, width)
VALUES ($1, 'date', '日期', 'date', 0, 124),
       ($1, 'description', '说明', 'text', 1, 300),
       ($1, 'account', '账户', 'relation', 2, 220),
       ($1, 'category', '分类', 'relation', 3, 140),
       ($1, 'amount', '金额', 'money', 4, 140)
)SQL",
        ledger_id);
    co_return ledger_id;
}

drogon::Task<SetupRecord>
PostgresRepository::create_initial_setup(const application::SetupRequest &input,
                                         const std::string &password_hash,
                                         const std::string &token_hash_hex,
                                         const std::string &csrf_hash_hex) const {
    auto transaction = co_await client_->newTransactionCoro();
    const auto user = co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO app_users(username, password_hash)
VALUES($1, $2)
RETURNING id, public_id::text AS public_id, username
)SQL",
        input.username, password_hash);
    const auto user_id = user.front()["id"].as<std::int64_t>();
    co_await create_ledger_in_transaction(transaction, input.ledgerName);
    const auto session = co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO sessions(user_id, token_hash, csrf_token_hash, expires_at)
VALUES($1, decode($2, 'hex'), decode($3, 'hex'), clock_timestamp() + interval '30 days')
RETURNING to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS expires_at
)SQL",
        user_id, token_hash_hex, csrf_hash_hex);
    co_return SetupRecord{
        .userPublicId = user.front()["public_id"].as<std::string>(),
        .username = user.front()["username"].as<std::string>(),
        .expiresAt = session.front()["expires_at"].as<std::string>(),
    };
}

drogon::Task<std::optional<UserRecord>>
PostgresRepository::find_user(std::string_view username) const {
    const auto result = co_await client_->execSqlCoro(
        R"SQL(
SELECT id, public_id::text AS public_id, username, password_hash
  FROM app_users
 WHERE lower(username) = lower($1)
)SQL",
        std::string(username));
    if (result.empty()) co_return std::nullopt;
    co_return UserRecord{
        .id = result.front()["id"].as<std::int64_t>(),
        .publicId = result.front()["public_id"].as<std::string>(),
        .username = result.front()["username"].as<std::string>(),
        .passwordHash = result.front()["password_hash"].as<std::string>(),
    };
}

drogon::Task<SessionRecord>
PostgresRepository::create_session(std::int64_t user_id,
                                   const std::string &token_hash_hex,
                                   const std::string &csrf_hash_hex) const {
    const auto result = co_await client_->execSqlCoro(
        R"SQL(
WITH inserted AS (
    INSERT INTO sessions(user_id, token_hash, csrf_token_hash, expires_at)
    VALUES($1, decode($2, 'hex'), decode($3, 'hex'), clock_timestamp() + interval '30 days')
    RETURNING user_id, expires_at
)
SELECT u.id AS user_id, u.public_id::text AS user_public_id, u.username,
       to_char(i.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS expires_at
  FROM inserted i JOIN app_users u ON u.id = i.user_id
)SQL",
        user_id, token_hash_hex, csrf_hash_hex);
    co_return SessionRecord{
        .userId = result.front()["user_id"].as<std::int64_t>(),
        .userPublicId = result.front()["user_public_id"].as<std::string>(),
        .username = result.front()["username"].as<std::string>(),
        .expiresAt = result.front()["expires_at"].as<std::string>(),
    };
}

drogon::Task<std::optional<SessionRecord>>
PostgresRepository::find_session(const std::string &token_hash_hex,
                                 const std::optional<std::string> &csrf_hash_hex) const {
    std::string query = R"SQL(
UPDATE sessions s
   SET last_seen_at = CASE WHEN s.last_seen_at < clock_timestamp() - interval '5 minutes'
                           THEN clock_timestamp() ELSE s.last_seen_at END
  FROM app_users u
 WHERE s.user_id = u.id
   AND s.token_hash = decode($1, 'hex')
   AND s.expires_at > clock_timestamp()
)SQL";
    if (csrf_hash_hex) query += " AND s.csrf_token_hash = decode($2, 'hex')\n";
    query += R"SQL(
RETURNING u.id AS user_id, u.public_id::text AS user_public_id, u.username,
          to_char(s.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS expires_at
)SQL";
    const auto result = csrf_hash_hex
                            ? co_await client_->execSqlCoro(query, token_hash_hex, *csrf_hash_hex)
                            : co_await client_->execSqlCoro(query, token_hash_hex);
    if (result.empty()) co_return std::nullopt;
    co_return SessionRecord{
        .userId = result.front()["user_id"].as<std::int64_t>(),
        .userPublicId = result.front()["user_public_id"].as<std::string>(),
        .username = result.front()["username"].as<std::string>(),
        .expiresAt = result.front()["expires_at"].as<std::string>(),
    };
}

drogon::Task<std::optional<SessionRecord>>
PostgresRepository::rotate_session_csrf(const std::string &token_hash_hex,
                                        const std::string &csrf_hash_hex) const {
    const auto result = co_await client_->execSqlCoro(
        R"SQL(
UPDATE sessions s
   SET csrf_token_hash = decode($2, 'hex'), last_seen_at = clock_timestamp()
  FROM app_users u
 WHERE s.user_id = u.id
   AND s.token_hash = decode($1, 'hex')
   AND s.expires_at > clock_timestamp()
RETURNING u.id AS user_id, u.public_id::text AS user_public_id, u.username,
          to_char(s.expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS expires_at
)SQL",
        token_hash_hex, csrf_hash_hex);
    if (result.empty()) co_return std::nullopt;
    co_return SessionRecord{
        .userId = result.front()["user_id"].as<std::int64_t>(),
        .userPublicId = result.front()["user_public_id"].as<std::string>(),
        .username = result.front()["username"].as<std::string>(),
        .expiresAt = result.front()["expires_at"].as<std::string>(),
    };
}

drogon::Task<> PostgresRepository::delete_session(const std::string &token_hash_hex) const {
    co_await client_->execSqlCoro("DELETE FROM sessions WHERE token_hash = decode($1, 'hex')",
                                  token_hash_hex);
}

drogon::Task<std::vector<application::LedgerView>> PostgresRepository::list_ledgers() const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT public_id::text AS id, name,
       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at
  FROM ledgers
 WHERE archived_at IS NULL
 ORDER BY id
)SQL");
    std::vector<application::LedgerView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(application::LedgerView{
            .id = row["id"].as<std::string>(),
            .name = row["name"].as<std::string>(),
            .createdAt = row["created_at"].as<std::string>(),
        });
    }
    co_return result;
}

drogon::Task<application::LedgerView>
PostgresRepository::create_ledger(std::string_view name) const {
    auto transaction = co_await client_->newTransactionCoro();
    const auto ledger_id = co_await create_ledger_in_transaction(transaction, name);
    const auto rows = co_await transaction->execSqlCoro(R"SQL(
SELECT public_id::text AS id, name,
       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at
  FROM ledgers WHERE id = $1
)SQL", ledger_id);
    co_return application::LedgerView{
        .id = rows.front()["id"].as<std::string>(),
        .name = rows.front()["name"].as<std::string>(),
        .createdAt = rows.front()["created_at"].as<std::string>(),
    };
}

drogon::Task<application::LedgerSummaryView>
PostgresRepository::ledger_summary(std::string_view ledger_public_id) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH selected_ledger AS (
    SELECT id FROM ledgers WHERE public_id = $1::uuid AND archived_at IS NULL
), posting_totals AS (
    SELECT p.account_id, sum(p.signed_amount) AS total
      FROM postings p JOIN journal_rows r ON r.id = p.row_id
     WHERE r.ledger_id = (SELECT id FROM selected_ledger) AND r.deleted_at IS NULL
     GROUP BY p.account_id
)
SELECT COALESCE((
           SELECT sum(a.opening_balance + COALESCE(pt.total, 0))
             FROM accounts a LEFT JOIN posting_totals pt ON pt.account_id = a.id
            WHERE a.ledger_id = (SELECT id FROM selected_ledger)
              AND a.system_code IS NULL AND a.archived_at IS NULL AND a.deleted_at IS NULL
       ), 0)::numeric(38,2)::text AS balance,
       COALESCE((SELECT sum(amount) FROM journal_rows
                  WHERE ledger_id = (SELECT id FROM selected_ledger)
                    AND deleted_at IS NULL AND kind = 'entry' AND amount > 0), 0)::numeric(38,2)::text AS income,
       COALESCE((SELECT sum(amount) FROM journal_rows
                  WHERE ledger_id = (SELECT id FROM selected_ledger)
                    AND deleted_at IS NULL AND kind = 'entry' AND amount < 0), 0)::numeric(38,2)::text AS expense,
       (SELECT count(*) FROM journal_rows
         WHERE ledger_id = (SELECT id FROM selected_ledger) AND deleted_at IS NULL)::bigint AS row_count
)SQL", std::string(ledger_public_id));
    co_return application::LedgerSummaryView{
        .balance = rows.front()["balance"].as<std::string>(),
        .income = rows.front()["income"].as<std::string>(),
        .expense = rows.front()["expense"].as<std::string>(),
        .rowCount = rows.front()["row_count"].as<std::int64_t>(),
    };
}

drogon::Task<std::vector<application::AccountView>>
PostgresRepository::list_accounts(std::string_view ledger_public_id) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT a.public_id::text AS id, a.name, a.opening_balance::text AS opening_balance,
       (a.opening_balance + COALESCE(sum(p.signed_amount)
          FILTER (WHERE r.deleted_at IS NULL), 0))::numeric(38,2)::text AS balance,
       (a.archived_at IS NOT NULL) AS archived
  FROM accounts a
  JOIN ledgers l ON l.id = a.ledger_id
  LEFT JOIN postings p ON p.account_id = a.id
  LEFT JOIN journal_rows r ON r.id = p.row_id
 WHERE l.public_id = $1::uuid AND a.system_code IS NULL AND a.deleted_at IS NULL
 GROUP BY a.id
 ORDER BY a.archived_at NULLS FIRST, lower(a.name), a.public_id
)SQL", std::string(ledger_public_id));
    std::vector<application::AccountView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) result.push_back(account_from_row(row));
    co_return result;
}

drogon::Task<application::AccountView>
PostgresRepository::create_account(std::string_view ledger_public_id,
                                   const application::AccountInput &input) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH inserted AS (
    INSERT INTO accounts(ledger_id, name, opening_balance)
    SELECT id, $2, $3::numeric FROM ledgers WHERE public_id = $1::uuid AND archived_at IS NULL
    RETURNING *
)
SELECT public_id::text AS id, name, opening_balance::text AS opening_balance,
       opening_balance::text AS balance, false AS archived
  FROM inserted
)SQL", std::string(ledger_public_id), input.name, input.openingBalance);
    if (rows.empty()) throw std::runtime_error("ledger not found");
    co_return account_from_row(rows.front());
}

drogon::Task<std::vector<application::CategoryView>>
PostgresRepository::list_categories(std::string_view ledger_public_id) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT c.public_id::text AS id, c.name, c.direction,
       (c.archived_at IS NOT NULL) AS archived
  FROM categories c JOIN ledgers l ON l.id = c.ledger_id
 WHERE l.public_id = $1::uuid AND c.system_code IS NULL AND c.deleted_at IS NULL
 ORDER BY c.archived_at NULLS FIRST, c.direction, lower(c.name), c.public_id
)SQL", std::string(ledger_public_id));
    std::vector<application::CategoryView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) result.push_back(category_from_row(row));
    co_return result;
}

drogon::Task<application::CategoryView>
PostgresRepository::create_category(std::string_view ledger_public_id,
                                    const application::CategoryInput &input) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH inserted AS (
    INSERT INTO categories(ledger_id, name, direction)
    SELECT id, $2, $3 FROM ledgers WHERE public_id = $1::uuid AND archived_at IS NULL
    RETURNING *
)
SELECT public_id::text AS id, name, direction, false AS archived FROM inserted
)SQL", std::string(ledger_public_id), input.name, input.direction);
    if (rows.empty()) throw std::runtime_error("ledger not found");
    co_return category_from_row(rows.front());
}

drogon::Task<std::vector<application::ColumnView>>
PostgresRepository::list_columns(std::string_view ledger_public_id, bool recycled) const {
    auto query = std::string(kColumnSelect) + R"SQL(
  JOIN ledgers l ON l.id = c.ledger_id
 WHERE l.public_id = $1::uuid AND c.deleted_at IS )SQL" + (recycled ? "NOT NULL" : "NULL") +
                 " ORDER BY c.position, c.id";
    const auto rows = co_await client_->execSqlCoro(query, std::string(ledger_public_id));
    std::vector<application::ColumnView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) result.push_back(column_from_row(row));
    co_return result;
}

drogon::Task<application::ColumnView>
PostgresRepository::create_column(std::string_view ledger_public_id,
                                  const application::ColumnInput &input) const {
    std::string dependencies;
    if (const auto error = glz::write_json(input.formulaDependencies, dependencies); error) {
        throw std::runtime_error("formula dependency serialization failed");
    }
    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH selected_ledger AS (
    SELECT id FROM ledgers WHERE public_id = $1::uuid AND archived_at IS NULL
), inserted AS (
    INSERT INTO ledger_columns(
        ledger_id, name, value_type, position, width, decimal_places,
        formula_source, formula_result_type, formula_dependencies)
    SELECT id, $2, $3,
           COALESCE((SELECT max(position) + 1 FROM ledger_columns
                      WHERE ledger_id = selected_ledger.id), 0),
           160, NULLIF($4, '')::smallint, NULLIF($5, ''), NULLIF($6, ''), $7::jsonb
      FROM selected_ledger
    RETURNING *
)
SELECT public_id::text AS id, name, value_type AS type, system_key AS system,
       position, width, decimal_places, formula_source, formula_result_type,
       formula_dependencies::text AS formula_dependencies, false AS recycled
  FROM inserted
)SQL", std::string(ledger_public_id), input.name, input.type,
        input.decimalPlaces ? std::to_string(*input.decimalPlaces) : std::string{},
        input.formulaSource.value_or(""), input.formulaResultType.value_or(""), dependencies);
    if (rows.empty()) throw std::runtime_error("ledger not found");
    co_return column_from_row(rows.front());
}

drogon::Task<application::ColumnView>
PostgresRepository::update_column(std::string_view column_public_id,
                                  const application::ColumnPatch &patch) const {
    auto transaction = co_await client_->newTransactionCoro();
    const auto current = co_await transaction->execSqlCoro(R"SQL(
SELECT id, ledger_id, name, position, width
  FROM ledger_columns WHERE public_id = $1::uuid AND deleted_at IS NULL
  FOR UPDATE
)SQL", std::string(column_public_id));
    if (current.empty()) throw std::runtime_error("column not found");
    const auto id = current.front()["id"].as<std::int64_t>();
    const auto ledger_id = current.front()["ledger_id"].as<std::int64_t>();
    const auto old_position = current.front()["position"].as<std::int32_t>();

    if (patch.position && *patch.position != old_position) {
        const auto occupied = co_await transaction->execSqlCoro(
            "SELECT id FROM ledger_columns WHERE ledger_id=$1 AND position=$2 AND deleted_at IS NULL FOR UPDATE",
            ledger_id, *patch.position);
        if (!occupied.empty()) {
            const auto other_id = occupied.front()["id"].as<std::int64_t>();
            const auto temporary = co_await transaction->execSqlCoro(
                "SELECT COALESCE(max(position),0)+100 AS value FROM ledger_columns WHERE ledger_id=$1",
                ledger_id);
            const auto temporary_position = temporary.front()["value"].as<std::int32_t>();
            co_await transaction->execSqlCoro("UPDATE ledger_columns SET position=$2 WHERE id=$1",
                                              other_id, temporary_position);
            co_await transaction->execSqlCoro("UPDATE ledger_columns SET position=$2 WHERE id=$1",
                                              id, *patch.position);
            co_await transaction->execSqlCoro("UPDATE ledger_columns SET position=$2 WHERE id=$1",
                                              other_id, old_position);
        } else {
            co_await transaction->execSqlCoro("UPDATE ledger_columns SET position=$2 WHERE id=$1",
                                              id, *patch.position);
        }
    }

    co_await transaction->execSqlCoro(
        "UPDATE ledger_columns SET name=$2, width=$3 WHERE id=$1",
        id,
        patch.name.value_or(current.front()["name"].as<std::string>()),
        patch.width.value_or(current.front()["width"].as<std::int32_t>()));
    const auto query = std::string(kColumnSelect) + " WHERE c.id = $1";
    const auto result = co_await transaction->execSqlCoro(query, id);
    co_return column_from_row(result.front());
}

drogon::Task<> PostgresRepository::recycle_column(std::string_view column_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE ledger_columns SET deleted_at=clock_timestamp()
 WHERE public_id=$1::uuid AND deleted_at IS NULL AND system_key IS DISTINCT FROM 'amount'
)SQL", std::string(column_public_id));
    if (result.affectedRows() == 0) throw std::runtime_error("column is fixed or not found");
}

drogon::Task<> PostgresRepository::restore_column(std::string_view column_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE ledger_columns SET deleted_at=NULL
 WHERE public_id=$1::uuid AND deleted_at IS NOT NULL
)SQL", std::string(column_public_id));
    if (result.affectedRows() == 0) throw std::runtime_error("recycled column not found");
}

drogon::Task<application::RowPage>
PostgresRepository::list_rows(std::string_view ledger_public_id,
                              const application::RowQuery &query) const {
    const std::string sort_expression = query.sortKey == "amount" ? "r.amount" : "r.occurred_on";
    const std::string cursor_cast = query.sortKey == "amount" ? "numeric" : "date";
    const std::string direction = query.ascending ? "ASC" : "DESC";
    const std::string comparison = query.ascending ? ">" : "<";

    std::string sql = std::string(kJournalRowSelect) + R"SQL(
  JOIN ledgers l ON l.id = r.ledger_id
 WHERE l.public_id = $1::uuid
)SQL";
    sql += query.recycled ? " AND r.deleted_at IS NOT NULL\n" : " AND r.deleted_at IS NULL\n";
    if (query.cursorValue && query.cursorId) {
        sql += " AND (" + sort_expression + ", r.public_id) " + comparison +
               " ($2::" + cursor_cast + ", $3::uuid)\n";
    }
    sql += " ORDER BY " + sort_expression + " " + direction + ", r.public_id " + direction;
    sql += query.cursorValue ? " LIMIT $4" : " LIMIT $2";

    const auto fetch_limit = static_cast<std::int64_t>(query.limit) + 1;
    const auto rows = query.cursorValue && query.cursorId
                          ? co_await client_->execSqlCoro(sql, std::string(ledger_public_id),
                                                         *query.cursorValue, *query.cursorId,
                                                         fetch_limit)
                          : co_await client_->execSqlCoro(sql, std::string(ledger_public_id),
                                                         fetch_limit);

    application::RowPage page{
        .items = {},
        .nextCursor = std::nullopt,
        .hasMore = rows.size() > query.limit,
    };
    page.items.reserve(std::min<std::size_t>(rows.size(), query.limit));
    for (std::size_t index = 0; index < rows.size() && index < query.limit; ++index) {
        page.items.push_back(journal_row_from_row(rows[index]));
    }
    if (page.hasMore && !page.items.empty()) {
        const auto &last = page.items.back();
        const auto &value = query.sortKey == "amount" ? last.amount : last.date;
        page.nextCursor = value + "|" + last.id;
    }
    co_return page;
}

drogon::Task<application::JournalRowView>
PostgresRepository::save_row(const DbClientPtr &transaction,
                             std::optional<std::int64_t> existing_row_id,
                             std::int64_t ledger_id,
                             std::int64_t user_id,
                             const application::RowInput &input) const {
    const auto amount = *domain::Money::parse(input.amount);

    std::optional<std::int64_t> account_id;
    std::optional<std::int64_t> category_id;
    std::optional<std::int64_t> transfer_account_id;
    std::optional<std::int64_t> counterparty_id;

    if (input.kind != "note") {
        Result account_result(nullptr);
        if (input.accountId) {
            account_result = co_await transaction->execSqlCoro(R"SQL(
SELECT id FROM accounts
 WHERE ledger_id=$1 AND public_id=$2::uuid AND system_code IS NULL
   AND archived_at IS NULL AND deleted_at IS NULL
)SQL", ledger_id, *input.accountId);
        } else {
            account_result = co_await transaction->execSqlCoro(
                "SELECT id FROM accounts WHERE ledger_id=$1 AND system_code='unallocated'",
                ledger_id);
        }
        if (account_result.empty()) throw std::runtime_error("account not found in ledger");
        account_id = account_result.front()["id"].as<std::int64_t>();
    }

    if (input.kind == "entry") {
        const std::string direction = amount.is_positive() ? "income" : "expense";
        Result category_result(nullptr);
        if (input.categoryId) {
            category_result = co_await transaction->execSqlCoro(R"SQL(
SELECT id FROM categories
 WHERE ledger_id=$1 AND public_id=$2::uuid AND direction=$3 AND system_code IS NULL
   AND archived_at IS NULL AND deleted_at IS NULL
)SQL", ledger_id, *input.categoryId, direction);
        } else {
            category_result = co_await transaction->execSqlCoro(
                "SELECT id FROM categories WHERE ledger_id=$1 AND system_code=$2",
                ledger_id, direction == "income" ? "unallocated_income" : "unallocated_expense");
        }
        if (category_result.empty()) throw std::runtime_error("category not found in ledger");
        category_id = category_result.front()["id"].as<std::int64_t>();

        const auto counterparty = co_await transaction->execSqlCoro(
            "SELECT id FROM accounts WHERE ledger_id=$1 AND system_code=$2",
            ledger_id, direction == "income" ? "income_clearing" : "expense_clearing");
        if (counterparty.empty()) throw std::runtime_error("ledger clearing account missing");
        counterparty_id = counterparty.front()["id"].as<std::int64_t>();
    } else if (input.kind == "transfer") {
        const auto destination = co_await transaction->execSqlCoro(R"SQL(
SELECT id FROM accounts
 WHERE ledger_id=$1 AND public_id=$2::uuid AND system_code IS NULL
   AND archived_at IS NULL AND deleted_at IS NULL
)SQL", ledger_id, *input.transferAccountId);
        if (destination.empty()) throw std::runtime_error("transfer destination not found in ledger");
        transfer_account_id = destination.front()["id"].as<std::int64_t>();
        if (transfer_account_id == account_id) {
            throw std::runtime_error("transfer accounts must differ");
        }
    }

    std::int64_t row_id = 0;
    if (existing_row_id) {
        constexpr std::string_view cell_tables[] = {
            "text_cells", "number_cells", "date_cells", "boolean_cells", "option_cells", "relation_cells"};
        for (const auto table : cell_tables) {
            co_await transaction->execSqlCoro(
                "DELETE FROM " + std::string(table) + " WHERE row_id=$1", *existing_row_id);
        }
        co_await transaction->execSqlCoro("DELETE FROM postings WHERE row_id=$1", *existing_row_id);
        const auto updated = co_await transaction->execSqlCoro(R"SQL(
UPDATE journal_rows
   SET occurred_on=$2::date, description=$3, kind=$4, amount=$5::numeric,
       account_id=$6, category_id=$7, transfer_account_id=$8, revision=revision+1
 WHERE id=$1
RETURNING id
)SQL", *existing_row_id, input.date, input.description, input.kind, input.amount,
            account_id, category_id, transfer_account_id);
        if (updated.empty()) throw std::runtime_error("journal row not found");
        row_id = *existing_row_id;
    } else {
        const auto inserted = co_await transaction->execSqlCoro(R"SQL(
INSERT INTO journal_rows(
    ledger_id, occurred_on, description, kind, amount,
    account_id, category_id, transfer_account_id, created_by)
VALUES($1, $2::date, $3, $4, $5::numeric, $6, $7, $8, $9)
RETURNING id
)SQL", ledger_id, input.date, input.description, input.kind, input.amount,
            account_id, category_id, transfer_account_id, user_id);
        row_id = inserted.front()["id"].as<std::int64_t>();
    }

    if (input.kind != "note") {
        const domain::JournalDraft draft{
            .kind = input.kind == "transfer" ? domain::TransactionKind::transfer
                                               : domain::TransactionKind::entry,
            .amount = amount,
            .account = domain::AccountId{*account_id},
            .category = category_id ? std::optional(domain::CategoryId{*category_id}) : std::nullopt,
            .transfer_account = transfer_account_id
                                    ? std::optional(domain::AccountId{*transfer_account_id})
                                    : std::nullopt,
        };
        const domain::PostingAccounts posting_accounts{
            .primary = domain::AccountId{*account_id},
            .counterparty = domain::AccountId{counterparty_id.value_or(*account_id)},
            .transfer_destination = transfer_account_id
                                        ? std::optional(domain::AccountId{*transfer_account_id})
                                        : std::nullopt,
        };
        const auto postings = domain::build_postings(draft, posting_accounts);
        if (!postings) throw std::runtime_error(postings.error().message);
        for (const auto &posting : *postings) {
            co_await transaction->execSqlCoro(R"SQL(
INSERT INTO postings(row_id, account_id, category_id, signed_amount, line_number)
VALUES($1, $2, $3, $4::numeric, $5)
)SQL", row_id, posting.account.value,
                posting.category ? std::optional(posting.category->value) : std::nullopt,
                posting.signed_amount.to_string(), static_cast<std::int16_t>(posting.line_number));
        }
    }

    for (const auto &[column_public_id, value] : input.cells) {
        if (!value) continue;
        const auto column = co_await transaction->execSqlCoro(R"SQL(
SELECT id, value_type
  FROM ledger_columns
 WHERE ledger_id=$1 AND public_id=$2::uuid AND system_key IS NULL AND deleted_at IS NULL
)SQL", ledger_id, column_public_id);
        if (column.empty()) throw std::runtime_error("custom column not found in ledger");
        const auto column_id = column.front()["id"].as<std::int64_t>();
        const auto type = column.front()["value_type"].as<std::string>();

        const auto *string_value = std::get_if<std::string>(&*value);
        const auto *boolean_value = std::get_if<bool>(&*value);
        if (type == "text") {
            if (!string_value || string_value->empty()) continue;
            co_await transaction->execSqlCoro(
                "INSERT INTO text_cells(row_id,column_id,value) VALUES($1,$2,$3)",
                row_id, column_id, *string_value);
        } else if (type == "number") {
            if (!string_value || string_value->empty()) continue;
            co_await transaction->execSqlCoro(
                "INSERT INTO number_cells(row_id,column_id,value) VALUES($1,$2,$3::numeric)",
                row_id, column_id, *string_value);
        } else if (type == "date") {
            if (!string_value || string_value->empty()) continue;
            co_await transaction->execSqlCoro(
                "INSERT INTO date_cells(row_id,column_id,value) VALUES($1,$2,$3::date)",
                row_id, column_id, *string_value);
        } else if (type == "boolean") {
            if (!boolean_value) continue;
            co_await transaction->execSqlCoro(
                "INSERT INTO boolean_cells(row_id,column_id,value) VALUES($1,$2,$3)",
                row_id, column_id, *boolean_value);
        } else if (type == "option") {
            if (!string_value || string_value->empty()) continue;
            const auto option = co_await transaction->execSqlCoro(R"SQL(
INSERT INTO column_options(column_id,label,position)
VALUES($1,$2,COALESCE((SELECT max(position)+1 FROM column_options WHERE column_id=$1),0))
ON CONFLICT(column_id,label) DO UPDATE SET label=excluded.label
RETURNING id
)SQL", column_id, *string_value);
            co_await transaction->execSqlCoro(
                "INSERT INTO option_cells(row_id,column_id,value) VALUES($1,$2,$3)",
                row_id, column_id, option.front()["id"].as<std::int64_t>());
        } else if (type == "relation") {
            if (!string_value || string_value->empty()) continue;
            const auto related = co_await transaction->execSqlCoro(R"SQL(
SELECT 'row' AS kind, r.id FROM journal_rows r
 WHERE r.ledger_id=$1 AND r.public_id=$2::uuid
UNION ALL
SELECT 'account', a.id FROM accounts a
 WHERE a.ledger_id=$1 AND a.public_id=$2::uuid AND a.system_code IS NULL
UNION ALL
SELECT 'category', c.id FROM categories c
 WHERE c.ledger_id=$1 AND c.public_id=$2::uuid AND c.system_code IS NULL
LIMIT 1
)SQL", ledger_id, *string_value);
            if (related.empty()) throw std::runtime_error("related record not found in ledger");
            const auto related_id = related.front()["id"].as<std::int64_t>();
            const auto related_kind = related.front()["kind"].as<std::string>();
            if (related_kind == "row") {
                co_await transaction->execSqlCoro(
                    "INSERT INTO relation_cells(row_id,column_id,related_row_id) VALUES($1,$2,$3)",
                    row_id, column_id, related_id);
            } else if (related_kind == "account") {
                co_await transaction->execSqlCoro(
                    "INSERT INTO relation_cells(row_id,column_id,related_account_id) VALUES($1,$2,$3)",
                    row_id, column_id, related_id);
            } else {
                co_await transaction->execSqlCoro(
                    "INSERT INTO relation_cells(row_id,column_id,related_category_id) VALUES($1,$2,$3)",
                    row_id, column_id, related_id);
            }
        } else if (type != "formula") {
            throw std::runtime_error("unsupported custom column type");
        }
    }

    co_await transaction->execSqlCoro("SET CONSTRAINTS postings_balance_guard IMMEDIATE");
    co_return co_await fetch_journal_row(transaction, row_id);
}

drogon::Task<application::JournalRowView>
PostgresRepository::create_row(std::string_view ledger_public_id,
                               std::int64_t user_id,
                               const application::RowInput &input) const {
    auto transaction = co_await client_->newTransactionCoro();
    const auto ledger = co_await transaction->execSqlCoro(
        "SELECT id FROM ledgers WHERE public_id=$1::uuid AND archived_at IS NULL FOR SHARE",
        std::string(ledger_public_id));
    if (ledger.empty()) throw std::runtime_error("ledger not found");
    co_return co_await save_row(transaction, std::nullopt,
                                ledger.front()["id"].as<std::int64_t>(), user_id, input);
}

drogon::Task<application::JournalRowView>
PostgresRepository::update_row(std::string_view row_public_id,
                               std::int64_t user_id,
                               const application::RowInput &input) const {
    auto transaction = co_await client_->newTransactionCoro();
    const auto existing = co_await transaction->execSqlCoro(R"SQL(
SELECT id, ledger_id FROM journal_rows
 WHERE public_id=$1::uuid AND deleted_at IS NULL
 FOR UPDATE
)SQL", std::string(row_public_id));
    if (existing.empty()) throw std::runtime_error("journal row not found");
    co_return co_await save_row(transaction,
                                existing.front()["id"].as<std::int64_t>(),
                                existing.front()["ledger_id"].as<std::int64_t>(),
                                user_id, input);
}

drogon::Task<> PostgresRepository::recycle_row(std::string_view row_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE journal_rows SET deleted_at=clock_timestamp(), revision=revision+1
 WHERE public_id=$1::uuid AND deleted_at IS NULL
)SQL", std::string(row_public_id));
    if (result.affectedRows() == 0) throw std::runtime_error("journal row not found");
}

drogon::Task<> PostgresRepository::restore_row(std::string_view row_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE journal_rows SET deleted_at=NULL, revision=revision+1
 WHERE public_id=$1::uuid AND deleted_at IS NOT NULL
)SQL", std::string(row_public_id));
    if (result.affectedRows() == 0) throw std::runtime_error("recycled journal row not found");
}

drogon::Task<std::vector<application::JobView>> PostgresRepository::list_jobs() const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT public_id::text AS id, kind, status, progress_done, progress_total,
       error::text AS error,
       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at
  FROM background_jobs
 ORDER BY created_at DESC, id DESC
 LIMIT 100
)SQL");
    std::vector<application::JobView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        std::optional<std::map<std::string, std::string>> error;
        if (!row["error"].isNull()) {
            error = std::map<std::string, std::string>{{"detail", row["error"].as<std::string>()}};
        }
        result.push_back(application::JobView{
            .id = row["id"].as<std::string>(),
            .kind = row["kind"].as<std::string>(),
            .status = row["status"].as<std::string>(),
            .done = row["progress_done"].as<std::int64_t>(),
            .total = row["progress_total"].as<std::int64_t>(),
            .error = std::move(error),
            .createdAt = row["created_at"].as<std::string>(),
        });
    }
    co_return result;
}

drogon::Task<> PostgresRepository::request_job_cancel(std::string_view job_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE background_jobs SET cancel_requested_at=clock_timestamp()
 WHERE public_id=$1::uuid AND status IN ('queued','running')
)SQL", std::string(job_public_id));
    if (result.affectedRows() == 0) throw std::runtime_error("active job not found");
}

}  // namespace journalseed::infrastructure
