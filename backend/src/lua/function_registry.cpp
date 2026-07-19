#include "journalseed/lua/function_registry.h"

#include "journalseed/lua/decimal.h"

#include <lua.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace journalseed::lua {
namespace {

struct FunctionEntry {
    FunctionMetadata metadata;
    sol::protected_function run;
};

thread_local std::uint64_t remaining_instructions = 0;
thread_local std::chrono::steady_clock::time_point execution_deadline;

void instruction_hook(lua_State *state, lua_Debug *) {
    constexpr std::uint64_t hook_interval = 1'000;
    if (remaining_instructions <= hook_interval) {
        luaL_error(state, "instruction limit exceeded");
    }
    remaining_instructions -= hook_interval;
    if (std::chrono::steady_clock::now() >= execution_deadline) {
        luaL_error(state, "wall-clock limit exceeded");
    }
}

RegistryError registry_error(RegistryErrorCode code,
                             std::string message,
                             std::string script = {}) {
    return RegistryError{
        .code = code,
        .message = std::move(message),
        .script = std::move(script),
    };
}

std::expected<LuaValue, RegistryError> from_sol_object(const sol::object &object,
                                                       std::size_t depth,
                                                       std::size_t depth_limit);

std::expected<LuaValue, RegistryError> from_sol_table(const sol::table &table,
                                                      std::size_t depth,
                                                      std::size_t depth_limit) {
    if (depth > depth_limit) {
        return std::unexpected(registry_error(
            RegistryErrorCode::limit_exceeded, "Lua 结果嵌套层级超出限制"));
    }

    const std::size_t array_size = table.size();
    bool is_array = array_size > 0;
    std::size_t item_count = 0;
    for (const auto &[key, value] : table) {
        static_cast<void>(value);
        ++item_count;
        if (!key.is<lua_Integer>()) {
            is_array = false;
            break;
        }
        const auto index = key.as<lua_Integer>();
        if (index < 1 || static_cast<std::size_t>(index) > array_size) {
            is_array = false;
            break;
        }
    }
    is_array = is_array && item_count == array_size;

    if (is_array) {
        LuaValue::Array result;
        result.reserve(array_size);
        for (std::size_t index = 1; index <= array_size; ++index) {
            auto converted = from_sol_object(table.get<sol::object>(index), depth + 1, depth_limit);
            if (!converted) return converted;
            result.push_back(std::move(*converted));
        }
        return LuaValue{.value = std::move(result)};
    }

    LuaValue::Object result;
    for (const auto &[key, value] : table) {
        if (!key.is<std::string>()) {
            return std::unexpected(registry_error(
                RegistryErrorCode::execution_error,
                "Lua 结果对象的键必须是字符串"));
        }
        auto converted = from_sol_object(value, depth + 1, depth_limit);
        if (!converted) return converted;
        result.emplace(key.as<std::string>(), std::move(*converted));
    }
    return LuaValue{.value = std::move(result)};
}

std::expected<LuaValue, RegistryError> from_sol_object(const sol::object &object,
                                                       std::size_t depth,
                                                       std::size_t depth_limit) {
    switch (object.get_type()) {
        case sol::type::none:
        case sol::type::lua_nil:
            return LuaValue{};
        case sol::type::boolean:
            return LuaValue{.value = object.as<bool>()};
        case sol::type::string:
            return LuaValue{.value = object.as<std::string>()};
        case sol::type::number: {
            if (object.is<lua_Integer>()) {
                return LuaValue{.value = std::to_string(object.as<lua_Integer>())};
            }
            return std::unexpected(registry_error(
                RegistryErrorCode::execution_error,
                "Lua 浮点数不能作为精确结果，请使用 dec()"));
        }
        case sol::type::userdata:
            if (object.is<Decimal>()) {
                return LuaValue{.value = object.as<Decimal>().to_string()};
            }
            return std::unexpected(registry_error(
                RegistryErrorCode::execution_error, "Lua 返回了不受支持的 userdata"));
        case sol::type::table:
            return from_sol_table(object.as<sol::table>(), depth, depth_limit);
        default:
            return std::unexpected(registry_error(
                RegistryErrorCode::execution_error, "Lua 返回了不受支持的值类型"));
    }
}

sol::object to_sol_object(sol::state_view state, const LuaValue &value) {
    return std::visit(
        [&](const auto &item) -> sol::object {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::nullptr_t>) {
                return sol::make_object(state, sol::lua_nil);
            } else if constexpr (std::is_same_v<Item, bool> || std::is_same_v<Item, std::string>) {
                return sol::make_object(state, item);
            } else if constexpr (std::is_same_v<Item, LuaValue::Array>) {
                auto table = state.create_table(static_cast<int>(item.size()), 0);
                for (std::size_t index = 0; index < item.size(); ++index) {
                    table[index + 1] = to_sol_object(state, item[index]);
                }
                return sol::make_object(state, table);
            } else {
                auto table = state.create_table(0, static_cast<int>(item.size()));
                for (const auto &[key, child] : item) {
                    table[key] = to_sol_object(state, child);
                }
                return sol::make_object(state, table);
            }
        },
        value.value);
}

