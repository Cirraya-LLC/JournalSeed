#include "journalseed/api/api_controller.h"

#include <drogon/drogon.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace journalseed::api {
namespace {

using application::AuthContext;
using application::Problem;
using application::ServiceResult;
using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;

constexpr std::string_view kSessionCookie = "journalseed_session";
constexpr std::size_t kMaximumJsonBody = 1024U * 1024U;

Problem make_problem(std::uint16_t status,
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

template <typename T>
HttpResponsePtr json_response(const T &value, int status = 200) {
    std::string body;
    if (const auto write_error = glz::write_json(value, body); write_error) {
        throw std::runtime_error("JSON response serialization failed");
    }
    auto response = HttpResponse::newHttpResponse();
    response->setCustomStatusCode(status);
    response->setContentTypeCodeAndCustomString(
        drogon::CT_APPLICATION_JSON, "application/json; charset=utf-8");
    response->addHeader("Cache-Control", "no-store");
    response->setBody(std::move(body));
    return response;
}

HttpResponsePtr problem_response(const Problem &problem) {
    auto response = json_response(problem, problem.status);
    response->setContentTypeCodeAndCustomString(
        drogon::CT_APPLICATION_JSON, "application/problem+json; charset=utf-8");
    return response;
}

HttpResponsePtr no_content_response() {
    auto response = HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    response->addHeader("Cache-Control", "no-store");
    return response;
}

template <typename T>
std::expected<T, Problem> parse_body(const HttpRequestPtr &request) {
    if (request->body().empty()) {
        return std::unexpected(make_problem(
            400, "invalid_json", "请求内容为空", "请提交 JSON 请求内容"));
    }
    if (request->body().size() > kMaximumJsonBody) {
        return std::unexpected(make_problem(
            413, "body_too_large", "请求内容过大", "JSON 请求内容上限为 1 MiB"));
    }

    T input{};
    if (const auto read_error = glz::read_json(input, request->body()); read_error) {
        return std::unexpected(make_problem(
            400,
            "invalid_json",
            "JSON 格式不正确",
            glz::format_error(read_error, request->body())));
    }
    return input;
}

std::optional<Problem> same_origin_issue(const HttpRequestPtr &request) {
    const auto &fetch_site = request->getHeader("Sec-Fetch-Site");
    if (!fetch_site.empty() && fetch_site != "same-origin") {
        return make_problem(403,
                            "cross_site_write",
                            "写入来源不匹配",
                            "写入请求必须来自当前 JournalSeed 页面");
    }
    return std::nullopt;
}

drogon::Task<ServiceResult<AuthContext>> authorize(
    const std::shared_ptr<application::JournalService> &service,
    const HttpRequestPtr &request,
    bool write) {
    if (write) {
        if (auto issue = same_origin_issue(request)) {
            co_return std::unexpected(std::move(*issue));
        }
    }

    const auto &cookie = request->getCookie(std::string(kSessionCookie));
    if (!write) co_return co_await service->authenticate(cookie);

    const auto &csrf = request->getHeader("X-JournalSeed-CSRF");
    co_return co_await service->authenticate(cookie, csrf);
}

void add_session_cookie(HttpResponsePtr &response,
                        std::string token,
                        const HttpRequestPtr &request) {
    drogon::Cookie cookie(std::string(kSessionCookie), std::move(token));
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSecure(request->isOnSecureConnection());
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    cookie.setMaxAge(30 * 24 * 60 * 60);
    response->addCookie(std::move(cookie));
}

void clear_session_cookie(HttpResponsePtr &response, const HttpRequestPtr &request) {
    drogon::Cookie cookie(std::string(kSessionCookie), "");
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSecure(request->isOnSecureConnection());
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    cookie.setMaxAge(0);
    response->addCookie(std::move(cookie));
}

template <typename T>
HttpResponsePtr result_response(ServiceResult<T> result, int success_status = 200) {
    if (!result) return problem_response(result.error());
    return json_response(*result, success_status);
}

std::expected<lua::LuaValue, Problem> generic_to_lua(const glz::generic &input,
                                                     std::size_t depth = 0) {
    if (depth > 8) {
        return std::unexpected(make_problem(
            422, "lua_input_depth", "函数参数层级过深", "参数最多允许 8 层嵌套"));
    }
    if (input.holds<glz::generic::null_t>()) return lua::LuaValue{};
    if (input.holds<bool>()) return lua::LuaValue{.value = input.get<bool>()};
    if (input.holds<std::string>()) {
        return lua::LuaValue{.value = input.get<std::string>()};
    }
    if (input.holds<double>()) {
        return std::unexpected(make_problem(
            422,
            "inexact_lua_number",
            "函数数字参数需要使用字符串",
            "精确数字请以 JSON 字符串传入，由 dec() 解析"));
    }
    if (input.holds<glz::generic::array_t>()) {
        lua::LuaValue::Array result;
        result.reserve(input.get<glz::generic::array_t>().size());
        for (const auto &item : input.get<glz::generic::array_t>()) {
            auto converted = generic_to_lua(item, depth + 1);
            if (!converted) return converted;
            result.push_back(std::move(*converted));
        }
        return lua::LuaValue{.value = std::move(result)};
    }

    lua::LuaValue::Object result;
    for (const auto &[key, item] : input.get<glz::generic::object_t>()) {
        auto converted = generic_to_lua(item, depth + 1);
        if (!converted) return converted;
        result.emplace(key, std::move(*converted));
    }
    return lua::LuaValue{.value = std::move(result)};
}

glz::generic lua_to_generic(const lua::LuaValue &input) {
    return std::visit(
        [](const auto &value) -> glz::generic {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::nullptr_t>) {
                return glz::generic(nullptr);
            } else if constexpr (std::is_same_v<Value, bool> ||
                                 std::is_same_v<Value, std::string>) {
                return glz::generic(value);
            } else if constexpr (std::is_same_v<Value, lua::LuaValue::Array>) {
                glz::generic::array_t array;
                array.reserve(value.size());
                for (const auto &item : value) array.push_back(lua_to_generic(item));
                return glz::generic(std::move(array));
            } else {
                glz::generic::object_t object;
                for (const auto &[key, item] : value) {
                    object.emplace(key, lua_to_generic(item));
                }
                return glz::generic(std::move(object));
            }
        },
        input.value);
}

std::uint16_t row_limit(const HttpRequestPtr &request) {
    const auto &text = request->getParameter("limit");
    if (text.empty()) return 100;
    unsigned int parsed = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) return 100;
    return static_cast<std::uint16_t>(std::clamp(parsed, 1U, 250U));
}

}  // namespace

