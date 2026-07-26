#include "journalseed/domain/money.h"

#include <catch2/catch_test_macros.hpp>

using journalseed::domain::Money;
using journalseed::domain::MoneyErrorCode;

TEST_CASE("money parses and formats exact decimal strings") {
    REQUIRE(Money::parse("0")->to_string() == "0.00");
    REQUIRE(Money::parse("12.3")->to_string() == "12.30");
    REQUIRE(Money::parse("-0042.07")->to_string() == "-42.07");
    REQUIRE(Money::parse("-0.00")->to_string() == "0.00");
    REQUIRE(Money::parse("0.000000000000000001")->to_string() == "0.000000000000000001");
    REQUIRE(Money::parse("99999999999999999999.999999999999999999")->to_string() ==
            "99999999999999999999.999999999999999999");
}

TEST_CASE("money rejects floating point ambiguity and overflow") {
    REQUIRE(Money::parse("0.0000000000000000001").error().code == MoneyErrorCode::invalid_scale);
    REQUIRE(Money::parse("1e3").error().code == MoneyErrorCode::invalid_character);
    REQUIRE(Money::parse("+2.00").error().code == MoneyErrorCode::invalid_character);
    REQUIRE(Money::parse("100000000000000000000.00").error().code ==
            MoneyErrorCode::out_of_range);
}

TEST_CASE("money reports whether values fit an asset decimal scale") {
    REQUIRE(Money::parse("42.00")->fits_scale(0));
    REQUIRE_FALSE(Money::parse("42.10")->fits_scale(0));

    REQUIRE(Money::parse("12.30")->fits_scale(2));
    REQUIRE(Money::parse("-12.30")->fits_scale(2));
    REQUIRE_FALSE(Money::parse("12.301")->fits_scale(2));

    REQUIRE(Money::parse("0.123456789")->fits_scale(9));
    REQUIRE_FALSE(Money::parse("0.1234567891")->fits_scale(9));

    REQUIRE(Money::parse("0.000000000000000001")->fits_scale(18));
    REQUIRE_FALSE(Money::parse("0.01")->fits_scale(19));
}

TEST_CASE("money checked addition retains the two-decimal representation") {
    const auto left = *Money::parse("10.25");
    const auto right = *Money::parse("-2.10");
    REQUIRE(Money::checked_add(left, right)->to_string() == "8.15");
}

TEST_CASE("money checked addition rejects both overflow directions before arithmetic") {
    const auto maximum = *Money::parse("99999999999999999999.999999999999999999");
    const auto minimum = *Money::parse("-99999999999999999999.999999999999999999");
    const auto cent = *Money::parse("0.01");

    const auto positive_overflow = Money::checked_add(maximum, cent);
    REQUIRE_FALSE(positive_overflow);
    REQUIRE(positive_overflow.error().code == MoneyErrorCode::out_of_range);

    const auto negative_overflow = Money::checked_add(minimum, cent.negated());
    REQUIRE_FALSE(negative_overflow);
    REQUIRE(negative_overflow.error().code == MoneyErrorCode::out_of_range);
}
