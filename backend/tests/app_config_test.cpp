#include "journalseed/config/app_config.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace journalseed::config;

TEST_CASE("configuration follows CLI environment JSON default precedence") {
    const std::string json = R"({
        "host": "file-host",
        "port": 7000,
        "background_workers": 3
    })";
    const Environment environment{
        {"JOURNALSEED_HOST", "environment-host"},
        {"JOURNALSEED_PORT", "7100"},
    };
    constexpr std::array arguments{
        std::string_view{"--host"},
        std::string_view{"cli-host"},
        std::string_view{"--port=7200"},
    };

    const auto config = resolve_app_config(arguments, environment, json);
    REQUIRE(config);
    REQUIRE(config->host == "cli-host");
    REQUIRE(config->port == 7200);
    REQUIRE(config->background_workers == 3);
    REQUIRE(config->lua_dir == "./scripts/functions");
}

TEST_CASE("configuration rejects malformed numeric values") {
    const Environment environment{{"JOURNALSEED_PORT", "zero"}};
    const auto config = resolve_app_config({}, environment);
    REQUIRE_FALSE(config);
    REQUIRE(config.error().code == ConfigErrorCode::invalid_value);
}

TEST_CASE("configuration rejects unknown switches") {
    constexpr std::array arguments{std::string_view{"--surprise"}};
    const auto config = resolve_app_config(arguments, {});
    REQUIRE_FALSE(config);
    REQUIRE(config.error().code == ConfigErrorCode::invalid_option);
}
