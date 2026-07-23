#include "journalseed/domain/tron.h"

#include <catch2/catch_test_macros.hpp>

using namespace journalseed::domain::tron;

TEST_CASE("tron addresses normalize and validate base58check and hex forms") {
    const auto normalized = normalize_address("TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU7");
    REQUIRE(normalized);
    REQUIRE(*normalized == "TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU7");
    REQUIRE(*hex_to_base58check("41a614f803b6fd780986a42c78ec9c7f77e6ded13c") ==
            "TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t");
    REQUIRE_FALSE(is_valid_address("TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU8"));
}

TEST_CASE("tron base units convert to exact decimal strings") {
    REQUIRE(base_units_to_decimal("1", 6)->compare("0.000001") == 0);
    REQUIRE(base_units_to_decimal("1000000", 6)->compare("1.00") == 0);
    REQUIRE(base_units_to_decimal("123456789", 6)->compare("123.456789") == 0);
    REQUIRE(base_units_to_decimal("2500000", 6)->compare("2.50") == 0);
}

TEST_CASE("tron decimal normalization respects asset decimals") {
    REQUIRE(normalize_decimal(" 001.230000 ", 6)->compare("1.23") == 0);
    REQUIRE(normalize_decimal("-0.000000", 6)->compare("0.00") == 0);
    REQUIRE(normalize_decimal("0.000001", 6)->compare("0.000001") == 0);
    REQUIRE(normalize_decimal("0.0000001", 6).error().code == TronErrorCode::invalid_amount);
}

TEST_CASE("address labels and movement keys are deterministic") {
    AddressLabelMap labels;
    labels.emplace("TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU7",
                   AddressLabelMatch{.address = "TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU7",
                                     .displayName = "客户 A",
                                     .kind = "customer"});
    REQUIRE(label_or_short("TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU7", labels) == "客户 A");
    REQUIRE(label_or_short("TLsV52sRDL79HXGGm9yzwKibb6BeruhUzy", labels) ==
            "TLsV52...hUzy");
    REQUIRE(movement_key("tron", "abc", "incoming", "USDT:contract", "Txxx", 2) ==
            "tron:abc:incoming:USDT:contract:Txxx:2");
}
