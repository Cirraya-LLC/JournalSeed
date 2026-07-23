#pragma once

#include <expected>
#include <cstdint>
#include <string>
#include <string_view>

namespace journalseed::domain::chain {

enum class ChainErrorCode {
    empty,
    unsupported_chain,
    invalid_character,
    invalid_length,
    invalid_prefix,
    invalid_checksum,
    invalid_amount,
};

struct ChainError {
    ChainErrorCode code;
    std::string message;
};

[[nodiscard]] bool is_supported_chain(std::string_view chain_code);
[[nodiscard]] bool is_evm_chain(std::string_view chain_code);
[[nodiscard]] bool is_solana_chain(std::string_view chain_code);
[[nodiscard]] std::string display_prefix(std::string_view chain_code);
[[nodiscard]] std::string native_symbol(std::string_view chain_code);
[[nodiscard]] std::uint8_t native_decimals(std::string_view chain_code);
[[nodiscard]] std::expected<std::string, ChainError> normalize_address(std::string_view chain_code,
                                                                       std::string_view value);
[[nodiscard]] bool is_valid_address(std::string_view chain_code, std::string_view value);
[[nodiscard]] std::string short_address(std::string_view value);
[[nodiscard]] std::expected<std::string, ChainError> base_units_to_decimal(std::string_view raw,
                                                                           std::uint8_t decimals);
[[nodiscard]] std::expected<std::string, ChainError> normalize_decimal(std::string_view value,
                                                                       std::uint8_t decimals);
[[nodiscard]] std::string movement_key(std::string_view chain_code,
                                       std::string_view tx_hash,
                                       std::string_view movement_kind,
                                       std::string_view asset_identifier,
                                       std::string_view address,
                                       std::uint32_t ordinal);

}  // namespace journalseed::domain::chain