struct ApiController::EventState {
    struct Item {
        std::uint64_t id;
        std::string name;
        std::string data;
    };

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Item> items;
    std::uint64_t sequence{0};
};

ApiController::ApiController(std::shared_ptr<application::JournalService> service)
    : service_(std::move(service)), events_(std::make_shared<EventState>()) {
    if (!service_) throw std::invalid_argument("journal service is required");
}

void ApiController::publish_event(std::string event_name, std::string json_data) {
    std::lock_guard lock(events_->mutex);
    events_->items.push_back(EventState::Item{
        .id = ++events_->sequence,
        .name = std::move(event_name),
        .data = std::move(json_data),
    });
    while (events_->items.size() > 128) events_->items.pop_front();
    events_->changed.notify_all();
}

void ApiController::register_routes() {
    const auto self = shared_from_this();

    drogon::app().registerHandler(
        "/health",
        [](HttpRequestPtr) -> drogon::Task<HttpResponsePtr> {
            const std::map<std::string, std::string> health{
                {"status", "ok"}, {"version", JOURNALSEED_VERSION}};
            co_return json_response(health);
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/setup/status",
        [self](HttpRequestPtr) -> drogon::Task<HttpResponsePtr> {
            co_return result_response(co_await self->service_->setup_status());
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/setup",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            if (auto issue = same_origin_issue(request)) co_return problem_response(*issue);
            auto input = parse_body<application::SetupRequest>(request);
            if (!input) co_return problem_response(input.error());
            auto result = co_await self->service_->setup(std::move(*input));
            if (!result) co_return problem_response(result.error());
            auto response = json_response(result->session, 201);
            add_session_cookie(response, std::move(result->cookieToken), request);
            co_return response;
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/login",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            if (auto issue = same_origin_issue(request)) co_return problem_response(*issue);
            auto input = parse_body<application::LoginRequest>(request);
            if (!input) co_return problem_response(input.error());
            auto result = co_await self->service_->login(std::move(*input));
            if (!result) co_return problem_response(result.error());
            auto response = json_response(result->session);
            add_session_cookie(response, std::move(result->cookieToken), request);
            co_return response;
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/session",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            co_return result_response(co_await self->service_->current_session(
                request->getCookie(std::string(kSessionCookie))));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/auth/logout",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto result = co_await self->service_->logout(
                request->getCookie(std::string(kSessionCookie)));
            if (!result) co_return problem_response(result.error());
            auto response = no_content_response();
            clear_session_cookie(response, request);
            co_return response;
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/ledgers",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            co_return result_response(co_await self->service_->ledgers());
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/ledgers",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto input = parse_body<application::CreateLedgerRequest>(request);
            if (!input) co_return problem_response(input.error());
            co_return result_response(
                co_await self->service_->create_ledger(std::move(*input)), 201);
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/summary",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            co_return result_response(co_await self->service_->summary(ledger_id));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/accounts",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            co_return result_response(co_await self->service_->accounts(ledger_id));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/accounts",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto input = parse_body<application::AccountInput>(request);
            if (!input) co_return problem_response(input.error());
            co_return result_response(
                co_await self->service_->create_account(ledger_id, std::move(*input)), 201);
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/categories",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            co_return result_response(co_await self->service_->categories(ledger_id));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/categories",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto input = parse_body<application::CategoryInput>(request);
            if (!input) co_return problem_response(input.error());
            co_return result_response(
                co_await self->service_->create_category(ledger_id, std::move(*input)), 201);
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/columns",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            const bool recycled = request->getParameter("recycled") == "true";
            co_return result_response(co_await self->service_->columns(ledger_id, recycled));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/columns",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto input = parse_body<application::ColumnInput>(request);
            if (!input) co_return problem_response(input.error());
            co_return result_response(
                co_await self->service_->create_column(ledger_id, std::move(*input)), 201);
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/columns/{columnId}",
        [self](HttpRequestPtr request,
               std::string column_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto patch = parse_body<application::ColumnPatch>(request);
            if (!patch) co_return problem_response(patch.error());
            co_return result_response(
                co_await self->service_->update_column(column_id, std::move(*patch)));
        },
        {drogon::Patch});

    drogon::app().registerHandler(
        "/api/v1/columns/{columnId}",
        [self](HttpRequestPtr request,
               std::string column_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto result = co_await self->service_->recycle_column(column_id);
            if (!result) co_return problem_response(result.error());
            co_return no_content_response();
        },
        {drogon::Delete});

    drogon::app().registerHandler(
        "/api/v1/columns/{columnId}/restore",
        [self](HttpRequestPtr request,
               std::string column_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto result = co_await self->service_->restore_column(column_id);
            if (!result) co_return problem_response(result.error());
            co_return no_content_response();
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/rows",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            const auto &sort_parameter = request->getParameter("sort");
            const auto sort = sort_parameter.empty() ? std::string_view{"date:desc"}
                                                     : std::string_view{sort_parameter};
            const bool recycled = request->getParameter("recycled") == "true";
            const auto &cursor_parameter = request->getParameter("cursor");
            const auto cursor = cursor_parameter.empty()
                                    ? std::optional<std::string_view>{}
                                    : std::optional<std::string_view>{cursor_parameter};
            co_return result_response(co_await self->service_->rows(
                ledger_id, row_limit(request), sort, recycled, cursor));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/ledgers/{ledgerId}/rows",
        [self](HttpRequestPtr request,
               std::string ledger_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto input = parse_body<application::RowInput>(request);
            if (!input) co_return problem_response(input.error());
            co_return result_response(co_await self->service_->create_row(
                ledger_id, auth->userId, std::move(*input)), 201);
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/rows/{rowId}",
        [self](HttpRequestPtr request,
               std::string row_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto input = parse_body<application::RowInput>(request);
            if (!input) co_return problem_response(input.error());
            co_return result_response(co_await self->service_->update_row(
                row_id, auth->userId, std::move(*input)));
        },
        {drogon::Patch});

    drogon::app().registerHandler(
        "/api/v1/rows/{rowId}",
        [self](HttpRequestPtr request,
               std::string row_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto result = co_await self->service_->recycle_row(row_id);
            if (!result) co_return problem_response(result.error());
            co_return no_content_response();
        },
        {drogon::Delete});

    drogon::app().registerHandler(
        "/api/v1/rows/{rowId}/restore",
        [self](HttpRequestPtr request,
               std::string row_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto result = co_await self->service_->restore_row(row_id);
            if (!result) co_return problem_response(result.error());
            co_return no_content_response();
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/functions",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            co_return json_response(self->service_->functions());
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/functions/{name}/invoke",
        [self](HttpRequestPtr request,
               std::string name) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto body = parse_body<glz::generic>(request);
            if (!body) co_return problem_response(body.error());
            if (!body->holds<glz::generic::object_t>()) {
                co_return problem_response(make_problem(
                    422, "invalid_function_input", "函数参数格式不正确", "参数需要是 JSON 对象"));
            }
            auto converted = generic_to_lua(*body);
            if (!converted) co_return problem_response(converted.error());
            auto result = self->service_->invoke_function(
                name, std::get<lua::LuaValue::Object>(converted->value));
            if (!result) co_return problem_response(result.error());
            co_return json_response(lua_to_generic(*result));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/jobs",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());
            co_return result_response(co_await self->service_->jobs());
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/jobs/{jobId}/cancel",
        [self](HttpRequestPtr request,
               std::string job_id) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, true);
            if (!auth) co_return problem_response(auth.error());
            auto result = co_await self->service_->cancel_job(job_id);
            if (!result) co_return problem_response(result.error());
            co_return no_content_response();
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/events",
        [self](HttpRequestPtr request) -> drogon::Task<HttpResponsePtr> {
            auto auth = co_await authorize(self->service_, request, false);
            if (!auth) co_return problem_response(auth.error());

            std::uint64_t last_id = 0;
            const auto &last_event_id = request->getHeader("Last-Event-ID");
            if (!last_event_id.empty()) {
                std::from_chars(last_event_id.data(),
                                last_event_id.data() + last_event_id.size(),
                                last_id);
            }
            auto state = self->events_;
            auto response = HttpResponse::newAsyncStreamResponse(
                [state, last_id](drogon::ResponseStreamPtr stream) mutable {
                    std::thread(
                        [state,
                         last_id,
                         stream = std::shared_ptr<drogon::ResponseStream>{
                             std::move(stream)}]() mutable {
                            if (!stream->send("event: connected\ndata: {}\n\n")) return;
                            while (true) {
                                std::vector<EventState::Item> pending;
                                {
                                    std::unique_lock lock(state->mutex);
                                    state->changed.wait_for(
                                        lock,
                                        std::chrono::seconds(15),
                                        [&] { return state->sequence > last_id; });
                                    for (const auto &event : state->items) {
                                        if (event.id > last_id) pending.push_back(event);
                                    }
                                }

                                if (pending.empty()) {
                                    if (!stream->send(": keepalive\n\n")) return;
                                    continue;
                                }
                                for (const auto &event : pending) {
                                    const auto message =
                                        "id: " + std::to_string(event.id) + "\n" +
                                        "event: " + event.name + "\n" +
                                        "data: " + event.data + "\n\n";
                                    if (!stream->send(message)) return;
                                    last_id = event.id;
                                }
                            }
                        })
                        .detach();
                },
                true);
            response->setContentTypeCodeAndCustomString(
                drogon::CT_TEXT_PLAIN, "text/event-stream; charset=utf-8");
            response->addHeader("Cache-Control", "no-cache, no-transform");
            response->addHeader("X-Accel-Buffering", "no");
            co_return response;
        },
        {drogon::Get});
}

}  // namespace journalseed::api
