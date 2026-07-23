#pragma once

#include "journalseed/infrastructure/postgres_repository.h"

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace journalseed::infrastructure {

struct EvmAdapterOptions {
    std::string baseUrl{"https://api.etherscan.io"};
    std::optional<std::string> apiKey;
    std::string chainCode{"ethereum-mainnet"};
    std::int64_t chainId{1};
    std::string nativeSymbol{"ETH"};
    std::string nativeName{"Ether"};
    std::uint16_t limit{100};
};

class EvmAdapter final {
  public:
    explicit EvmAdapter(EvmAdapterOptions options = {});

    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_wallet_transactions(std::string_view normalized_address) const;

  private:
    [[nodiscard]] drogon::Task<std::string> get(std::string query) const;

    EvmAdapterOptions options_;
};

}  // namespace journalseed::infrastructure
