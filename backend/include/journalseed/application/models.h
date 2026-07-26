#pragma once

#include "journalseed/lua/function_registry.h"

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace journalseed::application {

// 仓储层用它表示“目标实体不存在”，与真正的数据库故障区分开：服务层把它翻译成
// 404 与 <entity>_not_found，并且不写 ERROR 日志；其余异常仍然是 500 database_error。
// entity 取受支持的英文实体名：ledger、account、category、column、row、job、wallet、
// address_label；未登记的名字会退化成通用的“对象不存在”文案。
// 它住在 models.h 而不是 journal_service.h：基础设施层需要抛出它，让仓储去 include
// 应用服务的头文件是分层倒置，而 models.h 本来就是两层共享的契约。
class EntityNotFound final : public std::runtime_error {
  public:
    explicit EntityNotFound(std::string entity)
        : std::runtime_error(entity + " not found"), entity_(std::move(entity)) {}

    [[nodiscard]] const std::string &entity() const noexcept { return entity_; }

  private:
    std::string entity_;
};

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

// One line per asset the ledger holds. Every amount is an exact decimal string rendered at
// `decimals`, this asset's own scale.
struct AssetSummaryView {
    std::string assetId;
    std::string symbol;
    std::string name;
    std::int16_t decimals;
    std::string balance;
    std::string income;
    std::string expense;
};

// balance, income and expense describe the ledger's DEFAULT ASSET ONLY, at that asset's
// scale. Amounts in different assets are not addable, so a single figure spanning them all
// would be meaningless; every other asset is reported in `assetSummaries`, which always
// includes the default asset as its first entry. rowCount counts rows across all assets —
// a count is commensurable even when the amounts are not.
struct LedgerSummaryView {
    std::string balance;
    std::string income;
    std::string expense;
    std::int64_t rowCount;
    std::vector<AssetSummaryView> assetSummaries;
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
    std::string assetId;
    std::string assetSymbol;
    std::int16_t assetDecimals;
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

struct ChainAddressView {
    std::string address;
    std::string addressShort;
    std::optional<std::string> labelId;
    std::optional<std::string> displayName;
    std::optional<std::string> kind;
};

struct ChainSourceView {
    std::string txHash;
    std::string txHashShort;
    std::string chain;
    std::string chainName;
    std::string direction;
    std::string assetSymbol;
    std::int16_t assetDecimals;
    ChainAddressView origin;
    ChainAddressView target;
};

struct JournalRowView {
    std::string id;
    std::string date;
    std::string description;
    std::string kind;
    std::string amount;
    std::string assetId;
    std::string assetSymbol;
    std::int16_t assetDecimals;
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
    std::optional<ChainSourceView> chainSource;
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

struct ChainSettingsView {
    bool tronGridApiKeyConfigured;
    bool etherscanApiKeyConfigured;
    std::int32_t syncIntervalMinutes;
    std::string tronGridEndpoint;
    bool ethereumRpcUrlConfigured;
    std::string ethereumRpcEndpoint;
    bool polygonRpcUrlConfigured;
    std::string polygonRpcEndpoint;
    bool solanaRpcUrlConfigured;
    std::string solanaRpcEndpoint;
};

struct ChainSettingsPatch {
    std::optional<std::string> tronGridApiKey;
    std::optional<bool> clearTronGridApiKey;
    std::optional<std::string> etherscanApiKey;
    std::optional<bool> clearEtherscanApiKey;
    std::optional<std::string> ethereumRpcUrl;
    std::optional<bool> clearEthereumRpcUrl;
    std::optional<std::string> polygonRpcUrl;
    std::optional<bool> clearPolygonRpcUrl;
    std::optional<std::string> solanaRpcUrl;
    std::optional<bool> clearSolanaRpcUrl;
    std::optional<std::int32_t> syncIntervalMinutes;
};

struct WalletInput {
    std::string chain{"tron-mainnet"};
    std::string name;
    std::string address;
    bool enabled{true};
    bool autoSync{true};
};

struct WalletPatch {
    std::optional<std::string> name;
    std::optional<bool> enabled;
    std::optional<bool> autoSync;
};

struct WalletView {
    std::string id;
    std::string ledgerId;
    std::string chain;
    std::string chainName;
    std::string name;
    std::string address;
    std::string addressShort;
    bool enabled;
    bool autoSync;
    std::optional<std::string> lastSyncedAt;
    std::optional<std::string> lastError;
    std::string syncStatus;
    std::optional<std::string> createdAt;
};

struct AddressLabelInput {
    std::string chain{"tron-mainnet"};
    std::string address;
    std::string displayName;
    std::string kind;
    std::string note;
};

struct AddressLabelPatch {
    std::optional<std::string> displayName;
    std::optional<std::string> kind;
    std::optional<std::string> note;
};

struct AddressLabelView {
    std::string id;
    std::string ledgerId;
    std::string chain;
    std::string chainName;
    std::string scope;
    std::string address;
    std::string addressShort;
    std::string displayName;
    std::string kind;
    std::string note;
    std::string updatedAt;
};

struct ChainTransactionView {
    std::string id;
    std::string txHash;
    std::string txHashShort;
    std::optional<std::string> blockTimestamp;
    std::string chain;
    std::string chainName;
    std::string assetId;
    std::string assetSymbol;
    std::int16_t assetDecimals;
    std::string direction;
    std::string amount;
    ChainAddressView origin;
    ChainAddressView target;
    std::optional<std::string> rowId;
    std::optional<std::string> rowDescription;
};

struct SyncResultView {
    JobView job;
    std::int64_t transactionsSeen;
    std::int64_t movementsCreated;
    std::int64_t rowsCreated;
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
