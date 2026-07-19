#include "journalseed/api/api_controller.h"
#include "journalseed/application/journal_service.h"
#include "journalseed/config/app_config.h"
#include "journalseed/infrastructure/migration_runner.h"
#include "journalseed/infrastructure/postgres_repository.h"
#include "journalseed/lua/function_registry.h"

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::filesystem::path locate_directory(std::string_view configured,
                                       std::string_view source_relative) {
    std::filesystem::path selected(configured);
    if (std::filesystem::exists(selected)) return selected;
    const auto source_path = std::filesystem::path(JOURNALSEED_SOURCE_DIR) / source_relative;
    return std::filesystem::exists(source_path) ? source_path : selected;
}

std::string event_json(bool success, std::string detail) {
    glz::generic event;
    event["success"] = success;
    event["detail"] = std::move(detail);
    auto json = glz::write_json(event);
    return json ? std::move(*json) : std::string{"{\"success\":false}"};
}

std::string static_content_security_policy(const std::filesystem::path &index_file) {
    constexpr std::string_view marker =
        "<meta http-equiv=\"content-security-policy\" content=\"";
    std::ifstream stream(index_file, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    const auto source = contents.str();
    const auto marker_start = source.find(marker);
    if (marker_start != std::string::npos) {
        const auto policy_start = marker_start + marker.size();
        const auto policy_end = source.find('"', policy_start);
        if (policy_end != std::string::npos) {
            auto policy = source.substr(policy_start, policy_end - policy_start);
            if (!policy.contains("frame-ancestors")) {
                policy += "; frame-ancestors 'none'";
            }
            return policy;
        }
    }
    return "default-src 'self'; base-uri 'self'; connect-src 'self'; font-src 'self'; "
           "form-action 'self'; img-src 'self' data:; object-src 'none'; "
           "script-src 'self'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'";
}

}  // namespace

int main(int argc, char **argv) {
    try {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);

        auto config_result = journalseed::config::load_app_config(arguments);
        if (!config_result) {
            std::cerr << config_result.error().message << '\n';
            return 2;
        }
        const auto config = std::move(*config_result);
        if (config.show_help) {
            std::cout << journalseed::config::config_help();
            return 0;
        }

        const auto migrations = locate_directory("./migrations", "migrations");
        const journalseed::infrastructure::MigrationRunner migration_runner(
            config.database_url, migrations);
        const auto migration_report = migration_runner.run();
        std::cout << "数据库迁移：新增 " << migration_report.applied << "，已校验 "
                  << migration_report.verified << '\n';
        if (config.migrate_only) return 0;
        const auto lua_directory = locate_directory(config.lua_dir, "scripts/functions");
        auto functions = std::make_shared<journalseed::lua::FunctionRegistry>(
            journalseed::lua::FunctionRegistryOptions{
                .directory = lua_directory,
                .poll_interval = std::chrono::milliseconds(config.lua_poll_interval_ms),
            });
        auto function_count = functions->reload();
        if (!function_count) {
            throw std::runtime_error("Lua function registry failed: " +
                                     function_count.error().message);
        }

        auto database = drogon::orm::DbClient::newPgClient(
            config.database_url, config.interactive_db_connections);
        auto repository =
            std::make_shared<journalseed::infrastructure::PostgresRepository>(database);
        auto service = std::make_shared<journalseed::application::JournalService>(
            repository, functions);
        auto api = std::make_shared<journalseed::api::ApiController>(service);
        api->register_routes();

        functions->start([weak_api = std::weak_ptr(api)](const auto &result) {
            if (const auto controller = weak_api.lock()) {
                if (result) {
                    controller->publish_event(
                        "lua.reload",
                        event_json(true, "已载入 " + std::to_string(*result) + " 个函数"));
                } else {
                    controller->publish_event(
                        "lua.reload",
                        event_json(false, result.error().message));
                }
            }
        });

        const auto static_directory = locate_directory(config.static_dir, "frontend/build");
        const auto index_file = static_directory / "index.html";
        const auto content_security_policy = static_content_security_policy(index_file);
        if (std::filesystem::exists(index_file)) {
            drogon::app().setDocumentRoot(static_directory.string());
            drogon::app().setStaticFileHeaders({
                {"X-Content-Type-Options", "nosniff"},
                {"Referrer-Policy", "same-origin"},
            });
            drogon::app().registerHandler(
                "/",
                [index = index_file.string()](
                    const drogon::HttpRequestPtr &request,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                    auto response = drogon::HttpResponse::newFileResponse(
                        index, "", drogon::CT_TEXT_HTML, "text/html; charset=utf-8", request);
                    response->addHeader("Cache-Control", "no-cache");
                    callback(response);
                },
                {drogon::Get});
        } else {
            std::cerr << "前端静态目录尚未构建：" << static_directory << '\n';
        }

        drogon::app().registerPreSendingAdvice(
            [content_security_policy](const drogon::HttpRequestPtr &,
                                      const drogon::HttpResponsePtr &response) {
                response->addHeader("X-Content-Type-Options", "nosniff");
                response->addHeader("Referrer-Policy", "same-origin");
                response->addHeader("X-Frame-Options", "DENY");
                response->addHeader("Content-Security-Policy", content_security_policy);
            });

        const auto hardware_threads = std::max(2U, std::thread::hardware_concurrency());
        const auto http_threads = static_cast<std::size_t>(std::min(hardware_threads, 16U));
        std::cout << "JournalSeed http://" << config.host << ':' << config.port
                  << "，Lua 函数 " << *function_count << '\n';
        drogon::app()
            .setThreadNum(http_threads)
            .addListener(config.host, config.port)
            .run();
        functions->stop();
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "JournalSeed 启动失败：" << exception.what() << '\n';
        return 1;
    }
}
