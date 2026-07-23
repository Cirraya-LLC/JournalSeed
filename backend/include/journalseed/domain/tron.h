#pragma once

#include <expected>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace journalseed::domain::tron {

enum class TronErrorCode {
    empty,
    invalid_character,
    invalid_length,
    invalid_checksum,
    invalid_prefix,
    invalid_amount,
    invalid_decimals,
};

struct TronError {
    TronErrorCode code;
    std::string message;
};

[[nodiscard]] std::expected<std::string, TronError> normalize_address(std::string_view value);
[[nodiscard]] bool is_valid_address(std::string_view value);
[[nodiscard]] std::string short_address(std::string_view value);
[[nodiscard]] std::expected<std::string, TronError> hex_to_base58check(std::string_view hex);
[[nodiscard]] std::expected<std::string, TronError> base_units_to_decimal(std::string_view raw,
                                                                          std::uint8_t decimals);
[[nodiscard]] std::expected<std::string, TronError> normalize_decimal(std::string_view value,
                                                                      std::uint8_t decimals);

struct AddressLabelMatch {
    std::string address;
    std::string displayName;
    std::string kind;
};

using AddressLabelMap = std::unordered_map<std::string, AddressLabelMatch>;

[[nodiscard]] std::string label_or_short(std::string_view address,
                                         const AddressLabelMap &labels);
[[nodiscard]] std::string movement_key(std::string_view chain_code,
                                       std::string_view tx_hash,
                                       std::string_view movement_kind,
                                       std::string_view asset_identifier,
                                       std::string_view address,
                                       std::uint32_t ordinal);

}  // namespace journalseed::domain::tron
