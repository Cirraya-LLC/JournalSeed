#include "journalseed/infrastructure/tron_adapter.h"

#include "journalseed/domain/tron.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace journalseed::infrastructure {
namespace {

using Generic = glz::generic;
using Object = glz::generic::object_t;
using Array = glz::generic::array_t;

const Generic *member(const Generic &value, std::string_view key) {
    if (!value.holds<Object>()) return nullptr;
    const auto &object = value.get<Object>();
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

const Generic *at(const Generic &value, std::size_t index) {
    if (!value.holds<Array>()) return nullptr;
    const auto &array = value.get<Array>();
    return index < array.size() ? &array[index] : nullptr;
}

std::optional<std::string> string_value(const Generic *value) {
    if (!value || !value->holds<std::string>()) return std::nullopt;
    return value->get<std::string>();
}

std::optional<std::string> string_member(const Generic &value, std::string_view key) {
    return string_value(member(value, key));
}

std::optional<double> number_value(const Generic *value) {
    if (!value || !value->holds<double>()) return std::nullopt;
    return value->get<double>();
}

std::optional<double> number_member(const Generic &value, std::string_view key) {
    return number_value(member(value, key));
}

std::optional<std::int64_t> integer_member(const Generic &value, std::string_view key) {
    const auto number = number_member(value, key);
    if (!number) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(*number));
}

std::optional<std::int16_t> i16_from_string(std::optional<std::string> value) {
    if (!value || value->empty()) return std::nullopt;
    int result = 0;
    for (const char character : *value) {
        if (character < '0' || character > '9') return std::nullopt;
        result = result * 10 + (character - '0');
        if (result > 18) return std::nullopt;
    }
    return static_cast<std::int16_t>(result);
}

std::optional<std::string> integer_string(const Generic &value, std::string_view key) {
    if (auto text = string_member(value, key)) return text;
    const auto number = number_member(value, key);
    if (!number) return std::nullopt;
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(0);
    stream << *number;
    return stream.str();
}

std::string write_raw_json(const Generic &value) {
    std::string json;
    if (const auto error = glz::write_json(value, json); error) return "{}";
    return json;
}

std::optional<std::string> normalize_tron_address(std::optional<std::string> value) {
    if (!value || value->empty()) return std::nullopt;
    auto normalized = domain::tron::normalize_address(*value);
    if (!normalized) return std::nullopt;
    return *normalized;
}

std::string movement_key(std::string_view tx_hash,
                         std::string_view kind,
                         std::string_view asset_identifier,
                         std::string_view wallet,
                         std::uint32_t ordinal) {
    return domain::tron::movement_key("tron-mainnet", tx_hash, kind, asset_identifier, wallet, ordinal);
}

std::optional<ChainTransactionInput> parse_native_transaction(const Generic &item,
                                                              std::string_view wallet) {
    const auto tx_hash = string_member(item, "txID").or_else([&] { return string_member(item, "hash"); });
    if (!tx_hash || tx_hash->empty()) return std::nullopt;

    ChainTransactionInput tx{
        .txHash = *tx_hash,
        .blockNumber = integer_member(item, "blockNumber"),
        .blockTimestampMs = integer_member(item, "block_timestamp"),
        .confirmed = true,
        .success = true,
        .rawJson = write_raw_json(item),
        .movements = {},
    };
    if (const auto ret = member(item, "ret")) {
        if (const auto first = at(*ret, 0)) {
            if (auto contract_ret = string_member(*first, "contractRet")) {
                tx.success = *contract_ret == "SUCCESS";
            }
        }
    }
    if (!tx.success) return tx;

    std::uint32_t ordinal = 0;
    const auto raw = member(item, "raw_data");
    const auto contracts = raw ? member(*raw, "contract") : nullptr;
    if (contracts && contracts->holds<Array>()) {
        for (const auto &contract : contracts->get<Array>()) {
            const auto type = string_member(contract, "type");
            const auto parameter = member(contract, "parameter");
            const auto payload = parameter ? member(*parameter, "value") : nullptr;
            if (!payload) continue;

            const auto owner = normalize_tron_address(string_member(*payload, "owner_address"));
            const auto target = normalize_tron_address(string_member(*payload, "to_address"));
            if (type && *type == "TransferContract" && owner && target) {
                const auto amount_sun = integer_string(*payload, "amount");
                if (amount_sun) {
                    auto amount = domain::tron::base_units_to_decimal(*amount_sun, 6);
                    if (amount) {
                        const bool from_wallet = *owner == wallet;
                        const bool to_wallet = *target == wallet;
                        if (from_wallet || to_wallet) {
                            ++ordinal;
                            const std::string direction = from_wallet && to_wallet
                                ? "internal"
                                : (to_wallet ? "incoming" : "outgoing");
                            tx.movements.push_back(ChainAssetMovementInput{
                                .movementKey = movement_key(*tx_hash, direction, "TRX", wallet, ordinal),
                                .direction = direction,
                                .assetSymbol = "TRX",
                                .assetName = "TRON",
                                .assetDecimals = 6,
                                .isNative = true,
                                .contractAddress = std::nullopt,
                                .contractAddressNormalized = std::nullopt,
                                .amount = *amount,
                                .fromAddress = *owner,
                                .fromAddressNormalized = *owner,
                                .toAddress = *target,
                                .toAddressNormalized = *target,
                            });
                        }
                    }
                }
            }
        }
    }

    std::optional<std::string> fee_sun;
    if (const auto ret = member(item, "ret")) {
        if (const auto first = at(*ret, 0)) fee_sun = integer_string(*first, "fee");
    }
    if (!fee_sun) fee_sun = integer_string(item, "fee");
    if (fee_sun) {
        auto fee = domain::tron::base_units_to_decimal(*fee_sun, 6);
        if (fee && *fee != "0.00" && *fee != "0") {
            ++ordinal;
            tx.movements.push_back(ChainAssetMovementInput{
                .movementKey = movement_key(*tx_hash, "fee", "TRX", wallet, ordinal),
                .direction = "fee",
                .assetSymbol = "TRX",
                .assetName = "TRON",
                .assetDecimals = 6,
                .isNative = true,
                .contractAddress = std::nullopt,
                .contractAddressNormalized = std::nullopt,
                .amount = *fee,
                .fromAddress = std::string(wallet),
                .fromAddressNormalized = std::string(wallet),
                .toAddress = std::nullopt,
                .toAddressNormalized = std::nullopt,
            });
        }
    }

    return tx;
}

std::optional<ChainTransactionInput> parse_trc20_transaction(const Generic &item,
                                                             std::string_view wallet) {
    const auto tx_hash = string_member(item, "transaction_id").or_else([&] {
        return string_member(item, "txID");
    });
    if (!tx_hash || tx_hash->empty()) return std::nullopt;
    const auto from = normalize_tron_address(string_member(item, "from"));
    const auto target = normalize_tron_address(string_member(item, "to"));
    if (!from || !target) return std::nullopt;

    const auto token_info = member(item, "token_info");
    const auto symbol = token_info ? string_member(*token_info, "symbol").value_or("TRC20") : "TRC20";
    const auto name = token_info ? string_member(*token_info, "name").value_or(symbol) : symbol;
    const auto decimals = token_info ? i16_from_string(string_member(*token_info, "decimals")).value_or(std::int16_t{6}) : std::int16_t{6};
    const auto contract_address = token_info ? string_member(*token_info, "address") : std::nullopt;
    const auto contract_normalized = normalize_tron_address(contract_address);
    const auto raw_value = integer_string(item, "value");
    if (!raw_value) return std::nullopt;
    auto amount = domain::tron::base_units_to_decimal(*raw_value, static_cast<std::uint8_t>(decimals));
    if (!amount) return std::nullopt;

    const bool from_wallet = *from == wallet;
    const bool to_wallet = *target == wallet;
    if (!from_wallet && !to_wallet) return std::nullopt;
    const std::string direction = from_wallet && to_wallet ? "internal" : (to_wallet ? "incoming" : "outgoing");
    const auto asset_identifier = contract_normalized.value_or(symbol);

    ChainTransactionInput tx{
        .txHash = *tx_hash,
        .blockNumber = integer_member(item, "block"),
        .blockTimestampMs = integer_member(item, "block_timestamp"),
        .confirmed = true,
        .success = true,
        .rawJson = write_raw_json(item),
        .movements = {},
    };
    tx.movements.push_back(ChainAssetMovementInput{
        .movementKey = movement_key(*tx_hash, direction, asset_identifier, wallet, 1),
        .direction = direction,
        .assetSymbol = symbol,
        .assetName = name,
        .assetDecimals = decimals,
        .isNative = false,
        .contractAddress = contract_address,
        .contractAddressNormalized = contract_normalized,
        .amount = *amount,
        .fromAddress = *from,
        .fromAddressNormalized = *from,
        .toAddress = *target,
        .toAddressNormalized = *target,
    });
    return tx;
}

std::vector<ChainTransactionInput> parse_response(std::string_view body,
                                                  std::string_view wallet,
                                                  bool trc20) {
    Generic root;
    if (const auto error = glz::read_json(root, body); error) {
        throw std::runtime_error("TronGrid JSON 解析失败: " + glz::format_error(error, body));
    }
    const auto data = member(root, "data");
    if (!data || !data->holds<Array>()) return {};
    std::vector<ChainTransactionInput> result;
    for (const auto &item : data->get<Array>()) {
        auto parsed = trc20 ? parse_trc20_transaction(item, wallet)
                            : parse_native_transaction(item, wallet);
        if (parsed && !parsed->movements.empty()) result.push_back(std::move(*parsed));
    }
    return result;
}

}  // namespace

