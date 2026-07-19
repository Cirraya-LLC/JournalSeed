#include "journalseed/domain/money.h"

#include <catch2/catch_test_macros.hpp>

using journalseed::domain::Money;
using journalseed::domain::MoneyErrorCode;

TEST_CASE("money parses and formats exact decimal strings") {
    REQUIRE(Money::parse("0")->to_string() == "0.00");
    REQUIRE(Money::parse("12.3")->to_string() == "12.30");
    REQUIRE(Money::parse("-0042.07")->to_string() == "-42.07");
    REQUIRE(Money::parse("-0.00")->to_string() == "0.00");
    REQUIRE(Money::parse("999999999999999999999999999999999999.99")->to_string() ==
            "999999999999999999999999999999999999.99");
}

TEST_CASE("money rejects floating point ambiguity and overflow") {
    REQUIRE(Money::parse("0.001").error().code == MoneyErrorCode::invalid_scale);
    REQUIRE(Money::parse("1e3").error().code == MoneyErrorCode::invalid_character);
    REQUIRE(Money::parse("+2.00").error().code == MoneyErrorCode::invalid_character);
    REQUIRE(Money::parse("1000000000000000000000000000000000000.00").error().code ==
            MoneyErrorCode::out_of_range);
}

TEST_CASE("money checked addition retains the two-decimal representation") {
    const auto left = *Money::parse("10.25");
    const auto right = *Money::parse("-2.10");
    REQUIRE(Money::checked_add(left, right)->to_string() == "8.15");
}

TEST_CASE("money checked addition rejects both overflow directions before arithmetic") {
    const auto maximum = *Money::parse("999999999999999999999999999999999999.99");
    const auto minimum = *Money::parse("-999999999999999999999999999999999999.99");
    const auto cent = *Money::parse("0.01");

    REQUIRE(Money::checked_add(maximum, cent).error().code == MoneyErrorCode::out_of_range);
    REQUIRE(Money::checked_add(minimum, cent.negated()).error().code ==
            MoneyErrorCode::out_of_range);
}
