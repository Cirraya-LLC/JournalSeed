#include "journalseed/infrastructure/postgres_repository.h"

#include "journalseed/domain/chain.h"
#include "journalseed/domain/journal_entry.h"
#include "journalseed/domain/money.h"

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace journalseed::infrastructure {

// The two flattened payloads record_wallet_sync hands to PostgreSQL. Member names are
// the column names the jsonb_to_recordset() column definitions expect, so the glaze
// reflection output and the SQL stay in step. glaze derives those names from the type
// itself, which requires the type to have linkage, so these records cannot live in the
// anonymous namespace below.
namespace sync_payload {

struct Transaction {
    std::string hash;
    std::optional<std::int64_t> block;
    std::optional<std::int64_t> ts_ms;
    bool confirmed{true};
    bool success{true};
    std::string raw;
};

struct Movement {
    std::string mkey;
    std::string tx_hash;
    std::string direction;
    std::string symbol;
    std::string name;
    std::int16_t decimals{0};
    std::optional<std::string> contract;
    std::optional<std::string> contract_norm;
    std::string amount;
    std::optional<std::string> from_addr;
    std::optional<std::string> from_norm;
    std::optional<std::string> to_addr;
    std::optional<std::string> to_norm;
};

// Every fetched transaction, accepted or not, so the checkpoint still moves past a window
// that held nothing but filtered-out tokens instead of re-fetching it forever.
struct Checkpoint {
    std::string hash;
    std::optional<std::int64_t> block;
    std::optional<std::int64_t> ts_ms;
};

}  // namespace sync_payload

namespace {

using drogon::orm::DbClientPtr;
using drogon::orm::Result;
using drogon::orm::Row;

// Money rendering convention for every amount that leaves this file.
//
// `assetDecimals` is documented as the scale of the amounts shipped beside it, so one
// asset's amounts must all come back at that one scale: row amount, account balance and
// opening balance, summary figures and chain movement amounts alike. The storage scale is
// not that scale — accounts.opening_balance, journal_rows.amount, postings.signed_amount
// and chain_asset_movements.amount are all NUMERIC(38,18) — so a bare `::text` renders a
// 2-decimal asset as "-1.000000000000000000", while a fixed `::numeric(38,2)` cast silently
// rounds an 18-decimal one: 0.747900000000000000 ETH came back as "0.75".
//
// `round(value, assets.decimals)` is the fix: PostgreSQL returns a numeric whose scale is
// exactly the second argument, so `::text` renders exactly that many decimals and pads
// rather than truncates. A type modifier cannot do this — `::numeric(38,n)` needs a literal
// n, and the scale here is per asset and only known per row. Rounding is never lossy in
// practice: save_row rejects an amount that does not fit its asset's scale before writing
// it, and the chain sync converts base units at exactly the asset's own decimals.

std::optional<std::string> optional_string(const Row &row, const char *column) {
    if (row[column].isNull()) return std::nullopt;
    return row[column].as<std::string>();
}

std::string trim_copy(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
                          return std::isspace(character) != 0;
                      }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

// Truncates to at most `maximum` bytes without splitting a UTF-8 sequence: a
// resize() in the middle of a multi-byte code point produces text PostgreSQL
// rejects as invalid for the database encoding.
std::string bounded_text(std::string value, std::size_t maximum) {
    value = trim_copy(std::move(value));
    if (value.size() > maximum) {
        std::size_t cut = maximum;
        while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U) --cut;
        value.resize(cut);
        value = trim_copy(std::move(value));
    }
    return value;
}

std::optional<std::string> sanitized_optional(std::optional<std::string> value, std::size_t maximum) {
    if (!value) return std::nullopt;
    auto sanitized = bounded_text(std::move(*value), maximum);
    if (sanitized.empty()) return std::nullopt;
    return sanitized;
}

std::string sanitized_asset_symbol(std::string value) {
    value = bounded_text(std::move(value), 24);
    return value.empty() ? "UNKNOWN" : value;
}

std::string sanitized_asset_name(std::string value, std::string_view symbol) {
    value = bounded_text(std::move(value), 120);
    if (!value.empty()) return value;
    auto fallback = bounded_text(std::string(symbol), 120);
    return fallback.empty() ? "Unknown Asset" : fallback;
}

std::int16_t sanitized_asset_decimals(std::int16_t decimals) {
    return std::clamp<std::int16_t>(decimals, 0, 18);
}

// chain_asset_movements.amount is NUMERIC(38, 18) with CHECK (amount >= 0): the sign
// belongs to `direction`, never to the number. An amount that is negative, malformed
// or wider than 20 integer digits cannot be stored at all, and letting it reach the
// INSERT would abort the whole sync over a single spam token, so the movement is
// dropped here instead. Extra fraction digits are fine: PostgreSQL rounds them.
bool storable_chain_amount(std::string_view amount) {
    if (amount.empty()) return false;
    const auto point = amount.find('.');
    if (point != amount.rfind('.')) return false;
    const auto integer_digits = point == std::string_view::npos ? amount.size() : point;
    if (integer_digits == 0 || integer_digits > 20) return false;
    if (point != std::string_view::npos && point + 1 == amount.size()) return false;
    return std::ranges::all_of(amount, [](unsigned char character) {
        return std::isdigit(character) != 0 || character == '.';
    });
}

bool storable_movement_direction(std::string_view direction) {
    return direction == "incoming" || direction == "outgoing" || direction == "internal" ||
           direction == "fee";
}


// Builds the address side of a chain view. `short_address` is a pure helper, so
// the shortened form never needs to round-trip through the database.
application::ChainAddressView chain_address_from_row(const Row &row,
                                                     const char *address_column,
                                                     const char *label_id_column,
                                                     const char *display_name_column,
                                                     const char *kind_column) {
    auto address = optional_string(row, address_column).value_or(std::string{});
    auto shortened = address.empty() ? std::string{} : domain::chain::short_address(address);
    return application::ChainAddressView{
        .address = std::move(address),
        .addressShort = std::move(shortened),
        .labelId = optional_string(row, label_id_column),
        .displayName = optional_string(row, display_name_column),
        .kind = optional_string(row, kind_column),
    };
}

application::AccountView account_from_row(const Row &row) {
    return application::AccountView{
        .id = row["id"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .openingBalance = row["opening_balance"].as<std::string>(),
        .balance = row["balance"].as<std::string>(),
        .archived = row["archived"].as<bool>(),
        .assetId = row["asset_id"].as<std::string>(),
        .assetSymbol = row["asset_symbol"].as<std::string>(),
        .assetDecimals = row["asset_decimals"].as<std::int16_t>(),
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
        .assetId = row["asset_id"].as<std::string>(),
        .assetSymbol = row["asset_symbol"].as<std::string>(),
        .assetDecimals = row["asset_decimals"].as<std::int16_t>(),
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
    // Only rows the wallet sync generated carry a chain link. chain_tx_hash comes from a
    // NOT NULL column, so it is null exactly when the LEFT JOIN LATERAL found no link and
    // the row is an ordinary manual one, which must keep chainSource absent.
    if (!row["chain_tx_hash"].isNull()) {
        auto tx_hash = row["chain_tx_hash"].as<std::string>();
        auto tx_hash_short = domain::chain::short_address(tx_hash);
        result.chainSource = application::ChainSourceView{
            .txHash = std::move(tx_hash),
            .txHashShort = std::move(tx_hash_short),
            .chain = row["chain_code"].as<std::string>(),
            .chainName = row["chain_network_name"].as<std::string>(),
            .direction = row["chain_direction"].as<std::string>(),
            .assetSymbol = row["chain_asset_symbol"].as<std::string>(),
            .assetDecimals = row["chain_asset_decimals"].as<std::int16_t>(),
            .origin = chain_address_from_row(row, "chain_from_address", "chain_from_label_id",
                                             "chain_from_display_name", "chain_from_kind"),
            .target = chain_address_from_row(row, "chain_to_address", "chain_to_label_id",
                                             "chain_to_display_name", "chain_to_kind"),
        };
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

// Resolves the display label of a movement's two address sides. A label scoped to the
// movement's own chain outranks a broader 'evm'-scoped one that covers every EVM network,
// which is what the ORDER BY encodes; LIMIT 1 keeps each side single-valued. Both the row
// select and list_chain_transactions splice this in verbatim, so the precedence rule lives
// in one place — which is why it hard-codes the movement alias `m` and its transaction
// alias `ct`, and why both queries must keep using those names.
constexpr std::string_view kChainAddressLabelJoins = R"SQL(
  LEFT JOIN LATERAL (
      SELECT lb.public_id::text AS label_id, lb.display_name, lb.kind
        FROM wallet_address_labels lb
       WHERE lb.ledger_id = m.ledger_id
         AND lb.address_normalized = m.from_address_normalized
         AND (lb.chain_network_id = ct.chain_network_id OR lb.scope = 'evm')
       ORDER BY (lb.chain_network_id IS NOT NULL) DESC, lb.id
       LIMIT 1
  ) origin_label ON TRUE
  LEFT JOIN LATERAL (
      SELECT lb.public_id::text AS label_id, lb.display_name, lb.kind
        FROM wallet_address_labels lb
       WHERE lb.ledger_id = m.ledger_id
         AND lb.address_normalized = m.to_address_normalized
         AND (lb.chain_network_id = ct.chain_network_id OR lb.scope = 'evm')
       ORDER BY (lb.chain_network_id IS NOT NULL) DESC, lb.id
       LIMIT 1
  ) target_label ON TRUE
)SQL";

constexpr std::string_view kJournalRowSelectHead = R"SQL(
SELECT r.public_id::text AS id,
       r.occurred_on::text AS date,
       r.description,
       r.kind,
       round(r.amount, ast.decimals)::text AS amount,
       ast.public_id::text AS asset_id,
       ast.symbol AS asset_symbol,
       ast.decimals AS asset_decimals,
       CASE WHEN a.system_code IS NULL THEN a.public_id::text END AS account_id,
       CASE WHEN a.system_code IS NULL THEN a.name END AS account_name,
       CASE WHEN cat.system_code IS NULL THEN cat.public_id::text END AS category_id,
       CASE WHEN cat.system_code IS NULL THEN cat.name END AS category_name,
       ta.public_id::text AS transfer_account_id,
       ta.name AS transfer_account_name,
       chain.tx_hash AS chain_tx_hash,
       chain.chain_code,
       chain.chain_name AS chain_network_name,
       chain.direction AS chain_direction,
       chain.asset_symbol AS chain_asset_symbol,
       chain.asset_decimals AS chain_asset_decimals,
       chain.from_address AS chain_from_address,
       chain.from_label_id AS chain_from_label_id,
       chain.from_display_name AS chain_from_display_name,
       chain.from_kind AS chain_from_kind,
       chain.to_address AS chain_to_address,
       chain.to_label_id AS chain_to_label_id,
       chain.to_display_name AS chain_to_display_name,
       chain.to_kind AS chain_to_kind,
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
  JOIN assets ast ON ast.id = r.asset_id
  LEFT JOIN accounts a ON a.id = r.account_id
  LEFT JOIN categories cat ON cat.id = r.category_id
  LEFT JOIN accounts ta ON ta.id = r.transfer_account_id
  LEFT JOIN LATERAL (
SELECT ct.tx_hash,
       cn.code AS chain_code,
       cn.name AS chain_name,
       m.direction,
       mast.symbol AS asset_symbol,
       mast.decimals AS asset_decimals,
       m.from_address,
       origin_label.label_id AS from_label_id,
       origin_label.display_name AS from_display_name,
       origin_label.kind AS from_kind,
       m.to_address,
       target_label.label_id AS to_label_id,
       target_label.display_name AS to_display_name,
       target_label.kind AS to_kind
  FROM chain_movement_row_links ml
  JOIN chain_asset_movements m ON m.id = ml.movement_id
  JOIN chain_transactions ct ON ct.id = m.chain_transaction_id
  JOIN chain_networks cn ON cn.id = ct.chain_network_id
  JOIN assets mast ON mast.id = m.asset_id)SQL";

// The chain lookup is one LATERAL rather than five joins spliced into the outer FROM for
// two reasons. It cannot fan a journal row out into several result rows — LIMIT 1 caps it
// at one, so the cursor-paginated list keeps counting rows and journal rows one for one
// even if a row ever ends up linked from two movements. And it keeps the outer join list
// at six relations: eleven would pass join_collapse_limit, at which point PostgreSQL stops
// reordering and plans the hot list query in written order, which measures far worse than
// today's plan for chain-free ledgers as much as for chain ones.
//
// ORDER BY makes the pick deterministic rather than arbitrary if that ever happens; today
// it cannot, because record_wallet_sync draws a fresh journal_rows id from the sequence for
// every movement it links and is the only writer of chain_movement_row_links, so row_id is
// unique there in practice.
constexpr std::string_view kJournalRowSelectTail = R"SQL(
 WHERE ml.row_id = r.id
 ORDER BY ml.movement_id
 LIMIT 1
  ) chain ON TRUE
)SQL";

// Spliced once, at first use: the label lookup in the middle is shared text, so the row
// select cannot be a single literal.
const std::string &journal_row_select() {
    static const std::string sql = std::string(kJournalRowSelectHead) +
                                   std::string(kChainAddressLabelJoins) +
                                   std::string(kJournalRowSelectTail);
    return sql;
}

constexpr std::string_view kWalletSelect = R"SQL(
SELECT w.public_id::text AS id,
       l.public_id::text AS ledger_id,
       cn.code AS chain,
       cn.name AS chain_name,
       w.name,
       w.address,
       w.enabled,
       w.auto_sync,
       to_char(w.last_synced_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS last_synced_at,
       w.last_error,
       w.accept_all_tokens,
       w.token_rules::text AS token_rules,
       CASE WHEN w.sync_from = '-infinity' THEN NULL
            ELSE to_char(w.sync_from AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') END AS sync_from,
       COALESCE(s.status, 'idle') AS sync_status,
       to_char(w.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at
  FROM wallet_accounts w
  JOIN ledgers l ON l.id = w.ledger_id
  JOIN chain_networks cn ON cn.id = w.chain_network_id
  LEFT JOIN wallet_sync_states s ON s.wallet_id = w.id
)SQL";

std::string token_rules_json(const std::vector<application::WalletTokenRule> &rules) {
    std::string json;
    if (glz::write_json(rules, json)) throw std::runtime_error("token rule serialization failed");
    return json;
}

std::vector<application::WalletTokenRule> token_rules_from_json(const std::string &json) {
    std::vector<application::WalletTokenRule> rules;
    // Written only by this repository from validated input; a column that somehow fails to
    // parse is shown as "no rules" rather than taking the whole wallet list down.
    if (glz::read_json(rules, json)) rules.clear();
    return rules;
}

application::WalletView wallet_from_row(const Row &row) {
    auto address = row["address"].as<std::string>();
    auto shortened = domain::chain::short_address(address);
    return application::WalletView{
        .id = row["id"].as<std::string>(),
        .ledgerId = row["ledger_id"].as<std::string>(),
        .chain = row["chain"].as<std::string>(),
        .chainName = row["chain_name"].as<std::string>(),
        .name = row["name"].as<std::string>(),
        .address = std::move(address),
        .addressShort = std::move(shortened),
        .enabled = row["enabled"].as<bool>(),
        .autoSync = row["auto_sync"].as<bool>(),
        .lastSyncedAt = optional_string(row, "last_synced_at"),
        .lastError = optional_string(row, "last_error"),
        .syncStatus = row["sync_status"].as<std::string>(),
        .createdAt = optional_string(row, "created_at"),
        .acceptAllTokens = row["accept_all_tokens"].as<bool>(),
        .acceptedTokens = token_rules_from_json(row["token_rules"].as<std::string>()),
        .syncFrom = optional_string(row, "sync_from"),
    };
}

constexpr std::string_view kAddressLabelSelect = R"SQL(
SELECT al.public_id::text AS id,
       l.public_id::text AS ledger_id,
       COALESCE(cn.code, 'evm') AS chain,
       COALESCE(cn.name, 'EVM') AS chain_name,
       al.scope,
       al.address,
       al.display_name,
       al.kind,
       al.note,
       to_char(al.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS updated_at
  FROM wallet_address_labels al
  JOIN ledgers l ON l.id = al.ledger_id
  LEFT JOIN chain_networks cn ON cn.id = al.chain_network_id
)SQL";

application::AddressLabelView address_label_from_row(const Row &row) {
    auto address = row["address"].as<std::string>();
    auto shortened = domain::chain::short_address(address);
    return application::AddressLabelView{
        .id = row["id"].as<std::string>(),
        .ledgerId = row["ledger_id"].as<std::string>(),
        .chain = row["chain"].as<std::string>(),
        .chainName = row["chain_name"].as<std::string>(),
        .scope = row["scope"].as<std::string>(),
        .address = std::move(address),
        .addressShort = std::move(shortened),
        .displayName = row["display_name"].as<std::string>(),
        .kind = row["kind"].as<std::string>(),
        .note = row["note"].as<std::string>(),
        .updatedAt = row["updated_at"].as<std::string>(),
    };
}

drogon::Task<application::JournalRowView>
fetch_journal_row(const DbClientPtr &client, std::int64_t row_id) {
    const auto query = journal_row_select() + " WHERE r.id = $1";
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
    // 仲裁索引写成 ledgers_active_name_unique 的定义本身（lower(name) 上的部分唯一索引），
    // 于是重名在 PostgreSQL 内部就被判定完毕：RETURNING 不出行即为重名，其它任何唯一索引
    // 的冲突仍然照常抛 SQL 异常并保持 500。
    const auto ledger = co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO ledgers(name) VALUES($1)
ON CONFLICT (lower(name)) WHERE archived_at IS NULL DO NOTHING
RETURNING id
)SQL",
        std::string(name));
    if (ledger.empty()) throw DuplicateValue("ledger", "name");
    const auto ledger_id = ledger.front()["id"].as<std::int64_t>();

    // Migration 0002 only backfilled a default asset for the ledgers that
    // existed then; every ledger created afterwards needs its own, because
    // accounts, journal rows and postings all carry a NOT NULL asset_id.
    const auto asset = co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO assets(ledger_id, symbol, name, decimals, is_default)
VALUES ($1, 'DEFAULT', '默认本位资产', 2, TRUE)
RETURNING id
)SQL",
        ledger_id);
    const auto asset_id = asset.front()["id"].as<std::int64_t>();

    co_await transaction->execSqlCoro(
        R"SQL(
INSERT INTO accounts(ledger_id, name, system_code, asset_id)
VALUES ($1, '未分配账户', 'unallocated', $2),
       ($1, '收入结转', 'income_clearing', $2),
       ($1, '支出结转', 'expense_clearing', $2),
       ($1, '期初余额', 'opening_balance', $2)
)SQL",
        ledger_id, asset_id);
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
    // 不要把 co_await 写进条件运算符的两个分支：GCC 14 对这种写法生成的协程代码会执行错分支，
    // 曾导致无 CSRF 时解引用空 optional（std::bad_alloc 崩溃）并把 2 个参数绑定到 1 参数语句上。
    drogon::orm::Result result{nullptr};
    if (csrf_hash_hex) {
        result = co_await client_->execSqlCoro(query, token_hash_hex, *csrf_hash_hex);
    } else {
        result = co_await client_->execSqlCoro(query, token_hash_hex);
    }
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