std::expected<FunctionParameter, RegistryError> parse_parameter(const sol::table &table,
                                                                const std::string &script) {
    FunctionParameter parameter;
    parameter.name = table.get_or("name", std::string{});
    parameter.type = table.get_or("type", std::string{});
    parameter.label = table.get_or("label", parameter.name);
    parameter.required = table.get_or("required", false);
    if (parameter.name.empty() ||
        (parameter.type != "text" && parameter.type != "number" &&
         parameter.type != "date" && parameter.type != "boolean" &&
         parameter.type != "option")) {
        return std::unexpected(registry_error(
            RegistryErrorCode::schema_error,
            "函数参数需要有效的 name、type 和 label",
            script));
    }
    if (auto options = table["options"]; options.valid() && options.get_type() == sol::type::table) {
        for (const auto &item : options.get<sol::table>()) {
            parameter.options.push_back(item.second.as<std::string>());
        }
    }
    return parameter;
}

}  // namespace

struct FunctionRegistry::Registry {
    std::shared_ptr<sol::state> state;
    std::map<std::string, FunctionEntry> functions;
    mutable std::mutex execution_mutex;
};

FunctionRegistry::FunctionRegistry(FunctionRegistryOptions options)
    : options_(std::move(options)), registry_(std::make_shared<const Registry>()) {}

FunctionRegistry::~FunctionRegistry() { stop(); }

std::expected<std::size_t, RegistryError> FunctionRegistry::reload() {
    std::error_code filesystem_error;
    if (!std::filesystem::exists(options_.directory, filesystem_error) || filesystem_error) {
        return std::unexpected(registry_error(
            RegistryErrorCode::directory_error,
            "Lua 函数目录不存在：" + options_.directory.string()));
    }

    auto candidate = std::make_shared<Registry>();
    candidate->state = std::make_shared<sol::state>();
    auto &state = *candidate->state;
    state.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                         sol::lib::table, sol::lib::utf8);
    state["os"] = sol::lua_nil;
    state["io"] = sol::lua_nil;
    state["package"] = sol::lua_nil;
    state["debug"] = sol::lua_nil;
    state["require"] = sol::lua_nil;
    state["dofile"] = sol::lua_nil;
    state["loadfile"] = sol::lua_nil;
    state["load"] = sol::lua_nil;
    state["collectgarbage"] = sol::lua_nil;

    state.new_usertype<Decimal>(
        "Decimal",
        sol::no_constructor,
        sol::meta_function::addition,
        &Decimal::operator+,
        sol::meta_function::subtraction,
        static_cast<Decimal (Decimal::*)(const Decimal &) const>(&Decimal::operator-),
        sol::meta_function::multiplication,
        &Decimal::operator*,
        sol::meta_function::division,
        &Decimal::operator/,
        sol::meta_function::unary_minus,
        static_cast<Decimal (Decimal::*)() const>(&Decimal::operator-),
        sol::meta_function::equal_to,
        &Decimal::operator==,
        sol::meta_function::to_string,
        &Decimal::to_string);
    state.set_function("dec", [](const std::string &value) { return Decimal(value); });

    std::vector<std::filesystem::path> scripts;
    for (const auto &entry : std::filesystem::directory_iterator(options_.directory, filesystem_error)) {
        if (filesystem_error) {
            return std::unexpected(registry_error(
                RegistryErrorCode::directory_error,
                "读取 Lua 函数目录失败：" + filesystem_error.message()));
        }
        if (entry.is_regular_file() && entry.path().extension() == ".lua") {
            scripts.push_back(entry.path());
        }
    }
    std::ranges::sort(scripts);

    for (const auto &script : scripts) {
        const std::string script_name = script.filename().string();
        sol::load_result loaded = state.load_file(script.string());
        if (!loaded.valid()) {
            const sol::error load_error = loaded;
            return std::unexpected(registry_error(
                RegistryErrorCode::script_error, load_error.what(), script_name));
        }
        sol::protected_function chunk = loaded;
        sol::protected_function_result executed = chunk();
        if (!executed.valid()) {
            const sol::error execution_error = executed;
            return std::unexpected(registry_error(
                RegistryErrorCode::script_error, execution_error.what(), script_name));
        }
        if (executed.get_type() != sol::type::table) {
            return std::unexpected(registry_error(
                RegistryErrorCode::schema_error,
                "脚本必须返回函数定义 table",
                script_name));
        }

        const sol::table definition = executed;
        FunctionEntry function;
        function.metadata.name = definition.get_or("name", std::string{});
        function.metadata.version = definition.get_or("version", std::string{});
        function.metadata.description = definition.get_or("description", std::string{});
        function.run = definition["run"];
        if (function.metadata.name.empty() || function.metadata.version.empty() ||
            !function.run.valid()) {
            return std::unexpected(registry_error(
                RegistryErrorCode::schema_error,
                "函数定义需要 name、version、description 和 run",
                script_name));
        }

        if (auto params = definition["params"];
            params.valid() && params.get_type() == sol::type::table) {
            for (const auto &item : params.get<sol::table>()) {
                if (item.second.get_type() != sol::type::table) {
                    return std::unexpected(registry_error(
                        RegistryErrorCode::schema_error,
                        "params 中的每一项必须是 table",
                        script_name));
                }
                auto parameter = parse_parameter(item.second.as<sol::table>(), script_name);
                if (!parameter) return std::unexpected(parameter.error());
                function.metadata.params.push_back(std::move(*parameter));
            }
        }

        if (!candidate->functions.emplace(function.metadata.name, std::move(function)).second) {
            return std::unexpected(registry_error(
                RegistryErrorCode::schema_error,
                "函数名称重复",
                script_name));
        }
    }

    std::atomic_store_explicit(
        &registry_, std::const_pointer_cast<const Registry>(candidate), std::memory_order_release);
    return candidate->functions.size();
}

