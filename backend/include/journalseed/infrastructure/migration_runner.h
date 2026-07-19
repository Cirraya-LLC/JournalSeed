#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace journalseed::infrastructure {

struct MigrationReport {
    std::size_t applied{0};
    std::size_t verified{0};
};

class MigrationRunner final {
  public:
    MigrationRunner(std::string connection_info,
                    std::filesystem::path migrations_directory);

    [[nodiscard]] MigrationReport run() const;

  private:
    std::string connection_info_;
    std::filesystem::path migrations_directory_;
};

}  // namespace journalseed::infrastructure
