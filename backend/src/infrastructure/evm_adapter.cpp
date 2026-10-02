#include "journalseed/infrastructure/evm_adapter.h"

#include "journalseed/domain/chain.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <glaze/glaze.hpp>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace journalseed::infrastructure {
namespace {

using Generic = glz::generic;
using Object = glz::generic::object_t;
using Array = glz::generic::array_t;
using boost::multiprecision::cpp_int;

// 第三方响应先被解析成 glz::generic DOM、再逐条重新序列化进 rawJson，内存放大数倍，
// 因此在解析之前先按字节数封顶；对当前的 offset 上限来说 8 MiB 已经非常宽裕。
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
            throw std::runtime_error("Etherscan 响应超出大小上限");
        }
    }
    if (response->body().size() > kMaxResponseBytes) {
        throw std::runtime_error("Etherscan 响应超出大小上限");
    }
}

// assets.symbol / assets.name 在数据库层有 length(btrim(...)) BETWEEN 1 AND 24 / 1 AND 120
// 的 CHECK，并且必须是合法 UTF-8；恶意代币合约可以借超长或畸形字段让整个同步事务中止，
// 形成针对单个钱包的持久性拒绝服务，因此在适配器边界就截断到字符（而非字节）上限。
std::string bounded_text(std::optional<std::string> value,
                         std::size_t max_characters,
                         std::string_view fallback) {
    static constexpr std::uint32_t minimum_code[5] = {0, 0, 0x80U, 0x800U, 0x10000U};
    const std::string_view text = value ? std::string_view(*value) : std::string_view{};
    std::string output;
    std::size_t characters = 0;
    std::size_t index = 0;
    while (index < text.size() && characters < max_characters) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t width = 0;
        std::uint32_t code = 0;
        if (lead < 0x80U) { width = 1; code = lead; }
        else if ((lead & 0xE0U) == 0xC0U) { width = 2; code = lead & 0x1FU; }
        else if ((lead & 0xF0U) == 0xE0U) { width = 3; code = lead & 0x0FU; }
        else if ((lead & 0xF8U) == 0xF0U) { width = 4; code = lead & 0x07U; }
        else break;
        if (index + width > text.size()) break;
        bool valid = true;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) { valid = false; break; }
            code = (code << 6U) | (continuation & 0x3FU);
        }
        if (!valid) break;
        if (code < minimum_code[width] || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) break;
        index += width;
        if (code < 0x20U || code == 0x7FU) continue;  // 控制字符直接丢弃
        output.append(text.substr(index - width, width));
        ++characters;
    }
    const auto first = output.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string(fallback);
    output.erase(0, first);
    output.erase(output.find_last_not_of(" \t") + 1);
    if (output.empty()) return std::string(fallback);
    return output;
}

const Generic *member(const Generic &value, std::string_view key) {
    if (!value.holds<Object>()) return nullptr;
    const auto &object = value.get<Object>();
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

std::optional<std::string> string_value(const Generic *value) {
    if (!value) return std::nullopt;
    if (value->holds<std::string>()) return value->get<std::string>();
    if (value->holds<double>()) {
        const auto number = static_cast<std::int64_t>(value->get<double>());
        return std::to_string(number);
    }
    return std::nullopt;
}

std::optional<std::string> string_member(const Generic &value, std::string_view key) {
    return string_value(member(value, key));
}

std::optional<std::int64_t> i64_from_decimal(std::optional<std::string> value) {
    if (!value || value->empty()) return std::nullopt;
    std::int64_t result = 0;
    const auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), result);
    if (ec != std::errc{} || ptr != value->data() + value->size()) return std::nullopt;
    return result;
}

