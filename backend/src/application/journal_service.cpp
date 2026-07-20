#include "journalseed/application/journal_service.h"

#include "journalseed/domain/journal_entry.h"
#include "journalseed/domain/money.h"

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <regex>
#include <stdexcept>
#include <utility>

namespace journalseed::application {
namespace {

std::string trimmed(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
                          return std::isspace(character) != 0;
                      }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

template <typename T>
ServiceResult<T> database_failure(const std::exception &exception) {
    return std::unexpected(Problem{
        .type = "/problems/database-error",
        .title = "数据库操作未完成",
        .status = 500,
        .code = "database_error",
        .detail = exception.what(),
        .fields = {},
    });
}

LuaFunctionView lua_function_view(const lua::FunctionMetadata &function) {
    LuaFunctionView view{
        .name = function.name,
        .version = function.version,
        .description = function.description,
        .script = function.script,
        .source = function.source,
        .params = {},
    };
    for (const auto &parameter : function.params) {
        view.params.push_back(LuaParameterView{
            .name = parameter.name,
            .type = parameter.type,
            .label = parameter.label,
            .required = parameter.required,
            .options = parameter.options,
        });
    }
    return view;
}

template <typename T>
ServiceResult<T> lua_failure(const lua::RegistryError &error) {
    std::uint16_t status = 422;
    std::string code = "lua_execution_error";
    if (error.code == lua::RegistryErrorCode::function_missing) {
        status = 404;
        code = "function_missing";
    } else if (error.code == lua::RegistryErrorCode::directory_error ||
               error.code == lua::RegistryErrorCode::write_error) {
        status = 500;
        code = "lua_write_error";
    } else if (error.code == lua::RegistryErrorCode::function_conflict) {
        status = 409;
        code = "function_conflict";
    } else if (error.code == lua::RegistryErrorCode::schema_error ||
               error.code == lua::RegistryErrorCode::script_error) {
        code = "lua_script_error";
    } else if (error.code == lua::RegistryErrorCode::limit_exceeded) {
        code = "lua_limit_exceeded";
    }
    return std::unexpected(Problem{
        .type = "/problems/" + code,
        .title = "函数脚本未保存",
        .status = status,
        .code = std::move(code),
        .detail = error.script.empty() ? error.message : error.script + ": " + error.message,
        .fields = {},
    });
}

}  // namespace

JournalService::JournalService(
    std::shared_ptr<infrastructure::PostgresRepository> repository,
    std::shared_ptr<lua::FunctionRegistry> functions)
    : repository_(std::move(repository)), functions_(std::move(functions)) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

Problem JournalService::problem(std::uint16_t status,
                                std::string code,
                                std::string title,
                                std::string detail) {
    return Problem{
        .type = "/problems/" + code,
        .title = std::move(title),
        .status = status,
        .code = std::move(code),
        .detail = std::move(detail),
        .fields = {},
    };
}

std::string JournalService::random_token() {
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING)>
        encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return encoded.data();
}

std::string JournalService::hash_token(std::string_view token) {
    std::array<unsigned char, crypto_generichash_BYTES> hash{};
    crypto_generichash(hash.data(), hash.size(),
                       reinterpret_cast<const unsigned char *>(token.data()), token.size(),
                       nullptr, 0);
    std::array<char, crypto_generichash_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return hex.data();
}

drogon::Task<ServiceResult<SetupStatus>> JournalService::setup_status() const {
    try {
        co_return SetupStatus{.required = co_await repository_->setup_required()};
    } catch (const std::exception &exception) {
        co_return database_failure<SetupStatus>(exception);
    }
}