// One summary line per asset the ledger holds, because money in different assets cannot be
// added: the previous single figure summed DEFAULT + ETH + POL + SOL + TRX + USDT + USDC and
// rounded the mixture to two decimals, which is not a quantity of anything.
//
// The scalar balance/income/expense stay in the response — the schema requires them and the
// UI leads with them — but they now mean *the ledger's default asset only*, the one asset a
// ledger always has and the one its manually entered rows are denominated in. Chain assets
// are reported in assetSummaries beside it, never folded into it. The alternative, blanking
// the scalars unless the ledger is single-asset, would break every caller the moment a wallet
// is synced; scoping them keeps them true and keeps them defined.
//
// The period bounds are inclusive and apply to journal_rows.occurred_on. income, expense and
// rowCount are flows and are bounded on both sides. balance is a stock, so it is the closing
// balance as of `to` — every posting up to the end of the period, opening balances included;
// `from` cannot bound it without turning it into a movement that no longer reconciles with
// the accounts. Absent (or blank) bounds leave the corresponding side open.
drogon::Task<application::LedgerSummaryView>
PostgresRepository::ledger_summary(std::string_view ledger_public_id,
                                   std::optional<std::string> from,
                                   std::optional<std::string> to) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH selected_ledger AS (
    SELECT id FROM ledgers WHERE public_id = $1::uuid AND archived_at IS NULL
), period AS (
    SELECT NULLIF(btrim($2), '')::date AS from_day, NULLIF(btrim($3), '')::date AS to_day
), posting_totals AS (
    SELECT p.account_id, sum(p.signed_amount) AS total
      FROM postings p JOIN journal_rows r ON r.id = p.row_id
     WHERE r.ledger_id = (SELECT id FROM selected_ledger) AND r.deleted_at IS NULL
       AND ((SELECT to_day FROM period) IS NULL OR r.occurred_on <= (SELECT to_day FROM period))
     GROUP BY p.account_id
), asset_balances AS (
    SELECT a.asset_id, sum(a.opening_balance + COALESCE(pt.total, 0)) AS balance
      FROM accounts a LEFT JOIN posting_totals pt ON pt.account_id = a.id
     WHERE a.ledger_id = (SELECT id FROM selected_ledger)
       AND a.system_code IS NULL AND a.archived_at IS NULL AND a.deleted_at IS NULL
     GROUP BY a.asset_id
), asset_flows AS (
    SELECT r.asset_id,
           sum(r.amount) FILTER (WHERE r.kind = 'entry' AND r.amount > 0) AS income,
           sum(r.amount) FILTER (WHERE r.kind = 'entry' AND r.amount < 0) AS expense,
           count(*) AS row_count
      FROM journal_rows r
     WHERE r.ledger_id = (SELECT id FROM selected_ledger) AND r.deleted_at IS NULL
       AND ((SELECT from_day FROM period) IS NULL OR r.occurred_on >= (SELECT from_day FROM period))
       AND ((SELECT to_day FROM period) IS NULL OR r.occurred_on <= (SELECT to_day FROM period))
     GROUP BY r.asset_id
)
SELECT ast.public_id::text AS asset_id,
       ast.symbol,
       ast.name,
       ast.decimals,
       ast.is_default,
       round(COALESCE(ab.balance, 0), ast.decimals)::text AS balance,
       round(COALESCE(af.income, 0), ast.decimals)::text AS income,
       round(COALESCE(af.expense, 0), ast.decimals)::text AS expense,
       COALESCE(af.row_count, 0)::bigint AS row_count
  FROM assets ast
  LEFT JOIN asset_balances ab ON ab.asset_id = ast.id
  LEFT JOIN asset_flows af ON af.asset_id = ast.id
 WHERE ast.ledger_id = (SELECT id FROM selected_ledger)
 ORDER BY ast.is_default DESC, lower(ast.symbol), ast.id
)SQL", std::string(ledger_public_id), from, to);

    // An unknown or archived ledger selects no asset at all, which is the same empty answer
    // the old query produced through its COALESCEs. The zeros carry the two decimals every
    // default asset is created with, so the response still parses as Money.
    application::LedgerSummaryView summary{
        .balance = "0.00",
        .income = "0.00",
        .expense = "0.00",
        .rowCount = 0,
        .assetSummaries = {},
    };
    summary.assetSummaries.reserve(rows.size());
    for (const auto &row : rows) {
        application::AssetSummaryView asset{
            .assetId = row["asset_id"].as<std::string>(),
            .symbol = row["symbol"].as<std::string>(),
            .name = row["name"].as<std::string>(),
            .decimals = row["decimals"].as<std::int16_t>(),
            .balance = row["balance"].as<std::string>(),
            .income = row["income"].as<std::string>(),
            .expense = row["expense"].as<std::string>(),
        };
        // Rows are countable across assets even when their amounts are not, so rowCount
        // stays a ledger-wide count of the rows inside the period.
        summary.rowCount += row["row_count"].as<std::int64_t>();
        if (row["is_default"].as<bool>()) {
            summary.balance = asset.balance;
            summary.income = asset.income;
            summary.expense = asset.expense;
        }
        summary.assetSummaries.push_back(std::move(asset));
    }
    co_return summary;
}

