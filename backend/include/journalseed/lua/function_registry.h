#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace journalseed::lua {

struct LuaValue {
    using Object = std::map<std::string, LuaValue>;
    using Array = std::vector<LuaValue>;
    std::variant<std::nullptr_t, bool, std::string, Object, Array> value{nullptr};
};

struct FunctionParameter {
    std::string name;
    std::string type;
    std::string label;
    bool required{false};
    std::vector<std::string> options;
};

struct FunctionMetadata {
    std::string name;
    std::string version;
    std::string description;
    std::string script;
    std::string source;
    std::vector<FunctionParameter> params;
};

enum class RegistryErrorCode {
    directory_error,
    script_error,
    schema_error,
    function_missing,
    execution_error,
    limit_exceeded,
    write_error,
    function_conflict,
};

struct RegistryError {
    RegistryErrorCode code;
    std::string message;
    std::string script;
};

struct FunctionRegistryOptions {
    std::filesystem::path directory;
    std::chrono::milliseconds poll_interval{750};
    std::uint64_t instruction_limit{1'000'000};
    std::chrono::milliseconds wall_time_limit{100};
    std::size_t result_depth_limit{8};
};

class FunctionRegistry final {
  public:
    using ReloadCallback = std::function<void(const std::expected<std::size_t, RegistryError> &)>;

    explicit FunctionRegistry(FunctionRegistryOptions options);
    ~FunctionRegistry();

    FunctionRegistry(const FunctionRegistry &) = delete;
    FunctionRegistry &operator=(const FunctionRegistry &) = delete;

    [[nodiscard]] std::expected<std::size_t, RegistryError> reload();
    [[nodiscard]] std::vector<FunctionMetadata> list() const;
    [[nodiscard]] std::expected<FunctionMetadata, RegistryError>
    create(std::string source);
    [[nodiscard]] std::expected<FunctionMetadata, RegistryError>
    update(std::string_view name, std::string source);
    [[nodiscard]] std::expected<LuaValue, RegistryError>
    invoke(std::string_view name, const LuaValue::Object &parameters) const;

    void start(ReloadCallback callback = {});
    void stop();

  private:
    struct Registry;
    [[nodiscard]] std::expected<std::size_t, RegistryError> reload_unlocked();
    [[nodiscard]] std::string directory_signature() const;

    FunctionRegistryOptions options_;
    std::shared_ptr<const Registry> registry_;
    mutable std::mutex update_mutex_;
    std::jthread watcher_;
};

}  // namespace journalseed::lua