std::optional<std::int16_t> i16_from_decimal(std::optional<std::string> value) {
    const auto parsed = i64_from_decimal(std::move(value));
    if (!parsed || *parsed < 0 || *parsed > 18) return std::nullopt;
    return static_cast<std::int16_t>(*parsed);
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
    std::string digits;
    while (value > 0) {
        const auto digit = static_cast<unsigned>(value % 10);
        digits.push_back(static_cast<char>('0' + digit));
        value /= 10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::optional<std::string> normalize_evm_address(std::optional<std::string> value,
                                                 std::string_view chain_code) {
    if (!value || value->empty()) return std::nullopt;
    auto normalized = domain::chain::normalize_address(chain_code, *value);
    if (!normalized) return std::nullopt;
    return *normalized;
}

std::string write_raw_json(const Generic &value) {
    std::string json;
    if (const auto error = glz::write_json(value, json); error) return "{}";
    return json;
}

bool tx_success(const Generic &item) {
    if (string_member(item, "isError").value_or("0") == "1") return false;
    const auto receipt = string_member(item, "txreceipt_status");
    return !receipt || receipt->empty() || *receipt == "1";
}

void add_fee_if_outgoing(ChainTransactionInput &tx,
                         const Generic &item,
                         std::string_view wallet,
                         const EvmAdapterOptions &options,
                         std::uint32_t &ordinal) {
    const auto from = normalize_evm_address(string_member(item, "from"), options.chainCode);
    if (!from || *from != wallet) return;
    const auto gas_used = uint_from_decimal(string_member(item, "gasUsed"));
    const auto gas_price = uint_from_decimal(string_member(item, "gasPrice"));
    if (!gas_used || !gas_price) return;
    const auto raw_fee = *gas_used * *gas_price;
    if (raw_fee == 0) return;
    auto fee = domain::chain::base_units_to_decimal(cpp_int_to_string(raw_fee), 18);
    if (!fee || *fee == "0" || *fee == "0.00") return;
    ++ordinal;
    tx.movements.push_back(ChainAssetMovementInput{
        .movementKey = domain::chain::movement_key(options.chainCode, tx.txHash, "fee", options.nativeSymbol, wallet, ordinal),
        .direction = "fee",
        .assetSymbol = options.nativeSymbol,
        .assetName = options.nativeName,
        .assetDecimals = 18,
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

std::optional<ChainTransactionInput> parse_native_transaction(const Generic &item,
                                                              std::string_view wallet,
                                                              const EvmAdapterOptions &options) {
    const auto tx_hash = string_member(item, "hash");
    if (!tx_hash || tx_hash->empty()) return std::nullopt;

    ChainTransactionInput tx{
        .txHash = *tx_hash,
        .blockNumber = i64_from_decimal(string_member(item, "blockNumber")),
        .blockTimestampMs = std::nullopt,
        .confirmed = true,
        .success = tx_success(item),
        .rawJson = write_raw_json(item),
        .movements = {},
    };
    if (const auto seconds = i64_from_decimal(string_member(item, "timeStamp"))) {
        tx.blockTimestampMs = *seconds * 1000;
    }
    if (!tx.success) return tx;

    std::uint32_t ordinal = 0;
    const auto from = normalize_evm_address(string_member(item, "from"), options.chainCode);
    const auto to = normalize_evm_address(string_member(item, "to"), options.chainCode);
    const auto raw_value = string_member(item, "value");
    if (from && to && raw_value && *raw_value != "0") {
        auto amount = domain::chain::base_units_to_decimal(*raw_value, 18);
        if (amount) {
            const bool from_wallet = *from == wallet;
            const bool to_wallet = *to == wallet;
            if (from_wallet || to_wallet) {
                ++ordinal;
                const std::string direction = from_wallet && to_wallet ? "internal" : (to_wallet ? "incoming" : "outgoing");
                tx.movements.push_back(ChainAssetMovementInput{
                    .movementKey = domain::chain::movement_key(options.chainCode, *tx_hash, direction, options.nativeSymbol, wallet, ordinal),
                    .direction = direction,
                    .assetSymbol = options.nativeSymbol,
                    .assetName = options.nativeName,
                    .assetDecimals = 18,
                    .isNative = true,
                    .contractAddress = std::nullopt,
                    .contractAddressNormalized = std::nullopt,
                    .amount = *amount,
                    .fromAddress = *from,
                    .fromAddressNormalized = *from,
                    .toAddress = *to,
                    .toAddressNormalized = *to,
                });
            }
        }
    }
    add_fee_if_outgoing(tx, item, wallet, options, ordinal);
    return tx;
}

// The movement key's ordinal for a token transfer. It must not depend on which provider
// served the transfer or in what order: Etherscan reports logIndex but Blockscout does not,
// and a wallet can move between the two when an API key is added or removed — any
// difference would book the checkpoint block's transfers a second time. So the ordinal is a
// hash of what identifies the transfer within its transaction. Two transfers identical in
// all of those fields are indistinguishable, so bumping the later one is order-independent.
std::uint32_t transfer_ordinal(std::string_view contract, std::string_view from,
                               std::string_view to, std::string_view raw_value,
                               std::string_view tx_hash, std::set<std::string> &used) {
    std::uint32_t hash = 2166136261U;
    for (const auto part : {contract, from, to, raw_value}) {
        for (const auto character : part) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 16777619U;
        }
        hash ^= 0x1FU;
        hash *= 16777619U;
    }
    // Keep clear of the small ordinals the native/fee movements of the same transaction use.
    std::uint32_t ordinal = 1000U + hash % 4000000000U;
    while (!used.insert(std::string(tx_hash) + ':' + std::to_string(ordinal)).second) ++ordinal;
    return ordinal;
}

std::optional<ChainTransactionInput> parse_erc20_transaction(const Generic &item,
                                                             std::string_view wallet,
                                                             const EvmAdapterOptions &options,
                                                             std::set<std::string> &used_ordinals) {
    const auto tx_hash = string_member(item, "hash");
    if (!tx_hash || tx_hash->empty()) return std::nullopt;
    const auto from = normalize_evm_address(string_member(item, "from"), options.chainCode);
    const auto to = normalize_evm_address(string_member(item, "to"), options.chainCode);
    if (!from || !to) return std::nullopt;
    const bool from_wallet = *from == wallet;
    const bool to_wallet = *to == wallet;
    if (!from_wallet && !to_wallet) return std::nullopt;

    const auto decimals = i16_from_decimal(string_member(item, "tokenDecimal")).value_or(std::int16_t{18});
    const auto raw_value = string_member(item, "value");
    if (!raw_value) return std::nullopt;
    auto amount = domain::chain::base_units_to_decimal(*raw_value, static_cast<std::uint8_t>(decimals));
    if (!amount) return std::nullopt;

    const auto contract = normalize_evm_address(string_member(item, "contractAddress"), options.chainCode);
    const auto symbol = bounded_text(string_member(item, "tokenSymbol"), 24, "ERC20");
    const auto name = bounded_text(string_member(item, "tokenName"), 120, symbol);
    const auto ordinal = transfer_ordinal(contract ? std::string_view(*contract) : std::string_view(symbol),
                                          *from, *to, *raw_value, *tx_hash, used_ordinals);
    const std::string direction = from_wallet && to_wallet ? "internal" : (to_wallet ? "incoming" : "outgoing");
    const std::string asset_identifier = contract ? *contract : symbol;

    ChainTransactionInput tx{
        .txHash = *tx_hash,
        .blockNumber = i64_from_decimal(string_member(item, "blockNumber")),
        .blockTimestampMs = std::nullopt,
        .confirmed = true,
        .success = true,
        .rawJson = write_raw_json(item),
        .movements = {},
    };
    if (const auto seconds = i64_from_decimal(string_member(item, "timeStamp"))) {
        tx.blockTimestampMs = *seconds * 1000;
    }
    tx.movements.push_back(ChainAssetMovementInput{
        .movementKey = domain::chain::movement_key(options.chainCode, *tx_hash, direction, asset_identifier, wallet, ordinal),
        .direction = direction,
        .assetSymbol = symbol,
        .assetName = name,
        .assetDecimals = decimals,
        .isNative = false,
        .contractAddress = contract,
        .contractAddressNormalized = contract,
        .amount = *amount,
        .fromAddress = *from,
        .fromAddressNormalized = *from,
        .toAddress = *to,
        .toAddressNormalized = *to,
    });
    return tx;
}

std::vector<ChainTransactionInput> parse_response(std::string_view body,
                                                  std::string_view wallet,
                                                  const EvmAdapterOptions &options,
                                                  bool erc20,
                                                  std::set<std::string> &used_ordinals) {
    Generic root;
    if (const auto error = glz::read_json(root, body); error) {
        // 上游正文不可信，只写进服务端日志，绝不回显给调用方或落库。
        LOG_ERROR << options.providerName << " JSON 解析失败: " << glz::format_error(error, body);
        throw std::runtime_error(options.providerName + " 响应解析失败");
    }
    const auto result = member(root, "result");
    if (!result || !result->holds<Array>()) {
        const auto status = string_member(root, "status").value_or("");
        const auto message = string_member(root, "message").value_or("");
        if (status == "0" && (message == "No transactions found" || message == "No records found")) return {};
        if (status == "0" && result && result->holds<std::string>() && result->get<std::string>().find("No transactions") != std::string::npos) return {};
        if (status == "0" && message.find("No token transfers") != std::string::npos) return {};
        LOG_ERROR << options.providerName << " 请求未返回交易数组，上游 status=" << status
                  << " message=" << message;
        throw std::runtime_error(options.providerName + " 请求未返回交易数组");
    }
    std::vector<ChainTransactionInput> output;
    for (const auto &item : result->get<Array>()) {
        auto parsed = erc20 ? parse_erc20_transaction(item, wallet, options, used_ordinals)
                            : parse_native_transaction(item, wallet, options);
        if (parsed && !parsed->movements.empty()) output.push_back(std::move(*parsed));
    }
    return output;
}

}  // namespace

EvmAdapter::EvmAdapter(EvmAdapterOptions options) : options_(std::move(options)) {
    if (options_.baseUrl.empty()) options_.baseUrl = "https://api.etherscan.io";
    while (options_.baseUrl.ends_with('/')) options_.baseUrl.pop_back();
    options_.limit = std::clamp<std::uint16_t>(options_.limit, 1, 10000);
}

drogon::Task<std::string> EvmAdapter::get(std::string query) const {
    auto client = drogon::HttpClient::newHttpClient(options_.baseUrl);
    const auto path = options_.apiPath + "?" + std::move(query);
    // Keyless providers (Blockscout) rate-limit bursts with HTTP 429; one sync makes several
    // calls back to back, so back off 1 s, 2 s, 4 s before giving up on the pass.
    for (int attempt = 0;; ++attempt) {
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setMethod(drogon::Get);
        request->setPath(path);
        const auto response = co_await client->sendRequestCoro(request, 30.0);
        if (response && response->statusCode() == drogon::k429TooManyRequests && attempt < 3) {
            LOG_WARN << options_.providerName << " 限流（HTTP 429），第 " << attempt + 1 << " 次重试";
            auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
            if (!loop) loop = drogon::app().getLoop();
            co_await drogon::sleepCoro(loop, static_cast<double>(1 << attempt));
            continue;
        }
        if (!response || response->statusCode() < 200 || response->statusCode() >= 300) {
            if (response) {
                LOG_ERROR << options_.providerName << " 请求失败，HTTP 状态码 " << response->statusCode();
            }
            throw std::runtime_error(options_.providerName + " 请求失败");
        }
        reject_oversized_response(response);
        co_return std::string(response->body());
    }
}

drogon::Task<std::vector<ChainTransactionInput>>
EvmAdapter::fetch_action_pages(std::string_view address, std::string_view action,
                               const ChainFetchCursor &cursor) const {
    const std::string api_key = options_.apiKey && !options_.apiKey->empty()
        ? "&apikey=" + *options_.apiKey : std::string{};
    const bool resume = cursor.block && *cursor.block > 0;
    const std::string sort = resume ? "asc" : "desc";
    std::string shared = "chainid=" + std::to_string(options_.chainId) +
        "&module=account&address=" + std::string(address) +
        "&offset=" + std::to_string(options_.limit) + "&sort=" + sort +
        "&action=" + std::string(action) + api_key;
    if (resume) shared += "&startblock=" + std::to_string(*cursor.block);
    const int max_pages = resume ? 8 : 1;
    std::vector<ChainTransactionInput> output;
    std::set<std::string> used_ordinals;
    for (int page = 1; page <= max_pages; ++page) {
        auto parsed = parse_response(
            co_await get(shared + "&page=" + std::to_string(page)), address, options_,
            action == "tokentx", used_ordinals);
        if (parsed.empty()) break;
        output.insert(output.end(), parsed.begin(), parsed.end());
        if (!resume || parsed.size() < options_.limit) break;
    }
    co_return output;
}

drogon::Task<std::vector<ChainTransactionInput>>
EvmAdapter::fetch_wallet_transactions(std::string_view normalized_address,
                                      const ChainFetchCursor &cursor) const {
    const std::string address(normalized_address);
    std::vector<ChainTransactionInput> native;
    if (options_.includeNative) native = co_await fetch_action_pages(address, "txlist", cursor);
    auto tokens = co_await fetch_action_pages(address, "tokentx", cursor);
    native.reserve(native.size() + tokens.size());
    for (auto &tx : tokens) native.push_back(std::move(tx));
    co_return native;
}

}  // namespace journalseed::infrastructure
