#include "journalseed/domain/chain.h"

#include <catch2/catch_test_macros.hpp>

using namespace journalseed::domain::chain;

TEST_CASE("multi-chain address normalization validates EVM and Solana addresses") {
    REQUIRE(is_supported_chain("ethereum-mainnet"));
    REQUIRE(is_supported_chain("polygon-mainnet"));
    REQUIRE(is_supported_chain("solana-mainnet"));

    const auto eth = normalize_address("ethereum-mainnet", "0xA0B86991C6218B36C1D19D4A2E9EB0CE3606EB48");
    REQUIRE(eth);
    REQUIRE(*eth == "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48");
    REQUIRE_FALSE(normalize_address("polygon-mainnet", "0xnot-an-address"));

    const auto sol = normalize_address("solana-mainnet", "11111111111111111111111111111111");
    REQUIRE(sol);
    REQUIRE(*sol == "11111111111111111111111111111111");
    const auto sol_internal_one = normalize_address("solana-mainnet", "4vJ9JU1bJJE96FWSJKvHsmmFADCg4gpZQff4P3bkLKi");
    REQUIRE(sol_internal_one);
    REQUIRE(*sol_internal_one == "4vJ9JU1bJJE96FWSJKvHsmmFADCg4gpZQff4P3bkLKi");
    REQUIRE_FALSE(normalize_address("solana-mainnet", "0xA0B86991C6218B36C1D19D4A2E9EB0CE3606EB48"));
}

TEST_CASE("multi-chain native assets and base unit conversions are exact") {
    REQUIRE(native_symbol("ethereum-mainnet") == "ETH");
    REQUIRE(native_symbol("polygon-mainnet") == "POL");
    REQUIRE(native_symbol("solana-mainnet") == "SOL");
    REQUIRE(native_decimals("ethereum-mainnet") == 18);
    REQUIRE(native_decimals("solana-mainnet") == 9);

    REQUIRE(*base_units_to_decimal("1000000000000000000", 18) == "1.00");
    REQUIRE(*base_units_to_decimal("123456789", 9) == "0.123456789");
    REQUIRE(*base_units_to_decimal("9007199254740993", 9) == "9007199.254740993");
    REQUIRE(*normalize_decimal("0.000000001", 9) == "0.000000001");
}

TEST_CASE("multi-chain movement keys include chain code") {
    REQUIRE(movement_key("ethereum-mainnet", "0xabc", "fee", "ETH", "0xwallet", 2) ==
            "ethereum-mainnet:0xabc:fee:ETH:0xwallet:2");
    REQUIRE(movement_key("solana-mainnet", "sig", "incoming", "USDC:mint", "wallet", 1) ==
            "solana-mainnet:sig:incoming:USDC:mint:wallet:1");
}
