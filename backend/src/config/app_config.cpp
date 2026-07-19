#include "journalseed/config/app_config.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <glaze/glaze.hpp>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace journalseed::config {
struct FileConfig {
    std::optional<std::string> host;
    std::optional<std::uint16_t> port;
    std::optional<std::string> database_url;
    std::optional<std::string> lua_dir;
    std::optional<std::string> static_dir;
    std::optional<std::uint32_t> lua_poll_interval_ms;
    std::optional<std::uint16_t> interactive_db_connections;
    std::optional<std::uint16_t> background_db_connections;
    std::optional<std::uint16_t> background_workers;
};

namespace {

ConfigError error(ConfigErrorCode code, std::string message) {
    return ConfigError{.code = code, .message = std::move(message)};
}

template <typename Integer>
std::expected<Integer, ConfigError> parse_integer(std::string_view value,
                                                  std::string_view option,
                                                  Integer minimum,
                                                  Integer maximum) {
    std::uint64_t parsed = 0;
    const auto [end, parse_error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (parse_error != std::errc{} || end != value.data() + value.size() ||
        parsed < static_cast<std::uint64_t>(minimum) ||
        parsed > static_cast<std::uint64_t>(maximum)) {
        return std::unexpected(error(
            ConfigErrorCode::invalid_value,
            std::string(option) + " 需要是 " + std::to_string(minimum) + " 到 " +
                std::to_string(maximum) + " 之间的整数"));
    }
    return static_cast<Integer>(parsed);
}

void apply_file_config(AppConfig &target, const FileConfig &source) {
    if (source.host) target.host = *source.host;
    if (source.port) target.port = *source.port;
    if (source.database_url) target.database_url = *source.database_url;
    if (source.lua_dir) target.lua_dir = *source.lua_dir;
    if (source.static_dir) target.static_dir = *source.static_dir;
    if (source.lua_poll_interval_ms) target.lua_poll_interval_ms = *source.lua_poll_interval_ms;
    if (source.interactive_db_connections) {
        target.interactive_db_connections = *source.interactive_db_connections;
    }
    if (source.background_db_connections) {
        target.background_db_connections = *source.background_db_connections;
    }
    if (source.background_workers) target.background_workers = *source.background_workers;
}

std::optional<std::string_view> argument_value(std::span<const std::string_view> arguments,
                                               std::size_t &index,
                                               std::string_view name) {
    const auto argument = arguments[index];
    const std::string prefix = std::string(name) + "=";
    if (argument.starts_with(prefix)) {
        return argument.substr(prefix.size());
    }
    if (argument == name && index + 1 < arguments.size()) {
        ++index;
        return arguments[index];
    }
    return std::nullopt;
}

std::optional<std::string> selected_config_path(
    std::span<const std::string_view> arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (auto value = argument_value(arguments, index, "--config")) {
            return std::string(*value);
        }
    }
    return std::nullopt;
}

void apply_string_environment(const Environment &environment,
                              std::string_view name,
                              std::string &target) {
    if (auto iterator = environment.find(std::string(name)); iterator != environment.end()) {
        target = iterator->second;
    }
}

template <typename Integer>
std::expected<void, ConfigError> apply_integer_environment(
    const Environment &environment,
    std::string_view name,
    Integer &target,
    Integer minimum,
    Integer maximum) {
    if (auto iterator = environment.find(std::string(name)); iterator != environment.end()) {
        auto parsed = parse_integer<Integer>(iterator->second, name, minimum, maximum);
        if (!parsed) return std::unexpected(parsed.error());
        target = *parsed;
    }
    return {};
}

std::expected<void, ConfigError> apply_environment(AppConfig &config,
                                                   const Environment &environment) {
    apply_string_environment(environment, "JOURNALSEED_HOST", config.host);
    apply_string_environment(environment, "JOURNALSEED_DATABASE_URL", config.database_url);
    apply_string_environment(environment, "JOURNALSEED_LUA_DIR", config.lua_dir);
    apply_string_environment(environment, "JOURNALSEED_STATIC_DIR", config.static_dir);

    if (auto result = apply_integer_environment(environment, "JOURNALSEED_PORT", config.port,
                                                std::uint16_t{1},
                                                std::numeric_limits<std::uint16_t>::max());
        !result) {
        return result;
    }
    if (auto result = apply_integer_environment(
            environment, "JOURNALSEED_BACKGROUND_WORKERS", config.background_workers,
            std::uint16_t{1}, std::uint16_t{64});
        !result) {
        return result;
    }
    return {};
}

std::expected<void, ConfigError> apply_arguments(
    AppConfig &config, std::span<const std::string_view> arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            config.show_help = true;
        } else if (argument == "--migrate-only") {
            config.migrate_only = true;
        } else if (auto host_value = argument_value(arguments, index, "--host")) {
            config.host = *host_value;
        } else if (auto port_value = argument_value(arguments, index, "--port")) {
            auto parsed = parse_integer<std::uint16_t>(
                *port_value,
                "--port",
                std::uint16_t{1},
                std::numeric_limits<std::uint16_t>::max());
            if (!parsed) return std::unexpected(parsed.error());
            config.port = *parsed;
        } else if (auto config_value = argument_value(arguments, index, "--config")) {
            config.config_path = *config_value;
        } else if (auto database_value = argument_value(arguments, index, "--database-url")) {
            config.database_url = *database_value;
        } else if (auto lua_value = argument_value(arguments, index, "--lua-dir")) {
            config.lua_dir = *lua_value;
        } else if (auto static_value = argument_value(arguments, index, "--static-dir")) {
            config.static_dir = *static_value;
        } else if (auto worker_value =
                       argument_value(arguments, index, "--background-workers")) {
            auto parsed = parse_integer<std::uint16_t>(
                *worker_value,
                "--background-workers",
                std::uint16_t{1},
                std::uint16_t{64});
            if (!parsed) return std::unexpected(parsed.error());
            config.background_workers = *parsed;
        } else {
            return std::unexpected(error(ConfigErrorCode::invalid_option,
                                         "未知命令行参数：" + std::string(argument)));
        }
    }
    return {};
}