drogon::Task<std::vector<application::AccountView>>
PostgresRepository::list_accounts(std::string_view ledger_public_id) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT a.public_id::text AS id, a.name,
       round(a.opening_balance, ast.decimals)::text AS opening_balance,
       round(a.opening_balance + COALESCE(sum(p.signed_amount)
          FILTER (WHERE r.deleted_at IS NULL), 0), ast.decimals)::text AS balance,
       (a.archived_at IS NOT NULL) AS archived,
       ast.public_id::text AS asset_id,
       ast.symbol AS asset_symbol,
       ast.decimals AS asset_decimals
  FROM accounts a
  JOIN ledgers l ON l.id = a.ledger_id
  JOIN assets ast ON ast.id = a.asset_id
  LEFT JOIN postings p ON p.account_id = a.id
  LEFT JOIN journal_rows r ON r.id = p.row_id
 WHERE l.public_id = $1::uuid AND a.system_code IS NULL AND a.deleted_at IS NULL
 GROUP BY a.id, ast.id
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
WITH selected_ledger AS (
    SELECT id FROM ledgers WHERE public_id = $1::uuid AND archived_at IS NULL
), inserted AS (
    INSERT INTO accounts(ledger_id, name, opening_balance, asset_id)
    SELECT sl.id, $2, $3::numeric, ast.id
      FROM selected_ledger sl
      JOIN assets ast ON ast.ledger_id = sl.id AND ast.is_default
    ON CONFLICT (ledger_id, lower(name))
        WHERE archived_at IS NULL AND deleted_at IS NULL AND system_code IS NULL
    DO NOTHING
    RETURNING *
)
SELECT i.public_id::text AS id, i.name,
       round(i.opening_balance, ast.decimals)::text AS opening_balance,
       round(i.opening_balance, ast.decimals)::text AS balance, false AS archived,
       ast.public_id::text AS asset_id, ast.symbol AS asset_symbol, ast.decimals AS asset_decimals
  FROM inserted i JOIN assets ast ON ast.id = i.asset_id
)SQL", std::string(ledger_public_id), input.name, input.openingBalance);
    // 空结果现在有两种成因，必须分开报：CTE 的第一段没选出账本（含账本缺了默认资产这种
    // 部署问题，此前也是按 404 报的，保持不变），或者 accounts_active_name_unique 拦下了
    // 重名。先确认账本连同默认资产都在，剩下的唯一可能就是重名——ON CONFLICT 只对准了那
    // 一个索引，不会吞掉别的冲突。
    if (rows.empty()) {
        const auto ledger = co_await client_->execSqlCoro(
            R"SQL(
SELECT 1 FROM ledgers l
  JOIN assets a ON a.ledger_id = l.id AND a.is_default
 WHERE l.public_id = $1::uuid AND l.archived_at IS NULL
)SQL",
            std::string(ledger_public_id));
        if (ledger.empty()) throw application::EntityNotFound("ledger");
        throw DuplicateValue("account", "name");
    }
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
    ON CONFLICT (ledger_id, direction, lower(name))
        WHERE archived_at IS NULL AND deleted_at IS NULL AND system_code IS NULL
    DO NOTHING
    RETURNING *
)
SELECT public_id::text AS id, name, direction, false AS archived FROM inserted
)SQL", std::string(ledger_public_id), input.name, input.direction);
    // 与 create_account 同样的分诊：账本不在是 404，账本在就说明是
    // categories_active_name_unique（ledger_id, direction, lower(name)）拦下的重名。
    if (rows.empty()) {
        const auto ledger = co_await client_->execSqlCoro(
            "SELECT 1 FROM ledgers WHERE public_id=$1::uuid AND archived_at IS NULL",
            std::string(ledger_public_id));
        if (ledger.empty()) throw application::EntityNotFound("ledger");
        throw DuplicateValue("category", "name");
    }
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
    if (rows.empty()) throw application::EntityNotFound("ledger");
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
    if (current.empty()) throw application::EntityNotFound("column");
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
    if (result.affectedRows() == 0) throw application::EntityNotFound("column");
}

drogon::Task<> PostgresRepository::restore_column(std::string_view column_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE ledger_columns SET deleted_at=NULL
 WHERE public_id=$1::uuid AND deleted_at IS NOT NULL
)SQL", std::string(column_public_id));
    if (result.affectedRows() == 0) throw application::EntityNotFound("column");
}

drogon::Task<application::RowPage>
PostgresRepository::list_rows(std::string_view ledger_public_id,
                              const application::RowQuery &query) const {
    const std::string sort_expression = query.sortKey == "amount" ? "r.amount" : "r.occurred_on";
    const std::string cursor_cast = query.sortKey == "amount" ? "numeric" : "date";
    const std::string direction = query.ascending ? "ASC" : "DESC";
    const std::string comparison = query.ascending ? ">" : "<";

    std::string sql = journal_row_select() + R"SQL(
  JOIN ledgers l ON l.id = r.ledger_id
 WHERE l.public_id = $1::uuid
)SQL";
    sql += query.recycled ? " AND r.deleted_at IS NOT NULL\n" : " AND r.deleted_at IS NULL\n";
    const bool has_cursor = query.cursorValue && query.cursorId;
    if (has_cursor) {
        sql += " AND (" + sort_expression + ", r.public_id) " + comparison +
               " ($2::" + cursor_cast + ", $3::uuid)\n";
    }
    sql += " ORDER BY " + sort_expression + " " + direction + ", r.public_id " + direction;
    sql += has_cursor ? " LIMIT $4" : " LIMIT $2";

    const auto fetch_limit = static_cast<std::int64_t>(query.limit) + 1;
    // 同 find_session：co_await 不能放进条件运算符的分支里。
    drogon::orm::Result rows{nullptr};
    if (has_cursor) {
        rows = co_await client_->execSqlCoro(sql, std::string(ledger_public_id),
                                             *query.cursorValue, *query.cursorId, fetch_limit);
    } else {
        rows = co_await client_->execSqlCoro(sql, std::string(ledger_public_id), fetch_limit);
    }

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

    // 账户必须先于资产解析：一行流水的资产是它所指账户的资产（见 row_asset），资产又决定
    // 金额允许的小数位，所以顺序只能是 账户 -> 资产 -> 金额精度。副作用是“账户不存在 + 金额
    // 超精度”同时发生时先报 404 account_not_found —— 这是对的，精度要用哪一档小数位，本来
    // 就要等账户认出来才知道。
    if (input.kind != "note") {
        Result account_result(nullptr);
        if (input.accountId) {
            account_result = co_await transaction->execSqlCoro(R"SQL(
SELECT id FROM accounts
 WHERE ledger_id=$1 AND public_id=$2::uuid AND system_code IS NULL
   AND archived_at IS NULL AND deleted_at IS NULL
)SQL", ledger_id, *input.accountId);
        } else {
            // 兜底账户显式钉在默认资产上：accounts 的唯一键已经是
            // (ledger_id, system_code, asset_id)，同一个 system_code 在不同资产上可以并存，
            // 只按 system_code 取会随资产增多而变成多行。
            account_result = co_await transaction->execSqlCoro(R"SQL(
SELECT id FROM accounts
 WHERE ledger_id=$1 AND system_code='unallocated'
   AND asset_id=(SELECT id FROM assets WHERE ledger_id=$1 AND is_default)
)SQL", ledger_id);
        }
        // 客户端点名的账户不存在（或已归档/已删除）是缺失实体，走 404 account_not_found；
        // 兜底的 unallocated 系统账户缺失属于账本种子数据被破坏，必须留在 500。
        if (account_result.empty()) {
            if (input.accountId) throw application::EntityNotFound("account");
            throw std::runtime_error("ledger unallocated account missing");
        }
        account_id = account_result.front()["id"].as<std::int64_t>();
    }

    // The asset decides how much precision the amount may carry, so it has to be resolved
    // before anything is written. It is the row's account's asset — hard-coding the ledger's
    // default asset here is what made every manual row on a chain account a 500:
    // journal_rows_link_guard requires journal_rows.asset_id = accounts.asset_id, so a row on
    // an ETH account could never be written at the ledger's DEFAULT asset. A note row has no
    // account and falls back to the row's own asset, then to the ledger default; the guard
    // checks nothing there, because account_id and transfer_account_id are both NULL.
    //
    // Rejecting here is what makes assetDecimals mean something: without it a POST of
    // {"amount":"-1.23456789"} against a 2-decimal asset returned 201 and stored
    // -1.234567890000000000, a value no rendering at that asset's scale can show and no
    // balance can reproduce. Reading the scale off the *row's* asset is the other half:
    // an 18-decimal asset must accept 0.123456789012345678, which the default asset's 2
    // decimals rejected. journal_service maps std::invalid_argument from a row write to
    // 422 validation_error with fields.amount, which is where this violation belongs.
    const auto asset = co_await row_asset(transaction, ledger_id, existing_row_id, account_id);
    if (!amount.fits_scale(static_cast<std::uint8_t>(asset.decimals))) {
        throw std::invalid_argument("该资产金额最多保留 " + std::to_string(asset.decimals) +
                                    " 位小数");
    }

    if (input.kind == "entry") {
        const std::string direction = amount.is_positive() ? "income" : "expense";
        if (input.categoryId) {
            // 方向不再进 WHERE：把 direction=$3 留在 SQL 里，"分类不存在" 与 "分类方向与金额
            // 符号相反" 会塌缩成同一个空结果集，只能一起报成 500。改成按 id + 账本取回分类，
            // 再在 C++ 里比对方向，两种情况就分别对应 404 与 422。
            const auto category_result = co_await transaction->execSqlCoro(R"SQL(
SELECT id, direction FROM categories
 WHERE ledger_id=$1 AND public_id=$2::uuid AND system_code IS NULL
   AND archived_at IS NULL AND deleted_at IS NULL
)SQL", ledger_id, *input.categoryId);
            if (category_result.empty()) throw application::EntityNotFound("category");
            if (category_result.front()["direction"].as<std::string>() != direction) {
                throw CategoryDirectionMismatch(direction);
            }
            category_id = category_result.front()["id"].as<std::int64_t>();
        } else {
            // 未点名分类时落到账本的 unallocated 系统分类；它缺失说明账本种子数据被破坏，
            // 与 unallocated 账户同样留在 500。
            const auto category_result = co_await transaction->execSqlCoro(
                "SELECT id FROM categories WHERE ledger_id=$1 AND system_code=$2",
                ledger_id, direction == "income" ? "unallocated_income" : "unallocated_expense");
            if (category_result.empty()) {
                throw std::runtime_error("ledger unallocated category missing");
            }
            category_id = category_result.front()["id"].as<std::int64_t>();
        }

        // 结转账户必须与本行同资产（见 clearing_account）：账本种子里的那对结转账户只属于
        // 默认资产，链上资产的行拿它做对方科目会被 check_row_postings_balanced 拒绝。
        counterparty_id = co_await clearing_account(transaction, ledger_id, asset.id,
                                                    direction == "income");
    } else if (input.kind == "transfer") {
        const auto destination = co_await transaction->execSqlCoro(R"SQL(
SELECT a.id, a.asset_id, ast.symbol
  FROM accounts a JOIN assets ast ON ast.id = a.asset_id
 WHERE a.ledger_id=$1 AND a.public_id=$2::uuid AND a.system_code IS NULL
   AND a.archived_at IS NULL AND a.deleted_at IS NULL
)SQL", ledger_id, *input.transferAccountId);
        if (destination.empty()) throw application::EntityNotFound("account");
        // 一行流水只有一个 asset_id，postings_balance_guard 还要求一行的分录
        // count(DISTINCT asset_id) = 1，所以跨资产转账根本无法表达。放它进去只会在 INSERT
        // 时撞上触发器、退化成 500；这里提前抛一个有类型的异常，服务层能翻成 422。
        if (destination.front()["asset_id"].as<std::int64_t>() != asset.id) {
            throw TransferAssetMismatch(asset.symbol,
                                        destination.front()["symbol"].as<std::string>());
        }
        transfer_account_id = destination.front()["id"].as<std::int64_t>();
        if (transfer_account_id == account_id) {
            throw std::runtime_error("transfer accounts must differ");
        }
    }

    std::int64_t row_id = 0;
    const std::int64_t asset_id = asset.id;
    if (existing_row_id) {
        constexpr std::string_view cell_tables[] = {
            "text_cells", "number_cells", "date_cells", "boolean_cells", "option_cells", "relation_cells"};
        for (const auto table : cell_tables) {
            co_await transaction->execSqlCoro(
                "DELETE FROM " + std::string(table) + " WHERE row_id=$1", *existing_row_id);
        }
        co_await transaction->execSqlCoro("DELETE FROM postings WHERE row_id=$1", *existing_row_id);
        // asset_id 也要跟着写：改到另一个资产的账户上时，行的资产必须一起搬过去，否则
        // journal_rows_link_guard 立刻拒绝。账户没变时 row_asset 解析出的就是原来那个资产，
        // 这个赋值是恒等的。
        const auto updated = co_await transaction->execSqlCoro(R"SQL(
UPDATE journal_rows
   SET occurred_on=$2::date, description=$3, kind=$4, amount=$5::numeric,
       account_id=$6, category_id=$7, transfer_account_id=$8, asset_id=$9,
       revision=revision+1
 WHERE id=$1
RETURNING id
)SQL", *existing_row_id, input.date, input.description, input.kind, input.amount,
            account_id, category_id, transfer_account_id, asset_id);
        if (updated.empty()) throw application::EntityNotFound("row");
        row_id = *existing_row_id;
    } else {
        const auto inserted = co_await transaction->execSqlCoro(R"SQL(
INSERT INTO journal_rows(
    ledger_id, occurred_on, description, kind, amount,
    account_id, category_id, transfer_account_id, created_by, asset_id)
VALUES($1, $2::date, $3, $4, $5::numeric, $6, $7, $8, $9, $10)
RETURNING id
)SQL", ledger_id, input.date, input.description, input.kind, input.amount,
            account_id, category_id, transfer_account_id, user_id, asset_id);
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
INSERT INTO postings(row_id, account_id, category_id, signed_amount, line_number, asset_id)
VALUES($1, $2, $3, $4::numeric, $5, $6)
)SQL", row_id, posting.account.value,
                posting.category ? std::optional(posting.category->value) : std::nullopt,
                posting.signed_amount.to_string(), static_cast<std::int16_t>(posting.line_number),
                asset_id);
        }
    }

    for (const auto &[column_public_id, value] : input.cells) {
        if (!value) continue;
        const auto column = co_await transaction->execSqlCoro(R"SQL(
SELECT id, value_type
  FROM ledger_columns
 WHERE ledger_id=$1 AND public_id=$2::uuid AND system_key IS NULL AND deleted_at IS NULL
)SQL", ledger_id, column_public_id);
        if (column.empty()) throw application::EntityNotFound("column");
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
    if (ledger.empty()) throw application::EntityNotFound("ledger");
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
    if (existing.empty()) throw application::EntityNotFound("row");
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
    if (result.affectedRows() == 0) throw application::EntityNotFound("row");
}

