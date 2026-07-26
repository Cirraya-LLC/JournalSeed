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
    // 每次签名都要单独发一次 getTransaction，慢速端点会把整轮同步拖成分钟级；
    // 单次请求超时之外再加一条整体墙钟预算，超出后返回已取得的部分结果。
    double requestTimeoutSeconds{10.0};
    double fetchBudgetSeconds{30.0};
};

class SolanaAdapter final {
  public:
    explicit SolanaAdapter(SolanaAdapterOptions options = {});

    [[nodiscard]] drogon::Task<std::vector<ChainTransactionInput>>
    fetch_wallet_transactions(std::string_view normalized_address) const;

  private:
    [[nodiscard]] drogon::Task<std::string> post(std::string body, double timeout_seconds) const;

    SolanaAdapterOptions options_;
};

}  // namespace journalseed::infrastructure
