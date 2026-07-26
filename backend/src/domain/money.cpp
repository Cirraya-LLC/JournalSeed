#include "journalseed/domain/money.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace journalseed::domain {
namespace {

constexpr int kScale = 18;

constexpr Money::rep pow10(int exponent) {
    Money::rep result = 1;
    for (int index = 0; index < exponent; ++index) {
        result *= 10;
    }
    return result;
}

constexpr Money::rep decimal_limit() {
    Money::rep result = 1;
    for (int index = 0; index < 38; ++index) {
        result *= 10;
    }
    return result - 1;
}

constexpr Money::rep kScaleFactor = pow10(kScale);
constexpr Money::rep kMaximumMinorUnits = decimal_limit();
constexpr Money::rep kMaximumWholeUnits = kMaximumMinorUnits / kScaleFactor;

MoneyError error(MoneyErrorCode code, std::string message) {
    return MoneyError{.code = code, .message = std::move(message)};
}

bool is_digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

}  // namespace

std::expected<Money, MoneyError> Money::parse(std::string_view value) {
    if (value.empty()) {
        return std::unexpected(error(MoneyErrorCode::empty, "金额不能为空"));
    }

    bool negative = false;
    std::size_t offset = 0;
    if (value.front() == '-') {
        negative = true;
        offset = 1;
    }
    if (offset == value.size()) {
        return std::unexpected(
            error(MoneyErrorCode::invalid_character, "金额需要包含数字"));
    }

    const auto decimal = value.find('.', offset);
    const auto integer_end = decimal == std::string_view::npos ? value.size() : decimal;
    if (integer_end == offset) {
        return std::unexpected(
            error(MoneyErrorCode::invalid_character, "小数点前需要包含数字"));
    }

    const auto fraction_size = decimal == std::string_view::npos ? 0U : value.size() - decimal - 1;
    if (decimal != std::string_view::npos && (fraction_size == 0 || fraction_size > kScale)) {
        return std::unexpected(
            error(MoneyErrorCode::invalid_scale, "金额最多保留十八位小数"));
    }

    std::size_t first_significant = offset;
    while (first_significant < integer_end && value[first_significant] == '0') {
        ++first_significant;
    }
    if (integer_end - first_significant > 20) {
        return std::unexpected(
            error(MoneyErrorCode::out_of_range, "金额整数部分最多 20 位"));
    }

    rep whole_units = 0;
    for (std::size_t index = offset; index < integer_end; ++index) {
        const char character = value[index];
        if (!is_digit(character)) {
            return std::unexpected(
                error(MoneyErrorCode::invalid_character, "金额只能包含数字和小数点"));
        }
        whole_units = whole_units * 10 + static_cast<rep>(character - '0');
        if (whole_units > kMaximumWholeUnits) {
            return std::unexpected(
                error(MoneyErrorCode::out_of_range, "金额超出 38 位有效数字范围"));
        }
    }

    rep fractional_units = 0;
    if (decimal != std::string_view::npos) {
        for (std::size_t index = decimal + 1; index < value.size(); ++index) {
            const char character = value[index];
            if (!is_digit(character)) {
                return std::unexpected(
                    error(MoneyErrorCode::invalid_character, "金额只能包含数字和小数点"));
            }
            fractional_units = fractional_units * 10 + static_cast<rep>(character - '0');
        }
        for (std::size_t index = fraction_size; index < static_cast<std::size_t>(kScale); ++index) {
            fractional_units *= 10;
        }
    }

    rep minor_units = whole_units * kScaleFactor + fractional_units;
    if (minor_units > kMaximumMinorUnits) {
        return std::unexpected(error(MoneyErrorCode::out_of_range, "金额超出 38 位有效数字范围"));
    }
    if (negative && minor_units != 0) {
        minor_units = -minor_units;
    }
    return Money(minor_units);
}

bool Money::fits_scale(std::uint8_t decimals) const noexcept {
    if (decimals > storage_scale()) return false;
    const auto divisor = pow10(static_cast<int>(storage_scale() - decimals));
    return minor_units_ % divisor == 0;
}

std::string Money::to_string() const {
    rep magnitude = minor_units_ < 0 ? -minor_units_ : minor_units_;
    std::string digits;
    do {
        const auto digit = static_cast<unsigned int>(magnitude % 10);
        digits.push_back(static_cast<char>('0' + digit));
        magnitude /= 10;
    } while (magnitude != 0);

    while (digits.size() < static_cast<std::size_t>(kScale + 1)) {
        digits.push_back('0');
    }
    std::reverse(digits.begin(), digits.end());
    digits.insert(digits.end() - kScale, '.');

    const auto decimal = digits.find('.');
    while (digits.size() - decimal - 1 > 2 && digits.back() == '0') {
        digits.pop_back();
    }

    if (minor_units_ < 0) {
        digits.insert(digits.begin(), '-');
    }
    return digits;
}

std::expected<Money, MoneyError> Money::checked_add(Money left, Money right) {
    if ((right.minor_units_ > 0 &&
         left.minor_units_ > kMaximumMinorUnits - right.minor_units_) ||
        (right.minor_units_ < 0 &&
         left.minor_units_ < -kMaximumMinorUnits - right.minor_units_)) {
        return std::unexpected(error(MoneyErrorCode::out_of_range, "金额相加后超出范围"));
    }
    const rep result = left.minor_units_ + right.minor_units_;
    return Money(result);
}

}  // namespace journalseed::domain