drogon::Task<> PostgresRepository::restore_row(std::string_view row_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE journal_rows SET deleted_at=NULL, revision=revision+1
 WHERE public_id=$1::uuid AND deleted_at IS NOT NULL
)SQL", std::string(row_public_id));
    if (result.affectedRows() == 0) throw application::EntityNotFound("row");
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
    if (result.affectedRows() == 0) throw application::EntityNotFound("job");
}

drogon::Task<application::ChainSettingsView> PostgresRepository::chain_settings() const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT sync_interval_minutes,
       (tron_grid_api_key_ciphertext IS NOT NULL) AS tron_grid_api_key_configured,
       (etherscan_api_key_ciphertext IS NOT NULL) AS etherscan_api_key_configured,
       (ethereum_rpc_url_ciphertext IS NOT NULL) AS ethereum_rpc_url_configured,
       (polygon_rpc_url_ciphertext IS NOT NULL) AS polygon_rpc_url_configured,
       (solana_rpc_url_ciphertext IS NOT NULL) AS solana_rpc_url_configured
  FROM chain_settings WHERE singleton=TRUE
)SQL");
    if (rows.empty()) throw std::runtime_error("chain settings not found");
    const auto &row = rows[0];

    const bool ethereum_configured = row["ethereum_rpc_url_configured"].as<bool>();
    const bool polygon_configured = row["polygon_rpc_url_configured"].as<bool>();
    const bool solana_configured = row["solana_rpc_url_configured"].as<bool>();

    // Custom RPC URLs are stored encrypted because they carry provider secrets,
    // so their display value stays redacted (empty) whenever one is configured.
    // Only public defaults are echoed back. Ethereum and Polygon history is read
    // through Etherscan rather than an RPC endpoint, so they have no default.
    co_return application::ChainSettingsView{
        .tronGridApiKeyConfigured = row["tron_grid_api_key_configured"].as<bool>(),
        .etherscanApiKeyConfigured = row["etherscan_api_key_configured"].as<bool>(),
        .syncIntervalMinutes = row["sync_interval_minutes"].as<std::int32_t>(),
        .tronGridEndpoint = "https://api.trongrid.io",
        .ethereumRpcUrlConfigured = ethereum_configured,
        .ethereumRpcEndpoint = {},
        .polygonRpcUrlConfigured = polygon_configured,
        .polygonRpcEndpoint = {},
        .solanaRpcUrlConfigured = solana_configured,
        .solanaRpcEndpoint = solana_configured ? std::string{}
                                               : std::string{"https://api.mainnet-beta.solana.com"},
    };
}

drogon::Task<ChainSettingsSecretRecord> PostgresRepository::chain_settings_secret() const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT tron_grid_api_key_ciphertext, tron_grid_api_key_nonce,
       etherscan_api_key_ciphertext, etherscan_api_key_nonce,
       ethereum_rpc_url_ciphertext, ethereum_rpc_url_nonce,
       polygon_rpc_url_ciphertext, polygon_rpc_url_nonce,
       solana_rpc_url_ciphertext, solana_rpc_url_nonce,
       sync_interval_minutes
  FROM chain_settings WHERE singleton=TRUE
)SQL");
    if (rows.empty()) throw std::runtime_error("chain settings not found");
    const auto &row = rows[0];

    auto to_hex = [](const Row &r, const char *col) -> std::optional<std::string> {
        if (r[col].isNull()) return std::nullopt;
        const auto bytes = r[col].as<std::vector<char>>();
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (char byte : bytes) {
            static constexpr char digits[] = "0123456789abcdef";
            const auto ubyte = static_cast<unsigned char>(byte);
            hex.push_back(digits[ubyte >> 4]);
            hex.push_back(digits[ubyte & 0xf]);
        }
        return hex;
    };

    auto enc = [&](const char *cipher, const char *nonce) -> EncryptedSecretRecord {
        return EncryptedSecretRecord{
            .ciphertextHex = to_hex(row, cipher),
            .nonceHex = to_hex(row, nonce),
        };
    };

    co_return ChainSettingsSecretRecord{
        .tronGridApiKey = enc("tron_grid_api_key_ciphertext", "tron_grid_api_key_nonce"),
        .etherscanApiKey = enc("etherscan_api_key_ciphertext", "etherscan_api_key_nonce"),
        .ethereumRpcUrl = enc("ethereum_rpc_url_ciphertext", "ethereum_rpc_url_nonce"),
        .polygonRpcUrl = enc("polygon_rpc_url_ciphertext", "polygon_rpc_url_nonce"),
        .solanaRpcUrl = enc("solana_rpc_url_ciphertext", "solana_rpc_url_nonce"),
        .syncIntervalMinutes = row["sync_interval_minutes"].as<std::int32_t>(),
    };
}

drogon::Task<application::ChainSettingsView> PostgresRepository::update_chain_settings(
    const application::ChainSettingsPatch &patch,
    const ChainSettingsSecretPatch &secrets) const {

    auto from_hex = [](const std::optional<std::string> &hex) -> std::optional<std::vector<char>> {
        if (!hex || hex->empty()) return std::nullopt;
        if (hex->size() % 2 != 0) throw std::runtime_error("invalid hex string");
        std::vector<char> bytes;
        bytes.reserve(hex->size() / 2);
        for (size_t i = 0; i < hex->size(); i += 2) {
            unsigned val;
            if (std::sscanf(hex->c_str() + i, "%2x", &val) != 1) {
                throw std::runtime_error("invalid hex string");
            }
            bytes.push_back(static_cast<char>(val));
        }
        return bytes;
    };

    // One secret group: keep, replace or clear. Every group binds the same three
    // parameters so the statement below stays fixed and cannot silently lose a
    // field the way a hand-assembled placeholder list does.
    struct SecretBinding {
        bool clear{false};
        std::optional<std::vector<char>> ciphertext;
        std::optional<std::vector<char>> nonce;
    };

    auto binding = [&](const std::optional<EncryptedSecretRecord> &secret,
                       const std::optional<bool> &clear_flag) -> SecretBinding {
        SecretBinding result;
        if (secret) {
            result.ciphertext = from_hex(secret->ciphertextHex);
            result.nonce = from_hex(secret->nonceHex);
            // A patch that carries no usable ciphertext means "remove it".
            if (!result.ciphertext || !result.nonce) {
                result.ciphertext.reset();
                result.nonce.reset();
                result.clear = true;
            }
        }
        if (clear_flag.value_or(false)) {
            result.ciphertext.reset();
            result.nonce.reset();
            result.clear = true;
        }
        return result;
    };

    const auto tron = binding(secrets.tronGridApiKey, patch.clearTronGridApiKey);
    const auto etherscan = binding(secrets.etherscanApiKey, patch.clearEtherscanApiKey);
    const auto ethereum = binding(secrets.ethereumRpcUrl, patch.clearEthereumRpcUrl);
    const auto polygon = binding(secrets.polygonRpcUrl, patch.clearPolygonRpcUrl);
    const auto solana = binding(secrets.solanaRpcUrl, patch.clearSolanaRpcUrl);

    co_await client_->execSqlCoro(R"SQL(
UPDATE chain_settings SET
    sync_interval_minutes = COALESCE($1::int, sync_interval_minutes),
    tron_grid_api_key_ciphertext = CASE WHEN $2::boolean THEN NULL
        ELSE COALESCE($3::bytea, tron_grid_api_key_ciphertext) END,
    tron_grid_api_key_nonce = CASE WHEN $2::boolean THEN NULL
        ELSE COALESCE($4::bytea, tron_grid_api_key_nonce) END,
    tron_grid_api_key_updated_at = CASE WHEN $2::boolean THEN NULL
        WHEN $3::bytea IS NOT NULL THEN clock_timestamp()
        ELSE tron_grid_api_key_updated_at END,
    etherscan_api_key_ciphertext = CASE WHEN $5::boolean THEN NULL
        ELSE COALESCE($6::bytea, etherscan_api_key_ciphertext) END,
    etherscan_api_key_nonce = CASE WHEN $5::boolean THEN NULL
        ELSE COALESCE($7::bytea, etherscan_api_key_nonce) END,
    etherscan_api_key_updated_at = CASE WHEN $5::boolean THEN NULL
        WHEN $6::bytea IS NOT NULL THEN clock_timestamp()
        ELSE etherscan_api_key_updated_at END,
    ethereum_rpc_url_ciphertext = CASE WHEN $8::boolean THEN NULL
        ELSE COALESCE($9::bytea, ethereum_rpc_url_ciphertext) END,
    ethereum_rpc_url_nonce = CASE WHEN $8::boolean THEN NULL
        ELSE COALESCE($10::bytea, ethereum_rpc_url_nonce) END,
    ethereum_rpc_url_updated_at = CASE WHEN $8::boolean THEN NULL
        WHEN $9::bytea IS NOT NULL THEN clock_timestamp()
        ELSE ethereum_rpc_url_updated_at END,
    polygon_rpc_url_ciphertext = CASE WHEN $11::boolean THEN NULL
        ELSE COALESCE($12::bytea, polygon_rpc_url_ciphertext) END,
    polygon_rpc_url_nonce = CASE WHEN $11::boolean THEN NULL
        ELSE COALESCE($13::bytea, polygon_rpc_url_nonce) END,
    polygon_rpc_url_updated_at = CASE WHEN $11::boolean THEN NULL
        WHEN $12::bytea IS NOT NULL THEN clock_timestamp()
        ELSE polygon_rpc_url_updated_at END,
    solana_rpc_url_ciphertext = CASE WHEN $14::boolean THEN NULL
        ELSE COALESCE($15::bytea, solana_rpc_url_ciphertext) END,
    solana_rpc_url_nonce = CASE WHEN $14::boolean THEN NULL
        ELSE COALESCE($16::bytea, solana_rpc_url_nonce) END,
    solana_rpc_url_updated_at = CASE WHEN $14::boolean THEN NULL
        WHEN $15::bytea IS NOT NULL THEN clock_timestamp()
        ELSE solana_rpc_url_updated_at END
 WHERE singleton=TRUE
)SQL",
        patch.syncIntervalMinutes,
        tron.clear, tron.ciphertext, tron.nonce,
        etherscan.clear, etherscan.ciphertext, etherscan.nonce,
        ethereum.clear, ethereum.ciphertext, ethereum.nonce,
        polygon.clear, polygon.ciphertext, polygon.nonce,
        solana.clear, solana.ciphertext, solana.nonce);

    co_return co_await chain_settings();
}

