#pragma once

#include "journalseed/lua/function_registry.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace journalseed::application {

using CellValue = std::optional<std::variant<bool, std::string>>;
using CellMap = std::map<std::string, CellValue>;

struct SetupRequest {
    std::string username;
    std::string password;
    std::string ledgerName;
};

struct LoginRequest {
    std::string username;
    std::string password;
};

struct SetupStatus {
    bool required;
};

struct UserView {
    std::string id;
    std::string username;
};

struct SessionView {
    UserView user;
    std::string csrfToken;
    std::string expiresAt;
};

struct SessionEnvelope {
    SessionView session;
    std::string cookieToken;
};

struct AuthContext {
    std::int64_t userId;
    std::string userPublicId;
    std::string username;
};

struct LedgerView {
    std::string id;
    std::string name;
    std::string createdAt;
};

struct CreateLedgerRequest {
    std::string name;
};

struct LedgerSummaryView {
    std::string balance;
    std::string income;
    std::string expense;
    std::int64_t rowCount;
};

struct AccountInput {
    std::string name;
    std::string openingBalance;
};

struct AccountView {
    std::string id;
    std::string name;
    std::string openingBalance;
    std::string balance;
    bool archived;
};

struct CategoryInput {
    std::string name;
    std::string direction;
};

struct CategoryView {
    std::string id;
    std::string name;
    std::string direction;
    bool archived;
};

struct ColumnInput {
    std::string name;
    std::string type;
    std::optional<std::int16_t> decimalPlaces;
    std::optional<std::string> formulaSource;
    std::optional<std::string> formulaResultType;
    std::vector<std::string> formulaDependencies;
};

struct ColumnPatch {
    std::optional<std::string> name;
    std::optional<std::int32_t> position;
    std::optional<std::int32_t> width;
};

struct ColumnView {
    std::string id;
    std::string name;
    std::string type;
    std::optional<std::string> system;
    std::int32_t position;
    std::int32_t width;
    std::optional<std::int16_t> decimalPlaces;
    std::optional<std::string> formulaSource;
    std::optional<std::string> formulaResultType;
    std::vector<std::string> formulaDependencies;
    bool recycled;
};

struct RowInput {
    std::string date;
    std::string description;
    std::string kind;
    std::string amount;
    std::optional<std::string> accountId;
    std::optional<std::string> categoryId;
    std::optional<std::string> transferAccountId;
    CellMap cells;
};

struct JournalRowView {
    std::string id;
    std::string date;
    std::string description;
    std::string kind;
    std::string amount;
    std::optional<std::string> accountId;
    std::optional<std::string> accountName;
    std::optional<std::string> categoryId;
    std::optional<std::string> categoryName;
    std::optional<std::string> transferAccountId;
    std::optional<std::string> transferAccountName;
    CellMap cells;
    std::int64_t revision;
    std::string createdAt;
    std::string updatedAt;
};

struct RowPage {
    std::vector<JournalRowView> items;
    std::optional<std::string> nextCursor;
    bool hasMore;
};

struct RowQuery {
    std::uint16_t limit{100};
    std::string sortKey{"date"};
    bool ascending{false};
    bool recycled{false};
    std::optional<std::string> cursorValue;
    std::optional<std::string> cursorId;
};

struct CursorData {
    std::string sortKey;
    bool ascending;
    std::string value;
    std::string id;
};

struct JobView {
    std::string id;
    std::string kind;
    std::string status;
    std::int64_t done;
    std::int64_t total;
    std::optional<std::map<std::string, std::string>> error;
    std::string createdAt;
};

struct LuaParameterView {
    std::string name;
    std::string type;
    std::string label;
    bool required;
    std::vector<std::string> options;
};

struct LuaFunctionView {
    std::string name;
    std::string version;
    std::string description;
    std::string script;
    std::string source;
    std::vector<LuaParameterView> params;
};

struct LuaFunctionInput {
    std::string source;
};

struct Problem {
    std::string type;
    std::string title;
    std::uint16_t status;
    std::string code;
    std::string detail;
    std::map<std::string, std::string> fields;
};

}  // namespace journalseed::application