drogon::Task<ServiceResult<SessionEnvelope>>
JournalService::setup(SetupRequest input) const {
    input.username = trimmed(std::move(input.username));
    input.ledgerName = trimmed(std::move(input.ledgerName));
    if (input.username.empty() || input.username.size() > 80) {
        auto issue = problem(422, "validation_error", "设置内容需要调整",
                             "管理员名称需要包含 1 到 80 个字符");
        issue.fields.emplace("username", "请填写管理员名称");
        co_return std::unexpected(std::move(issue));
    }
    if (input.password.size() < 10 || input.password.size() > 1024) {
        auto issue = problem(422, "validation_error", "设置内容需要调整",
                             "密码需要包含 10 到 1024 个字符");
        issue.fields.emplace("password", "密码至少需要 10 个字符");
        co_return std::unexpected(std::move(issue));
    }
    if (input.ledgerName.empty() || input.ledgerName.size() > 120) {
        auto issue = problem(422, "validation_error", "设置内容需要调整",
                             "账本名称需要包含 1 到 120 个字符");
        issue.fields.emplace("ledgerName", "请填写首个账本名称");
        co_return std::unexpected(std::move(issue));
    }

    std::array<char, crypto_pwhash_STRBYTES> password_hash{};
    if (crypto_pwhash_str_alg(password_hash.data(), input.password.data(), input.password.size(),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE,
                             crypto_pwhash_ALG_ARGON2ID13) != 0) {
        co_return std::unexpected(problem(500, "password_hash_error", "管理员创建失败",
                                          "密码哈希所需内存分配失败"));
    }

    const auto cookie_token = random_token();
    const auto csrf_token = random_token();
    try {
        const auto record = co_await repository_->create_initial_setup(
            input, password_hash.data(), hash_token(cookie_token), hash_token(csrf_token));
        co_return SessionEnvelope{
            .session = SessionView{
                .user = UserView{.id = record.userPublicId, .username = record.username},
                .csrfToken = csrf_token,
                .expiresAt = record.expiresAt,
            },
            .cookieToken = cookie_token,
        };
    } catch (const std::exception &) {
        co_return std::unexpected(problem(409, "setup_already_completed", "设置已经完成",
                                          "已有管理员时不会再次创建管理员"));
    }
}