drogon::Task<std::vector<application::WalletView>> PostgresRepository::list_wallets(
    std::string_view ledger_public_id) const {
    const auto rows = co_await client_->execSqlCoro(std::string(kWalletSelect) + R"SQL(
 WHERE l.public_id = $1::uuid AND w.deleted_at IS NULL
 ORDER BY lower(w.name), w.id
)SQL", std::string(ledger_public_id));

    std::vector<application::WalletView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) result.push_back(wallet_from_row(row));
    co_return result;
}

drogon::Task<std::vector<std::string>> PostgresRepository::list_due_wallet_ids(std::uint16_t limit) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT w.public_id::text AS id
  FROM wallet_accounts w
  LEFT JOIN wallet_sync_states s ON s.wallet_id = w.id
 WHERE w.deleted_at IS NULL
   AND w.enabled = TRUE
   AND w.auto_sync = TRUE
   AND (s.status IS NULL OR s.status IN ('idle', 'failed'))
   AND (w.last_synced_at IS NULL
        OR w.last_synced_at < clock_timestamp() - INTERVAL '1 minute' * (
            SELECT sync_interval_minutes FROM chain_settings WHERE singleton = TRUE
        ))
 ORDER BY COALESCE(w.last_synced_at, '1970-01-01'::timestamptz), w.id
 LIMIT $1
)SQL", static_cast<std::int64_t>(limit));

    std::vector<std::string> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        result.push_back(row["id"].as<std::string>());
    }
    co_return result;
}

drogon::Task<application::WalletView> PostgresRepository::create_wallet(
    std::string_view ledger_public_id,
    const application::WalletInput &input,
    std::string_view normalized_address) const {

    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH target AS (
    SELECT l.id AS ledger_id, l.public_id AS ledger_public_id,
           cn.id AS chain_network_id, cn.code AS chain_code, cn.name AS chain_name
      FROM ledgers l
      JOIN chain_networks cn ON cn.code = $7
     WHERE l.public_id = $1::uuid AND l.archived_at IS NULL
), inserted AS (
    INSERT INTO wallet_accounts
        (ledger_id, chain_network_id, name, address, address_normalized, enabled, auto_sync,
         accept_all_tokens, token_rules)
    SELECT t.ledger_id, t.chain_network_id, $2, $3, $4, $5, $6, FALSE, $8::jsonb FROM target t
    ON CONFLICT (ledger_id, chain_network_id, address_normalized)
        WHERE deleted_at IS NULL
    DO NOTHING
    RETURNING *
)
SELECT i.public_id::text AS id,
       t.ledger_public_id::text AS ledger_id,
       t.chain_code AS chain,
       t.chain_name AS chain_name,
       i.name,
       i.address,
       i.enabled,
       i.auto_sync,
       to_char(i.last_synced_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS last_synced_at,
       i.last_error,
       i.accept_all_tokens,
       i.token_rules::text AS token_rules,
       to_char(i.sync_from AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS sync_from,
       'idle' AS sync_status,
       to_char(i.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at
  FROM inserted i CROSS JOIN target t
)SQL",
        std::string(ledger_public_id),
        input.name,
        input.address,
        std::string(normalized_address),
        input.enabled,
        input.autoSync,
        input.chain,
        token_rules_json(input.acceptedTokens.value_or(std::vector<application::WalletTokenRule>{})));

    // 与 create_address_label 同样的分诊：账本缺失（或已归档）是缺失实体，走 404
    // ledger_not_found；链的取值已经由服务层按支持列表校验过，这里仍然落空说明
    // chain_networks 与支持列表脱节，属于部署问题，保留 500。账本与链都在的话，空结果
    // 只剩一种解释：wallet_accounts_active_address_unique 拦下了同一账本同一条链上重复
    // 添加的地址，这是调用方重新贴了一遍地址，走 409。
    if (rows.empty()) {
        const auto ledger = co_await client_->execSqlCoro(
            "SELECT 1 FROM ledgers WHERE public_id=$1::uuid AND archived_at IS NULL",
            std::string(ledger_public_id));
        if (ledger.empty()) throw application::EntityNotFound("ledger");
        const auto chain = co_await client_->execSqlCoro(
            "SELECT 1 FROM chain_networks WHERE code=$1", input.chain);
        if (chain.empty()) throw std::runtime_error("chain not found");
        throw DuplicateValue("wallet", "address");
    }
    co_return wallet_from_row(rows[0]);
}

drogon::Task<application::WalletView> PostgresRepository::update_wallet(
    std::string_view wallet_public_id,
    const application::WalletPatch &patch) const {

    // A fixed statement with COALESCE keeps every field bound: an omitted patch
    // field binds NULL and the column keeps its current value.
    std::optional<std::string> rules;
    if (patch.acceptedTokens) rules = token_rules_json(*patch.acceptedTokens);
    const auto updated = co_await client_->execSqlCoro(R"SQL(
UPDATE wallet_accounts
   SET name = COALESCE($2::text, name),
       enabled = COALESCE($3::boolean, enabled),
       auto_sync = COALESCE($4::boolean, auto_sync),
       token_rules = COALESCE($5::jsonb, token_rules),
       accept_all_tokens = CASE WHEN $5::jsonb IS NULL THEN accept_all_tokens ELSE FALSE END
 WHERE public_id=$1::uuid AND deleted_at IS NULL
RETURNING id
)SQL",
        std::string(wallet_public_id), patch.name, patch.enabled, patch.autoSync, rules);
    if (updated.empty()) throw application::EntityNotFound("wallet");

    const auto result = co_await wallet(wallet_public_id);
    if (!result) throw application::EntityNotFound("wallet");
    co_return *result;
}

drogon::Task<bool> PostgresRepository::accounts_bookable_in_default_asset(
    std::string_view ledger_public_id,
    const std::vector<std::string> &account_public_ids) const {
    if (account_public_ids.empty()) co_return true;
    std::string ids_json;
    if (glz::write_json(account_public_ids, ids_json)) throw std::runtime_error("id serialization failed");
    // Ids are compared as text so a malformed one simply fails to match instead of making
    // the uuid cast throw.
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT count(DISTINCT wanted.id) AS missing
  FROM jsonb_array_elements_text($2::jsonb) AS wanted(id)
 WHERE NOT EXISTS (
       SELECT 1
         FROM accounts a
         JOIN ledgers l ON l.id = a.ledger_id
         JOIN assets ast ON ast.id = a.asset_id AND ast.is_default
        WHERE l.public_id = $1::uuid
          AND a.public_id::text = wanted.id
          AND a.system_code IS NULL
          AND a.archived_at IS NULL AND a.deleted_at IS NULL)
)SQL", std::string(ledger_public_id), ids_json);
    co_return rows.front()["missing"].as<std::int64_t>() == 0;
}

drogon::Task<> PostgresRepository::delete_wallet(std::string_view wallet_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
UPDATE wallet_accounts SET deleted_at=clock_timestamp()
 WHERE public_id=$1::uuid AND deleted_at IS NULL
)SQL", std::string(wallet_public_id));
    if (result.affectedRows() == 0) throw application::EntityNotFound("wallet");
}

drogon::Task<std::optional<application::WalletView>> PostgresRepository::wallet(
    std::string_view wallet_public_id) const {
    const auto rows = co_await client_->execSqlCoro(std::string(kWalletSelect) + R"SQL(
 WHERE w.public_id = $1::uuid AND w.deleted_at IS NULL
)SQL", std::string(wallet_public_id));

    if (rows.empty()) co_return std::nullopt;
    co_return wallet_from_row(rows[0]);
}

drogon::Task<ChainFetchCursor> PostgresRepository::wallet_sync_cursor(
    std::string_view wallet_public_id) const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT s.checkpoint_block,
       CASE WHEN s.checkpoint_timestamp IS NULL THEN NULL
            ELSE (EXTRACT(EPOCH FROM s.checkpoint_timestamp) * 1000)::bigint
       END AS checkpoint_timestamp_ms,
       s.checkpoint_signature
  FROM wallet_accounts w
  LEFT JOIN wallet_sync_states s ON s.wallet_id = w.id
 WHERE w.public_id = $1::uuid AND w.deleted_at IS NULL
)SQL", std::string(wallet_public_id));
    if (rows.empty()) throw application::EntityNotFound("wallet");
    ChainFetchCursor cursor;
    const auto &row = rows.front();
    if (!row["checkpoint_block"].isNull()) {
        cursor.block = row["checkpoint_block"].as<std::int64_t>();
    }
    if (!row["checkpoint_timestamp_ms"].isNull()) {
        cursor.timestampMs = row["checkpoint_timestamp_ms"].as<std::int64_t>();
    }
    if (!row["checkpoint_signature"].isNull()) {
        cursor.signature = row["checkpoint_signature"].as<std::string>();
    }
    co_return cursor;
}

drogon::Task<bool> PostgresRepository::try_begin_wallet_sync(
    std::string_view wallet_public_id) const {
    const auto claimed = co_await client_->execSqlCoro(R"SQL(
INSERT INTO wallet_sync_states (wallet_id, status, last_started_at, last_error)
SELECT w.id, 'running', clock_timestamp(), NULL
  FROM wallet_accounts w
 WHERE w.public_id = $1::uuid AND w.deleted_at IS NULL
ON CONFLICT (wallet_id) DO UPDATE
   SET status = 'running',
       last_started_at = clock_timestamp(),
       last_error = NULL
 WHERE wallet_sync_states.status IN ('idle', 'failed')
RETURNING wallet_id
)SQL", std::string(wallet_public_id));
    if (!claimed.empty()) co_return true;
    const auto exists = co_await client_->execSqlCoro(R"SQL(
SELECT 1 FROM wallet_accounts WHERE public_id = $1::uuid AND deleted_at IS NULL
)SQL", std::string(wallet_public_id));
    if (exists.empty()) throw application::EntityNotFound("wallet");
    co_return false;
}

drogon::Task<> PostgresRepository::fail_wallet_sync(
    std::string_view wallet_public_id, std::string_view error) const {
    co_await client_->execSqlCoro(R"SQL(
UPDATE wallet_accounts
   SET last_error = $2
 WHERE public_id = $1::uuid AND deleted_at IS NULL
)SQL", std::string(wallet_public_id), std::string(error));
    co_await client_->execSqlCoro(R"SQL(
UPDATE wallet_sync_states s
   SET status = 'failed',
       last_error = $2,
       last_completed_at = clock_timestamp()
  FROM wallet_accounts w
 WHERE w.public_id = $1::uuid AND s.wallet_id = w.id AND s.status = 'running'
)SQL", std::string(wallet_public_id), std::string(error));
    co_return;
}

