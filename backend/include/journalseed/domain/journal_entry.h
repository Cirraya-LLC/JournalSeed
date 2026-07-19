#pragma once

#include "journalseed/domain/money.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace journalseed::domain {

struct AccountId {
    std::int64_t value;
    auto operator<=>(const AccountId &) const = default;
};

struct CategoryId {
    std::int64_t value;
    auto operator<=>(const CategoryId &) const = default;
};

enum class TransactionKind { entry, transfer, note };
enum class EntryDirection { income, expense, neutral };

struct JournalDraft {
    TransactionKind kind{TransactionKind::note};
    Money amount{};
    std::optional<AccountId> account;
    std::optional<CategoryId> category;
    std::optional<AccountId> transfer_account;
};

struct PostingAccounts {
    AccountId primary;
    AccountId counterparty;
    std::optional<AccountId> transfer_destination;
};

struct Posting {
    AccountId account;
    std::optional<CategoryId> category;
    Money signed_amount;
    std::uint8_t line_number;
};

enum class JournalErrorCode {
    invalid_note,
    zero_financial_entry,
    invalid_transfer,
    unbalanced,
};

struct JournalError {
    JournalErrorCode code;
    std::string message;
};

[[nodiscard]] EntryDirection classify(const JournalDraft &draft) noexcept;
[[nodiscard]] std::expected<std::vector<Posting>, JournalError>
build_postings(const JournalDraft &draft, const PostingAccounts &accounts);
[[nodiscard]] bool postings_are_balanced(std::span<const Posting> postings);

struct PeriodTotals {
    Money income;
    Money expense;
};

[[nodiscard]] std::expected<PeriodTotals, MoneyError>
calculate_period_totals(std::span<const JournalDraft> drafts);

}  // namespace journalseed::domain
