#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace journalseed::config {

struct AppConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    std::string database_url{
        "postgresql://journalseed:journalseed@127.0.0.1:5432/journalseed"};
    std::string lua_dir{"./scripts/functions"};
    std::string static_dir{"./frontend/build"};
    std::uint32_t lua_poll_interval_ms{750};
    std::uint16_t interactive_db_connections{8};
    std::uint16_t background_db_connections{2};
    std::uint16_t background_workers{2};
    std::string config_path{"./config.json"};
    bool migrate_only{false};
    bool show_help{false};
};

enum class ConfigErrorCode {
    invalid_json,
    invalid_option,
    invalid_value,
    file_error,
};

struct ConfigError {
    ConfigErrorCode code;
    std::string message;
};

using Environment = std::unordered_map<std::string, std::string>;

// `config_json` represents the already-selected JSON file. This pure function
// makes precedence behavior deterministic in unit tests.
[[nodiscard]] std::expected<AppConfig, ConfigError>
resolve_app_config(std::span<const std::string_view> arguments,
                   const Environment &environment,
                   std::optional<std::string_view> config_json = std::nullopt);

[[nodiscard]] std::expected<AppConfig, ConfigError>
load_app_config(std::span<const std::string_view> arguments);

[[nodiscard]] std::string config_help();

}  // namespace journalseed::config
