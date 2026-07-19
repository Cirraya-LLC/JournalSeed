#include "journalseed/domain/journal_entry.h"

#include <benchmark/benchmark.h>

using namespace journalseed::domain;

static void benchmark_posting_plan(benchmark::State &state) {
    const JournalDraft draft{
        .kind = TransactionKind::entry,
        .amount = *Money::parse("-1234567.89"),
        .account = AccountId{42},
        .category = CategoryId{9},
    };
    const PostingAccounts accounts{
        .primary = AccountId{42},
        .counterparty = AccountId{2},
        .transfer_destination = std::nullopt,
    };
    for (auto _ : state) {
        benchmark::DoNotOptimize(build_postings(draft, accounts));
    }
}

BENCHMARK(benchmark_posting_plan);
