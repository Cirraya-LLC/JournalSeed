#include "journalseed/domain/journal_entry.h"

#include <catch2/catch_test_macros.hpp>

using namespace journalseed::domain;

TEST_CASE("income and expense entries create balanced postings") {
    const PostingAccounts accounts{
        .primary = AccountId{12},
        .counterparty = AccountId{90},
        .transfer_destination = std::nullopt,
    };

    const JournalDraft income{
        .kind = TransactionKind::entry,
        .amount = *Money::parse("8000.00"),
        .account = AccountId{12},
        .category = CategoryId{4},
    };
    auto income_postings = build_postings(income, accounts);
    REQUIRE(income_postings);
    REQUIRE(income_postings->size() == 2);
    REQUIRE((*income_postings)[0].signed_amount.to_string() == "8000.00");
    REQUIRE((*income_postings)[1].signed_amount.to_string() == "-8000.00");
    REQUIRE(postings_are_balanced(*income_postings));

    auto expense = income;
    expense.amount = *Money::parse("-38.50");
    auto expense_postings = build_postings(expense, accounts);
    REQUIRE(expense_postings);
    REQUIRE((*expense_postings)[0].signed_amount.to_string() == "-38.50");
    REQUIRE((*expense_postings)[1].signed_amount.to_string() == "38.50");
}

TEST_CASE("transfers are neutral and move a positive magnitude") {
    const JournalDraft transfer{
        .kind = TransactionKind::transfer,
        .amount = *Money::parse("500.00"),
        .account = AccountId{1},
        .category = std::nullopt,
        .transfer_account = AccountId{2},
    };
    const PostingAccounts accounts{
        .primary = AccountId{1},
        .counterparty = AccountId{99},
        .transfer_destination = AccountId{2},
    };
    const auto postings = build_postings(transfer, accounts);
    REQUIRE(postings);
    REQUIRE((*postings)[0].signed_amount.to_string() == "-500.00");
    REQUIRE((*postings)[1].signed_amount.to_string() == "500.00");
    REQUIRE(classify(transfer) == EntryDirection::neutral);
}

TEST_CASE("zero amount is a note and produces no postings") {
    const JournalDraft note{.kind = TransactionKind::note, .amount = Money{}};
    const PostingAccounts accounts{
        .primary = AccountId{1},
        .counterparty = AccountId{99},
        .transfer_destination = std::nullopt,
    };
    const auto postings = build_postings(note, accounts);
    REQUIRE(postings);
    REQUIRE(postings->empty());
}

TEST_CASE("period totals exclude transfers and notes") {
    const std::vector drafts{
        JournalDraft{.kind = TransactionKind::entry, .amount = *Money::parse("100.00")},
        JournalDraft{.kind = TransactionKind::entry, .amount = *Money::parse("-32.50")},
        JournalDraft{.kind = TransactionKind::transfer, .amount = *Money::parse("40.00")},
        JournalDraft{.kind = TransactionKind::note, .amount = Money{}},
    };
    const auto totals = calculate_period_totals(drafts);
    REQUIRE(totals);
    REQUIRE(totals->income.to_string() == "100.00");
    REQUIRE(totals->expense.to_string() == "-32.50");
}