drogon::Task<SyncWriteStats> PostgresRepository::record_wallet_sync(
    std::string_view wallet_public_id,
    std::int64_t user_id,
    const std::vector<ChainTransactionInput> &transactions) const {

    // One transaction for the whole sync: assets, bookkeeping accounts, chain rows,
    // journal rows and the checkpoint either all land or none of them do.
    auto transaction = co_await client_->newTransactionCoro();
    const auto wallet = co_await transaction->execSqlCoro(R"SQL(
SELECT id, ledger_id, chain_network_id, accept_all_tokens, token_rules::text AS token_rules,
       CASE WHEN sync_from = '-infinity' THEN NULL
            ELSE (EXTRACT(EPOCH FROM sync_from) * 1000)::bigint END AS sync_from_ms
  FROM wallet_accounts
 WHERE public_id=$1::uuid AND deleted_at IS NULL
 FOR UPDATE
)SQL", std::string(wallet_public_id));
    if (wallet.empty()) throw application::EntityNotFound("wallet");
    const auto wallet_id = wallet.front()["id"].as<std::int64_t>();
    const auto ledger_id = wallet.front()["ledger_id"].as<std::int64_t>();
    const auto chain_network_id = wallet.front()["chain_network_id"].as<std::int64_t>();
    const bool accept_all_tokens = wallet.front()["accept_all_tokens"].as<bool>();
    // Read under the same lock the rows are written under, so an edit to the rules that
    // races a sync applies either wholly before it or wholly after it.
    const auto rules_json = wallet.front()["token_rules"].as<std::string>();
    std::set<std::string> accepted_assets;
    for (const auto &rule : token_rules_from_json(rules_json)) accepted_assets.insert(rule.contract);
    std::optional<std::int64_t> sync_from_ms;
    if (!wallet.front()["sync_from_ms"].isNull()) {
        sync_from_ms = wallet.front()["sync_from_ms"].as<std::int64_t>();
    }

    // Adapter output is flattened into two arrays and handed over as one jsonb
    // parameter each. A sync can carry hundreds of movements and every statement
    // below runs inside one transaction holding the wallet row locked, so a round
    // trip per movement would keep that lock for far longer than necessary.
    std::vector<sync_payload::Transaction> transaction_payload;
    std::vector<sync_payload::Movement> movement_payload;
    std::vector<sync_payload::Checkpoint> checkpoint_payload;
    transaction_payload.reserve(transactions.size());
    checkpoint_payload.reserve(transactions.size());
    for (const auto &input : transactions) {
        // tx_hash is half of the chain_transactions unique key: without it the row
        // can neither be deduplicated nor joined to its own movements.
        auto tx_hash = trim_copy(input.txHash);
        if (tx_hash.empty()) continue;
        checkpoint_payload.push_back(sync_payload::Checkpoint{
            .hash = tx_hash, .block = input.blockNumber, .ts_ms = input.blockTimestampMs});
        // History from before the wallet was added is not the ledger's to book; it only
        // advances the checkpoint. A transaction without a block time yet is still pending,
        // which means it is new.
        if (sync_from_ms && input.blockTimestampMs && *input.blockTimestampMs < *sync_from_ms) {
            continue;
        }
        const auto movements_before = movement_payload.size();

        for (const auto &movement : input.movements) {
            // The movement key is the idempotency key; an empty one would let the
            // same transfer be inserted again on every sync.
            if (trim_copy(movement.movementKey).empty()) continue;
            if (!storable_movement_direction(movement.direction)) continue;
            auto amount = trim_copy(movement.amount);
            if (!storable_chain_amount(amount)) continue;

            auto symbol = sanitized_asset_symbol(movement.assetSymbol);
            auto name = sanitized_asset_name(movement.assetName, symbol);
            auto contract = sanitized_optional(movement.contractAddress, 128);
            auto contract_normalized = sanitized_optional(movement.contractAddressNormalized, 128);
            // The two contract columns are kept in lockstep: assets_chain_identifier_unique
            // keys off the normalized one, while the assets is_native CHECK only accepts
            // is_native when it is NULL. `isNative` from the adapter is therefore derived
            // from the contract rather than trusted, so a token that also claims to be
            // native cannot trip the CHECK and abort the sync.
            if (!contract_normalized) contract_normalized = contract;
            if (!contract) contract = contract_normalized;
            // A currency the wallet does not accept leaves no trace at all: no asset, no
            // per-token account and no row. Address-poisoning spam is the common case.
            if (!accept_all_tokens &&
                !accepted_assets.contains(contract_normalized
                                              ? *contract_normalized
                                              : std::string(domain::chain::kNativeAssetKey))) {
                continue;
            }

            movement_payload.push_back(sync_payload::Movement{
                .mkey = movement.movementKey,
                .tx_hash = tx_hash,
                .direction = movement.direction,
                .symbol = std::move(symbol),
                .name = std::move(name),
                .decimals = sanitized_asset_decimals(movement.assetDecimals),
                .contract = std::move(contract),
                .contract_norm = std::move(contract_normalized),
                .amount = std::move(amount),
                .from_addr = sanitized_optional(movement.fromAddress, 128),
                .from_norm = sanitized_optional(movement.fromAddressNormalized, 128),
                .to_addr = sanitized_optional(movement.toAddress, 128),
                .to_norm = sanitized_optional(movement.toAddressNormalized, 128),
            });
        }

        // A transaction whose every movement was filtered out is not the ledger's business.
        if (!accept_all_tokens && movement_payload.size() == movements_before) continue;
        transaction_payload.push_back(sync_payload::Transaction{
            .hash = std::move(tx_hash),
            .block = input.blockNumber,
            .ts_ms = input.blockTimestampMs,
            .confirmed = input.confirmed,
            .success = input.success,
            .raw = input.rawJson,
        });
    }

    std::string transaction_json;
    if (const auto error = glz::write_json(transaction_payload, transaction_json); error) {
        throw std::runtime_error("chain transaction serialization failed");
    }
    std::string movement_json;
    if (const auto error = glz::write_json(movement_payload, movement_json); error) {
        throw std::runtime_error("chain movement serialization failed");
    }
    std::string checkpoint_json;
    if (const auto error = glz::write_json(checkpoint_payload, checkpoint_json); error) {
        throw std::runtime_error("chain checkpoint serialization failed");
    }


    // Creates the state row on first sync and stamps the true start time. The
    // 'running' status is only visible inside this transaction; a failure rolls it
    // back, so a crashed sync can never leave a wallet stuck out of list_due_wallet_ids.
    co_await transaction->execSqlCoro(R"SQL(
INSERT INTO wallet_sync_states(wallet_id, status, last_started_at, last_error)
VALUES ($1, 'running', clock_timestamp(), NULL)
ON CONFLICT (wallet_id) DO UPDATE
   SET status='running', last_started_at=clock_timestamp(), last_error=NULL
)SQL", wallet_id);

    // Chain facts are refreshed on re-sync: a transaction seen while unconfirmed gains
    // its block, timestamp and receipt status later. DISTINCT ON is required because
    // ON CONFLICT DO UPDATE cannot touch the same row twice in one statement.
    const auto upserted_transactions = co_await transaction->execSqlCoro(R"SQL(
WITH input AS (
    SELECT DISTINCT ON (t.hash)
           t.hash, t.block, t.ts_ms, t.confirmed, t.success, t.raw
      FROM jsonb_to_recordset($1::jsonb)
        AS t(hash text, block bigint, ts_ms bigint, confirmed boolean, success boolean, raw text)
     ORDER BY t.hash
)
INSERT INTO chain_transactions(
    ledger_id, chain_network_id, tx_hash, block_number, block_timestamp,
    confirmed, success, raw_json)
SELECT $2, $3, i.hash, i.block,
       CASE WHEN i.ts_ms IS NULL THEN NULL
            ELSE 'epoch'::timestamptz + i.ts_ms * interval '1 millisecond' END,
       COALESCE(i.confirmed, TRUE), COALESCE(i.success, TRUE),
       COALESCE(NULLIF(i.raw, ''), '{}')::jsonb
  FROM input i
ON CONFLICT (ledger_id, chain_network_id, tx_hash) DO UPDATE
   SET block_number = COALESCE(EXCLUDED.block_number, chain_transactions.block_number),
       block_timestamp = COALESCE(EXCLUDED.block_timestamp, chain_transactions.block_timestamp),
       confirmed = EXCLUDED.confirmed,
       success = EXCLUDED.success,
       raw_json = EXCLUDED.raw_json
RETURNING id
)SQL", transaction_json, ledger_id, chain_network_id);

    // One asset per (ledger, chain network, contract) with the native asset keyed by
    // symbol, exactly as assets_chain_identifier_unique defines it. Existing rows are
    // left alone: asset metadata is only ever discovered once.
    co_await transaction->execSqlCoro(R"SQL(
WITH input AS (
    SELECT DISTINCT ON (COALESCE(m.contract_norm, 'native:' || lower(m.symbol)))
           m.symbol, m.name, m.decimals, m.contract, m.contract_norm
      FROM jsonb_to_recordset($1::jsonb)
        AS m(symbol text, name text, decimals smallint, contract text, contract_norm text)
     ORDER BY COALESCE(m.contract_norm, 'native:' || lower(m.symbol)), m.symbol
)
INSERT INTO assets(ledger_id, chain_network_id, contract_address, contract_address_normalized,
                   symbol, name, decimals, is_native)
SELECT $2, $3, i.contract, i.contract_norm, i.symbol, i.name, i.decimals,
       i.contract_norm IS NULL
  FROM input i
ON CONFLICT (ledger_id, chain_network_id,
             COALESCE(contract_address_normalized, 'native:' || lower(symbol)))
    WHERE chain_network_id IS NOT NULL
DO NOTHING
)SQL", movement_json, ledger_id, chain_network_id);

    // Every asset needs its own clearing accounts: accounts.asset_id is NOT NULL and
    // check_row_postings_balanced rejects a posting whose account sits on another
    // asset, so the ledger-wide clearing accounts on the default asset cannot be used.
    // The wallet's own account is created once per (wallet, asset); its name falls back
    // to a numbered form when the plain one is already taken, because two wallets can
    // share a name and two tokens can share a symbol while accounts_active_name_unique
    // still demands one active name per ledger.
    co_await transaction->execSqlCoro(R"SQL(
WITH input AS (
    SELECT DISTINCT COALESCE(m.contract_norm, 'native:' || lower(m.symbol)) AS asset_key
      FROM jsonb_to_recordset($1::jsonb) AS m(symbol text, contract_norm text)
), converted AS (
    SELECT r.contract AS asset_key
      FROM jsonb_to_recordset($5::jsonb) AS r(contract text, "exchangeRate" text)
     WHERE NULLIF(btrim(r."exchangeRate"), '') IS NOT NULL
), used_assets AS (
    -- Converted currencies are booked in the default asset, which already has its clearing
    -- accounts and the user-chosen account, so they need no per-token accounts.
    SELECT a.id, a.symbol
      FROM assets a
      JOIN input i
        ON i.asset_key = COALESCE(a.contract_address_normalized, 'native:' || lower(a.symbol))
     WHERE a.ledger_id = $2 AND a.chain_network_id = $3
       AND NOT EXISTS (SELECT 1 FROM converted c
                        WHERE c.asset_key = COALESCE(a.contract_address_normalized, 'native'))
), clearing AS (
    INSERT INTO accounts(ledger_id, name, system_code, asset_id)
    SELECT $2, code.label || ' · ' || u.symbol, code.system_code, u.id
      FROM used_assets u
      CROSS JOIN (VALUES ('income_clearing'::text, '收入结转'::text),
                         ('expense_clearing', '支出结转')) AS code(system_code, label)
    ON CONFLICT (ledger_id, system_code, asset_id) DO NOTHING
    RETURNING id
), missing AS (
    SELECT u.id AS asset_id,
           left((SELECT w.name FROM wallet_accounts w WHERE w.id = $4), 70)
               || ' · ' || u.symbol AS base_name
      FROM used_assets u
     WHERE NOT EXISTS (SELECT 1 FROM wallet_asset_accounts wa
                        WHERE wa.wallet_id = $4 AND wa.asset_id = u.id)
), named AS (
    SELECT m.asset_id,
           CASE WHEN count(*) OVER (PARTITION BY lower(m.base_name)) > 1
                  OR EXISTS (SELECT 1 FROM accounts x
                              WHERE x.ledger_id = $2 AND x.system_code IS NULL
                                AND x.archived_at IS NULL AND x.deleted_at IS NULL
                                AND lower(x.name) = lower(m.base_name))
                THEN m.base_name || ' #' || m.asset_id::text
                ELSE m.base_name END AS name
      FROM missing m
), created AS (
    INSERT INTO accounts(ledger_id, name, asset_id)
    SELECT $2, n.name, n.asset_id FROM named n
    RETURNING id, asset_id
)
INSERT INTO wallet_asset_accounts(wallet_id, asset_id, account_id)
SELECT $4, c.asset_id, c.id FROM created c
)SQL", movement_json, ledger_id, chain_network_id, wallet_id, rules_json);

    // A movement is an immutable historical fact, so a re-sync of the same window must
    // leave the stored one untouched rather than rewrite it: its asset_id is already
    // baked into the journal row and postings created below.
    const auto created_movements = co_await transaction->execSqlCoro(R"SQL(
WITH input AS (
    SELECT DISTINCT ON (m.mkey)
           m.mkey, m.tx_hash, m.direction, m.symbol, m.contract_norm, m.amount,
           m.from_addr, m.from_norm, m.to_addr, m.to_norm
      FROM jsonb_to_recordset($1::jsonb)
        AS m(mkey text, tx_hash text, direction text, symbol text, contract_norm text,
             amount text, from_addr text, from_norm text, to_addr text, to_norm text)
     ORDER BY m.mkey
)
INSERT INTO chain_asset_movements(
    ledger_id, chain_transaction_id, wallet_id, asset_id, movement_key, direction, amount,
    from_address, from_address_normalized, to_address, to_address_normalized, occurred_at)
SELECT $2, ct.id, $4, a.id, i.mkey, i.direction, i.amount::numeric,
       i.from_addr, i.from_norm, i.to_addr, i.to_norm, ct.block_timestamp
  FROM input i
  JOIN chain_transactions ct
    ON ct.ledger_id = $2 AND ct.chain_network_id = $3 AND ct.tx_hash = i.tx_hash
  JOIN assets a
    ON a.ledger_id = $2 AND a.chain_network_id = $3
   AND COALESCE(a.contract_address_normalized, 'native:' || lower(a.symbol))
     = COALESCE(i.contract_norm, 'native:' || lower(i.symbol))
ON CONFLICT (ledger_id, movement_key) DO NOTHING
RETURNING id
)SQL", movement_json, ledger_id, chain_network_id, wallet_id);

    // Journal rows are created only for movements that have no link yet, which is what
    // keeps a re-sync from overwriting the user's later edits. Row ids are drawn from
    // the identity sequence up front so postings and links can be attached in the same
    // statement: RETURNING cannot carry the movement id back out of the row INSERT.
    //
    // Direction mapping. 'incoming' is income (+amount on the wallet account against
    // income_clearing), 'outgoing' and 'fee' are expenses (-amount against
    // expense_clearing) and differ only in wording. 'internal' is a transfer between two
    // addresses the user already owns: both sides are synced separately, so booking it
    // as money would count it twice — it becomes a zero-amount note row, which is also
    // the only kind the postings guard allows to have no postings at all. A movement of
    // amount zero in any other direction gets no row either, because postings carry
    // CHECK (signed_amount <> 0); the movement itself is still recorded.
    const auto created_rows = co_await transaction->execSqlCoro(R"SQL(
WITH input AS (
    SELECT DISTINCT m.mkey FROM jsonb_to_recordset($1::jsonb) AS m(mkey text)
), rule AS (
    SELECT DISTINCT ON (r.contract)
           r.contract AS asset_key,
           NULLIF(btrim(r."exchangeRate"), '')::numeric AS rate,
           NULLIF(btrim(r."accountId"), '') AS account_public_id
      FROM jsonb_to_recordset($5::jsonb) AS r(contract text, "exchangeRate" text, "accountId" text)
     WHERE NULLIF(btrim(r."exchangeRate"), '') IS NOT NULL
     ORDER BY r.contract
), base AS (
    SELECT a.id, a.decimals FROM assets a WHERE a.ledger_id = $2 AND a.is_default
), prepared AS (
    -- A converted movement moves to the default asset at amount * rate, rounded to that
    -- asset's scale, and lands on the account the rule names. That account must be a live
    -- user account on the default asset; anything else (deleted, archived, another asset)
    -- falls back to the unallocated account rather than failing the whole sync.
    SELECT mv.id AS movement_id,
           mv.direction,
           mv.occurred_at,
           ast.symbol,
           mv.amount AS chain_amount,
           ru.rate,
           CASE WHEN ru.rate IS NULL THEN mv.asset_id ELSE base.id END AS asset_id,
           CASE WHEN ru.rate IS NULL THEN mv.amount
                ELSE round(mv.amount * ru.rate, base.decimals) END AS amount,
           CASE WHEN ru.rate IS NULL THEN wa.account_id
                ELSE COALESCE(chosen.id, unallocated.id) END AS account_id
      FROM chain_asset_movements mv
      JOIN input i ON i.mkey = mv.movement_key
      JOIN assets ast ON ast.id = mv.asset_id
      CROSS JOIN base
      LEFT JOIN rule ru ON ru.asset_key = COALESCE(ast.contract_address_normalized, 'native')
      LEFT JOIN wallet_asset_accounts wa ON wa.wallet_id = mv.wallet_id AND wa.asset_id = mv.asset_id
      LEFT JOIN accounts chosen
        ON chosen.public_id::text = ru.account_public_id
       AND chosen.ledger_id = mv.ledger_id AND chosen.asset_id = base.id
       AND chosen.system_code IS NULL
       AND chosen.archived_at IS NULL AND chosen.deleted_at IS NULL
      LEFT JOIN accounts unallocated
        ON unallocated.ledger_id = mv.ledger_id AND unallocated.asset_id = base.id
       AND unallocated.system_code = 'unallocated'
     WHERE mv.ledger_id = $2 AND mv.wallet_id = $3
       AND (mv.direction = 'internal' OR mv.amount > 0)
       AND NOT EXISTS (SELECT 1 FROM chain_movement_row_links k WHERE k.movement_id = mv.id)
), target AS MATERIALIZED (
    -- Dust that rounds to zero after conversion gets no row: postings cannot be zero.
    SELECT nextval(pg_get_serial_sequence('journal_rows', 'id')) AS row_id,
           p.movement_id,
           p.asset_id,
           CASE WHEN p.direction = 'internal' THEN 'note' ELSE 'entry' END AS kind,
           CASE WHEN p.direction = 'internal' THEN 0
                WHEN p.direction = 'incoming' THEN p.amount
                ELSE -p.amount END AS signed_amount,
           COALESCE((p.occurred_at AT TIME ZONE 'UTC')::date, CURRENT_DATE) AS occurred_on,
           CASE p.direction
                WHEN 'incoming' THEN '链上收款 · '
                WHEN 'outgoing' THEN '链上转出 · '
                WHEN 'fee' THEN '链上手续费 · '
                ELSE '内部转账 · ' END
             || CASE WHEN p.rate IS NULL THEN p.symbol
                     ELSE trim_scale(p.chain_amount)::text || ' ' || p.symbol
                          || ' × ' || trim_scale(p.rate)::text END AS description,
           p.account_id AS wallet_account_id,
           CASE WHEN p.direction = 'internal' THEN NULL ELSE cat.id END AS category_id,
           clearing.id AS clearing_account_id
      FROM prepared p
      LEFT JOIN accounts clearing
        ON clearing.ledger_id = $2 AND clearing.asset_id = p.asset_id
       AND clearing.system_code = CASE WHEN p.direction = 'incoming'
                                       THEN 'income_clearing' ELSE 'expense_clearing' END
      LEFT JOIN categories cat
        ON cat.ledger_id = $2
       AND cat.system_code = CASE WHEN p.direction = 'incoming'
                                  THEN 'unallocated_income' ELSE 'unallocated_expense' END
     WHERE p.account_id IS NOT NULL
       AND (p.direction = 'internal' OR p.amount > 0)
), new_rows AS (
    INSERT INTO journal_rows(id, ledger_id, occurred_on, description, kind, amount,
                             account_id, category_id, transfer_account_id, created_by, asset_id)
    OVERRIDING SYSTEM VALUE
    SELECT t.row_id, $2, t.occurred_on, t.description, t.kind, t.signed_amount,
           t.wallet_account_id, t.category_id, NULL::bigint, $4, t.asset_id
      FROM target t
    RETURNING id
), new_postings AS (
    INSERT INTO postings(row_id, account_id, category_id, signed_amount, line_number, asset_id)
    SELECT t.row_id, t.wallet_account_id, t.category_id, t.signed_amount, 1, t.asset_id
      FROM target t WHERE t.kind <> 'note'
     UNION ALL
    SELECT t.row_id, t.clearing_account_id, t.category_id, -t.signed_amount, 2, t.asset_id
      FROM target t WHERE t.kind <> 'note'
    RETURNING id
)
INSERT INTO chain_movement_row_links(movement_id, row_id)
SELECT t.movement_id, t.row_id FROM target t
RETURNING movement_id
)SQL", movement_json, ledger_id, wallet_id, user_id, rules_json);

    // postings_balance_guard is DEFERRABLE INITIALLY DEFERRED, so an unbalanced write
    // would otherwise only fail at COMMIT — after this coroutine has already returned
    // its counts and journal_service has reported success. Forcing it here turns that
    // into an exception the caller can still see, and the rollback is honest.
    co_await transaction->execSqlCoro("SET CONSTRAINTS postings_balance_guard IMMEDIATE");

    co_await transaction->execSqlCoro(R"SQL(
UPDATE wallet_accounts SET last_synced_at=clock_timestamp(), last_error=NULL WHERE id=$1
)SQL", wallet_id);

    // Checkpoints only move forward: GREATEST ignores NULLs, so a window that returned
    // nothing leaves the previous position untouched. checkpoint_signature is the hash
    // of the newest transaction in this window and only replaces the stored one when
    // that window actually reaches at least as far as the stored checkpoint (Solana
    // resumes from a signature rather than from a block number).
    co_await transaction->execSqlCoro(R"SQL(
WITH input AS (
    SELECT t.hash, t.block,
           CASE WHEN t.ts_ms IS NULL THEN NULL
                ELSE 'epoch'::timestamptz + t.ts_ms * interval '1 millisecond' END AS occurred_at
      FROM jsonb_to_recordset($1::jsonb) AS t(hash text, block bigint, ts_ms bigint)
), newest AS (
    SELECT i.hash, i.occurred_at FROM input i
     ORDER BY i.occurred_at DESC NULLS LAST, i.block DESC NULLS LAST, i.hash DESC
     LIMIT 1
)
UPDATE wallet_sync_states s
   SET status = 'idle',
       checkpoint_block = GREATEST(s.checkpoint_block, (SELECT max(i.block) FROM input i)),
       checkpoint_timestamp = GREATEST(s.checkpoint_timestamp,
                                       (SELECT max(i.occurred_at) FROM input i)),
       checkpoint_signature = CASE
           WHEN (SELECT n.occurred_at FROM newest n) IS NOT NULL
                AND (s.checkpoint_timestamp IS NULL
                     OR (SELECT n.occurred_at FROM newest n) >= s.checkpoint_timestamp)
           THEN (SELECT n.hash FROM newest n)
           ELSE s.checkpoint_signature END,
       last_completed_at = clock_timestamp(),
       last_error = NULL
 WHERE s.wallet_id = $2
)SQL", checkpoint_json, wallet_id);

    co_return SyncWriteStats{
        .transactionsSeen = static_cast<std::int64_t>(upserted_transactions.size()),
        .movementsCreated = static_cast<std::int64_t>(created_movements.size()),
        .rowsCreated = static_cast<std::int64_t>(created_rows.size()),
    };
}