Environment process_environment() {
    constexpr std::string_view names[] = {
        "JOURNALSEED_HOST",
        "JOURNALSEED_PORT",
        "JOURNALSEED_DATABASE_URL",
        "JOURNALSEED_LUA_DIR",
        "JOURNALSEED_STATIC_DIR",
        "JOURNALSEED_BACKGROUND_WORKERS",
    };
    Environment result;
    for (const auto name : names) {
        if (const char *value = std::getenv(std::string(name).c_str()); value != nullptr) {
            result.emplace(name, value);
        }
    }
    return result;
}

}  // namespace

std::expected<AppConfig, ConfigError>
resolve_app_config(std::span<const std::string_view> arguments,
                   const Environment &environment,
                   std::optional<std::string_view> config_json) {
    AppConfig config;
    if (config_json) {
        FileConfig from_file;
        const auto parse_result = glz::read_json(from_file, *config_json);
        if (parse_result) {
            return std::unexpected(error(
                ConfigErrorCode::invalid_json,
                "配置文件 JSON 解析失败：" + glz::format_error(parse_result, *config_json)));
        }
        apply_file_config(config, from_file);
    }

    if (auto environment_result = apply_environment(config, environment); !environment_result) {
        return std::unexpected(environment_result.error());
    }
    if (auto argument_result = apply_arguments(config, arguments); !argument_result) {
        return std::unexpected(argument_result.error());
    }

    if (config.host.empty()) {
        return std::unexpected(
            error(ConfigErrorCode::invalid_value, "监听地址不能为空"));
    }
    if (config.database_url.empty()) {
        return std::unexpected(
            error(ConfigErrorCode::invalid_value, "数据库连接地址不能为空"));
    }
    return config;
}

std::expected<AppConfig, ConfigError>
load_app_config(std::span<const std::string_view> arguments) {
    const auto path = selected_config_path(arguments).value_or("./config.json");
    std::optional<std::string> json;
    std::ifstream stream(path, std::ios::binary);
    if (stream) {
        std::ostringstream contents;
        contents << stream.rdbuf();
        json = contents.str();
    } else if (selected_config_path(arguments).has_value()) {
        return std::unexpected(error(ConfigErrorCode::file_error,
                                     "读取配置文件失败：" + path));
    }

    auto result = resolve_app_config(arguments, process_environment(), json);
    if (result) {
        result->config_path = path;
    }
    return result;
}

std::string config_help() {
    return
        "JournalSeed 0.1.0\n"
        "用法：journalseed [选项]\n\n"
        "  --host <地址>                 监听地址（默认 127.0.0.1）\n"
        "  --port <端口>                 监听端口（默认 8080）\n"
        "  --config <文件>               JSON 配置文件（默认 ./config.json）\n"
        "  --database-url <URL>          PostgreSQL 连接地址\n"
        "  --lua-dir <目录>              Lua 命名函数目录\n"
        "  --static-dir <目录>           前端静态产物目录\n"
        "  --background-workers <数量>   后台工作线程数（1 到 64）\n"
        "  --migrate-only                完成迁移后退出\n"
        "  --help                        显示本帮助\n";
}

}  // namespace journalseed::config
