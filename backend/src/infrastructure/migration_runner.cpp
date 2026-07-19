#include "journalseed/infrastructure/migration_runner.h"

#include <libpq-fe.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace journalseed::infrastructure {
namespace {

constexpr std::int64_t kMigrationLockId = 7'462'920'746'027'639;

struct MigrationFile {
    std::int64_t version;
    std::string name;
    std::string checksum;
    std::string sql;
};

using ResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

class PgConnection final {
  public:
    explicit PgConnection(const std::string &connection_info)
        : connection_(PQconnectdb(connection_info.c_str()), &PQfinish) {
        if (!connection_ || PQstatus(connection_.get()) != CONNECTION_OK) {
            const std::string detail = connection_ ? PQerrorMessage(connection_.get())
                                                   : "allocation failed";
            throw std::runtime_error("PostgreSQL migration connection failed: " + detail);
        }
    }

    [[nodiscard]] ResultPtr execute(const std::string &sql) const {
        ResultPtr result(PQexec(connection_.get(), sql.c_str()), &PQclear);
        validate(result, sql);
        return result;
    }

    [[nodiscard]] ResultPtr execute_params(
        const std::string &sql, const std::vector<std::string> &parameters) const {
        std::vector<const char *> values;
        values.reserve(parameters.size());
        for (const auto &parameter : parameters) values.push_back(parameter.c_str());
        ResultPtr result(PQexecParams(connection_.get(),
                                      sql.c_str(),
                                      static_cast<int>(values.size()),
                                      nullptr,
                                      values.data(),
                                      nullptr,
                                      nullptr,
                                      0),
                         &PQclear);
        validate(result, sql);
        return result;
    }

  private:
    void validate(const ResultPtr &result, std::string_view sql) const {
        if (!result) {
            throw std::runtime_error("PostgreSQL migration query failed: " +
                                     std::string(PQerrorMessage(connection_.get())));
        }
        const auto status = PQresultStatus(result.get());
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            const auto preview_size = std::min<std::size_t>(sql.size(), 80);
            throw std::runtime_error(
                "PostgreSQL migration query failed near `" +
                std::string(sql.substr(0, preview_size)) + "`: " +
                std::string(PQresultErrorMessage(result.get())));
        }
    }

    std::unique_ptr<PGconn, decltype(&PQfinish)> connection_;
};

std::string read_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read migration: " + path.string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error("failed while reading migration: " + path.string());
    }
    return contents.str();
}

std::string sha256_hex(std::string_view contents) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(
        digest.data(), reinterpret_cast<const unsigned char *>(contents.data()), contents.size());
    std::array<char, crypto_hash_sha256_BYTES * 2U + 1U> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

std::vector<MigrationFile> discover_migrations(const std::filesystem::path &directory) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(directory, filesystem_error) || filesystem_error) {
        throw std::runtime_error("migration directory does not exist: " + directory.string());
    }

    static const std::regex filename_pattern(R"(^([0-9]+)_([A-Za-z0-9_-]+)\.sql$)");
    std::vector<MigrationFile> migrations;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        std::smatch match;
        const auto filename = entry.path().filename().string();
        if (!std::regex_match(filename, match, filename_pattern)) continue;

        std::int64_t version = 0;
        const auto version_text = match[1].str();
        const auto [end, error] = std::from_chars(
            version_text.data(), version_text.data() + version_text.size(), version);
        if (error != std::errc{} || end != version_text.data() + version_text.size() ||
            version <= 0) {
            throw std::runtime_error("invalid migration version: " + filename);
        }

        auto sql = read_file(entry.path());
        migrations.push_back(MigrationFile{
            .version = version,
            .name = filename,
            .checksum = sha256_hex(sql),
            .sql = std::move(sql),
        });
    }

    std::ranges::sort(migrations, {}, &MigrationFile::version);
    for (std::size_t index = 1; index < migrations.size(); ++index) {
        if (migrations[index - 1].version == migrations[index].version) {
            throw std::runtime_error("duplicate migration version: " +
                                     std::to_string(migrations[index].version));
        }
    }
    if (migrations.empty()) {
        throw std::runtime_error("no migration scripts found in: " + directory.string());
    }
    return migrations;
}

class AdvisoryLock final {
  public:
    explicit AdvisoryLock(const PgConnection &connection) : connection_(connection) {
        static_cast<void>(connection_.execute_params(
            "SELECT pg_advisory_lock($1::bigint)", {std::to_string(kMigrationLockId)}));
    }

    AdvisoryLock(const AdvisoryLock &) = delete;
    AdvisoryLock &operator=(const AdvisoryLock &) = delete;

    ~AdvisoryLock() {
        try {
            static_cast<void>(connection_.execute_params(
                "SELECT pg_advisory_unlock($1::bigint)",
                {std::to_string(kMigrationLockId)}));
        } catch (...) {
            // Closing this dedicated connection also releases the session lock.
        }
    }

  private:
    const PgConnection &connection_;
};

}  // namespace

MigrationRunner::MigrationRunner(std::string connection_info,
                                 std::filesystem::path migrations_directory)
    : connection_info_(std::move(connection_info)),
      migrations_directory_(std::move(migrations_directory)) {
    if (connection_info_.empty()) {
        throw std::invalid_argument("migration database connection is required");
    }
}

MigrationReport MigrationRunner::run() const {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialization failed");
    const auto migrations = discover_migrations(migrations_directory_);
    const PgConnection connection(connection_info_);
    AdvisoryLock lock(connection);

    static_cast<void>(connection.execute(R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version BIGINT PRIMARY KEY,
    name TEXT NOT NULL,
    checksum TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
)
)SQL"));

    std::map<std::int64_t, std::pair<std::string, std::string>> applied;
    const auto rows = connection.execute(
        "SELECT version::text, name, checksum FROM schema_migrations ORDER BY version");
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        std::int64_t version = 0;
        const std::string version_text = PQgetvalue(rows.get(), row, 0);
        const auto [end, error] = std::from_chars(
            version_text.data(), version_text.data() + version_text.size(), version);
        if (error != std::errc{} || end != version_text.data() + version_text.size()) {
            throw std::runtime_error("invalid version in schema_migrations");
        }
        applied.emplace(version,
                        std::pair{std::string(PQgetvalue(rows.get(), row, 1)),
                                  std::string(PQgetvalue(rows.get(), row, 2))});
    }

    MigrationReport report;
    for (const auto &migration : migrations) {
        if (const auto found = applied.find(migration.version); found != applied.end()) {
            if (found->second.first != migration.name ||
                found->second.second != migration.checksum) {
                throw std::runtime_error(
                    "migration checksum mismatch for " + migration.name +
                    "; applied migrations are immutable");
            }
            ++report.verified;
            continue;
        }

        static_cast<void>(connection.execute("BEGIN"));
        try {
            static_cast<void>(connection.execute(migration.sql));
            static_cast<void>(connection.execute_params(
                "INSERT INTO schema_migrations(version, name, checksum) "
                "VALUES($1::bigint, $2, $3)",
                {std::to_string(migration.version), migration.name, migration.checksum}));
            static_cast<void>(connection.execute("COMMIT"));
            ++report.applied;
        } catch (...) {
            try {
                static_cast<void>(connection.execute("ROLLBACK"));
            } catch (...) {
            }
            throw;
        }
    }
    return report;
}

}  // namespace journalseed::infrastructure
