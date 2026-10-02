#include "journalseed/domain/chain.h"

#include "journalseed/domain/tron.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace journalseed::domain::chain {
namespace {

ChainError error(ChainErrorCode code, std::string message) {
    return ChainError{.code = code, .message = std::move(message)};
}

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool is_base58(std::string_view value) {
    return std::ranges::all_of(value, [](char character) {
        return kBase58Alphabet.find(character) != std::string_view::npos;
    });
}

std::vector<unsigned char> decode_base58(std::string_view input) {
    std::vector<unsigned char> bytes((input.size() * 733) / 1000 + 1);
    for (char character : input) {
        const auto digit = kBase58Alphabet.find(character);
        if (digit == std::string_view::npos) return {};
        int carry = static_cast<int>(digit);
        for (auto iter = bytes.rbegin(); iter != bytes.rend(); ++iter) {
            carry += 58 * *iter;
            *iter = static_cast<unsigned char>(carry & 0xff);
            carry >>= 8;
        }
        if (carry != 0) return {};
    }
    const auto first_non_leading_zero = std::ranges::find_if(input, [](char character) {
        return character != '1';
    });
    const auto leading_zeroes = static_cast<std::size_t>(first_non_leading_zero - input.begin());
    auto first_non_zero = std::ranges::find_if(bytes, [](unsigned char byte) { return byte != 0; });
    std::vector<unsigned char> result(leading_zeroes, 0);
    result.insert(result.end(), first_non_zero, bytes.end());
    return result;
}

std::expected<std::string, ChainError> normalize_evm_address(std::string_view value) {
    if (value.empty()) return std::unexpected(error(ChainErrorCode::empty, "EVM 地址不能为空"));
    if (value.size() != 42 || !(value.starts_with("0x") || value.starts_with("0X"))) {
        return std::unexpected(error(ChainErrorCode::invalid_length, "EVM 地址需要 0x 加 40 个十六进制字符"));
    }
    std::string result(value);
    result[1] = 'x';
    for (std::size_t index = 2; index < result.size(); ++index) {
        const auto character = static_cast<unsigned char>(result[index]);
        if (!std::isxdigit(character)) {
            return std::unexpected(error(ChainErrorCode::invalid_character, "EVM 地址包含非法十六进制字符"));
        }
        result[index] = static_cast<char>(std::tolower(character));
    }
    return result;
}

std::expected<std::string, ChainError> normalize_solana_address(std::string_view value) {
    if (value.empty()) return std::unexpected(error(ChainErrorCode::empty, "Solana 地址不能为空"));
    if (value.size() < 32 || value.size() > 44) {
        return std::unexpected(error(ChainErrorCode::invalid_length, "Solana 地址长度无效"));
    }
    if (!is_base58(value)) {
        return std::unexpected(error(ChainErrorCode::invalid_character, "Solana 地址只能包含 Base58 字符"));
    }
    const auto decoded = decode_base58(value);
    if (decoded.size() != 32) {
        return std::unexpected(error(ChainErrorCode::invalid_length, "Solana public key 需要解码为 32 字节"));
    }
    return std::string(value);
}

}  // namespace

bool is_supported_chain(std::string_view chain_code) {
    return chain_code == "tron-mainnet" || chain_code == "ethereum-mainnet" ||
           chain_code == "polygon-mainnet" || chain_code == "solana-mainnet";
}

bool is_evm_chain(std::string_view chain_code) {
    return chain_code == "ethereum-mainnet" || chain_code == "polygon-mainnet";
}

bool is_solana_chain(std::string_view chain_code) {
    return chain_code == "solana-mainnet";
}

std::string display_prefix(std::string_view chain_code) {
    if (chain_code == "ethereum-mainnet") return "Ethereum";
    if (chain_code == "polygon-mainnet") return "Polygon";
    if (chain_code == "solana-mainnet") return "Solana";
    return "TRON";
}

