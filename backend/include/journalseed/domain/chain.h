#pragma once

#include <expected>
#include <cstdint>
#include <string>
#include <span>
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

// A token offered as a ready-made choice when a wallet's accepted currencies are
// configured. `contract` is written the way block explorers show it; callers that
// compare it with synced assets must run it through normalize_address first.
struct TokenPreset {
    std::string_view symbol;
    std::string_view name;
    std::string_view contract;
};

// The asset key a wallet token rule uses for the chain's own coin (TRX, ETH, POL, SOL),
// which has no contract address.
inline constexpr std::string_view kNativeAssetKey = "native";

[[nodiscard]] bool is_supported_chain(std::string_view chain_code);
[[nodiscard]] bool is_evm_chain(std::string_view chain_code);
[[nodiscard]] bool is_solana_chain(std::string_view chain_code);
[[nodiscard]] std::string display_prefix(std::string_view chain_code);
[[nodiscard]] std::string native_symbol(std::string_view chain_code);
[[nodiscard]] std::uint8_t native_decimals(std::string_view chain_code);
// USDT / USDC contracts on the chain, verified on-chain; empty for unsupported chains.
[[nodiscard]] std::span<const TokenPreset> token_presets(std::string_view chain_code);
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
