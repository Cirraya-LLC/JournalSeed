#include "journalseed/domain/tron.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace journalseed::domain::tron {
namespace {

constexpr std::string_view kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr unsigned char kMainnetPrefix = 0x41;

TronError error(TronErrorCode code, std::string message) {
    return TronError{.code = code, .message = std::move(message)};
}

void ensure_sodium() {
    static const int initialized = sodium_init();
    static_cast<void>(initialized);
}

std::optional<unsigned int> alphabet_index(char character) {
    const auto position = kAlphabet.find(character);
    if (position == std::string_view::npos) return std::nullopt;
    return static_cast<unsigned int>(position);
}

std::expected<std::vector<unsigned char>, TronError> decode_base58(std::string_view value) {
    if (value.empty()) {
        return std::unexpected(error(TronErrorCode::empty, "TRON 地址不能为空"));
    }

    std::vector<unsigned char> bytes(value.size() * 733 / 1000 + 1);
    for (const char character : value) {
        const auto digit = alphabet_index(character);
        if (!digit) {
            return std::unexpected(error(TronErrorCode::invalid_character,
                                         "TRON 地址只能包含 Base58 字符"));
        }
        unsigned int carry = *digit;
        for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
            carry += 58U * static_cast<unsigned int>(*iterator);
            *iterator = static_cast<unsigned char>(carry & 0xffU);
            carry >>= 8U;
        }
        if (carry != 0) {
            return std::unexpected(error(TronErrorCode::invalid_length, "TRON 地址长度无效"));
        }
    }

    std::size_t leading_zeroes = 0;
    while (leading_zeroes < value.size() && value[leading_zeroes] == '1') ++leading_zeroes;

    auto first_non_zero = std::ranges::find_if(bytes, [](unsigned char byte) { return byte != 0; });
    std::vector<unsigned char> result(leading_zeroes, 0);
    result.insert(result.end(), first_non_zero, bytes.end());
    return result;
}

std::string encode_base58(std::span<const unsigned char> input) {
    std::vector<unsigned char> digits(input.size() * 138 / 100 + 1);
    for (const unsigned char byte : input) {
        unsigned int carry = byte;
        for (auto iterator = digits.rbegin(); iterator != digits.rend(); ++iterator) {
            carry += 256U * static_cast<unsigned int>(*iterator);
            *iterator = static_cast<unsigned char>(carry % 58U);
            carry /= 58U;
        }
    }

    std::size_t leading_zeroes = 0;
    while (leading_zeroes < input.size() && input[leading_zeroes] == 0) ++leading_zeroes;

    auto first_non_zero = std::ranges::find_if(digits, [](unsigned char digit) { return digit != 0; });
    std::string result(leading_zeroes, '1');
    for (auto iterator = first_non_zero; iterator != digits.end(); ++iterator) {
        result.push_back(kAlphabet[*iterator]);
    }
    return result.empty() ? "1" : result;
}

std::array<unsigned char, crypto_hash_sha256_BYTES> sha256(std::span<const unsigned char> input) {
    ensure_sodium();
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), input.data(), input.size());
    return digest;
}

std::string encode_base58check(std::span<const unsigned char> payload) {
    const auto first = sha256(payload);
    const auto second = sha256(first);
    std::vector<unsigned char> with_checksum(payload.begin(), payload.end());
    with_checksum.insert(with_checksum.end(), second.begin(), second.begin() + 4);
    return encode_base58(with_checksum);
}

std::optional<unsigned char> hex_digit(char character) {
    if (character >= '0' && character <= '9') return static_cast<unsigned char>(character - '0');
    if (character >= 'a' && character <= 'f') return static_cast<unsigned char>(10 + character - 'a');
    if (character >= 'A' && character <= 'F') return static_cast<unsigned char>(10 + character - 'A');
    return std::nullopt;
}

