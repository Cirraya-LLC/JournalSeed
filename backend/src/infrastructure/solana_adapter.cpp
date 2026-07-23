#include "journalseed/infrastructure/solana_adapter.h"

#include "journalseed/domain/chain.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>

#include <algorithm>
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
        throw std::runtime_error("Solana RPC JSON 解析失败: " + glz::format_error(error, body));
    }
    if (const auto rpc_error = member(root, "error"); rpc_error && rpc_error->holds<Object>()) {
        const auto message = string_member(*rpc_error, "message").value_or("Solana RPC error");
        throw std::runtime_error("Solana RPC 请求失败: " + message);
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
    if (mint == "Es9vMFrzaCERmJfrF4H2FYD4V5XUzs4Vc4YiKecEwP2") return "USDT";
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
        const auto mint = string_member(item, "mint");
        if (!mint) continue;
        const auto ui = member(item, "uiTokenAmount");
        if (!ui) continue;
        const auto raw = uint_from_decimal(string_member(*ui, "amount"));
        if (!raw) continue;
        const auto decimals = static_cast<std::int16_t>(int_member(*ui, "decimals").value_or(0));
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
}

drogon::Task<std::string> SolanaAdapter::post(std::string body) const {
    auto client = drogon::HttpClient::newHttpClient(options_.rpcUrl);
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(std::move(body));
    const auto response = co_await client->sendRequestCoro(request, 30.0);
    if (!response || response->statusCode() < 200 || response->statusCode() >= 300) {
        throw std::runtime_error("Solana RPC 请求失败");
    }
    co_return std::string(response->body());
}

drogon::Task<std::vector<ChainTransactionInput>>
SolanaAdapter::fetch_wallet_transactions(std::string_view normalized_address) const {
    const std::string address(normalized_address);
    const std::string signatures_body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getSignaturesForAddress\",\"params\":[\"" +
        json_escape(address) + "\",{\"limit\":" + std::to_string(options_.limit) + ",\"commitment\":\"finalized\"}]}";
    const auto signatures_root = parse_json(co_await post(signatures_body));
    const auto signatures = result_member(signatures_root);
    if (!signatures || !signatures->holds<Array>()) co_return std::vector<ChainTransactionInput>{};

    std::vector<ChainTransactionInput> output;
    for (const auto &item : signatures->get<Array>()) {
        if (const auto err = member(item, "err"); err && !err->holds<glz::generic::null_t>()) continue;
        const auto signature = string_member(item, "signature");
        if (!signature || signature->empty()) continue;
        const std::string transaction_body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getTransaction\",\"params\":[\"" +
            json_escape(*signature) + "\",{\"encoding\":\"jsonParsed\",\"commitment\":\"finalized\",\"maxSupportedTransactionVersion\":0}]}";
        const auto transaction_json = co_await post(transaction_body);
        const auto numbers = parse_transaction_numbers(transaction_json).value_or(SolanaTransactionNumbers{});
        const auto transaction_root = parse_json(transaction_json);
        const auto transaction = result_member(transaction_root);
        if (!transaction || transaction->holds<glz::generic::null_t>()) continue;
        auto parsed = parse_transaction_result(*transaction, numbers, *signature, address);
        if (parsed && !parsed->movements.empty()) output.push_back(std::move(*parsed));
    }
    co_return output;
}

}  // namespace journalseed::infrastructure