drogon::Task<application::JobView> PostgresRepository::create_wallet_sync_job(
    std::string_view wallet_public_id,
    std::string_view status,
    std::int64_t done,
    std::int64_t total,
    const std::optional<std::string> &error) const {

    // background_jobs.error is JSONB, so a bare message has to be wrapped; the
    // ledger is resolved from the wallet so jobs stay attributable and cascade
    // with their ledger.
    const auto rows = co_await client_->execSqlCoro(R"SQL(
INSERT INTO background_jobs (ledger_id, kind, status, progress_done, progress_total, error)
VALUES ((SELECT w.ledger_id FROM wallet_accounts w WHERE w.public_id = $1::uuid),
        'wallet_sync', $2, $3, $4,
        CASE WHEN $5::text IS NULL THEN NULL ELSE jsonb_build_object('detail', $5::text) END)
RETURNING public_id::text AS id, kind, status, progress_done, progress_total,
          error->>'detail' AS error,
          to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at
)SQL", std::string(wallet_public_id), std::string(status), done, total, error);

    if (rows.empty()) throw std::runtime_error("failed to create job");
    const auto &row = rows[0];

    std::optional<std::map<std::string, std::string>> errorMap;
    if (!row["error"].isNull() && !row["error"].as<std::string>().empty()) {
        errorMap = std::map<std::string, std::string>{{"detail", row["error"].as<std::string>()}};
    }

    co_return application::JobView{
        .id = row["id"].as<std::string>(),
        .kind = row["kind"].as<std::string>(),
        .status = row["status"].as<std::string>(),
        .done = row["progress_done"].as<std::int64_t>(),
        .total = row["progress_total"].as<std::int64_t>(),
        .error = std::move(errorMap),
        .createdAt = row["created_at"].as<std::string>(),
    };
}

drogon::Task<std::optional<std::int64_t>> PostgresRepository::first_admin_user_id() const {
    const auto rows = co_await client_->execSqlCoro(R"SQL(
SELECT id FROM app_users WHERE role='admin' ORDER BY id LIMIT 1
)SQL");
    if (rows.empty()) co_return std::nullopt;
    co_return rows[0]["id"].as<std::int64_t>();
}