std::expected<std::vector<unsigned char>, TronError> decode_hex(std::string_view hex) {
    if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
    if (hex.size() != 42) {
        return std::unexpected(error(TronErrorCode::invalid_length,
                                     "TRON 十六进制地址需要 21 字节"));
    }
    std::vector<unsigned char> bytes;
    bytes.reserve(21);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        const auto high = hex_digit(hex[index]);
        const auto low = hex_digit(hex[index + 1]);
        if (!high || !low) {
            return std::unexpected(error(TronErrorCode::invalid_character,
                                         "TRON 十六进制地址包含非法字符"));
        }
        bytes.push_back(static_cast<unsigned char>((*high << 4U) | *low));
    }
    return bytes;
}

bool is_ascii_space(char character) noexcept {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string trim(std::string_view value) {
    auto first = value.begin();
    while (first != value.end() && is_ascii_space(*first)) ++first;
    auto last = value.end();
    while (last != first && is_ascii_space(*(last - 1))) --last;
    return std::string(first, last);
}

bool is_decimal_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

}  // namespace

std::expected<std::string, TronError> hex_to_base58check(std::string_view hex) {
    auto bytes = decode_hex(hex);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() != 21) {
        return std::unexpected(error(TronErrorCode::invalid_length, "TRON 地址长度无效"));
    }
    if ((*bytes)[0] != kMainnetPrefix) {
        return std::unexpected(error(TronErrorCode::invalid_prefix,
                                     "TRON Mainnet 地址需要 0x41 前缀"));
    }
    return encode_base58check(*bytes);
}

std::expected<std::string, TronError> normalize_address(std::string_view value) {
    const auto candidate = trim(value);
    if (candidate.empty()) {
        return std::unexpected(error(TronErrorCode::empty, "TRON 地址不能为空"));
    }
    if (candidate.starts_with("41") || candidate.starts_with("0x41") || candidate.starts_with("0X41")) {
        return hex_to_base58check(candidate);
    }

    auto decoded = decode_base58(candidate);
    if (!decoded) return std::unexpected(decoded.error());
    if (decoded->size() != 25) {
        return std::unexpected(error(TronErrorCode::invalid_length,
                                     "TRON 地址 Base58Check 长度无效"));
    }
    if ((*decoded)[0] != kMainnetPrefix) {
        return std::unexpected(error(TronErrorCode::invalid_prefix,
                                     "TRON Mainnet 地址必须以 T 开头"));
    }

    std::array<unsigned char, 21> payload{};
    std::copy_n(decoded->begin(), payload.size(), payload.begin());
    const auto first = sha256(payload);
    const auto second = sha256(first);
    if (!std::equal(second.begin(), second.begin() + 4, decoded->begin() + 21)) {
        return std::unexpected(error(TronErrorCode::invalid_checksum,
                                     "TRON 地址校验和不匹配"));
    }
    return encode_base58check(payload);
}

bool is_valid_address(std::string_view value) {
    return normalize_address(value).has_value();
}

std::string short_address(std::string_view value) {
    const auto normalized = normalize_address(value).value_or(std::string(value));
    if (normalized.size() <= 14) return normalized;
    return normalized.substr(0, 6) + "..." + normalized.substr(normalized.size() - 4);
}

std::expected<std::string, TronError> base_units_to_decimal(std::string_view raw,
                                                            std::uint8_t decimals) {
    if (decimals > 18) {
        return std::unexpected(error(TronErrorCode::invalid_decimals,
                                     "资产精度最多支持 18 位"));
    }
    if (raw.empty()) {
        return std::unexpected(error(TronErrorCode::invalid_amount, "链上金额不能为空"));
    }
    for (const char character : raw) {
        if (!is_decimal_digit(character)) {
            return std::unexpected(error(TronErrorCode::invalid_amount,
                                         "链上金额需要是不带小数点的整数"));
        }
    }

    std::string digits(raw);
    auto first_non_zero = digits.find_first_not_of('0');
    if (first_non_zero == std::string::npos) return decimals == 0 ? "0" : "0.00";
    digits.erase(0, first_non_zero);

    if (decimals == 0) return digits;
    if (digits.size() <= decimals) {
        digits.insert(digits.begin(), decimals - digits.size() + 1, '0');
    }
    digits.insert(digits.end() - decimals, '.');
    while (digits.size() - digits.find('.') - 1 > 2 && digits.back() == '0') {
        digits.pop_back();
    }
    return digits;
}