std::vector<FunctionMetadata> FunctionRegistry::list() const {
    const auto snapshot = std::atomic_load_explicit(&registry_, std::memory_order_acquire);
    std::vector<FunctionMetadata> result;
    result.reserve(snapshot->functions.size());
    for (const auto &[name, function] : snapshot->functions) {
        static_cast<void>(name);
        result.push_back(function.metadata);
    }
    return result;
}

std::expected<LuaValue, RegistryError>
FunctionRegistry::invoke(std::string_view name, const LuaValue::Object &parameters) const {
    const auto snapshot = std::atomic_load_explicit(&registry_, std::memory_order_acquire);
    const auto function = snapshot->functions.find(std::string(name));
    if (function == snapshot->functions.end()) {
        return std::unexpected(registry_error(
            RegistryErrorCode::function_missing, "未找到命名函数：" + std::string(name)));
    }

    std::scoped_lock lock(snapshot->execution_mutex);
    auto &state = *snapshot->state;
    sol::table input = state.create_table(0, static_cast<int>(parameters.size()));
    for (const auto &[key, value] : parameters) {
        input[key] = to_sol_object(state, value);
    }

    remaining_instructions = options_.instruction_limit;
    execution_deadline = std::chrono::steady_clock::now() + options_.wall_time_limit;
    lua_sethook(state.lua_state(), instruction_hook, LUA_MASKCOUNT, 1'000);
    sol::protected_function_result execution = function->second.run(input);
    lua_sethook(state.lua_state(), nullptr, 0, 0);

    if (!execution.valid()) {
        const sol::error execution_error = execution;
        const std::string message = execution_error.what();
        const auto code = message.find("limit exceeded") != std::string::npos
                              ? RegistryErrorCode::limit_exceeded
                              : RegistryErrorCode::execution_error;
        return std::unexpected(registry_error(code, message, function->second.metadata.name));
    }
    if (execution.return_count() != 1) {
        return std::unexpected(registry_error(
            RegistryErrorCode::execution_error,
            "命名函数需要返回一个结果",
            function->second.metadata.name));
    }
    return from_sol_object(execution.get<sol::object>(), 0, options_.result_depth_limit);
}

std::string FunctionRegistry::directory_signature() const {
    std::error_code error_code;
    std::vector<std::string> entries;
    for (const auto &entry : std::filesystem::directory_iterator(options_.directory, error_code)) {
        if (error_code) return error_code.message();
        if (!entry.is_regular_file() || entry.path().extension() != ".lua") continue;
        const auto timestamp = entry.last_write_time(error_code).time_since_epoch().count();
        if (error_code) return error_code.message();
        entries.push_back(entry.path().filename().string() + ":" +
                          std::to_string(static_cast<long long>(timestamp)) + ":" +
                          std::to_string(entry.file_size(error_code)));
        if (error_code) return error_code.message();
    }
    std::ranges::sort(entries);
    std::ostringstream result;
    for (const auto &entry : entries) result << entry << ';';
    return result.str();
}

void FunctionRegistry::start(ReloadCallback callback) {
    stop();
    watcher_ = std::jthread([this, callback = std::move(callback)](std::stop_token stop_token) {
        auto signature = directory_signature();
        while (!stop_token.stop_requested()) {
            std::this_thread::sleep_for(options_.poll_interval);
            const auto next_signature = directory_signature();
            if (next_signature == signature) continue;
            auto result = reload();
            if (result) signature = next_signature;
            if (callback) callback(result);
        }
    });
}

void FunctionRegistry::stop() {
    if (watcher_.joinable()) {
        watcher_.request_stop();
        watcher_.join();
    }
}

}  // namespace journalseed::lua