drogon::Task<std::vector<application::ChainTransactionView>> PostgresRepository::list_chain_transactions(
    std::string_view ledger_public_id,
    std::uint16_t limit) const {

    // One view item per asset movement: direction, amount and the two address
    // sides only exist at movement level. Transactions without movements carry
    // none of the required fields, so they are not reported.
    const auto query = std::string(R"SQL(
SELECT m.public_id::text AS id,
       ct.tx_hash,
       to_char(ct.block_timestamp AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS block_timestamp,
       cn.code AS chain,
       cn.name AS chain_name,
       ast.public_id::text AS asset_id,
       ast.symbol AS asset_symbol,
       ast.decimals AS asset_decimals,
       m.direction,
       round(m.amount, ast.decimals)::text AS amount,
       m.from_address,
       origin_label.label_id AS from_label_id,
       origin_label.display_name AS from_display_name,
       origin_label.kind AS from_kind,
       m.to_address,
       target_label.label_id AS to_label_id,
       target_label.display_name AS to_display_name,
       target_label.kind AS to_kind,
       jr.public_id::text AS row_id,
       jr.description AS row_description
  FROM chain_asset_movements m
  JOIN chain_transactions ct ON ct.id = m.chain_transaction_id
  JOIN ledgers l ON l.id = m.ledger_id
  JOIN chain_networks cn ON cn.id = ct.chain_network_id
  JOIN assets ast ON ast.id = m.asset_id
  LEFT JOIN chain_movement_row_links ml ON ml.movement_id = m.id
  LEFT JOIN journal_rows jr ON jr.id = ml.row_id)SQL") +
                       std::string(kChainAddressLabelJoins) + R"SQL(
 WHERE l.public_id = $1::uuid
 ORDER BY m.occurred_at DESC NULLS LAST, ct.id DESC, m.id DESC
 LIMIT $2
)SQL";
    const auto rows = co_await client_->execSqlCoro(
        query, std::string(ledger_public_id), static_cast<std::int64_t>(limit));

    std::vector<application::ChainTransactionView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) {
        auto tx_hash = row["tx_hash"].as<std::string>();
        auto tx_hash_short = domain::chain::short_address(tx_hash);
        result.push_back(application::ChainTransactionView{
            .id = row["id"].as<std::string>(),
            .txHash = std::move(tx_hash),
            .txHashShort = std::move(tx_hash_short),
            .blockTimestamp = optional_string(row, "block_timestamp"),
            .chain = row["chain"].as<std::string>(),
            .chainName = row["chain_name"].as<std::string>(),
            .assetId = row["asset_id"].as<std::string>(),
            .assetSymbol = row["asset_symbol"].as<std::string>(),
            .assetDecimals = row["asset_decimals"].as<std::int16_t>(),
            .direction = row["direction"].as<std::string>(),
            .amount = row["amount"].as<std::string>(),
            .origin = chain_address_from_row(row, "from_address", "from_label_id",
                                             "from_display_name", "from_kind"),
            .target = chain_address_from_row(row, "to_address", "to_label_id",
                                             "to_display_name", "to_kind"),
            .rowId = optional_string(row, "row_id"),
            .rowDescription = optional_string(row, "row_description"),
        });
    }
    co_return result;
}

drogon::Task<std::vector<application::AddressLabelView>> PostgresRepository::list_address_labels(
    std::string_view ledger_public_id) const {

    const auto rows = co_await client_->execSqlCoro(std::string(kAddressLabelSelect) + R"SQL(
 WHERE l.public_id = $1::uuid
 ORDER BY al.kind, lower(al.display_name), al.id
)SQL", std::string(ledger_public_id));

    std::vector<application::AddressLabelView> result;
    result.reserve(rows.size());
    for (const auto &row : rows) result.push_back(address_label_from_row(row));
    co_return result;
}

drogon::Task<application::AddressLabelView> PostgresRepository::create_address_label(
    std::string_view ledger_public_id,
    const application::AddressLabelInput &input,
    std::string_view normalized_address) const {

    // The chain network is resolved up front so an unknown chain code fails as a
    // clean "chain not found" instead of inserting chain_network_id=NULL with
    // scope='chain' and tripping wallet_address_labels_scope_shape_check.
    const auto rows = co_await client_->execSqlCoro(R"SQL(
WITH target AS (
    SELECT l.id AS ledger_id, l.public_id AS ledger_public_id,
           cn.id AS chain_network_id,
           COALESCE(cn.code, 'evm') AS chain_code,
           COALESCE(cn.name, 'EVM') AS chain_name,
           CASE WHEN cn.id IS NULL THEN 'evm' ELSE 'chain' END AS scope
      FROM ledgers l
      LEFT JOIN chain_networks cn ON cn.code = $7
     WHERE l.public_id = $1::uuid AND l.archived_at IS NULL
       AND (cn.id IS NOT NULL OR $7 = 'evm')
), inserted AS (
    INSERT INTO wallet_address_labels (ledger_id, chain_network_id, address, address_normalized,
                                       display_name, kind, note, scope)
    SELECT t.ledger_id, t.chain_network_id, $2, $3, $4, $5, $6, t.scope FROM target t
    ON CONFLICT DO NOTHING
    RETURNING *
)
SELECT i.public_id::text AS id,
       t.ledger_public_id::text AS ledger_id,
       t.chain_code AS chain,
       t.chain_name AS chain_name,
       i.scope,
       i.address,
       i.display_name,
       i.kind,
       i.note,
       to_char(i.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS updated_at
  FROM inserted i CROSS JOIN target t
)SQL",
        std::string(ledger_public_id),
        input.address,
        std::string(normalized_address),
        input.displayName,
        input.kind,
        input.note,
        input.chain);

    // 这里的 ON CONFLICT 不点名仲裁索引：作用域是 SQL 里算出来的，scope='chain' 归
    // wallet_address_labels_chain_unique 管，scope='evm' 归 wallet_address_labels_evm_unique
    // 管，一条语句选不出唯一的仲裁索引。这张表上除这两个之外只剩 public_id 的唯一约束
    // （uuidv7 默认值，撞不上），所以裸的 DO NOTHING 与点名两者等价。
    if (rows.empty()) {
        const auto ledger = co_await client_->execSqlCoro(
            "SELECT 1 FROM ledgers WHERE public_id=$1::uuid AND archived_at IS NULL",
            std::string(ledger_public_id));
        if (ledger.empty()) throw application::EntityNotFound("ledger");
        if (input.chain != "evm") {
            const auto chain = co_await client_->execSqlCoro(
                "SELECT 1 FROM chain_networks WHERE code=$1", input.chain);
            if (chain.empty()) throw std::runtime_error("chain not found");
        }
        throw DuplicateValue("address_label", "address");
    }
    co_return address_label_from_row(rows[0]);
}

drogon::Task<application::AddressLabelView> PostgresRepository::update_address_label(
    std::string_view label_public_id,
    const application::AddressLabelPatch &patch) const {

    // Fixed statement: an omitted patch field binds NULL and keeps its value, so
    // adding a field cannot leave a placeholder unbound.
    const auto updated = co_await client_->execSqlCoro(R"SQL(
UPDATE wallet_address_labels
   SET display_name = COALESCE($2::text, display_name),
       kind = COALESCE($3::text, kind),
       note = COALESCE($4::text, note)
 WHERE public_id=$1::uuid
RETURNING id
)SQL", std::string(label_public_id), patch.displayName, patch.kind, patch.note);
    if (updated.empty()) throw application::EntityNotFound("address_label");

    const auto rows = co_await client_->execSqlCoro(std::string(kAddressLabelSelect) + R"SQL(
 WHERE al.public_id = $1::uuid
)SQL", std::string(label_public_id));

    if (rows.empty()) throw application::EntityNotFound("address_label");
    co_return address_label_from_row(rows[0]);
}

drogon::Task<> PostgresRepository::delete_address_label(std::string_view label_public_id) const {
    const auto result = co_await client_->execSqlCoro(R"SQL(
DELETE FROM wallet_address_labels WHERE public_id=$1::uuid
)SQL", std::string(label_public_id));
    if (result.affectedRows() == 0) throw application::EntityNotFound("address_label");
}

// COALESCE 的三段就是资产解析规则本身，顺序不可交换：
//   1. $3 —— 本行最终指向的账户的资产。账户是唯一被 journal_rows_link_guard 拿来跟
//      journal_rows.asset_id 逐一比对的东西，所以只要有账户，资产就由它说了算；
//   2. $2 —— 已有行自己的资产。只有 note 行会走到这里（它没有账户），改说明不会挪动资产；
//   3. 账本默认资产。新建的 note 行没有任何东西钉住资产，用默认资产；此时
//      account_id / transfer_account_id 均为 NULL，触发器不检查任何链接，成立。
// 于是“更新时保持原资产，除非账户变了”是这条规则的推论，而不是额外的分支：账户没变则
// 第 1 段给出同一个资产，账户换到别的资产上则行跟着搬过去（UPDATE 会一并写 asset_id）。
drogon::Task<AssetScaleRecord> PostgresRepository::row_asset(
    const drogon::orm::DbClientPtr &transaction,
    std::int64_t ledger_id,
    std::optional<std::int64_t> existing_row_id,
    std::optional<std::int64_t> account_id) const {
    const auto rows = co_await transaction->execSqlCoro(R"SQL(
SELECT ast.id, ast.decimals, ast.symbol
  FROM assets ast
 WHERE ast.id = COALESCE(
           (SELECT a.asset_id FROM accounts a WHERE a.id = $3),
           (SELECT r.asset_id FROM journal_rows r WHERE r.id = $2),
           (SELECT d.id FROM assets d WHERE d.ledger_id = $1 AND d.is_default))
)SQL", ledger_id, existing_row_id, account_id);
    if (rows.empty()) throw std::runtime_error("default asset not found");
    co_return AssetScaleRecord{
        .id = rows[0]["id"].as<std::int64_t>(),
        .decimals = rows[0]["decimals"].as<std::int16_t>(),
        .symbol = rows[0]["symbol"].as<std::string>(),
    };
}

// 结转账户是按资产分的，命名与冲突键都沿用 record_wallet_sync 里那套：名字是
// '收入结转 · <symbol>'，冲突键是 accounts 的 (ledger_id, system_code, asset_id) 唯一约束。
// 因此链上同步先建过的结转账户会被这里直接复用，而手工在一个还没同步过的资产上记一笔，
// 也能就地把缺的那一个补出来，而不是像以前那样退回默认资产的结转账户、再被
// check_row_postings_balanced 判成 500。
//
// 建不出来只剩一种可能：并发事务刚插入了同一行、ON CONFLICT DO NOTHING 什么都没返回。
// 重新读一次即可（READ COMMITTED 下新语句取新快照）；还读不到才是真的坏了，留在 500。
drogon::Task<std::int64_t> PostgresRepository::clearing_account(
    const drogon::orm::DbClientPtr &transaction,
    std::int64_t ledger_id,
    std::int64_t asset_id,
    bool income) const {
    const std::string system_code = income ? "income_clearing" : "expense_clearing";
    constexpr std::string_view kLookup = R"SQL(
SELECT id FROM accounts WHERE ledger_id=$1 AND system_code=$2 AND asset_id=$3
)SQL";

    const auto existing = co_await transaction->execSqlCoro(std::string(kLookup),
                                                            ledger_id, system_code, asset_id);
    if (!existing.empty()) co_return existing.front()["id"].as<std::int64_t>();

    const auto created = co_await transaction->execSqlCoro(R"SQL(
INSERT INTO accounts(ledger_id, name, system_code, asset_id)
SELECT $1, $4 || ' · ' || ast.symbol, $2, ast.id
  FROM assets ast
 WHERE ast.id = $3 AND ast.ledger_id = $1
ON CONFLICT (ledger_id, system_code, asset_id) DO NOTHING
RETURNING id
)SQL", ledger_id, system_code, asset_id, std::string(income ? "收入结转" : "支出结转"));
    if (!created.empty()) co_return created.front()["id"].as<std::int64_t>();

    const auto retried = co_await transaction->execSqlCoro(std::string(kLookup),
                                                           ledger_id, system_code, asset_id);
    if (retried.empty()) throw std::runtime_error("ledger clearing account missing");
    co_return retried.front()["id"].as<std::int64_t>();
}

}  // namespace journalseed::infrastructure
