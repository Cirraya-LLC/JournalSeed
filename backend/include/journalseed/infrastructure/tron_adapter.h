#pragma once

#include "journalseed/infrastructure/postgres_repository.h"

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace journalseed::infrastructure {

struct TronAdapterOptions {
    std::string baseUrl{"https://api.trongrid.io"};
    std::optional<std::string> apiKey;
    std::uint16_t limit{50};
};

class TronAdapter final {
  public:
    explicit TronAdapter(TronAdapterOptions options = {});

    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_wallet_transactions(std::string_view normalized_address,
                              const ChainFetchCursor &cursor = {}) const;

  private:
    [[nodiscard]] drogon::Task<std::string> get(std::string path) const;
    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_account_pages(std::string_view address, bool trc20,
                        const ChainFetchCursor &cursor) const;

    TronAdapterOptions options_;
};

}  // namespace journalseed::infrastructure