drogon::Task<ServiceResult<SessionEnvelope>>
JournalService::login(LoginRequest input) const {
    input.username = trimmed(std::move(input.username));
    try {
        const auto user = co_await repository_->find_user(input.username);
        if (!user || crypto_pwhash_str_verify(user->passwordHash.c_str(), input.password.data(),
                                             input.password.size()) != 0) {
            co_return std::unexpected(problem(401, "invalid_credentials", "登录信息不匹配",
                                              "请检查管理员名称和密码"));
        }
        if (crypto_pwhash_str_needs_rehash(user->passwordHash.c_str(),
                                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            // Rehashing is intentionally deferred to a password-changing flow;
            // authentication remains constant in observable behavior here.
        }

        const auto cookie_token = random_token();
        const auto csrf_token = random_token();
        const auto session = co_await repository_->create_session(
            user->id, hash_token(cookie_token), hash_token(csrf_token));
        co_return SessionEnvelope{
            .session = SessionView{
                .user = UserView{.id = session.userPublicId, .username = session.username},
                .csrfToken = csrf_token,
                .expiresAt = session.expiresAt,
            },
            .cookieToken = cookie_token,
        };
    } catch (const std::exception &exception) {
        co_return database_failure<SessionEnvelope>(exception);
    }
}

drogon::Task<ServiceResult<AuthContext>>
JournalService::authenticate(std::string_view cookie_token,
                             std::optional<std::string_view> csrf_token) const {
    if (cookie_token.empty() || (csrf_token && csrf_token->empty())) {
        co_return std::unexpected(problem(401, "session_required", "需要登录",
                                          "当前会话不存在或已过期"));
    }
    try {
        std::optional<std::string> csrf_hash;
        if (csrf_token) csrf_hash = hash_token(*csrf_token);
        const auto session = co_await repository_->find_session(hash_token(cookie_token), csrf_hash);
        if (!session) {
            co_return std::unexpected(problem(csrf_token ? 403 : 401,
                                              csrf_token ? "csrf_mismatch" : "session_required",
                                              csrf_token ? "写入校验失败" : "需要登录",
                                              csrf_token ? "请刷新页面后再保存" : "当前会话不存在或已过期"));
        }
        co_return AuthContext{
            .userId = session->userId,
            .userPublicId = session->userPublicId,
            .username = session->username,
        };
    } catch (const std::exception &exception) {
        co_return database_failure<AuthContext>(exception);
    }
}

drogon::Task<ServiceResult<SessionView>>
JournalService::current_session(std::string_view cookie_token) const {
    if (cookie_token.empty()) {
        co_return std::unexpected(problem(401, "session_required", "需要登录",
                                          "当前会话不存在或已过期"));
    }
    const auto csrf_token = random_token();
    try {
        const auto session = co_await repository_->rotate_session_csrf(
            hash_token(cookie_token), hash_token(csrf_token));
        if (!session) {
            co_return std::unexpected(problem(401, "session_required", "需要登录",
                                              "当前会话不存在或已过期"));
        }
        co_return SessionView{
            .user = UserView{.id = session->userPublicId, .username = session->username},
            .csrfToken = csrf_token,
            .expiresAt = session->expiresAt,
        };
    } catch (const std::exception &exception) {
        co_return database_failure<SessionView>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::logout(std::string_view cookie_token) const {
    try {
        if (!cookie_token.empty()) co_await repository_->delete_session(hash_token(cookie_token));
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return database_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<LedgerView>>> JournalService::ledgers() const {
    try {
        co_return co_await repository_->list_ledgers();
    } catch (const std::exception &exception) {
        co_return database_failure<std::vector<LedgerView>>(exception);
    }
}

drogon::Task<ServiceResult<LedgerView>>
JournalService::create_ledger(CreateLedgerRequest input) const {
    input.name = trimmed(std::move(input.name));
    if (input.name.empty() || input.name.size() > 120) {
        co_return std::unexpected(problem(422, "validation_error", "账本名称需要调整",
                                          "账本名称需要包含 1 到 120 个字符"));
    }
    try {
        co_return co_await repository_->create_ledger(input.name);
    } catch (const std::exception &exception) {
        co_return database_failure<LedgerView>(exception);
    }
}

drogon::Task<ServiceResult<LedgerSummaryView>>
JournalService::summary(std::string_view ledger_id) const {
    try {
        co_return co_await repository_->ledger_summary(ledger_id);
    } catch (const std::exception &exception) {
        co_return database_failure<LedgerSummaryView>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<AccountView>>>
JournalService::accounts(std::string_view ledger_id) const {
    try {
        co_return co_await repository_->list_accounts(ledger_id);
    } catch (const std::exception &exception) {
        co_return database_failure<std::vector<AccountView>>(exception);
    }
}

drogon::Task<ServiceResult<AccountView>>
JournalService::create_account(std::string_view ledger_id, AccountInput input) const {
    input.name = trimmed(std::move(input.name));
    const auto money = domain::Money::parse(input.openingBalance);
    if (input.name.empty() || input.name.size() > 120 || !money) {
        co_return std::unexpected(problem(422, "validation_error", "账户内容需要调整",
                                          !money ? money.error().message
                                                 : "账户名称需要包含 1 到 120 个字符"));
    }
    input.openingBalance = money->to_string();
    try {
        co_return co_await repository_->create_account(ledger_id, input);
    } catch (const std::exception &exception) {
        co_return database_failure<AccountView>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<CategoryView>>>
JournalService::categories(std::string_view ledger_id) const {
    try {
        co_return co_await repository_->list_categories(ledger_id);
    } catch (const std::exception &exception) {
        co_return database_failure<std::vector<CategoryView>>(exception);
    }
}

drogon::Task<ServiceResult<CategoryView>>
JournalService::create_category(std::string_view ledger_id, CategoryInput input) const {
    input.name = trimmed(std::move(input.name));
    if (input.name.empty() || input.name.size() > 120 ||
        (input.direction != "income" && input.direction != "expense")) {
        co_return std::unexpected(problem(422, "validation_error", "分类内容需要调整",
                                          "分类需要有效名称和收入或支出方向"));
    }
    try {
        co_return co_await repository_->create_category(ledger_id, input);
    } catch (const std::exception &exception) {
        co_return database_failure<CategoryView>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<ColumnView>>>
JournalService::columns(std::string_view ledger_id, bool recycled) const {
    try {
        co_return co_await repository_->list_columns(ledger_id, recycled);
    } catch (const std::exception &exception) {
        co_return database_failure<std::vector<ColumnView>>(exception);
    }
}

drogon::Task<ServiceResult<ColumnView>>
JournalService::create_column(std::string_view ledger_id, ColumnInput input) const {
    input.name = trimmed(std::move(input.name));
    constexpr std::string_view valid_types[] = {
        "text", "number", "date", "boolean", "option", "relation", "formula"};
    const bool valid_type = std::ranges::find(valid_types, input.type) != std::end(valid_types);
    if (input.name.empty() || input.name.size() > 80 || !valid_type ||
        (input.type == "number" &&
         (!input.decimalPlaces || *input.decimalPlaces < 0 || *input.decimalPlaces > 18)) ||
        (input.type == "formula" && (!input.formulaSource || input.formulaSource->empty()))) {
        co_return std::unexpected(problem(422, "validation_error", "列定义需要调整",
                                          "请检查列名、数据类型及类型参数"));
    }
    try {
        co_return co_await repository_->create_column(ledger_id, input);
    } catch (const std::exception &exception) {
        co_return database_failure<ColumnView>(exception);
    }
}

drogon::Task<ServiceResult<ColumnView>>
JournalService::update_column(std::string_view column_id, ColumnPatch patch) const {
    if (patch.name) *patch.name = trimmed(std::move(*patch.name));
    if ((patch.name && (patch.name->empty() || patch.name->size() > 80)) ||
        (patch.position && *patch.position < 0) ||
        (patch.width && (*patch.width < 72 || *patch.width > 720))) {
        co_return std::unexpected(problem(422, "validation_error", "列设置需要调整",
                                          "列名、顺序或宽度超出允许范围"));
    }
    try {
        co_return co_await repository_->update_column(column_id, patch);
    } catch (const std::exception &exception) {
        co_return database_failure<ColumnView>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::recycle_column(std::string_view column_id) const {
    try {
        co_await repository_->recycle_column(column_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return database_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::restore_column(std::string_view column_id) const {
    try {
        co_await repository_->restore_column(column_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return database_failure<std::monostate>(exception);
    }
}

std::string JournalService::encode_cursor(const CursorData &cursor) {
    std::string json;
    const auto write_error = glz::write_json(cursor, json);
    if (write_error) throw std::runtime_error("cursor serialization failed");
    std::string encoded(sodium_base64_ENCODED_LEN(json.size(),
                                                  sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(encoded.data(), encoded.size(),
                      reinterpret_cast<const unsigned char *>(json.data()), json.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    encoded.resize(std::char_traits<char>::length(encoded.c_str()));
    return encoded;
}

std::expected<CursorData, Problem> JournalService::decode_cursor(std::string_view cursor) {
    std::string decoded(cursor.size(), '\0');
    std::size_t decoded_size = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char *>(decoded.data()), decoded.size(),
                          cursor.data(), cursor.size(), nullptr, &decoded_size, nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return std::unexpected(problem(422, "invalid_cursor", "分页位置无效",
                                       "请从第一页重新载入流水"));
    }
    decoded.resize(decoded_size);
    CursorData result;
    if (const auto read_error = glz::read_json(result, decoded); read_error) {
        return std::unexpected(problem(422, "invalid_cursor", "分页位置无效",
                                       "请从第一页重新载入流水"));
    }
    return result;
}

drogon::Task<ServiceResult<RowPage>>
JournalService::rows(std::string_view ledger_id,
                     std::uint16_t limit,
                     std::string_view sort,
                     bool recycled,
                     std::optional<std::string_view> cursor) const {
    RowQuery query;
    query.limit = std::clamp<std::uint16_t>(limit, 1, 250);
    query.recycled = recycled;
    const auto separator = sort.find(':');
    query.sortKey = std::string(sort.substr(0, separator));
    const auto direction = separator == std::string_view::npos ? "desc" : sort.substr(separator + 1);
    query.ascending = direction == "asc";
    if ((query.sortKey != "date" && query.sortKey != "amount") ||
        (direction != "asc" && direction != "desc")) {
        co_return std::unexpected(problem(422, "invalid_sort", "排序条件无效",
                                          "目前支持按日期或金额排序"));
    }
    if (cursor) {
        auto decoded = decode_cursor(*cursor);
        if (!decoded || decoded->sortKey != query.sortKey || decoded->ascending != query.ascending) {
            co_return std::unexpected(decoded ? problem(422, "invalid_cursor", "分页位置无效",
                                                        "分页位置与当前排序不一致")
                                              : decoded.error());
        }
        query.cursorValue = decoded->value;
        query.cursorId = decoded->id;
    }

    try {
        auto page = co_await repository_->list_rows(ledger_id, query);
        if (page.nextCursor) {
            const auto separator_at = page.nextCursor->rfind('|');
            if (separator_at == std::string::npos) throw std::runtime_error("invalid repository cursor");
            page.nextCursor = encode_cursor(CursorData{
                .sortKey = query.sortKey,
                .ascending = query.ascending,
                .value = page.nextCursor->substr(0, separator_at),
                .id = page.nextCursor->substr(separator_at + 1),
            });
        }
        co_return page;
    } catch (const std::exception &exception) {
        co_return database_failure<RowPage>(exception);
    }
}

std::expected<RowInput, Problem> JournalService::validate_row(RowInput input) {
    static const std::regex date_pattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (!std::regex_match(input.date, date_pattern) || input.description.size() > 4000) {
        return std::unexpected(problem(422, "validation_error", "流水内容需要调整",
                                       "日期或说明格式不正确"));
    }
    const auto amount = domain::Money::parse(input.amount);
    if (!amount) {
        return std::unexpected(problem(422, "invalid_amount", "金额格式不正确",
                                       amount.error().message));
    }
    input.amount = amount->to_string();

    domain::JournalDraft draft{.amount = *amount};
    domain::PostingAccounts posting_accounts{
        .primary = domain::AccountId{1},
        .counterparty = domain::AccountId{2},
        .transfer_destination = std::nullopt,
    };
    if (input.kind == "note") {
        draft.kind = domain::TransactionKind::note;
        input.accountId.reset();
        input.categoryId.reset();
        input.transferAccountId.reset();
    } else if (input.kind == "transfer") {
        draft.kind = domain::TransactionKind::transfer;
        if (!input.accountId || !input.transferAccountId || *input.accountId == *input.transferAccountId) {
            return std::unexpected(problem(422, "invalid_transfer", "转账内容需要调整",
                                           "请选择两个不同的账户"));
        }
        posting_accounts.transfer_destination = domain::AccountId{3};
        input.categoryId.reset();
    } else if (input.kind == "entry") {
        draft.kind = domain::TransactionKind::entry;
        input.transferAccountId.reset();
    } else {
        return std::unexpected(problem(422, "invalid_kind", "流水类型无效",
                                       "请选择收支、转账或备注"));
    }
    const auto postings = domain::build_postings(draft, posting_accounts);
    if (!postings) {
        return std::unexpected(problem(422, "invalid_postings", "流水内容需要调整",
                                       postings.error().message));
    }
    return input;
}

drogon::Task<ServiceResult<JournalRowView>>
JournalService::create_row(std::string_view ledger_id,
                           std::int64_t user_id,
                           RowInput input) const {
    auto validated = validate_row(std::move(input));
    if (!validated) co_return std::unexpected(validated.error());
    try {
        co_return co_await repository_->create_row(ledger_id, user_id, *validated);
    } catch (const std::exception &exception) {
        co_return database_failure<JournalRowView>(exception);
    }
}

drogon::Task<ServiceResult<JournalRowView>>
JournalService::update_row(std::string_view row_id,
                           std::int64_t user_id,
                           RowInput input) const {
    auto validated = validate_row(std::move(input));
    if (!validated) co_return std::unexpected(validated.error());
    try {
        co_return co_await repository_->update_row(row_id, user_id, *validated);
    } catch (const std::exception &exception) {
        co_return database_failure<JournalRowView>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::recycle_row(std::string_view row_id) const {
    try {
        co_await repository_->recycle_row(row_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return database_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::restore_row(std::string_view row_id) const {
    try {
        co_await repository_->restore_row(row_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return database_failure<std::monostate>(exception);
    }
}

std::vector<LuaFunctionView> JournalService::functions() const {
    std::vector<LuaFunctionView> result;
    for (const auto &function : functions_->list()) {
        result.push_back(lua_function_view(function));
    }
    return result;
}

ServiceResult<LuaFunctionView>
JournalService::create_function(LuaFunctionInput input) const {
    if (trimmed(input.source).empty()) {
        return std::unexpected(problem(
            422, "empty_lua_source", "函数脚本为空", "请填写 Lua 函数定义"));
    }
    auto result = functions_->create(std::move(input.source));
    if (!result) return lua_failure<LuaFunctionView>(result.error());
    return lua_function_view(*result);
}

ServiceResult<LuaFunctionView>
JournalService::update_function(std::string_view name, LuaFunctionInput input) const {
    if (trimmed(input.source).empty()) {
        return std::unexpected(problem(
            422, "empty_lua_source", "函数脚本为空", "请填写 Lua 函数定义"));
    }
    auto result = functions_->update(name, std::move(input.source));
    if (!result) return lua_failure<LuaFunctionView>(result.error());
    return lua_function_view(*result);
}

ServiceResult<lua::LuaValue>
JournalService::invoke_function(std::string_view name, const lua::LuaValue::Object &input) const {
    auto result = functions_->invoke(name, input);
    if (!result) {
        return lua_failure<lua::LuaValue>(result.error());
    }
    return *result;
}

drogon::Task<ServiceResult<std::vector<JobView>>> JournalService::jobs() const {
    try {
        co_return co_await repository_->list_jobs();
    } catch (const std::exception &exception) {
        co_return database_failure<std::vector<JobView>>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::cancel_job(std::string_view job_id) const {
    try {
        co_await repository_->request_job_cancel(job_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return database_failure<std::monostate>(exception);
    }
}

}  // namespace journalseed::application
