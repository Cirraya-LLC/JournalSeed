#pragma once

#include "journalseed/infrastructure/postgres_repository.h"

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace journalseed::infrastructure {

// Talks to any Etherscan-compatible account API. Etherscan itself serves it at /v2/api and
// needs a key; Blockscout instances serve the same actions keyless at /api, which is what
// the sync falls back to when no Etherscan key is configured.
struct EvmAdapterOptions {
    std::string baseUrl{"https://api.etherscan.io"};
    std::string apiPath{"/v2/api"};
    // Only used in log lines and error messages.
    std::string providerName{"Etherscan"};
    std::optional<std::string> apiKey;
    std::string chainCode{"ethereum-mainnet"};
    std::int64_t chainId{1};
    std::string nativeSymbol{"ETH"};
    std::string nativeName{"Ether"};
    std::uint16_t limit{100};
    // The plain transaction list only yields native-coin transfers and fees. A wallet that
    // books tokens alone can skip it, halving the calls against rate-limited free providers.
    bool includeNative{true};
};

class EvmAdapter final {
  public:
    explicit EvmAdapter(EvmAdapterOptions options = {});

    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_wallet_transactions(std::string_view normalized_address,
                              const ChainFetchCursor &cursor = {}) const;

  private:
    [[nodiscard]] drogon::Task<std::string> get(std::string query) const;
    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_action_pages(std::string_view address, std::string_view action,
                       const ChainFetchCursor &cursor) const;

    EvmAdapterOptions options_;
};

}  // namespace journalseed::infrastructure
