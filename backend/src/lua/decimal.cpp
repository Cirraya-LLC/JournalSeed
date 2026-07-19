#include "journalseed/lua/decimal.h"

#include <mpdecimal.h>

#include <stdexcept>
#include <utility>

namespace journalseed::lua {
namespace {

mpd_context_t decimal_context() {
    mpd_context_t context;
    mpd_defaultcontext(&context);
    context.prec = 38;
    context.round = MPD_ROUND_HALF_EVEN;
    context.traps = 0;
    context.status = 0;
    return context;
}

}  // namespace

Decimal::Decimal(std::string_view value) {
    auto context = decimal_context();
    value_ = mpd_new(&context);
    if (value_ == nullptr) {
        throw std::bad_alloc();
    }
    const std::string owned(value);
    mpd_set_string(value_, owned.c_str(), &context);
    if ((context.status & MPD_Errors) != 0 || mpd_isspecial(value_)) {
        mpd_del(value_);
        value_ = nullptr;
        throw std::invalid_argument("invalid exact decimal: " + owned);
    }
}

Decimal::Decimal(mpd_t *value, AdoptTag) noexcept : value_(value) {}

Decimal::Decimal(const Decimal &other) {
    auto context = decimal_context();
    value_ = mpd_new(&context);
    if (value_ == nullptr) {
        throw std::bad_alloc();
    }
    mpd_copy(value_, other.value_, &context);
    if ((context.status & MPD_Errors) != 0) {
        mpd_del(value_);
        value_ = nullptr;
        throw std::bad_alloc();
    }
}

Decimal::Decimal(Decimal &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

Decimal &Decimal::operator=(const Decimal &other) {
    if (this == &other) return *this;
    Decimal copy(other);
    std::swap(value_, copy.value_);
    return *this;
}

Decimal &Decimal::operator=(Decimal &&other) noexcept {
    if (this == &other) return *this;
    if (value_ != nullptr) mpd_del(value_);
    value_ = std::exchange(other.value_, nullptr);
    return *this;
}

Decimal::~Decimal() {
    if (value_ != nullptr) mpd_del(value_);
}

Decimal Decimal::make_result() {
    auto context = decimal_context();
    auto *value = mpd_new(&context);
    if (value == nullptr) throw std::bad_alloc();
    return Decimal(value, AdoptTag{});
}

Decimal Decimal::operator+(const Decimal &other) const {
    auto result = make_result();
    auto context = decimal_context();
    mpd_add(result.value_, value_, other.value_, &context);
    if ((context.status & MPD_Errors) != 0 || mpd_isspecial(result.value_)) {
        throw std::domain_error("decimal addition failed");
    }
    return result;
}

Decimal Decimal::operator-(const Decimal &other) const {
    auto result = make_result();
    auto context = decimal_context();
    mpd_sub(result.value_, value_, other.value_, &context);
    if ((context.status & MPD_Errors) != 0 || mpd_isspecial(result.value_)) {
        throw std::domain_error("decimal subtraction failed");
    }
    return result;
}

Decimal Decimal::operator*(const Decimal &other) const {
    auto result = make_result();
    auto context = decimal_context();
    mpd_mul(result.value_, value_, other.value_, &context);
    if ((context.status & MPD_Errors) != 0 || mpd_isspecial(result.value_)) {
        throw std::domain_error("decimal multiplication failed");
    }
    return result;
}

Decimal Decimal::operator/(const Decimal &other) const {
    auto result = make_result();
    auto context = decimal_context();
    mpd_div(result.value_, value_, other.value_, &context);
    if ((context.status & MPD_Errors) != 0 || mpd_isspecial(result.value_)) {
        throw std::domain_error("decimal division failed");
    }
    return result;
}

Decimal Decimal::operator-() const {
    auto result = make_result();
    auto context = decimal_context();
    mpd_copy_negate(result.value_, value_, &context);
    if ((context.status & MPD_Errors) != 0) {
        throw std::domain_error("decimal negation failed");
    }
    return result;
}

std::string Decimal::to_string() const {
    char *text = mpd_to_sci(value_, 1);
    if (text == nullptr) throw std::bad_alloc();
    std::string result(text);
    mpd_free(text);
    return result;
}

bool Decimal::operator==(const Decimal &other) const {
    auto context = decimal_context();
    return mpd_cmp(value_, other.value_, &context) == 0;
}

std::strong_ordering Decimal::operator<=>(const Decimal &other) const {
    auto context = decimal_context();
    const int comparison = mpd_cmp(value_, other.value_, &context);
    if (comparison < 0) return std::strong_ordering::less;
    if (comparison > 0) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

}  // namespace journalseed::lua
