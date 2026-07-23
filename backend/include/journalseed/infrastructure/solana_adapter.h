#pragma once

#include "journalseed/infrastructure/postgres_repository.h"

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace journalseed::infrastructure {

struct SolanaAdapterOptions {
    std::string rpcUrl{"https://api.mainnet-beta.solana.com"};
    std::uint16_t limit{50};
};

class SolanaAdapter final {
  public:
    explicit SolanaAdapter(SolanaAdapterOptions options = {});

    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_wallet_transactions(std::string_view normalized_address) const;

  private:
    [[nodiscard]] drogon::Task<std::string> post(std::string body) const;

    SolanaAdapterOptions options_;
};

}  // namespace journalseed::infrastructure
