#pragma once

#include <compare>
#include <memory>
#include <string>
#include <string_view>

struct mpd_t;

namespace journalseed::lua {

class Decimal final {
  public:
    explicit Decimal(std::string_view value);
    Decimal(const Decimal &other);
    Decimal(Decimal &&other) noexcept;
    Decimal &operator=(const Decimal &other);
    Decimal &operator=(Decimal &&other) noexcept;
    ~Decimal();

    [[nodiscard]] Decimal operator+(const Decimal &other) const;
    [[nodiscard]] Decimal operator-(const Decimal &other) const;
    [[nodiscard]] Decimal operator*(const Decimal &other) const;
    [[nodiscard]] Decimal operator/(const Decimal &other) const;
    [[nodiscard]] Decimal operator-() const;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Decimal &other) const;
    [[nodiscard]] std::strong_ordering operator<=>(const Decimal &other) const;

  private:
    struct AdoptTag {};
    Decimal(mpd_t *value, AdoptTag) noexcept;
    [[nodiscard]] static Decimal make_result();

    mpd_t *value_{nullptr};
};

}  // namespace journalseed::lua
