#include "journalseed/domain/journal_entry.h"

namespace journalseed::domain {
namespace {

JournalError error(JournalErrorCode code, std::string message) {
    return JournalError{.code = code, .message = std::move(message)};
}

}  // namespace

EntryDirection classify(const JournalDraft &draft) noexcept {
    if (draft.kind != TransactionKind::entry || draft.amount.is_zero()) {
        return EntryDirection::neutral;
    }
    return draft.amount.is_positive() ? EntryDirection::income : EntryDirection::expense;
}

std::expected<std::vector<Posting>, JournalError>
build_postings(const JournalDraft &draft, const PostingAccounts &accounts) {
    if (draft.kind == TransactionKind::note) {
        if (!draft.amount.is_zero()) {
            return std::unexpected(
                error(JournalErrorCode::invalid_note, "备注行的金额必须为零"));
        }
        return std::vector<Posting>{};
    }

    if (draft.amount.is_zero()) {
        return std::unexpected(
            error(JournalErrorCode::zero_financial_entry, "收入、支出和转账的金额不能为零"));
    }

    std::vector<Posting> result;
    result.reserve(2);

    if (draft.kind == TransactionKind::transfer) {
        if (!draft.amount.is_positive() || !accounts.transfer_destination.has_value() ||
            accounts.primary == *accounts.transfer_destination) {
            return std::unexpected(error(
                JournalErrorCode::invalid_transfer,
                "转账金额需要为正数，且转出账户和转入账户必须不同"));
        }
        result.push_back(Posting{
            .account = accounts.primary,
            .category = std::nullopt,
            .signed_amount = draft.amount.negated(),
            .line_number = 1,
        });
        result.push_back(Posting{
            .account = *accounts.transfer_destination,
            .category = std::nullopt,
            .signed_amount = draft.amount,
            .line_number = 2,
        });
    } else {
        result.push_back(Posting{
            .account = accounts.primary,
            .category = draft.category,
            .signed_amount = draft.amount,
            .line_number = 1,
        });
        result.push_back(Posting{
            .account = accounts.counterparty,
            .category = draft.category,
            .signed_amount = draft.amount.negated(),
            .line_number = 2,
        });
    }

    if (!postings_are_balanced(result)) {
        return std::unexpected(
            error(JournalErrorCode::unbalanced, "分录借贷不平衡"));
    }
    return result;
}

bool postings_are_balanced(std::span<const Posting> postings) {
    Money sum{};
    for (const auto &posting : postings) {
        auto next = Money::checked_add(sum, posting.signed_amount);
        if (!next.has_value()) {
            return false;
        }
        sum = *next;
    }
    return sum.is_zero();
}

std::expected<PeriodTotals, MoneyError>
calculate_period_totals(std::span<const JournalDraft> drafts) {
    PeriodTotals totals{};
    for (const auto &draft : drafts) {
        switch (classify(draft)) {
            case EntryDirection::income: {
                auto result = Money::checked_add(totals.income, draft.amount);
                if (!result) {
                    return std::unexpected(result.error());
                }
                totals.income = *result;
                break;
            }
            case EntryDirection::expense: {
                auto result = Money::checked_add(totals.expense, draft.amount);
                if (!result) {
                    return std::unexpected(result.error());
                }
                totals.expense = *result;
                break;
            }
            case EntryDirection::neutral:
                break;
        }
    }
    return totals;
}

}  // namespace journalseed::domain