// Every contract below was checked against its chain (symbol() and decimals() = 6)
// before being listed. Polygon carries both the native USDC and the older bridged
// USDC.e, since payers still send either.
constexpr std::array kTronPresets{
    TokenPreset{"USDT", "Tether USD", "TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t"},
    TokenPreset{"USDC", "USD Coin", "TEkxiTehnzSmSe2XqrBj4w32RUN966rdz8"},
};
constexpr std::array kEthereumPresets{
    TokenPreset{"USDT", "Tether USD", "0xdAC17F958D2ee523a2206206994597C13D831ec7"},
    TokenPreset{"USDC", "USD Coin", "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"},
};
constexpr std::array kPolygonPresets{
    TokenPreset{"USDT", "Tether USD", "0xc2132D05D31c914a87C6611C10748AEb04B58e8F"},
    TokenPreset{"USDC", "USD Coin", "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359"},
    TokenPreset{"USDC.e", "USD Coin (PoS bridged)", "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174"},
};
constexpr std::array kSolanaPresets{
    TokenPreset{"USDT", "Tether USD", "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"},
    TokenPreset{"USDC", "USD Coin", "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"},
};

std::span<const TokenPreset> token_presets(std::string_view chain_code) {
    if (chain_code == "tron-mainnet") return kTronPresets;
    if (chain_code == "ethereum-mainnet") return kEthereumPresets;
    if (chain_code == "polygon-mainnet") return kPolygonPresets;
    if (chain_code == "solana-mainnet") return kSolanaPresets;
    return {};
}

std::string native_symbol(std::string_view chain_code) {
    if (chain_code == "ethereum-mainnet") return "ETH";
    if (chain_code == "polygon-mainnet") return "POL";
    if (chain_code == "solana-mainnet") return "SOL";
    return "TRX";
}

std::uint8_t native_decimals(std::string_view chain_code) {
    if (chain_code == "solana-mainnet") return 9;
    if (chain_code == "tron-mainnet") return 6;
    return 18;
}

std::expected<std::string, ChainError> normalize_address(std::string_view chain_code,
                                                         std::string_view value) {
    if (chain_code == "tron-mainnet") {
        auto normalized = tron::normalize_address(value);
        if (!normalized) {
            ChainErrorCode code = ChainErrorCode::invalid_character;
            switch (normalized.error().code) {
                case tron::TronErrorCode::empty: code = ChainErrorCode::empty; break;
                case tron::TronErrorCode::invalid_length: code = ChainErrorCode::invalid_length; break;
                case tron::TronErrorCode::invalid_checksum: code = ChainErrorCode::invalid_checksum; break;
                case tron::TronErrorCode::invalid_prefix: code = ChainErrorCode::invalid_prefix; break;
                case tron::TronErrorCode::invalid_amount:
                case tron::TronErrorCode::invalid_decimals:
                case tron::TronErrorCode::invalid_character:
                    code = ChainErrorCode::invalid_character;
                    break;
            }
            return std::unexpected(error(code, normalized.error().message));
        }
        return *normalized;
    }
    if (is_evm_chain(chain_code)) return normalize_evm_address(value);
    if (is_solana_chain(chain_code)) return normalize_solana_address(value);
    return std::unexpected(error(ChainErrorCode::unsupported_chain, "暂不支持该链"));
}

bool is_valid_address(std::string_view chain_code, std::string_view value) {
    return normalize_address(chain_code, value).has_value();
}

std::string short_address(std::string_view value) {
    return tron::short_address(value);
}

std::expected<std::string, ChainError> base_units_to_decimal(std::string_view raw,
                                                             std::uint8_t decimals) {
    auto converted = tron::base_units_to_decimal(raw, decimals);
    if (!converted) {
        return std::unexpected(error(ChainErrorCode::invalid_amount, converted.error().message));
    }
    return *converted;
}

std::expected<std::string, ChainError> normalize_decimal(std::string_view value,
                                                         std::uint8_t decimals) {
    auto converted = tron::normalize_decimal(value, decimals);
    if (!converted) {
        return std::unexpected(error(ChainErrorCode::invalid_amount, converted.error().message));
    }
    return *converted;
}

std::string movement_key(std::string_view chain_code,
                         std::string_view tx_hash,
                         std::string_view movement_kind,
                         std::string_view asset_identifier,
                         std::string_view address,
                         std::uint32_t ordinal) {
    return tron::movement_key(chain_code, tx_hash, movement_kind, asset_identifier, address, ordinal);
}

}  // namespace journalseed::domain::chain
