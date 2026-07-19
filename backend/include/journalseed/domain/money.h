#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace journalseed::domain {

enum class MoneyErrorCode {
    empty,
    invalid_character,
    invalid_scale,
    out_of_range,
};

struct MoneyError {
    MoneyErrorCode code;
    std::string message;
};

class Money final {
  public:
    using rep = __int128_t;

    constexpr Money() noexcept = default;

    [[nodiscard]] static std::expected<Money, MoneyError> parse(std::string_view value);
    [[nodiscard]] static constexpr Money from_minor_units(rep value) noexcept {
        return Money(value);
    }

    [[nodiscard]] constexpr rep minor_units() const noexcept { return minor_units_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return minor_units_ == 0; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return minor_units_ > 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return minor_units_ < 0; }
    [[nodiscard]] constexpr Money negated() const noexcept { return Money(-minor_units_); }
    [[nodiscard]] constexpr Money absolute() const noexcept {
        return minor_units_ < 0 ? Money(-minor_units_) : *this;
    }
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] static std::expected<Money, MoneyError> checked_add(Money left,
                                                                      Money right);

    auto operator<=>(const Money &) const = default;

  private:
    explicit constexpr Money(rep value) noexcept : minor_units_(value) {}

    rep minor_units_{0};
};

}  // namespace journalseed::domain
