#include "journalseed/infrastructure/solana_adapter.h"

#include "journalseed/domain/chain.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace journalseed::infrastructure {

using Generic = glz::generic;
using Object = glz::generic::object_t;
using Array = glz::generic::array_t;
using boost::multiprecision::cpp_int;

struct SolanaTransactionNumberMeta {
    std::vector<std::uint64_t> preBalances;
    std::vector<std::uint64_t> postBalances;
    std::uint64_t fee{0};
};

struct SolanaTransactionNumbers {
    std::optional<std::int64_t> slot;
    std::optional<std::int64_t> blockTime;
    std::optional<SolanaTransactionNumberMeta> meta;
};

struct SolanaTransactionNumberRoot {
    std::optional<SolanaTransactionNumbers> result;
};

namespace {

// 第三方响应先被解析成 glz::generic DOM、再逐条重新序列化进 rawJson，内存放大数倍，
// 因此在解析之前先按字节数封顶；对当前的 limit 上限来说 8 MiB 已经非常宽裕。
constexpr std::size_t kMaxResponseBytes = 8U * 1024U * 1024U;

// Content-Length 由上游控制，只在"明确声明超限"时提前拒绝；缺失或分块编码一律不可信，
// 最终以实际读到的字节数为准。
void reject_oversized_response(const drogon::HttpResponsePtr &response) {
    const auto declared = response->getHeader("content-length");
    if (!declared.empty()) {
        std::uint64_t length = 0;
        const auto *begin = declared.data();
        const auto *end = begin + declared.size();
        const auto [ptr, ec] = std::from_chars(begin, end, length);
        if (ec == std::errc{} && ptr == end && length > kMaxResponseBytes) {
            throw std::runtime_error("Solana RPC 响应超出大小上限");
        }
    }
    if (response->body().size() > kMaxResponseBytes) {
        throw std::runtime_error("Solana RPC 响应超出大小上限");
    }
}

const Generic *member(const Generic &value, std::string_view key) {
    if (!value.holds<Object>()) return nullptr;
    const auto &object = value.get<Object>();
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

std::optional<std::string> string_value(const Generic *value) {
    if (!value || !value->holds<std::string>()) return std::nullopt;
    return value->get<std::string>();
}

std::optional<std::string> string_member(const Generic &value, std::string_view key) {
    return string_value(member(value, key));
}

std::optional<std::int64_t> int_value(const Generic *value) {
    if (!value || !value->holds<double>()) return std::nullopt;
    return std::llround(value->get<double>());
}

std::optional<std::int64_t> int_member(const Generic &value, std::string_view key) {
    return int_value(member(value, key));
}

std::optional<cpp_int> uint_from_decimal(std::optional<std::string> value) {
    if (!value || value->empty()) return std::nullopt;
    cpp_int result = 0;
    for (const char character : *value) {
        if (character < '0' || character > '9') return std::nullopt;
        result *= 10;
        result += static_cast<unsigned>(character - '0');
    }
    return result;
}

std::string cpp_int_to_string(cpp_int value) {
    if (value == 0) return "0";
    if (value < 0) value = -value;
    std::string digits;
    while (value > 0) {
        const auto digit = static_cast<unsigned>(value % 10);
        digits.push_back(static_cast<char>('0' + digit));
        value /= 10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::optional<std::string> normalize_solana_address(std::optional<std::string> value) {
    if (!value || value->empty()) return std::nullopt;
    auto normalized = domain::chain::normalize_address("solana-mainnet", *value);
    if (!normalized) return std::nullopt;
    return *normalized;
}

std::string write_raw_json(const Generic &value) {
    std::string json;
    if (const auto error = glz::write_json(value, json); error) return "{}";
    return json;
}

Generic parse_json(std::string_view body) {
    Generic root;
    if (const auto error = glz::read_json(root, body); error) {
        // 上游正文不可信，只写进服务端日志，绝不回显给调用方或落库。
        LOG_ERROR << "Solana RPC JSON 解析失败: " << glz::format_error(error, body);
        throw std::runtime_error("Solana RPC 响应解析失败");
    }
    if (const auto rpc_error = member(root, "error"); rpc_error && rpc_error->holds<Object>()) {
        const auto message = string_member(*rpc_error, "message").value_or("Solana RPC error");
        LOG_ERROR << "Solana RPC 返回错误: " << message;
        throw std::runtime_error("Solana RPC 请求失败");
    }
    return root;
}

const Generic *result_member(const Generic &root) {
    return member(root, "result");
}

std::optional<std::string> account_key_pubkey(const Generic &key) {
    if (key.holds<std::string>()) return key.get<std::string>();
    if (key.holds<Object>()) return string_member(key, "pubkey");
    return std::nullopt;
}

std::vector<std::string> account_keys(const Generic &transaction) {
    const auto tx = member(transaction, "transaction");
    const auto message = tx ? member(*tx, "message") : nullptr;
    const auto keys = message ? member(*message, "accountKeys") : nullptr;
    std::vector<std::string> output;
    if (!keys || !keys->holds<Array>()) return output;
    for (const auto &key : keys->get<Array>()) {
        output.push_back(account_key_pubkey(key).value_or(""));
    }
    return output;
}

std::pair<std::optional<std::string>, std::optional<std::string>> find_system_transfer_counterparty(
    const Generic &transaction,
    std::string_view wallet) {
    const auto tx = member(transaction, "transaction");
    const auto message = tx ? member(*tx, "message") : nullptr;
    const auto instructions = message ? member(*message, "instructions") : nullptr;
    if (!instructions || !instructions->holds<Array>()) return {std::nullopt, std::nullopt};
    for (const auto &instruction : instructions->get<Array>()) {
        const auto parsed = member(instruction, "parsed");
        const auto info = parsed ? member(*parsed, "info") : nullptr;
        if (!info) continue;
        const auto source = normalize_solana_address(string_member(*info, "source"));
        const auto destination = normalize_solana_address(string_member(*info, "destination"));
        if (source && destination && (*source == wallet || *destination == wallet)) {
            return {source, destination};
        }
    }
    return {std::nullopt, std::nullopt};
}

std::string token_symbol(std::string_view mint) {
    if (mint == "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v") return "USDC";
    if (mint == "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB") return "USDT";
    return "SPL-" + domain::chain::short_address(mint);
}

std::optional<SolanaTransactionNumbers> parse_transaction_numbers(std::string_view body) {
    SolanaTransactionNumberRoot root;
    if (const auto error = glz::read_json(root, body); error) return std::nullopt;
    return std::move(root.result);
}

struct TokenBalanceSnapshot {
    std::string mint;
    std::int16_t decimals{0};
    cpp_int amount{0};
};

void collect_token_balances(const Generic *balances,
                            std::string_view wallet,
                            bool post,
                            std::map<std::string, std::pair<TokenBalanceSnapshot, TokenBalanceSnapshot>> &groups) {
    if (!balances || !balances->holds<Array>()) return;
    for (const auto &item : balances->get<Array>()) {
        const auto owner = normalize_solana_address(string_member(item, "owner"));
        if (!owner || *owner != wallet) continue;
        // mint 直接决定 assets.contract_address 与 symbol，必须先按 Solana 地址规范化，
        // 否则上游可以塞进任意长度、任意字节的字符串。
        const auto mint = normalize_solana_address(string_member(item, "mint"));
        if (!mint) continue;
        const auto ui = member(item, "uiTokenAmount");
        if (!ui) continue;
        const auto raw = uint_from_decimal(string_member(*ui, "amount"));
        if (!raw) continue;
        // assets.decimals 有 CHECK (decimals BETWEEN 0 AND 18)；越界值既无法落库，
        // 也会让金额换算失去意义，因此整条余额记录直接丢弃而不是截断。
        const auto raw_decimals = int_member(*ui, "decimals").value_or(-1);
        if (raw_decimals < 0 || raw_decimals > 18) continue;
        const auto decimals = static_cast<std::int16_t>(raw_decimals);
        const auto index = int_member(item, "accountIndex").value_or(0);
        const std::string key = *mint + "#" + std::to_string(index);
        auto &slot = groups[key];
        auto &snapshot = post ? slot.second : slot.first;
        snapshot.mint = *mint;
        snapshot.decimals = decimals;
        snapshot.amount = *raw;
    }
}

void add_token_movements(ChainTransactionInput &tx,
                         const Generic &transaction,
                         std::string_view wallet,
                         std::uint32_t &ordinal) {
    const auto meta = member(transaction, "meta");
    if (!meta) return;
    std::map<std::string, std::pair<TokenBalanceSnapshot, TokenBalanceSnapshot>> groups;
    collect_token_balances(member(*meta, "preTokenBalances"), wallet, false, groups);
    collect_token_balances(member(*meta, "postTokenBalances"), wallet, true, groups);
    for (const auto &[_, snapshots] : groups) {
        const auto &pre = snapshots.first;
        const auto &post = snapshots.second;
        const std::string mint = !post.mint.empty() ? post.mint : pre.mint;
        if (mint.empty()) continue;
        const std::int16_t decimals = post.decimals != 0 ? post.decimals : pre.decimals;
        const cpp_int delta = post.amount - pre.amount;
        if (delta == 0) continue;
        auto amount = domain::chain::base_units_to_decimal(cpp_int_to_string(delta), static_cast<std::uint8_t>(decimals));
        if (!amount) continue;
        ++ordinal;
        const std::string symbol = token_symbol(mint);
        const std::string direction = delta > 0 ? "incoming" : "outgoing";
        tx.movements.push_back(ChainAssetMovementInput{
            .movementKey = domain::chain::movement_key("solana-mainnet", tx.txHash, direction, symbol + ":" + mint, wallet, ordinal),
            .direction = direction,
            .assetSymbol = symbol,
            .assetName = symbol,
            .assetDecimals = decimals,
            .isNative = false,
            .contractAddress = mint,
            .contractAddressNormalized = mint,
            .amount = *amount,
            .fromAddress = delta < 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt,
            .fromAddressNormalized = delta < 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt,
            .toAddress = delta > 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt,
            .toAddressNormalized = delta > 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt,
        });
    }
}

std::optional<ChainTransactionInput> parse_transaction_result(const Generic &transaction,
                                                              const SolanaTransactionNumbers &numbers,
                                                              std::string_view signature,
                                                              std::string_view wallet) {
    const auto meta = member(transaction, "meta");
    if (!meta) return std::nullopt;
    if (const auto err = member(*meta, "err"); err && !err->holds<glz::generic::null_t>()) return std::nullopt;

    ChainTransactionInput tx{
        .txHash = std::string(signature),
        .blockNumber = numbers.slot,
        .blockTimestampMs = std::nullopt,
        .confirmed = true,
        .success = true,
        .rawJson = write_raw_json(transaction),
        .movements = {},
    };
    if (numbers.blockTime) tx.blockTimestampMs = *numbers.blockTime * 1000;

    std::uint32_t ordinal = 0;
    const auto keys = account_keys(transaction);
    const auto wallet_iter = std::ranges::find(keys, wallet);
    const bool wallet_in_keys = wallet_iter != keys.end();
    if (wallet_in_keys) {
        const auto wallet_index = static_cast<std::size_t>(wallet_iter - keys.begin());
        const auto pre = numbers.meta && wallet_index < numbers.meta->preBalances.size()
            ? std::optional<std::uint64_t>{numbers.meta->preBalances[wallet_index]}
            : std::nullopt;
        const auto post = numbers.meta && wallet_index < numbers.meta->postBalances.size()
            ? std::optional<std::uint64_t>{numbers.meta->postBalances[wallet_index]}
            : std::nullopt;
        const auto fee = numbers.meta ? numbers.meta->fee : std::uint64_t{0};
        const bool fee_payer = !keys.empty() && keys.front() == wallet;
        const auto [source, destination] = find_system_transfer_counterparty(transaction, wallet);
        if (pre && post) {
            cpp_int transfer_delta = cpp_int(*post) - cpp_int(*pre);
            if (fee_payer) transfer_delta += cpp_int(fee);
            if (transfer_delta != 0) {
                auto amount = domain::chain::base_units_to_decimal(cpp_int_to_string(transfer_delta), 9);
                if (amount) {
                    ++ordinal;
                    const std::string direction = transfer_delta > 0 ? "incoming" : "outgoing";
                    tx.movements.push_back(ChainAssetMovementInput{
                        .movementKey = domain::chain::movement_key("solana-mainnet", tx.txHash, direction, "SOL", wallet, ordinal),
                        .direction = direction,
                        .assetSymbol = "SOL",
                        .assetName = "Solana",
                        .assetDecimals = 9,
                        .isNative = true,
                        .contractAddress = std::nullopt,
                        .contractAddressNormalized = std::nullopt,
                        .amount = *amount,
                        .fromAddress = source ? source : (transfer_delta < 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt),
                        .fromAddressNormalized = source ? source : (transfer_delta < 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt),
                        .toAddress = destination ? destination : (transfer_delta > 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt),
                        .toAddressNormalized = destination ? destination : (transfer_delta > 0 ? std::optional<std::string>{std::string(wallet)} : std::nullopt),
                    });
                }
            }
        }
        if (fee_payer && fee > 0) {
            auto amount = domain::chain::base_units_to_decimal(cpp_int_to_string(cpp_int(fee)), 9);
            if (amount) {
                ++ordinal;
                tx.movements.push_back(ChainAssetMovementInput{
                    .movementKey = domain::chain::movement_key("solana-mainnet", tx.txHash, "fee", "SOL", wallet, ordinal),
                    .direction = "fee",
                    .assetSymbol = "SOL",
                    .assetName = "Solana",
                    .assetDecimals = 9,
                    .isNative = true,
                    .contractAddress = std::nullopt,
                    .contractAddressNormalized = std::nullopt,
                    .amount = *amount,
                    .fromAddress = std::string(wallet),
                    .fromAddressNormalized = std::string(wallet),
                    .toAddress = std::nullopt,
                    .toAddressNormalized = std::nullopt,
                });
            }
        }
    }

    add_token_movements(tx, transaction, wallet, ordinal);
    return tx;
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else escaped.push_back(c);
    }
    return escaped;
}

}  // namespace

SolanaAdapter::SolanaAdapter(SolanaAdapterOptions options) : options_(std::move(options)) {
    if (options_.rpcUrl.empty()) options_.rpcUrl = "https://api.mainnet-beta.solana.com";
    while (options_.rpcUrl.ends_with('/')) options_.rpcUrl.pop_back();
    options_.limit = std::clamp<std::uint16_t>(options_.limit, 1, 1000);
    options_.requestTimeoutSeconds = std::clamp(options_.requestTimeoutSeconds, 1.0, 60.0);
    options_.fetchBudgetSeconds =
        std::clamp(options_.fetchBudgetSeconds, options_.requestTimeoutSeconds, 300.0);
}

drogon::Task<std::string> SolanaAdapter::post(std::string body, double timeout_seconds) const {
    auto client = drogon::HttpClient::newHttpClient(options_.rpcUrl);
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(std::move(body));
    const auto response = co_await client->sendRequestCoro(request, timeout_seconds);
    if (!response || response->statusCode() < 200 || response->statusCode() >= 300) {
        if (response) LOG_ERROR << "Solana RPC 请求失败，HTTP 状态码 " << response->statusCode();
        throw std::runtime_error("Solana RPC 请求失败");
    }
    reject_oversized_response(response);
    co_return std::string(response->body());
}

drogon::Task<std::vector<ChainTransactionInput>>
SolanaAdapter::fetch_wallet_transactions(std::string_view normalized_address,
                                         const ChainFetchCursor &cursor) const {
    const std::string address(normalized_address);
    // 整轮抓取的墙钟预算：慢速或恶意端点最多消耗这么久，之后返回已取得的部分结果，
    // 而不是让 HTTP 请求和自动同步调度器一起挂起。
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(options_.fetchBudgetSeconds));
    const auto remaining_seconds = [&deadline] {
        return std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
    };

    const bool resume = cursor.signature && !cursor.signature->empty();
    const int max_pages = resume ? 8 : 1;
    std::vector<ChainTransactionInput> output;
    std::optional<std::string> before;
    for (int page = 0; page < max_pages; ++page) {
        const auto left = remaining_seconds();
        if (left <= 0.0) {
            LOG_WARN << "Solana 同步超出 " << options_.fetchBudgetSeconds
                     << " 秒预算，返回部分结果：已解析 " << output.size() << " 笔交易";
            break;
        }
        std::string options_json = "{\"limit\":" + std::to_string(options_.limit) +
            ",\"commitment\":\"finalized\"";
        if (resume) {
            options_json += ",\"until\":\"" + json_escape(*cursor.signature) + "\"";
        }
        if (before) {
            options_json += ",\"before\":\"" + json_escape(*before) + "\"";
        }
        options_json += "}";
        const std::string signatures_body =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getSignaturesForAddress\",\"params\":[\"" +
            json_escape(address) + "\"," + options_json + "]}";
        const auto signatures_root = parse_json(
            co_await post(signatures_body, std::min(options_.requestTimeoutSeconds, left)));
        const auto signatures = result_member(signatures_root);
        if (!signatures || !signatures->holds<Array>() || signatures->get<Array>().empty()) break;

        std::optional<std::string> oldest;
        std::size_t seen = 0;
        for (const auto &item : signatures->get<Array>()) {
            const auto remaining = remaining_seconds();
            if (remaining <= 0.0) {
                LOG_WARN << "Solana 同步超出 " << options_.fetchBudgetSeconds
                         << " 秒预算，返回部分结果：已解析 " << output.size() << " 笔交易";
                co_return output;
            }
            ++seen;
            if (const auto err = member(item, "err"); err && !err->holds<glz::generic::null_t>()) continue;
            const auto signature = string_member(item, "signature");
            if (!signature || signature->empty()) continue;
            oldest = *signature;
            const std::string transaction_body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"" +
                json_escape(*signature) + "\",{\"encoding\":\"jsonParsed\",\"commitment\":\"finalized\",\"maxSupportedTransactionVersion\":0}]}";
            const auto transaction_json =
                co_await post(transaction_body, std::min(options_.requestTimeoutSeconds, remaining));
            const auto numbers = parse_transaction_numbers(transaction_json).value_or(SolanaTransactionNumbers{});
            const auto transaction_root = parse_json(transaction_json);
            const auto transaction = result_member(transaction_root);
            if (!transaction || transaction->holds<glz::generic::null_t>()) continue;
            auto parsed = parse_transaction_result(*transaction, numbers, *signature, address);
            if (parsed && !parsed->movements.empty()) output.push_back(std::move(*parsed));
        }
        if (!resume || !oldest || seen < options_.limit) break;
        before = std::move(oldest);
    }
    co_return output;
}

}  // namespace journalseed::infrastructure