std::expected<std::string, TronError> normalize_decimal(std::string_view value,
                                                        std::uint8_t decimals) {
    if (decimals > 18) {
        return std::unexpected(error(TronErrorCode::invalid_decimals,
                                     "资产精度最多支持 18 位"));
    }
    const auto candidate = trim(value);
    if (candidate.empty()) {
        return std::unexpected(error(TronErrorCode::invalid_amount, "金额不能为空"));
    }
    bool negative = false;
    std::size_t offset = 0;
    if (candidate.front() == '-') {
        negative = true;
        offset = 1;
    }
    if (offset == candidate.size()) {
        return std::unexpected(error(TronErrorCode::invalid_amount, "金额需要包含数字"));
    }
    const auto point = candidate.find('.', offset);
    const auto integer_end = point == std::string::npos ? candidate.size() : point;
    if (integer_end == offset) {
        return std::unexpected(error(TronErrorCode::invalid_amount, "小数点前需要包含数字"));
    }
    const auto fraction_size = point == std::string::npos ? 0U : candidate.size() - point - 1;
    if (point != std::string::npos && (fraction_size == 0 || fraction_size > decimals)) {
        return std::unexpected(error(TronErrorCode::invalid_amount,
                                     "金额小数位超过资产精度"));
    }
    for (std::size_t index = offset; index < integer_end; ++index) {
        if (!is_decimal_digit(candidate[index])) {
            return std::unexpected(error(TronErrorCode::invalid_amount,
                                         "金额只能包含数字和小数点"));
        }
    }
    for (std::size_t index = point == std::string::npos ? candidate.size() : point + 1;
         index < candidate.size(); ++index) {
        if (!is_decimal_digit(candidate[index])) {
            return std::unexpected(error(TronErrorCode::invalid_amount,
                                         "金额只能包含数字和小数点"));
        }
    }

    std::string integer(candidate.substr(offset, integer_end - offset));
    auto first_non_zero = integer.find_first_not_of('0');
    if (first_non_zero == std::string::npos) integer = "0";
    else integer.erase(0, first_non_zero);

    std::string fraction = point == std::string::npos ? std::string{} : candidate.substr(point + 1);
    while (fraction.size() < 2 && fraction.size() < decimals) fraction.push_back('0');
    while (fraction.size() > 2 && fraction.back() == '0') fraction.pop_back();
    const bool zero = integer == "0" && std::ranges::all_of(fraction, [](char c) { return c == '0'; });
    std::string result = (negative && !zero ? "-" : "") + integer;
    if (!fraction.empty()) result += "." + fraction;
    else if (decimals > 0) result += ".00";
    return result;
}

std::string label_or_short(std::string_view address, const AddressLabelMap &labels) {
    auto normalized = normalize_address(address);
    if (normalized) {
        if (const auto found = labels.find(*normalized); found != labels.end() &&
            !found->second.displayName.empty()) {
            return found->second.displayName;
        }
        return short_address(*normalized);
    }
    return short_address(address);
}

std::string movement_key(std::string_view chain_code,
                         std::string_view tx_hash,
                         std::string_view movement_kind,
                         std::string_view asset_identifier,
                         std::string_view address,
                         std::uint32_t ordinal) {
    return std::string(chain_code) + ":" + std::string(tx_hash) + ":" +
           std::string(movement_kind) + ":" + std::string(asset_identifier) + ":" +
           std::string(address) + ":" + std::to_string(ordinal);
}

}  // namespace journalseed::domain::tron