TronAdapter::TronAdapter(TronAdapterOptions options) : options_(std::move(options)) {
    if (options_.baseUrl.empty()) options_.baseUrl = "https://api.trongrid.io";
    while (options_.baseUrl.ends_with('/')) options_.baseUrl.pop_back();
    options_.limit = std::clamp<std::uint16_t>(options_.limit, 1, 200);
}

drogon::Task<std::string> TronAdapter::get(std::string path) const {
    auto client = drogon::HttpClient::newHttpClient(options_.baseUrl);
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(std::move(path));
    if (options_.apiKey && !options_.apiKey->empty()) {
        request->addHeader("TRON-PRO-API-KEY", *options_.apiKey);
    }
    const auto response = co_await client->sendRequestCoro(request, 20.0);
    if (!response || response->statusCode() < 200 || response->statusCode() >= 300) {
        throw std::runtime_error("TronGrid 请求失败");
    }
    co_return std::string(response->body());
}

drogon::Task<std::vector<ChainTransactionInput>>
TronAdapter::fetch_wallet_transactions(std::string_view normalized_address) const {
    const std::string address(normalized_address);
    const auto limit = std::to_string(options_.limit);
    const auto native_path = "/v1/accounts/" + address +
        "/transactions?only_confirmed=true&limit=" + limit + "&order_by=block_timestamp,desc";
    const auto trc20_path = "/v1/accounts/" + address +
        "/transactions/trc20?only_confirmed=true&limit=" + limit + "&order_by=block_timestamp,desc";

    auto native = parse_response(co_await get(native_path), address, false);
    auto trc20 = parse_response(co_await get(trc20_path), address, true);
    native.reserve(native.size() + trc20.size());
    for (auto &transaction : trc20) native.push_back(std::move(transaction));
    co_return native;
}

}  // namespace journalseed::infrastructure
