#include "journalseed/lua/decimal.h"
#include "journalseed/lua/function_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("journalseed-lua-test-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path &path() const { return path_; }

    void write(std::string_view name, std::string_view contents) const {
        std::ofstream output(path_ / name, std::ios::binary);
        output << contents;
        output.close();
        REQUIRE(output.good());
    }

  private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("decimal userdata keeps common decimal arithmetic exact") {
    const journalseed::lua::Decimal left("0.1");
    const journalseed::lua::Decimal right("0.2");
    REQUIRE((left + right).to_string() == "0.3");
    REQUIRE((journalseed::lua::Decimal("10") / journalseed::lua::Decimal("4")).to_string() ==
            "2.5");
}

TEST_CASE("Lua registry publishes complete snapshots and retains the previous valid one") {
    TemporaryDirectory scripts;
    scripts.write("expense.lua", R"LUA(
return {
  name = "expense",
  version = "1.0.0",
  description = "exact expense",
  params = {{ name = "amount", type = "number", label = "Amount", required = true }},
  run = function(params)
    return { amount = tostring(-dec(params.amount)), blocked_os = os }
  end
}
)LUA");

    journalseed::lua::FunctionRegistry registry({.directory = scripts.path()});
    REQUIRE(registry.reload() == 1);
    REQUIRE(registry.list().front().name == "expense");
    REQUIRE(registry.list().front().script == "expense.lua");
    REQUIRE(registry.list().front().source.find("exact expense") != std::string::npos);

    journalseed::lua::LuaValue::Object input;
    input.emplace("amount", journalseed::lua::LuaValue{.value = std::string("12.50")});
    const auto invoked = registry.invoke("expense", input);
    REQUIRE(invoked.has_value());
    const auto &result = std::get<journalseed::lua::LuaValue::Object>(invoked->value);
    REQUIRE(std::get<std::string>(result.at("amount").value) == "-12.50");
    REQUIRE_FALSE(result.contains("blocked_os"));

    scripts.write("broken.lua", "return { name = 'broken' }");
    REQUIRE_FALSE(registry.reload().has_value());
    REQUIRE(registry.list().size() == 1);
    REQUIRE(registry.invoke("expense", input).has_value());
}

TEST_CASE("Lua registry creates and updates editable scripts with rollback") {
    TemporaryDirectory scripts;
    journalseed::lua::FunctionRegistry registry({.directory = scripts.path()});
    REQUIRE(registry.reload() == 0);

    const auto created = registry.create(R"LUA(
return {
  name = "custom",
  version = "1.0.0",
  description = "created from API",
  params = {{ name = "amount", type = "number", label = "Amount", required = true }},
  run = function(params)
    return { amount = tostring(dec(params.amount) * dec("2")) }
  end
}
)LUA");
    REQUIRE(created.has_value());
    REQUIRE(created->name == "custom");
    REQUIRE(std::filesystem::exists(scripts.path() / "custom.lua"));
    REQUIRE(registry.list().size() == 1);

    journalseed::lua::LuaValue::Object input;
    input.emplace("amount", journalseed::lua::LuaValue{.value = std::string("7.50")});
    auto invoked = registry.invoke("custom", input);
    REQUIRE(invoked.has_value());
    REQUIRE(std::get<std::string>(
                std::get<journalseed::lua::LuaValue::Object>(invoked->value).at("amount").value) ==
            "15.00");

    const auto updated = registry.update("custom", R"LUA(
return {
  name = "custom",
  version = "1.1.0",
  description = "updated from API",
  params = {{ name = "amount", type = "number", label = "Amount", required = true }},
  run = function(params)
    return { amount = tostring(dec(params.amount) * dec("3")) }
  end
}
)LUA");
    REQUIRE(updated.has_value());
    REQUIRE(updated->version == "1.1.0");
    invoked = registry.invoke("custom", input);
    REQUIRE(invoked.has_value());
    REQUIRE(std::get<std::string>(
                std::get<journalseed::lua::LuaValue::Object>(invoked->value).at("amount").value) ==
            "22.50");

    const auto broken = registry.update("custom", "return { name = 'broken' }");
    REQUIRE_FALSE(broken.has_value());
    REQUIRE(registry.list().front().name == "custom");
    REQUIRE(registry.list().front().version == "1.1.0");
    REQUIRE(registry.invoke("custom", input).has_value());
}

TEST_CASE("Lua registry interrupts scripts that exceed the instruction budget") {
    TemporaryDirectory scripts;
    scripts.write("loop.lua", R"LUA(
return {
  name = "loop",
  version = "1.0.0",
  description = "limit fixture",
  params = {},
  run = function()
    local value = 0
    while true do value = value + 1 end
  end
}
)LUA");

    journalseed::lua::FunctionRegistry registry({
        .directory = scripts.path(),
        .instruction_limit = 10'000,
        .wall_time_limit = std::chrono::milliseconds(100),
    });
    REQUIRE(registry.reload() == 1);
    const auto result = registry.invoke("loop", {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == journalseed::lua::RegistryErrorCode::limit_exceeded);
}
