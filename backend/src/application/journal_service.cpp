#include "journalseed/application/journal_service.h"

#include "journalseed/domain/journal_entry.h"
#include "journalseed/domain/money.h"
#include "journalseed/domain/chain.h"
#include "journalseed/domain/tron.h"
#include "journalseed/infrastructure/evm_adapter.h"
#include "journalseed/infrastructure/solana_adapter.h"
#include "journalseed/infrastructure/tron_adapter.h"

#include <drogon/orm/Exception.h>
#include <glaze/glaze.hpp>
#include <sodium.h>
#include <trantor/utils/Logger.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

namespace journalseed::application {
namespace {

std::string trimmed(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
                          return std::isspace(character) != 0;
                      }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

// Problem 的唯一构造入口：契约规定 type 恒等于 "/problems/" + code，所以这里由 code
// 推导 type，而不是让每个出错点各写一遍字面量。手写过一次就会漂移——database_error 的
// type 曾经写成 "/problems/database-error"，与 code 里的下划线对不上。
Problem make_problem(std::uint16_t status,
                     std::string code,
                     std::string title,
                     std::string detail) {
    return Problem{
        .type = "/problems/" + code,
        .title = std::move(title),
        .status = status,
        .code = std::move(code),
        .detail = std::move(detail),
        .fields = {},
    };
}

// 客户端看到的固定文案与服务端日志里的完整诊断之间的对账用编号。
std::string diagnostic_reference() {
    std::array<unsigned char, 6> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, bytes.size() * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

// 异常正文可能包含 SQL、连接串，甚至第三方 RPC 原样回传的任意文本；一旦进入
// problem.detail 就会被回显给调用方，还会被 background_jobs.error 落库，成为运维
// 信任面上的注入点。因此完整信息只写日志，客户端只拿到固定文案加事件编号。
template <typename T>
ServiceResult<T> database_failure(const std::exception &exception) {
    const auto reference = diagnostic_reference();
    LOG_ERROR << "database_error ref=" << reference << " detail=" << exception.what();
    return std::unexpected(make_problem(
        500, "database_error", "数据库操作未完成",
        "服务端处理失败，请在服务端日志中查看事件编号 " + reference));
}

// 实体英文名 -> 客户端可见的中文名词。仓储抛出的 EntityNotFound 与服务层自己做的
// 存在性预检共用这张表，保证同一种“对象不存在”在各个入口给出一致的 code 与文案。
std::string_view entity_noun(std::string_view entity) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> nouns{{
        {"ledger", "账本"},
        {"account", "账户"},
        {"category", "分类"},
        {"column", "列"},
        {"row", "流水"},
        {"job", "任务"},
        {"wallet", "钱包"},
        {"address_label", "地址标签"},
    }};
    const auto found = std::ranges::find(nouns, entity, &std::pair<std::string_view, std::string_view>::first);
    return found == nouns.end() ? std::string_view{"对象"} : found->second;
}

// 目标实体不存在属于调用方的问题，不是服务端故障：返回 404 并且不写 ERROR 日志。
Problem not_found_problem(std::string_view entity) {
    const std::string noun(entity_noun(entity));
    return make_problem(404, std::string(entity) + "_not_found", noun + "不存在",
                        "找不到指定的" + noun + "，可能已被删除");
}

// 唯一约束冲突的文案表。每种冲突的成因不一样（账本重名是全局的、账户与分类重名是账本
// 内的、钱包地址是“已经在监控了”、地址标签是“已经标过了”），拼一条通用句子只会变成
// “同一账本内账本的名称不能重复”这种读不通的话，所以每种各写各的。
struct DuplicateCopy {
    std::string_view entity;
    std::string_view title;
    std::string_view detail;
    std::string_view field_message;
};

// 唯一约束冲突同样是调用方的问题——重新敲了一个已经被占用的取值——而不是服务端故障：
// 和 404 一样返回明确的 Problem，并且不写 ERROR 日志。fields 必须点名冲突字段，否则
// 调用方只知道“撞了”，不知道撞在哪一格上。
Problem duplicate_problem(const infrastructure::DuplicateValue &conflict) {
    static constexpr std::array<DuplicateCopy, 5> copies{{
        {"ledger", "账本名称已被占用", "已经有一个同名的账本，请换一个名称", "该名称已被占用"},
        {"account", "账户名称已被占用", "这个账本里已经有同名的账户，请换一个名称",
         "该名称已被占用"},
        {"category", "分类名称已被占用", "这个账本里已经有同名同方向的分类，请换一个名称",
         "该名称在这个收支方向上已被占用"},
        {"wallet", "钱包地址已在监控中", "这个账本里已经有一个钱包在监控同一条链上的这个地址",
         "该地址已经在这个账本里被监控"},
        {"address_label", "地址标签已存在", "这个账本里已经为该地址建立过标签，请直接修改已有标签",
         "该地址已经有标签"},
    }};
    const auto found = std::ranges::find(copies, conflict.entity(), &DuplicateCopy::entity);
    const DuplicateCopy copy =
        found == copies.end()
            ? DuplicateCopy{"", "内容重复", "已经存在一条相同的记录，请修改后重试", "该取值已被占用"}
            : *found;
    auto issue = make_problem(409, "duplicate_value", std::string(copy.title),
                              std::string(copy.detail));
    issue.fields.emplace(conflict.field(), std::string(copy.field_message));
    return issue;
}

// 仓储异常的统一出口：类型化的“不存在”走 404，类型化的“取值重复”走 409，
// 其余一律按数据库故障处理。这里用 dynamic_cast 判定，而不是匹配异常文案。
template <typename T>
ServiceResult<T> repository_failure(const std::exception &exception) {
    if (const auto *missing = dynamic_cast<const EntityNotFound *>(&exception)) {
        return std::unexpected(not_found_problem(missing->entity()));
    }
    if (const auto *duplicate =
            dynamic_cast<const infrastructure::DuplicateValue *>(&exception)) {
        return std::unexpected(duplicate_problem(*duplicate));
    }
    return database_failure<T>(exception);
}

// 标识会被拼进 SQL 的 ::uuid 转换：格式不合法的取值必须在进库之前挡下来，
// 否则只会得到一条 PostgreSQL 的 invalid input syntax 与 500。
bool is_uuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (character != '-') return false;
        } else if (std::isxdigit(character) == 0) {
            return false;
        }
    }
    return true;
}

// 严格的公历日期判定：只做正则会把 2026-13-45 放进 SQL，最终变成 500。
bool is_iso_date(std::string_view value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (std::isdigit(static_cast<unsigned char>(value[index])) == 0) return false;
    }
    const auto number = [value](std::size_t offset, std::size_t length) {
        int parsed = 0;
        std::from_chars(value.data() + offset, value.data() + offset + length, parsed);
        return parsed;
    };
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int limit = month == 2 && leap ? 29 : days[static_cast<std::size_t>(month - 1)];
    return day <= limit;
}

// Money::parse 把两种情况都归到 MoneyErrorCode::invalid_scale：小数位多于 18 位
//（是个合法的十进制数，只是放不进存储标度），以及小数点后一位数字都没有的 "1."
//（压根不是十进制数）。前者是精度问题，后者是格式问题，对外的 code 不一样，
// 所以这里直接按小数位数把两者分开，而不是照单全收地信任那一个错误码。
bool exceeds_storage_scale(std::string_view amount) {
    const auto decimal = amount.find('.');
    if (decimal == std::string_view::npos) return false;
    return amount.size() - decimal - 1 >
           static_cast<std::size_t>(domain::Money::storage_scale());
}

LuaFunctionView lua_function_view(const lua::FunctionMetadata &function) {
    LuaFunctionView view{
        .name = function.name,
        .version = function.version,
        .description = function.description,
        .script = function.script,
        .source = function.source,
        .params = {},
    };
    for (const auto &parameter : function.params) {
        view.params.push_back(LuaParameterView{
            .name = parameter.name,
            .type = parameter.type,
            .label = parameter.label,
            .required = parameter.required,
            .options = parameter.options,
        });
    }
    return view;
}

template <typename T>
ServiceResult<T> lua_failure(const lua::RegistryError &error) {
    std::uint16_t status = 422;
    std::string code = "lua_execution_error";
    if (error.code == lua::RegistryErrorCode::function_missing) {
        status = 404;
        code = "function_missing";
    } else if (error.code == lua::RegistryErrorCode::directory_error ||
               error.code == lua::RegistryErrorCode::write_error) {
        status = 500;
        code = "lua_write_error";
    } else if (error.code == lua::RegistryErrorCode::function_conflict) {
        status = 409;
        code = "function_conflict";
    } else if (error.code == lua::RegistryErrorCode::schema_error ||
               error.code == lua::RegistryErrorCode::script_error) {
        code = "lua_script_error";
    } else if (error.code == lua::RegistryErrorCode::limit_exceeded) {
        code = "lua_limit_exceeded";
    }
    return std::unexpected(make_problem(
        status, std::move(code), "函数脚本未保存",
        error.script.empty() ? error.message : error.script + ": " + error.message));
}

bool is_address_label_kind(std::string_view kind) {
    static constexpr std::array<std::string_view, 6> kinds{
        "customer", "self", "exchange", "merchant", "contract", "other"};
    return std::ranges::find(kinds, kind) != kinds.end();
}

// 链配置 secretbox 主密钥。
//
// 首选形态是运维直接提供 32 字节随机密钥（十六进制或 Base64），此时完全不经过 KDF。
// 只有在给出口令时才退回 Argon2id；口令至少 16 个字符，避免数据库被拖走后用 GPU
// 直接离线爆破。数据库里没有 salt / key_version 列（见 README「已知缺口」一节：密钥
// 目前无法轮换），因此口令派生必须是确定性的，这里使用固定的应用级 salt。
constexpr std::size_t kChainSettingsKeyBytes = crypto_secretbox_KEYBYTES;
constexpr std::size_t kMinimumPassphraseBytes = 16;
constexpr unsigned char kChainSettingsKdfSalt[crypto_pwhash_SALTBYTES] = {
    'J', 'o', 'u', 'r', 'n', 'a', 'l', 'S', 'e', 'e', 'd', '-', 'c', 'h', 'a', 'i',
};

struct ChainSettingsKey {
    std::array<unsigned char, kChainSettingsKeyBytes> bytes{};
    bool valid{false};
    std::string failure;
};

bool decode_fixed_key(std::string_view material, std::array<unsigned char, kChainSettingsKeyBytes> &out) {
    std::size_t decoded_size = 0;
    if (material.size() == kChainSettingsKeyBytes * 2 &&
        sodium_hex2bin(out.data(), out.size(), material.data(), material.size(), nullptr,
                       &decoded_size, nullptr) == 0 &&
        decoded_size == out.size()) {
        return true;
    }
    constexpr std::array<int, 4> variants{
        sodium_base64_VARIANT_ORIGINAL,
        sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
        sodium_base64_VARIANT_URLSAFE,
        sodium_base64_VARIANT_URLSAFE_NO_PADDING,
    };
    for (const auto variant : variants) {
        decoded_size = 0;
        if (sodium_base642bin(out.data(), out.size(), material.data(), material.size(), nullptr,
                              &decoded_size, nullptr, variant) == 0 &&
            decoded_size == out.size()) {
            return true;
        }
    }
    return false;
}

// 派生一次即缓存：Argon2id 每次几百毫秒，而同步流程每轮都要解密。缓存缓冲区用
// sodium_mlock 钉在内存里，避免主密钥被换页到磁盘。
const ChainSettingsKey &chain_settings_key() {
    static const ChainSettingsKey cached = [] {
        ChainSettingsKey result;
        const char *configured = std::getenv("JOURNALSEED_CHAIN_SETTINGS_KEY");
        if (configured == nullptr || *configured == '\0') {
            result.failure = "服务端未配置 JOURNALSEED_CHAIN_SETTINGS_KEY";
            return result;
        }
        static_cast<void>(sodium_mlock(result.bytes.data(), result.bytes.size()));
        std::string material = trimmed(configured);
        if (decode_fixed_key(material, result.bytes)) {
            result.valid = true;
        } else if (material.size() < kMinimumPassphraseBytes) {
            result.failure =
                "JOURNALSEED_CHAIN_SETTINGS_KEY 强度不足：请提供 32 字节的十六进制或 Base64 "
                "随机密钥，或至少 16 个字符的口令";
        } else if (crypto_pwhash(result.bytes.data(), result.bytes.size(), material.data(),
                                 material.size(), kChainSettingsKdfSalt,
                                 crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                 crypto_pwhash_ALG_ARGON2ID13) != 0) {
            result.failure = "链配置密钥派生失败：Argon2id 无法分配所需内存";
        } else {
            result.valid = true;
        }
        if (!material.empty()) sodium_memzero(material.data(), material.size());
        if (!result.valid) sodium_memzero(result.bytes.data(), result.bytes.size());
        return result;
    }();
    return cached;
}

Problem chain_settings_key_problem(std::string_view title) {
    const auto &key = chain_settings_key();
    return make_problem(500, "chain_settings_key_unconfigured", std::string(title),
                        key.failure);
}

Problem chain_settings_undecryptable_problem() {
    return make_problem(500, "chain_settings_secret_undecryptable", "链配置无法解密",
                        "已保存的链配置无法用当前 JOURNALSEED_CHAIN_SETTINGS_KEY 解密。"
                        "若刚刚更换过密钥或从旧版本升级（密钥派生方式已变更），"
                        "请在链配置页面重新填写并保存这些凭据");
}

std::expected<infrastructure::EncryptedSecretRecord, Problem>
encrypt_secret(std::string_view value) {
    const auto &key = chain_settings_key();
    if (!key.valid) return std::unexpected(chain_settings_key_problem("链配置无法保存"));

    std::array<unsigned char, crypto_secretbox_NONCEBYTES> nonce{};
    randombytes_buf(nonce.data(), nonce.size());
    std::vector<unsigned char> ciphertext(value.size() + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(ciphertext.data(),
                          reinterpret_cast<const unsigned char *>(value.data()), value.size(),
                          nonce.data(), key.bytes.data());

    std::string ciphertext_hex(ciphertext.size() * 2 + 1, '\0');
    sodium_bin2hex(ciphertext_hex.data(), ciphertext_hex.size(), ciphertext.data(), ciphertext.size());
    ciphertext_hex.pop_back();
    std::string nonce_hex(nonce.size() * 2 + 1, '\0');
    sodium_bin2hex(nonce_hex.data(), nonce_hex.size(), nonce.data(), nonce.size());
    nonce_hex.pop_back();
    return infrastructure::EncryptedSecretRecord{
        .ciphertextHex = std::move(ciphertext_hex),
        .nonceHex = std::move(nonce_hex),
    };
}

// 返回空 optional 只代表"这项配置没有保存过"；密钥缺失或密文无法解开一律返回错误，
// 由调用方向管理员报错，而不是静默退化成匿名调用。
std::expected<std::optional<std::string>, Problem>
decrypt_secret(const infrastructure::EncryptedSecretRecord &secret) {
    if (!secret.ciphertextHex || !secret.nonceHex || secret.ciphertextHex->empty() || secret.nonceHex->empty()) {
        return std::optional<std::string>{};
    }
    const auto &key = chain_settings_key();
    if (!key.valid) return std::unexpected(chain_settings_key_problem("链配置无法读取"));

    std::vector<unsigned char> ciphertext(secret.ciphertextHex->size() / 2);
    std::size_t ciphertext_size = 0;
    if (sodium_hex2bin(ciphertext.data(), ciphertext.size(), secret.ciphertextHex->data(),
                       secret.ciphertextHex->size(), nullptr, &ciphertext_size, nullptr) != 0) {
        return std::unexpected(chain_settings_undecryptable_problem());
    }
    ciphertext.resize(ciphertext_size);
    if (ciphertext.size() < crypto_secretbox_MACBYTES) {
        return std::unexpected(chain_settings_undecryptable_problem());
    }

    std::array<unsigned char, crypto_secretbox_NONCEBYTES> nonce{};
    std::size_t nonce_size = 0;
    if (sodium_hex2bin(nonce.data(), nonce.size(), secret.nonceHex->data(), secret.nonceHex->size(),
                       nullptr, &nonce_size, nullptr) != 0 || nonce_size != nonce.size()) {
        return std::unexpected(chain_settings_undecryptable_problem());
    }

    std::vector<unsigned char> plain(ciphertext.size() - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(plain.data(), ciphertext.data(), ciphertext.size(), nonce.data(),
                                   key.bytes.data()) != 0) {
        if (!plain.empty()) sodium_memzero(plain.data(), plain.size());
        LOG_ERROR << "链配置密文解密失败：JOURNALSEED_CHAIN_SETTINGS_KEY 可能已更换";
        return std::unexpected(chain_settings_undecryptable_problem());
    }
    std::string decrypted(reinterpret_cast<const char *>(plain.data()), plain.size());
    if (!plain.empty()) sodium_memzero(plain.data(), plain.size());
    return std::optional<std::string>{std::move(decrypted)};
}

// ---------------------------------------------------------------------------
// 出网目标校验（SSRF 防护）
//
// 选型：解析时校验（DNS resolution-time checking），而不是固定主机白名单。自建部署
// 会接各种自选 RPC 供应商，写死白名单没法用。残余风险是 DNS 重绑定：这里解析出的
// 地址和 Drogon 真正建连时解析出的地址可能不同，攻击者若同时控制权威 DNS 并使用极短
// TTL，仍可能把连接引向内网。要彻底关闭这个窗口需要固定 IP 建连（自行解析后按 IP
// 连接并单独设置 SNI/Host），代价与本应用的定位不相称，故明确接受。
//
// 唯一的显式豁免是 JOURNALSEED_RPC_ALLOW_HOSTS（逗号分隔的主机名列表），只能由拥有
// shell 的运维通过进程环境设置，管理 API 无法改写；用于把节点部署在内网/本机的场景。
// ---------------------------------------------------------------------------

bool is_blocked_ipv4(const unsigned char *octets) {
    const unsigned first = octets[0];
    const unsigned second = octets[1];
    if (first == 0) return true;                                      // 0.0.0.0/8 本网络与未指定地址
    if (first == 10) return true;                                     // RFC1918
    if (first == 127) return true;                                    // 回环
    if (first == 100 && second >= 64 && second <= 127) return true;   // 100.64.0.0/10 运营商级 NAT
    if (first == 169 && second == 254) return true;                   // 链路本地，含 169.254.169.254 云元数据
    if (first == 172 && second >= 16 && second <= 31) return true;    // RFC1918
    if (first == 192 && second == 0) return true;                     // 192.0.0.0/16 协议保留与文档地址
    if (first == 192 && second == 168) return true;                   // RFC1918
    if (first == 198 && (second == 18 || second == 19)) return true;  // 198.18.0.0/15 基准测试
    if (first >= 224) return true;                                    // 组播、保留段与广播地址
    return false;
}

bool is_blocked_ipv6(const in6_addr &address) {
    const unsigned char *bytes = address.s6_addr;
    // 所有把 IPv4 嵌在低 32 位的标准写法都要先还原再按 IPv4 规则判定，
    // 否则 ::ffff:169.254.169.254 之类的写法可以绕过整套内网判断。
    static constexpr unsigned char v4_mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    static constexpr unsigned char v4_translated_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0};
    static constexpr unsigned char nat64_prefix[12] = {0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
    if (std::memcmp(bytes, v4_mapped_prefix, sizeof(v4_mapped_prefix)) == 0 ||
        std::memcmp(bytes, v4_translated_prefix, sizeof(v4_translated_prefix)) == 0 ||
        std::memcmp(bytes, nat64_prefix, sizeof(nat64_prefix)) == 0) {
        return is_blocked_ipv4(bytes + 12);
    }
    if (bytes[0] == 0xFF) return true;                                  // ff00::/8 组播
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0U) == 0x80U) return true;   // fe80::/10 链路本地
    if ((bytes[0] & 0xFEU) == 0xFCU) return true;                       // fc00::/7 唯一本地
    // ::/96 一并拒绝：未指定地址、::1 回环，以及已废弃的 IPv4 兼容写法
    return std::all_of(bytes, bytes + 12, [](unsigned char byte) { return byte == 0; });
}

std::string lowercased(std::string_view value) {
    std::string output(value);
    std::ranges::transform(output, output.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return output;
}

struct EgressTarget {
    std::string scheme;
    std::string host;
    std::string port;
};

std::optional<EgressTarget> parse_egress_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    EgressTarget target;
    target.scheme = lowercased(url.substr(0, scheme_end));
    if (target.scheme != "http" && target.scheme != "https") return std::nullopt;

    const auto rest = url.substr(scheme_end + 3);
    const auto authority_end = rest.find_first_of("/?#");
    const auto authority = authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    if (authority.empty()) return std::nullopt;
    if (authority.find('@') != std::string_view::npos) return std::nullopt;  // 拒绝 URL 内嵌凭据

    target.port = target.scheme == "https" ? "443" : "80";
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        target.host = lowercased(authority.substr(1, close - 1));
        const auto tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') return std::nullopt;
            target.port = std::string(tail.substr(1));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            target.host = lowercased(authority);
        } else {
            target.host = lowercased(authority.substr(0, colon));
            target.port = std::string(authority.substr(colon + 1));
        }
    }
    if (target.host.empty() || target.port.empty() || target.port.size() > 5) return std::nullopt;
    std::uint32_t port_number = 0;
    const auto *port_begin = target.port.data();
    const auto *port_end = port_begin + target.port.size();
    const auto [ptr, ec] = std::from_chars(port_begin, port_end, port_number);
    if (ec != std::errc{} || ptr != port_end || port_number == 0 || port_number > 65535) {
        return std::nullopt;
    }
    return target;
}

bool host_explicitly_allowed(const std::string &host) {
    const char *allowlist = std::getenv("JOURNALSEED_RPC_ALLOW_HOSTS");
    if (allowlist == nullptr || *allowlist == '\0') return false;
    std::string_view remaining(allowlist);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        auto entry = comma == std::string_view::npos ? remaining : remaining.substr(0, comma);
        remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
        while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.front())) != 0) {
            entry.remove_prefix(1);
        }
        while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.back())) != 0) {
            entry.remove_suffix(1);
        }
        if (!entry.empty() && lowercased(entry) == host) return true;
    }
    return false;
}

// 返回拒绝原因（我方固定文案，绝不含上游内容），通过校验时返回 std::nullopt。
std::optional<std::string> egress_rejection(std::string_view url) {
    if (url.empty() || url.size() > 2048) return "RPC 地址长度不合法";
    if (std::ranges::any_of(url, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte <= 0x20U || byte == 0x7FU;
        })) {
        return "RPC 地址不能包含空白或控制字符";
    }
    const auto target = parse_egress_url(url);
    if (!target) return "RPC 地址需要是不含账号密码、端口有效的 http:// 或 https:// 地址";
    if (host_explicitly_allowed(target->host)) return std::nullopt;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *resolved = nullptr;
    if (getaddrinfo(target->host.c_str(), target->port.c_str(), &hints, &resolved) != 0 ||
        resolved == nullptr) {
        return "RPC 主机无法解析";
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(resolved, &freeaddrinfo);
    constexpr std::string_view blocked = "RPC 主机解析到本机、内网、链路本地或组播地址";
    for (const addrinfo *entry = resolved; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET && entry->ai_addr != nullptr) {
            const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(entry->ai_addr);
            const auto *octets = reinterpret_cast<const unsigned char *>(&ipv4->sin_addr.s_addr);
            if (is_blocked_ipv4(octets)) return std::string(blocked);
        } else if (entry->ai_family == AF_INET6 && entry->ai_addr != nullptr) {
            const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(entry->ai_addr);
            if (is_blocked_ipv6(ipv6->sin6_addr)) return std::string(blocked);
        } else {
            return "RPC 主机解析到不支持的地址族";
        }
    }
    return std::nullopt;
}

Problem rpc_endpoint_problem(std::string_view label, std::string_view rejection) {
    return make_problem(422, "rpc_endpoint_rejected", "链 RPC 地址不可用",
                        std::string(label) + " 端点未通过出网校验：" + std::string(rejection));
}



std::vector<infrastructure::ChainTransactionInput> mock_tron_transactions(
    std::string_view wallet_address) {
    constexpr std::string_view counterparty = "TLa2f6VPqDgRE67v1736s7bJ8Ray5wYjU7";
    constexpr std::string_view usdt_contract = "TXLAQ63Xg1NAzckPwKHvzw7CSEmLMEqcdj";
    const auto contract = domain::tron::normalize_address(usdt_contract).value();
    const std::string wallet(wallet_address);
    const std::string source(counterparty);

    return {
        infrastructure::ChainTransactionInput{
            .txHash = "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0011223344556677",
            .blockNumber = 12345678,
            .blockTimestampMs = 1760000000000,
            .rawJson = R"({"provider":"mock","chain":"tron-mainnet","asset":"TRX"})",
            .movements = {infrastructure::ChainAssetMovementInput{
                .movementKey = domain::chain::movement_key("tron-mainnet", "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0011223344556677", "incoming", "TRX", wallet, 1),
                .direction = "incoming", .assetSymbol = "TRX", .assetName = "TRON", .assetDecimals = 6,
                .isNative = true, .amount = "12.50", .fromAddress = source,
                .fromAddressNormalized = source, .toAddress = wallet, .toAddressNormalized = wallet,
            }},
        },
        infrastructure::ChainTransactionInput{
            .txHash = "b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef001122334455667788",
            .blockNumber = 12345679,
            .blockTimestampMs = 1760000060000,
            .rawJson = R"({"provider":"mock","chain":"tron-mainnet","asset":"USDT"})",
            .movements = {infrastructure::ChainAssetMovementInput{
                .movementKey = domain::chain::movement_key("tron-mainnet", "b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef001122334455667788", "incoming", "USDT:" + contract, wallet, 1),
                .direction = "incoming", .assetSymbol = "USDT", .assetName = "Tether USD", .assetDecimals = 6,
                .contractAddress = contract, .contractAddressNormalized = contract, .amount = "25.00",
                .fromAddress = source, .fromAddressNormalized = source, .toAddress = wallet, .toAddressNormalized = wallet,
            }},
        },
    };
}

std::vector<infrastructure::ChainTransactionInput> mock_chain_transactions(
    std::string_view chain_code,
    std::string_view wallet_address) {
    if (chain_code == "tron-mainnet") return mock_tron_transactions(wallet_address);

    const std::string chain(chain_code);
    const std::string wallet(wallet_address);
    if (domain::chain::is_evm_chain(chain)) {
        const bool polygon = chain == "polygon-mainnet";
        const std::string native_symbol = polygon ? "POL" : "ETH";
        const std::string native_name = polygon ? "Polygon Ecosystem Token" : "Ether";
        const std::string counterparty = polygon
            ? "0x2222222222222222222222222222222222222222"
            : "0x1111111111111111111111111111111111111111";
        const std::string usdc_contract = polygon
            ? "0x3c499c542cef5e3811e1192ce70d8cc03d5c3359"
            : "0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48";
        const std::string native_hash = polygon
            ? "0x9000000000000000000000000000000000000000000000000000000000000137"
            : "0x8000000000000000000000000000000000000000000000000000000000000001";
        const std::string token_hash = polygon
            ? "0x9100000000000000000000000000000000000000000000000000000000000137"
            : "0x8100000000000000000000000000000000000000000000000000000000000001";
        return {
            infrastructure::ChainTransactionInput{
                .txHash = native_hash,
                .blockNumber = polygon ? std::optional<std::int64_t>{48000000} : std::optional<std::int64_t>{19000000},
                .blockTimestampMs = 1760000120000,
                .rawJson = R"({"provider":"mock","type":"native"})",
                .movements = {
                    infrastructure::ChainAssetMovementInput{
                        .movementKey = domain::chain::movement_key(chain, native_hash, "incoming", native_symbol, wallet, 1),
                        .direction = "incoming", .assetSymbol = native_symbol, .assetName = native_name,
                        .assetDecimals = 18, .isNative = true, .amount = polygon ? "42.00" : "0.75",
                        .fromAddress = counterparty, .fromAddressNormalized = counterparty,
                        .toAddress = wallet, .toAddressNormalized = wallet,
                    }
                },
            },
            infrastructure::ChainTransactionInput{
                .txHash = token_hash,
                .blockNumber = polygon ? std::optional<std::int64_t>{48000001} : std::optional<std::int64_t>{19000001},
                .blockTimestampMs = 1760000180000,
                .rawJson = R"({"provider":"mock","type":"erc20"})",
                .movements = {
                    infrastructure::ChainAssetMovementInput{
                        .movementKey = domain::chain::movement_key(chain, token_hash, "outgoing", "USDC:" + usdc_contract, wallet, 1),
                        .direction = "outgoing", .assetSymbol = "USDC", .assetName = "USD Coin",
                        .assetDecimals = 6, .contractAddress = usdc_contract,
                        .contractAddressNormalized = usdc_contract, .amount = "15.25",
                        .fromAddress = wallet, .fromAddressNormalized = wallet,
                        .toAddress = counterparty, .toAddressNormalized = counterparty,
                    },
                    infrastructure::ChainAssetMovementInput{
                        .movementKey = domain::chain::movement_key(chain, token_hash, "fee", native_symbol, wallet, 2),
                        .direction = "fee", .assetSymbol = native_symbol, .assetName = native_name,
                        .assetDecimals = 18, .isNative = true, .amount = polygon ? "0.001" : "0.0021",
                        .fromAddress = wallet, .fromAddressNormalized = wallet,
                    }
                },
            },
        };
    }

    if (chain == "solana-mainnet") {
        constexpr std::string_view counterparty = "11111111111111111111111111111111";
        constexpr std::string_view usdc_mint = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
        return {
            infrastructure::ChainTransactionInput{
                .txHash = "5SolMockNative111111111111111111111111111111111111111111111111",
                .blockNumber = 280000000,
                .blockTimestampMs = 1760000240000,
                .rawJson = R"({"provider":"mock","chain":"solana-mainnet","asset":"SOL"})",
                .movements = {infrastructure::ChainAssetMovementInput{
                    .movementKey = domain::chain::movement_key(chain, "5SolMockNative111111111111111111111111111111111111111111111111", "incoming", "SOL", wallet, 1),
                    .direction = "incoming", .assetSymbol = "SOL", .assetName = "Solana", .assetDecimals = 9,
                    .isNative = true, .amount = "1.23456789", .fromAddress = std::string(counterparty),
                    .fromAddressNormalized = std::string(counterparty), .toAddress = wallet, .toAddressNormalized = wallet,
                }},
            },
            infrastructure::ChainTransactionInput{
                .txHash = "5SolMockToken2222222222222222222222222222222222222222222222222",
                .blockNumber = 280000001,
                .blockTimestampMs = 1760000300000,
                .rawJson = R"({"provider":"mock","chain":"solana-mainnet","asset":"USDC"})",
                .movements = {
                    infrastructure::ChainAssetMovementInput{
                        .movementKey = domain::chain::movement_key(chain, "5SolMockToken2222222222222222222222222222222222222222222222222", "outgoing", std::string("USDC:") + std::string(usdc_mint), wallet, 1),
                        .direction = "outgoing", .assetSymbol = "USDC", .assetName = "USD Coin", .assetDecimals = 6,
                        .contractAddress = std::string(usdc_mint), .contractAddressNormalized = std::string(usdc_mint),
                        .amount = "8.00", .fromAddress = wallet, .fromAddressNormalized = wallet,
                        .toAddress = std::string(counterparty), .toAddressNormalized = std::string(counterparty),
                    },
                    infrastructure::ChainAssetMovementInput{
                        .movementKey = domain::chain::movement_key(chain, "5SolMockToken2222222222222222222222222222222222222222222222222", "fee", "SOL", wallet, 2),
                        .direction = "fee", .assetSymbol = "SOL", .assetName = "Solana", .assetDecimals = 9,
                        .isNative = true, .amount = "0.000005", .fromAddress = wallet, .fromAddressNormalized = wallet,
                    },
                },
            },
        };
    }
    return {};
}


}  // namespace

JournalService::JournalService(
    std::shared_ptr<infrastructure::PostgresRepository> repository,
    std::shared_ptr<lua::FunctionRegistry> functions)
    : repository_(std::move(repository)), functions_(std::move(functions)) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

Problem JournalService::problem(std::uint16_t status,
                                std::string code,
                                std::string title,
                                std::string detail) {
    return make_problem(status, std::move(code), std::move(title), std::move(detail));
}

std::string JournalService::random_token() {
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING)>
        encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return encoded.data();
}

std::string JournalService::hash_token(std::string_view token) {
    std::array<unsigned char, crypto_generichash_BYTES> hash{};
    crypto_generichash(hash.data(), hash.size(),
                       reinterpret_cast<const unsigned char *>(token.data()), token.size(),
                       nullptr, 0);
    std::array<char, crypto_generichash_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return hex.data();
}

drogon::Task<ServiceResult<SetupStatus>> JournalService::setup_status() const {
    try {
        co_return SetupStatus{.required = co_await repository_->setup_required()};
    } catch (const std::exception &exception) {
        co_return repository_failure<SetupStatus>(exception);
    }
}

drogon::Task<ServiceResult<SessionEnvelope>>
JournalService::setup(SetupRequest input) const {
    input.username = trimmed(std::move(input.username));
    input.ledgerName = trimmed(std::move(input.ledgerName));
    if (input.username.empty() || input.username.size() > 80) {
        auto issue = problem(422, "validation_error", "设置内容需要调整",
                             "管理员名称需要包含 1 到 80 个字符");
        issue.fields.emplace("username", "请填写管理员名称");
        co_return std::unexpected(std::move(issue));
    }
    if (input.password.size() < 10 || input.password.size() > 1024) {
        auto issue = problem(422, "validation_error", "设置内容需要调整",
                             "密码需要包含 10 到 1024 个字符");
        issue.fields.emplace("password", "密码至少需要 10 个字符");
        co_return std::unexpected(std::move(issue));
    }
    if (input.ledgerName.empty() || input.ledgerName.size() > 120) {
        auto issue = problem(422, "validation_error", "设置内容需要调整",
                             "账本名称需要包含 1 到 120 个字符");
        issue.fields.emplace("ledgerName", "请填写首个账本名称");
        co_return std::unexpected(std::move(issue));
    }

    std::array<char, crypto_pwhash_STRBYTES> password_hash{};
    if (crypto_pwhash_str_alg(password_hash.data(), input.password.data(), input.password.size(),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE,
                             crypto_pwhash_ALG_ARGON2ID13) != 0) {
        co_return std::unexpected(problem(500, "password_hash_error", "管理员创建失败",
                                          "密码哈希所需内存分配失败"));
    }

    const auto cookie_token = random_token();
    const auto csrf_token = random_token();
    try {
        const auto record = co_await repository_->create_initial_setup(
            input, password_hash.data(), hash_token(cookie_token), hash_token(csrf_token));
        co_return SessionEnvelope{
            .session = SessionView{
                .user = UserView{.id = record.userPublicId, .username = record.username},
                .csrfToken = csrf_token,
                .expiresAt = record.expiresAt,
            },
            .cookieToken = cookie_token,
        };
    } catch (const std::exception &) {
        co_return std::unexpected(problem(409, "setup_already_completed", "设置已经完成",
                                          "已有管理员时不会再次创建管理员"));
    }
}

drogon::Task<ServiceResult<SessionEnvelope>>
JournalService::login(LoginRequest input) const {
    input.username = trimmed(std::move(input.username));
    try {
        const auto user = co_await repository_->find_user(input.username);
        if (!user || crypto_pwhash_str_verify(user->passwordHash.c_str(), input.password.data(),
                                             input.password.size()) != 0) {
            co_return std::unexpected(problem(401, "invalid_credentials", "登录信息不匹配",
                                              "请检查管理员名称和密码"));
        }
        if (crypto_pwhash_str_needs_rehash(user->passwordHash.c_str(),
                                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            // Rehashing is intentionally deferred to a password-changing flow;
            // authentication remains constant in observable behavior here.
        }

        const auto cookie_token = random_token();
        const auto csrf_token = random_token();
        const auto session = co_await repository_->create_session(
            user->id, hash_token(cookie_token), hash_token(csrf_token));
        co_return SessionEnvelope{
            .session = SessionView{
                .user = UserView{.id = session.userPublicId, .username = session.username},
                .csrfToken = csrf_token,
                .expiresAt = session.expiresAt,
            },
            .cookieToken = cookie_token,
        };
    } catch (const std::exception &exception) {
        co_return repository_failure<SessionEnvelope>(exception);
    }
}

drogon::Task<ServiceResult<AuthContext>>
JournalService::authenticate(std::string_view cookie_token,
                             std::optional<std::string_view> csrf_token) const {
    if (cookie_token.empty()) {
        co_return std::unexpected(problem(401, "session_required", "需要登录",
                                          "当前会话不存在或已过期"));
    }
    // 带着会话 cookie 但没有携带 CSRF 头，是写入校验失败而不是未登录：
    // 与“CSRF 头存在但取值不对”保持同一种结果，避免前端把它当成会话过期去重登录。
    if (csrf_token && csrf_token->empty()) {
        co_return std::unexpected(problem(403, "csrf_mismatch", "写入校验失败",
                                          "请求缺少 X-JournalSeed-CSRF 头，请刷新页面后再保存"));
    }
    try {
        std::optional<std::string> csrf_hash;
        if (csrf_token) csrf_hash = hash_token(*csrf_token);
        const auto session = co_await repository_->find_session(hash_token(cookie_token), csrf_hash);
        if (!session) {
            co_return std::unexpected(problem(csrf_token ? 403 : 401,
                                              csrf_token ? "csrf_mismatch" : "session_required",
                                              csrf_token ? "写入校验失败" : "需要登录",
                                              csrf_token ? "请刷新页面后再保存" : "当前会话不存在或已过期"));
        }
        co_return AuthContext{
            .userId = session->userId,
            .userPublicId = session->userPublicId,
            .username = session->username,
        };
    } catch (const std::exception &exception) {
        co_return repository_failure<AuthContext>(exception);
    }
}

drogon::Task<ServiceResult<SessionView>>
JournalService::current_session(std::string_view cookie_token) const {
    if (cookie_token.empty()) {
        co_return std::unexpected(problem(401, "session_required", "需要登录",
                                          "当前会话不存在或已过期"));
    }
    const auto csrf_token = random_token();
    try {
        const auto session = co_await repository_->rotate_session_csrf(
            hash_token(cookie_token), hash_token(csrf_token));
        if (!session) {
            co_return std::unexpected(problem(401, "session_required", "需要登录",
                                              "当前会话不存在或已过期"));
        }
        co_return SessionView{
            .user = UserView{.id = session->userPublicId, .username = session->username},
            .csrfToken = csrf_token,
            .expiresAt = session->expiresAt,
        };
    } catch (const std::exception &exception) {
        co_return repository_failure<SessionView>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::logout(std::string_view cookie_token) const {
    try {
        if (!cookie_token.empty()) co_await repository_->delete_session(hash_token(cookie_token));
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return repository_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<LedgerView>>> JournalService::ledgers() const {
    try {
        co_return co_await repository_->list_ledgers();
    } catch (const std::exception &exception) {
        co_return repository_failure<std::vector<LedgerView>>(exception);
    }
}

drogon::Task<ServiceResult<LedgerView>>
JournalService::create_ledger(CreateLedgerRequest input) const {
    input.name = trimmed(std::move(input.name));
    if (input.name.empty() || input.name.size() > 120) {
        co_return std::unexpected(problem(422, "validation_error", "账本名称需要调整",
                                          "账本名称需要包含 1 到 120 个字符"));
    }
    try {
        co_return co_await repository_->create_ledger(input.name);
    } catch (const std::exception &exception) {
        co_return repository_failure<LedgerView>(exception);
    }
}

drogon::Task<ServiceResult<LedgerSummaryView>>
JournalService::summary(std::string_view ledger_id,
                        std::optional<std::string> from,
                        std::optional<std::string> to) const {
    // from/to 是含端点的 ISO 日期；不校验就直接进 SQL 的话，"2026-13-45" 之类
    // 会在 PostgreSQL 里炸成 500。
    auto invalid_date = [](std::string_view field) {
        auto issue = problem(422, "validation_error", "统计区间需要调整",
                             "统计区间需要使用 YYYY-MM-DD 格式的日期");
        issue.fields.emplace(std::string(field), "请填写 YYYY-MM-DD 格式的日期");
        return issue;
    };
    if (from && !is_iso_date(*from)) co_return std::unexpected(invalid_date("from"));
    if (to && !is_iso_date(*to)) co_return std::unexpected(invalid_date("to"));
    if (from && to && *from > *to) {
        auto issue = problem(422, "validation_error", "统计区间需要调整",
                             "统计区间的开始日期不能晚于结束日期");
        issue.fields.emplace("from", "开始日期不能晚于结束日期");
        co_return std::unexpected(std::move(issue));
    }

    try {
        co_return co_await repository_->ledger_summary(ledger_id, std::move(from), std::move(to));
    } catch (const std::exception &exception) {
        co_return repository_failure<LedgerSummaryView>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<AccountView>>>
JournalService::accounts(std::string_view ledger_id) const {
    try {
        co_return co_await repository_->list_accounts(ledger_id);
    } catch (const std::exception &exception) {
        co_return repository_failure<std::vector<AccountView>>(exception);
    }
}

drogon::Task<ServiceResult<AccountView>>
JournalService::create_account(std::string_view ledger_id, AccountInput input) const {
    input.name = trimmed(std::move(input.name));
    const bool name_invalid = input.name.empty() || input.name.size() > 120;

    // openingBalance 与流水金额一样是 Money：小数位超限（无论是超过 Money 的 18 位存储
    // 精度，还是超过账户自身的两位小数）与“根本不是十进制数”要分开判定。两者都是 422
    // validation_error，但必须各自带上 fields.openingBalance——只给 detail 而 fields 为空
    // 的话，调用方无从知道是哪个字段出了问题。
    const auto money = domain::Money::parse(input.openingBalance);
    const bool balance_over_precision =
        money ? !money->fits_scale(2)
              : money.error().code == domain::MoneyErrorCode::invalid_scale &&
                    exceeds_storage_scale(input.openingBalance);
    const bool balance_unparsable = !money && !balance_over_precision;

    if (name_invalid || balance_over_precision || balance_unparsable) {
        auto issue = problem(422, "validation_error", "账户内容需要调整",
                             balance_unparsable      ? money.error().message
                             : balance_over_precision ? "默认账户金额最多保留两位小数"
                                                      : "账户名称需要包含 1 到 120 个字符");
        if (name_invalid) issue.fields.emplace("name", "账户名称需要包含 1 到 120 个字符");
        if (balance_over_precision) {
            issue.fields.emplace("openingBalance", "请减少金额的小数位数，最多两位");
        }
        if (balance_unparsable) issue.fields.emplace("openingBalance", "请填写有效的金额");
        co_return std::unexpected(std::move(issue));
    }
    input.openingBalance = money->to_string();
    try {
        co_return co_await repository_->create_account(ledger_id, input);
    } catch (const std::exception &exception) {
        co_return repository_failure<AccountView>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<CategoryView>>>
JournalService::categories(std::string_view ledger_id) const {
    try {
        co_return co_await repository_->list_categories(ledger_id);
    } catch (const std::exception &exception) {
        co_return repository_failure<std::vector<CategoryView>>(exception);
    }
}

drogon::Task<ServiceResult<CategoryView>>
JournalService::create_category(std::string_view ledger_id, CategoryInput input) const {
    input.name = trimmed(std::move(input.name));
    if (input.name.empty() || input.name.size() > 120 ||
        (input.direction != "income" && input.direction != "expense")) {
        co_return std::unexpected(problem(422, "validation_error", "分类内容需要调整",
                                          "分类需要有效名称和收入或支出方向"));
    }
    try {
        co_return co_await repository_->create_category(ledger_id, input);
    } catch (const std::exception &exception) {
        co_return repository_failure<CategoryView>(exception);
    }
}

drogon::Task<ServiceResult<std::vector<ColumnView>>>
JournalService::columns(std::string_view ledger_id, bool recycled) const {
    try {
        co_return co_await repository_->list_columns(ledger_id, recycled);
    } catch (const std::exception &exception) {
        co_return repository_failure<std::vector<ColumnView>>(exception);
    }
}

drogon::Task<ServiceResult<ColumnView>>
JournalService::create_column(std::string_view ledger_id, ColumnInput input) const {
    input.name = trimmed(std::move(input.name));
    constexpr std::string_view valid_types[] = {
        "text", "number", "date", "boolean", "option", "relation", "formula"};
    const bool valid_type = std::ranges::find(valid_types, input.type) != std::end(valid_types);
    if (input.name.empty() || input.name.size() > 80 || !valid_type ||
        (input.type == "number" &&
         (!input.decimalPlaces || *input.decimalPlaces < 0 || *input.decimalPlaces > 18)) ||
        (input.type == "formula" && (!input.formulaSource || input.formulaSource->empty()))) {
        co_return std::unexpected(problem(422, "validation_error", "列定义需要调整",
                                          "请检查列名、数据类型及类型参数"));
    }
    try {
        co_return co_await repository_->create_column(ledger_id, input);
    } catch (const std::exception &exception) {
        co_return repository_failure<ColumnView>(exception);
    }
}

drogon::Task<ServiceResult<ColumnView>>
JournalService::update_column(std::string_view column_id, ColumnPatch patch) const {
    if (patch.name) *patch.name = trimmed(std::move(*patch.name));
    if ((patch.name && (patch.name->empty() || patch.name->size() > 80)) ||
        (patch.position && *patch.position < 0) ||
        (patch.width && (*patch.width < 72 || *patch.width > 720))) {
        co_return std::unexpected(problem(422, "validation_error", "列设置需要调整",
                                          "列名、顺序或宽度超出允许范围"));
    }
    try {
        co_return co_await repository_->update_column(column_id, patch);
    } catch (const std::exception &exception) {
        co_return repository_failure<ColumnView>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::recycle_column(std::string_view column_id) const {
    try {
        co_await repository_->recycle_column(column_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return repository_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::restore_column(std::string_view column_id) const {
    try {
        co_await repository_->restore_column(column_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return repository_failure<std::monostate>(exception);
    }
}

std::string JournalService::encode_cursor(const CursorData &cursor) {
    std::string json;
    const auto write_error = glz::write_json(cursor, json);
    if (write_error) throw std::runtime_error("cursor serialization failed");
    std::string encoded(sodium_base64_ENCODED_LEN(json.size(),
                                                  sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
    sodium_bin2base64(encoded.data(), encoded.size(),
                      reinterpret_cast<const unsigned char *>(json.data()), json.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    encoded.resize(std::char_traits<char>::length(encoded.c_str()));
    return encoded;
}

std::expected<CursorData, Problem> JournalService::decode_cursor(std::string_view cursor) {
    std::string decoded(cursor.size(), '\0');
    std::size_t decoded_size = 0;
    if (sodium_base642bin(reinterpret_cast<unsigned char *>(decoded.data()), decoded.size(),
                          cursor.data(), cursor.size(), nullptr, &decoded_size, nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        return std::unexpected(problem(422, "invalid_cursor", "分页位置无效",
                                       "请从第一页重新载入流水"));
    }
    decoded.resize(decoded_size);
    CursorData result;
    if (const auto read_error = glz::read_json(result, decoded); read_error) {
        return std::unexpected(problem(422, "invalid_cursor", "分页位置无效",
                                       "请从第一页重新载入流水"));
    }
    return result;
}

drogon::Task<ServiceResult<RowPage>>
JournalService::rows(std::string_view ledger_id,
                     std::uint16_t limit,
                     std::string_view sort,
                     bool recycled,
                     std::optional<std::string_view> cursor) const {
    RowQuery query;
    query.limit = std::clamp<std::uint16_t>(limit, 1, 250);
    query.recycled = recycled;
    const auto separator = sort.find(':');
    query.sortKey = std::string(sort.substr(0, separator));
    const auto direction = separator == std::string_view::npos ? "desc" : sort.substr(separator + 1);
    query.ascending = direction == "asc";
    if ((query.sortKey != "date" && query.sortKey != "amount") ||
        (direction != "asc" && direction != "desc")) {
        co_return std::unexpected(problem(422, "invalid_sort", "排序条件无效",
                                          "目前支持按日期或金额排序"));
    }
    if (cursor) {
        auto decoded = decode_cursor(*cursor);
        if (!decoded || decoded->sortKey != query.sortKey || decoded->ascending != query.ascending) {
            co_return std::unexpected(decoded ? problem(422, "invalid_cursor", "分页位置无效",
                                                        "分页位置与当前排序不一致")
                                              : decoded.error());
        }
        query.cursorValue = decoded->value;
        query.cursorId = decoded->id;
    }

    try {
        auto page = co_await repository_->list_rows(ledger_id, query);
        if (page.nextCursor) {
            const auto separator_at = page.nextCursor->rfind('|');
            if (separator_at == std::string::npos) throw std::runtime_error("invalid repository cursor");
            page.nextCursor = encode_cursor(CursorData{
                .sortKey = query.sortKey,
                .ascending = query.ascending,
                .value = page.nextCursor->substr(0, separator_at),
                .id = page.nextCursor->substr(separator_at + 1),
            });
        }
        co_return page;
    } catch (const std::exception &exception) {
        co_return repository_failure<RowPage>(exception);
    }
}

std::expected<RowInput, Problem> JournalService::validate_row(RowInput input) {
    if (!is_iso_date(input.date) || input.description.size() > 4000) {
        auto issue = problem(422, "validation_error", "流水内容需要调整",
                             "日期需要是 YYYY-MM-DD 格式的有效日期，说明最多 4000 个字符");
        if (!is_iso_date(input.date)) issue.fields.emplace("date", "请填写 YYYY-MM-DD 格式的有效日期");
        if (input.description.size() > 4000) issue.fields.emplace("description", "说明最多 4000 个字符");
        return std::unexpected(std::move(issue));
    }
    const auto amount = domain::Money::parse(input.amount);
    if (!amount) {
        // 小数位超过存储精度（18 位）的取值本身是可解析的十进制数，只是放不进列的标度：
        // 这与“根本不是十进制数”不是一回事。前者与仓储侧的资产小数位越界是同一类问题，
        // 走同一个 Problem（validation_error + fields.amount），保证两处报法完全一致；
        // invalid_amount 只留给解析不出来的取值。
        if (amount.error().code == domain::MoneyErrorCode::invalid_scale &&
            exceeds_storage_scale(input.amount)) {
            return std::unexpected(amount_scale_problem());
        }
        return std::unexpected(problem(422, "invalid_amount", "金额格式不正确",
                                       amount.error().message));
    }
    input.amount = amount->to_string();

    domain::JournalDraft draft{.amount = *amount};
    domain::PostingAccounts posting_accounts{
        .primary = domain::AccountId{1},
        .counterparty = domain::AccountId{2},
        .transfer_destination = std::nullopt,
    };
    if (input.kind == "note") {
        draft.kind = domain::TransactionKind::note;
        input.accountId.reset();
        input.categoryId.reset();
        input.transferAccountId.reset();
    } else if (input.kind == "transfer") {
        draft.kind = domain::TransactionKind::transfer;
        if (!input.accountId || !input.transferAccountId || *input.accountId == *input.transferAccountId) {
            return std::unexpected(problem(422, "invalid_transfer", "转账内容需要调整",
                                           "请选择两个不同的账户"));
        }
        posting_accounts.transfer_destination = domain::AccountId{3};
        input.categoryId.reset();
    } else if (input.kind == "entry") {
        draft.kind = domain::TransactionKind::entry;
        input.transferAccountId.reset();
    } else {
        return std::unexpected(problem(422, "invalid_kind", "流水类型无效",
                                       "请选择收支、转账或备注"));
    }
    const auto postings = domain::build_postings(draft, posting_accounts);
    if (!postings) {
        return std::unexpected(problem(422, "invalid_postings", "流水内容需要调整",
                                       postings.error().message));
    }

    // 主体里携带的标识同样会进 ::uuid 转换：PATCH /rows/{id} 拿不到账本 id，无法做
    // 归属预检，至少要保证格式非法的取值不会以 500 结束。
    const std::array<std::pair<const std::optional<std::string> *, std::string_view>, 3> references{{
        {&input.accountId, "accountId"},
        {&input.categoryId, "categoryId"},
        {&input.transferAccountId, "transferAccountId"},
    }};
    for (const auto &[value, field] : references) {
        if (value->has_value() && !is_uuid(**value)) {
            auto issue = problem(422, "validation_error", "流水内容需要调整",
                                 "所选账户或分类的标识格式不正确");
            issue.fields.emplace(std::string(field), "标识需要是 UUID");
            return std::unexpected(std::move(issue));
        }
    }
    return input;
}

// 分类存在但方向与金额符号相反。仓储用 CategoryDirectionMismatch 把它与“分类不存在”
// 分开抛出，这里翻成带 fields.categoryId 的 422。文案完全由 required_direction 决定，
// 异常正文不进响应，因此 create_row 与 update_row 两条路径必然给出同一个 Problem。
Problem JournalService::category_direction_problem(std::string_view required_direction) {
    const bool income = required_direction == "income";
    auto issue = problem(422, "validation_error", "流水内容需要调整",
                         income ? "金额为正数记为收入，请选择收入方向的分类"
                                : "金额为负数记为支出，请选择支出方向的分类");
    issue.fields.emplace("categoryId", income ? "该分类是支出分类，与正数金额不匹配"
                                              : "该分类是收入分类，与负数金额不匹配");
    return issue;
}

// 转账两端账户不在同一资产上。仓储用 TransferAssetMismatch 把它与其他写入失败分开抛出，
// 这里翻成带 fields.transferAccountId 的 422：与 category_direction_problem 同样，文案只由
// 异常携带的两个资产符号决定，异常正文不进响应，create_row 与 update_row 给出同一个 Problem。
Problem JournalService::transfer_asset_problem(std::string_view account_symbol,
                                               std::string_view transfer_symbol) {
    auto issue = problem(422, "validation_error", "转账内容需要调整",
                         "转账的两个账户需要使用同一种资产");
    issue.fields.emplace("transferAccountId",
                         "转出账户使用 " + std::string(account_symbol) + "，转入账户使用 " +
                             std::string(transfer_symbol) + "，请选择同一资产的账户");
    return issue;
}

// 金额小数位超限的唯一 Problem。两个入口共用：请求校验阶段发现超过 Money 的 18 位
// 存储精度，以及仓储发现超过该资产的小数位。同一类问题必须给出同一个 code 与 fields，
// 调用方不该因为触发点在服务层还是仓储层而看到两种报法。
Problem JournalService::amount_scale_problem() {
    auto issue = problem(422, "validation_error", "金额精度不正确",
                         "金额的小数位数超出该资产允许的精度");
    issue.fields.emplace("amount", "请减少金额的小数位数");
    return issue;
}

// 仓储对超出资产小数位的金额抛 std::invalid_argument；异常正文可能含内部细节，
// 只写日志，客户端拿到固定文案与 fields.amount。
Problem JournalService::amount_scale_problem(const std::exception &exception) {
    LOG_WARN << "row_amount_rejected detail=" << exception.what();
    return amount_scale_problem();
}

drogon::Task<ServiceResult<JournalRowView>>
JournalService::create_row(std::string_view ledger_id,
                           std::int64_t user_id,
                           RowInput input) const {
    auto validated = validate_row(std::move(input));
    if (!validated) co_return std::unexpected(validated.error());
    try {
        co_return co_await repository_->create_row(ledger_id, user_id, *validated);
    } catch (const infrastructure::CategoryDirectionMismatch &exception) {
        co_return std::unexpected(category_direction_problem(exception.required_direction()));
    } catch (const infrastructure::TransferAssetMismatch &exception) {
        co_return std::unexpected(transfer_asset_problem(exception.account_symbol(),
                                                         exception.transfer_symbol()));
    } catch (const std::invalid_argument &exception) {
        co_return std::unexpected(amount_scale_problem(exception));
    } catch (const std::exception &exception) {
        co_return repository_failure<JournalRowView>(exception);
    }
}

drogon::Task<ServiceResult<JournalRowView>>
JournalService::update_row(std::string_view row_id,
                           std::int64_t user_id,
                           RowInput input) const {
    auto validated = validate_row(std::move(input));
    if (!validated) co_return std::unexpected(validated.error());
    try {
        co_return co_await repository_->update_row(row_id, user_id, *validated);
    } catch (const infrastructure::CategoryDirectionMismatch &exception) {
        co_return std::unexpected(category_direction_problem(exception.required_direction()));
    } catch (const infrastructure::TransferAssetMismatch &exception) {
        co_return std::unexpected(transfer_asset_problem(exception.account_symbol(),
                                                         exception.transfer_symbol()));
    } catch (const std::invalid_argument &exception) {
        co_return std::unexpected(amount_scale_problem(exception));
    } catch (const std::exception &exception) {
        co_return repository_failure<JournalRowView>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::recycle_row(std::string_view row_id) const {
    try {
        co_await repository_->recycle_row(row_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return repository_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::restore_row(std::string_view row_id) const {
    try {
        co_await repository_->restore_row(row_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return repository_failure<std::monostate>(exception);
    }
}

std::vector<LuaFunctionView> JournalService::functions() const {
    std::vector<LuaFunctionView> result;
    for (const auto &function : functions_->list()) {
        result.push_back(lua_function_view(function));
    }
    return result;
}

ServiceResult<LuaFunctionView>
JournalService::create_function(LuaFunctionInput input) const {
    if (trimmed(input.source).empty()) {
        return std::unexpected(problem(
            422, "empty_lua_source", "函数脚本为空", "请填写 Lua 函数定义"));
    }
    auto result = functions_->create(std::move(input.source));
    if (!result) return lua_failure<LuaFunctionView>(result.error());
    return lua_function_view(*result);
}

ServiceResult<LuaFunctionView>
JournalService::update_function(std::string_view name, LuaFunctionInput input) const {
    if (trimmed(input.source).empty()) {
        return std::unexpected(problem(
            422, "empty_lua_source", "函数脚本为空", "请填写 Lua 函数定义"));
    }
    auto result = functions_->update(name, std::move(input.source));
    if (!result) return lua_failure<LuaFunctionView>(result.error());
    return lua_function_view(*result);
}

ServiceResult<lua::LuaValue>
JournalService::invoke_function(std::string_view name, const lua::LuaValue::Object &input) const {
    auto result = functions_->invoke(name, input);
    if (!result) {
        return lua_failure<lua::LuaValue>(result.error());
    }
    return *result;
}

drogon::Task<ServiceResult<std::vector<JobView>>> JournalService::jobs() const {
    try {
        co_return co_await repository_->list_jobs();
    } catch (const std::exception &exception) {
        co_return repository_failure<std::vector<JobView>>(exception);
    }
}

drogon::Task<ServiceResult<std::monostate>>
JournalService::cancel_job(std::string_view job_id) const {
    try {
        co_await repository_->request_job_cancel(job_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) {
        co_return repository_failure<std::monostate>(exception);
    }
}

drogon::Task<ServiceResult<ChainSettingsView>> JournalService::chain_settings() const {
    try {
        co_return co_await repository_->chain_settings();
    } catch (const std::exception &exception) {
        co_return repository_failure<ChainSettingsView>(exception);
    }
}

drogon::Task<ServiceResult<ChainSettingsView>>
JournalService::update_chain_settings(ChainSettingsPatch patch) const {
    if (patch.syncIntervalMinutes && (*patch.syncIntervalMinutes < 5 || *patch.syncIntervalMinutes > 1440)) {
        auto issue = problem(422, "validation_error", "链配置需要调整", "同步间隔需要在 5 到 1440 分钟之间");
        issue.fields.emplace("syncIntervalMinutes", "请输入 5 到 1440 之间的分钟数");
        co_return std::unexpected(std::move(issue));
    }

    auto validate_secret = [&](std::optional<std::string> &value,
                               bool clear,
                               std::string_view field,
                               std::string_view label,
                               std::size_t max_size) -> std::optional<Problem> {
        if (!value) return std::nullopt;
        *value = trimmed(std::move(*value));
        if (value->empty() || value->size() > max_size) {
            auto issue = problem(422, "validation_error", "链配置需要调整",
                                 std::string(label) + "需要包含 1 到 " + std::to_string(max_size) + " 个字符");
            issue.fields.emplace(std::string(field), "请输入有效的配置值");
            return issue;
        }
        if (clear) {
            return problem(422, "validation_error", "链配置需要调整",
                           "不能同时设置和清除 " + std::string(label));
        }
        return std::nullopt;
    };
    auto validate_url = [&](std::optional<std::string> &value,
                            bool clear,
                            std::string_view field,
                            std::string_view label) -> std::optional<Problem> {
        if (auto issue = validate_secret(value, clear, field, label, 2048)) return issue;
        if (!value) return std::nullopt;
        if (const auto rejection = egress_rejection(*value)) {
            auto issue = problem(422, "validation_error", "链配置需要调整",
                                 std::string(label) + "未通过出网校验：" + *rejection);
            issue.fields.emplace(std::string(field), *rejection);
            return issue;
        }
        return std::nullopt;
    };

    // 明文 secret 只应存在于本函数内：用 RAII 保证任何退出路径都先擦除缓冲区再释放，
    // 而不是把明文留在被回收的堆内存里。仓储调用期间 patch 保持原样。
    struct SecretScrubber {
        ChainSettingsPatch *target;
        ~SecretScrubber() {
            for (auto *value : {&target->tronGridApiKey, &target->etherscanApiKey,
                                &target->ethereumRpcUrl, &target->polygonRpcUrl,
                                &target->solanaRpcUrl}) {
                if (*value && !(*value)->empty()) sodium_memzero((*value)->data(), (*value)->size());
            }
        }
    } const scrubber{&patch};

    if (auto issue = validate_secret(patch.tronGridApiKey, patch.clearTronGridApiKey.value_or(false),
                                     "tronGridApiKey", "TronGrid API Key", 1024)) co_return std::unexpected(*issue);
    if (auto issue = validate_secret(patch.etherscanApiKey, patch.clearEtherscanApiKey.value_or(false),
                                     "etherscanApiKey", "Etherscan API Key", 1024)) co_return std::unexpected(*issue);
    if (auto issue = validate_url(patch.ethereumRpcUrl, patch.clearEthereumRpcUrl.value_or(false),
                                  "ethereumRpcUrl", "Ethereum RPC URL")) co_return std::unexpected(*issue);
    if (auto issue = validate_url(patch.polygonRpcUrl, patch.clearPolygonRpcUrl.value_or(false),
                                  "polygonRpcUrl", "Polygon RPC URL")) co_return std::unexpected(*issue);
    if (auto issue = validate_url(patch.solanaRpcUrl, patch.clearSolanaRpcUrl.value_or(false),
                                  "solanaRpcUrl", "Solana RPC URL")) co_return std::unexpected(*issue);

    try {
        infrastructure::ChainSettingsSecretPatch secrets;
        auto encrypt_into = [&](const std::optional<std::string> &value)
            -> std::expected<std::optional<infrastructure::EncryptedSecretRecord>, Problem> {
            if (!value) return std::optional<infrastructure::EncryptedSecretRecord>{};
            auto encrypted = encrypt_secret(*value);
            if (!encrypted) return std::unexpected(encrypted.error());
            return std::optional<infrastructure::EncryptedSecretRecord>{std::move(*encrypted)};
        };

        auto tron = encrypt_into(patch.tronGridApiKey);
        if (!tron) co_return std::unexpected(tron.error());
        secrets.tronGridApiKey = std::move(*tron);
        auto etherscan = encrypt_into(patch.etherscanApiKey);
        if (!etherscan) co_return std::unexpected(etherscan.error());
        secrets.etherscanApiKey = std::move(*etherscan);
        auto ethereum_rpc = encrypt_into(patch.ethereumRpcUrl);
        if (!ethereum_rpc) co_return std::unexpected(ethereum_rpc.error());
        secrets.ethereumRpcUrl = std::move(*ethereum_rpc);
        auto polygon_rpc = encrypt_into(patch.polygonRpcUrl);
        if (!polygon_rpc) co_return std::unexpected(polygon_rpc.error());
        secrets.polygonRpcUrl = std::move(*polygon_rpc);
        auto solana_rpc = encrypt_into(patch.solanaRpcUrl);
        if (!solana_rpc) co_return std::unexpected(solana_rpc.error());
        secrets.solanaRpcUrl = std::move(*solana_rpc);

        co_return co_await repository_->update_chain_settings(patch, secrets);
    } catch (const std::exception &exception) {
        co_return repository_failure<ChainSettingsView>(exception);
    }
}


drogon::Task<ServiceResult<std::vector<WalletView>>>
JournalService::wallets(std::string_view ledger_id) const {
    try { co_return co_await repository_->list_wallets(ledger_id); }
    catch (const std::exception &exception) { co_return repository_failure<std::vector<WalletView>>(exception); }
}

drogon::Task<ServiceResult<WalletView>>
JournalService::create_wallet(std::string_view ledger_id, WalletInput input) const {
    input.chain = trimmed(std::move(input.chain));
    if (input.chain.empty()) input.chain = "tron-mainnet";
    input.name = trimmed(std::move(input.name));
    if (!domain::chain::is_supported_chain(input.chain)) {
        auto issue = problem(422, "validation_error", "钱包内容需要调整", "暂不支持该链");
        issue.fields.emplace("chain", "请选择 TRON、Ethereum、Polygon 或 Solana Mainnet");
        co_return std::unexpected(std::move(issue));
    }
    if (input.name.empty() || input.name.size() > 120) {
        auto issue = problem(422, "validation_error", "钱包内容需要调整", "钱包名称需要包含 1 到 120 个字符");
        issue.fields.emplace("name", "请输入钱包名称");
        co_return std::unexpected(std::move(issue));
    }
    auto address = domain::chain::normalize_address(input.chain, input.address);
    if (!address) {
        auto issue = problem(422, "validation_error", "钱包内容需要调整", address.error().message);
        issue.fields.emplace("address", "请输入有效的 " + domain::chain::display_prefix(input.chain) + " 地址");
        co_return std::unexpected(std::move(issue));
    }
    try {
        co_return co_await repository_->create_wallet(ledger_id, input, *address);
    } catch (const std::exception &exception) { co_return repository_failure<WalletView>(exception); }
}

drogon::Task<ServiceResult<WalletView>>
JournalService::wallet(std::string_view wallet_id) const {
    try {
        auto record = co_await repository_->wallet(wallet_id);
        if (!record) co_return std::unexpected(not_found_problem("wallet"));
        co_return std::move(*record);
    } catch (const std::exception &exception) {
        co_return repository_failure<WalletView>(exception);
    }
}

drogon::Task<ServiceResult<WalletView>>
JournalService::update_wallet(std::string_view wallet_id, WalletPatch patch) const {
    if (patch.name) {
        *patch.name = trimmed(std::move(*patch.name));
        if (patch.name->empty() || patch.name->size() > 120) {
            auto issue = problem(422, "validation_error", "钱包内容需要调整", "钱包名称需要包含 1 到 120 个字符");
            issue.fields.emplace("name", "请输入钱包名称");
            co_return std::unexpected(std::move(issue));
        }
    }
    try {
        // 不再预读一次钱包：update_wallet 的 UPDATE 命中 0 行时自己抛 EntityNotFound("wallet")，
        // repository_failure 会把它翻成 404 wallet_not_found。
        co_return co_await repository_->update_wallet(wallet_id, patch);
    } catch (const std::exception &exception) { co_return repository_failure<WalletView>(exception); }
}

drogon::Task<ServiceResult<std::monostate>> JournalService::delete_wallet(std::string_view wallet_id) const {
    try {
        co_await repository_->delete_wallet(wallet_id);
        co_return std::monostate{};
    } catch (const std::exception &exception) { co_return repository_failure<std::monostate>(exception); }
}

drogon::Task<ServiceResult<SyncResultView>>
JournalService::sync_wallet(std::string_view wallet_id, std::int64_t user_id) const {
    std::optional<std::string> failure_detail;
    try {
        const auto target = co_await repository_->wallet(wallet_id);
        if (!target) co_return std::unexpected(not_found_problem("wallet"));
        infrastructure::SyncWriteStats stats;
        const bool mock_all = [] {
            if (const char *mock = std::getenv("JOURNALSEED_MOCK_CHAIN_SYNC")) return std::string_view(mock) == "1";
            if (const char *mock = std::getenv("JOURNALSEED_MOCK_MULTICHAIN_SYNC")) return std::string_view(mock) == "1";
            return false;
        }();
        const bool mock_tron = [] {
            if (const char *mock = std::getenv("JOURNALSEED_MOCK_TRON_SYNC")) return std::string_view(mock) == "1";
            return false;
        }();
        if (mock_all || (mock_tron && target->chain == "tron-mainnet")) {
            stats = co_await repository_->record_wallet_sync(
                wallet_id, user_id, mock_chain_transactions(target->chain, target->address));
        } else {
            const auto settings = co_await repository_->chain_settings();
            const auto secret = co_await repository_->chain_settings_secret();
            std::vector<infrastructure::ChainTransactionInput> transactions;
            if (target->chain == "tron-mainnet") {
                auto api_key = decrypt_secret(secret.tronGridApiKey);
                if (!api_key) co_return std::unexpected(api_key.error());
                if (const auto rejection = egress_rejection(settings.tronGridEndpoint)) {
                    co_return std::unexpected(rpc_endpoint_problem("TronGrid", *rejection));
                }
                infrastructure::TronAdapter adapter(infrastructure::TronAdapterOptions{
                    .baseUrl = settings.tronGridEndpoint,
                    .apiKey = std::move(*api_key),
                    .limit = 100,
                });
                transactions = co_await adapter.fetch_wallet_transactions(target->address);
            } else if (target->chain == "ethereum-mainnet" || target->chain == "polygon-mainnet") {
                auto api_key = decrypt_secret(secret.etherscanApiKey);
                if (!api_key) co_return std::unexpected(api_key.error());
                const bool polygon = target->chain == "polygon-mainnet";
                const char *base = std::getenv("JOURNALSEED_ETHERSCAN_BASE_URL");
                const std::string base_url = base && *base
                    ? std::string(base)
                    : std::string("https://api.etherscan.io");
                if (const auto rejection = egress_rejection(base_url)) {
                    co_return std::unexpected(rpc_endpoint_problem("Etherscan", *rejection));
                }
                infrastructure::EvmAdapter adapter(infrastructure::EvmAdapterOptions{
                    .baseUrl = base_url,
                    .apiKey = std::move(*api_key),
                    .chainCode = target->chain,
                    .chainId = polygon ? 137 : 1,
                    .nativeSymbol = polygon ? "POL" : "ETH",
                    .nativeName = polygon ? "Polygon Ecosystem Token" : "Ether",
                    .limit = 100,
                });
                transactions = co_await adapter.fetch_wallet_transactions(target->address);
            } else if (target->chain == "solana-mainnet") {
                auto configured_rpc = decrypt_secret(secret.solanaRpcUrl);
                if (!configured_rpc) co_return std::unexpected(configured_rpc.error());
                const char *solana_env = std::getenv("JOURNALSEED_SOLANA_RPC_URL");
                const std::string default_rpc = solana_env && *solana_env
                    ? std::string(solana_env)
                    : std::string("https://api.mainnet-beta.solana.com");
                const std::string rpc_url = configured_rpc->value_or(default_rpc);
                // 出网校验在每次同步前重跑：配置时通过不代表现在仍然安全（DNS 可能已改指内网）。
                if (const auto rejection = egress_rejection(rpc_url)) {
                    co_return std::unexpected(rpc_endpoint_problem("Solana RPC", *rejection));
                }
                infrastructure::SolanaAdapter adapter(infrastructure::SolanaAdapterOptions{
                    .rpcUrl = rpc_url,
                    .limit = 50,
                });
                transactions = co_await adapter.fetch_wallet_transactions(target->address);
            } else {
                co_return std::unexpected(problem(422, "unsupported_chain", "链暂不支持", "该钱包链暂不支持同步"));
            }
            stats = co_await repository_->record_wallet_sync(wallet_id, user_id, transactions);
        }
        const auto job = co_await repository_->create_wallet_sync_job(
            wallet_id, "completed", stats.transactionsSeen, stats.transactionsSeen, std::nullopt);
        co_return SyncResultView{.job = job, .transactionsSeen = stats.transactionsSeen,
                                 .movementsCreated = stats.movementsCreated, .rowsCreated = stats.rowsCreated};
    } catch (const std::exception &exception) {
        failure_detail = exception.what();
    }
    if (failure_detail) {
        // 失败原因可能整段来自第三方 RPC：完整内容只进服务端日志，
        // background_jobs.error 与 API 响应都只保留固定文案加事件编号。
        const auto reference = diagnostic_reference();
        LOG_ERROR << "wallet_sync_failed ref=" << reference << " wallet=" << std::string(wallet_id)
                  << " detail=" << *failure_detail;
        const std::string safe_detail = "钱包同步失败，请在服务端日志中查看事件编号 " + reference;
        try {
            co_await repository_->create_wallet_sync_job(wallet_id, "failed", 0, 0, safe_detail);
        } catch (...) {
            // Preserve the original sync failure for the API response.
        }
        co_return std::unexpected(problem(500, "wallet_sync_failed", "钱包同步未完成", safe_detail));
    }
    co_return std::unexpected(problem(500, "wallet_sync_failed", "钱包同步未完成", "同步未返回结果"));
}

drogon::Task<ServiceResult<std::vector<AddressLabelView>>>
JournalService::address_labels(std::string_view ledger_id) const {
    try { co_return co_await repository_->list_address_labels(ledger_id); }
    catch (const std::exception &exception) { co_return repository_failure<std::vector<AddressLabelView>>(exception); }
}

drogon::Task<ServiceResult<AddressLabelView>>
JournalService::create_address_label(std::string_view ledger_id, AddressLabelInput input) const {
    input.chain = trimmed(std::move(input.chain));
    if (input.chain.empty()) input.chain = "tron-mainnet";
    input.displayName = trimmed(std::move(input.displayName));
    input.kind = trimmed(std::move(input.kind));
    input.note = trimmed(std::move(input.note));
    if (!domain::chain::is_supported_chain(input.chain)) {
        auto issue = problem(422, "validation_error", "地址标签需要调整", "暂不支持该链");
        issue.fields.emplace("chain", "请选择 TRON、Ethereum、Polygon 或 Solana Mainnet");
        co_return std::unexpected(std::move(issue));
    }
    if (input.displayName.empty() || input.displayName.size() > 120 || input.note.size() > 2000 || !is_address_label_kind(input.kind)) {
        auto issue = problem(422, "validation_error", "地址标签需要调整", "请检查显示名称、标签类型和备注长度");
        if (input.displayName.empty() || input.displayName.size() > 120) issue.fields.emplace("displayName", "显示名称需要包含 1 到 120 个字符");
        if (!is_address_label_kind(input.kind)) issue.fields.emplace("kind", "标签类型必须为 customer、self、exchange、merchant、contract 或 other");
        if (input.note.size() > 2000) issue.fields.emplace("note", "备注最多 2000 个字符");
        co_return std::unexpected(std::move(issue));
    }
    auto address = domain::chain::normalize_address(input.chain, input.address);
    if (!address) {
        auto issue = problem(422, "validation_error", "地址标签需要调整", address.error().message);
        issue.fields.emplace("address", "请输入有效的 " + domain::chain::display_prefix(input.chain) + " 地址");
        co_return std::unexpected(std::move(issue));
    }
    try {
        co_return co_await repository_->create_address_label(ledger_id, input, *address);
    } catch (const std::exception &exception) { co_return repository_failure<AddressLabelView>(exception); }
}

drogon::Task<ServiceResult<AddressLabelView>>
JournalService::update_address_label(std::string_view label_id, AddressLabelPatch patch) const {
    if (patch.displayName) *patch.displayName = trimmed(std::move(*patch.displayName));
    if (patch.kind) *patch.kind = trimmed(std::move(*patch.kind));
    if (patch.note) *patch.note = trimmed(std::move(*patch.note));
    if ((patch.displayName && (patch.displayName->empty() || patch.displayName->size() > 120)) ||
        (patch.kind && !is_address_label_kind(*patch.kind)) || (patch.note && patch.note->size() > 2000)) {
        auto issue = problem(422, "validation_error", "地址标签需要调整", "请检查显示名称、标签类型和备注长度");
        if (patch.displayName && (patch.displayName->empty() || patch.displayName->size() > 120)) issue.fields.emplace("displayName", "显示名称需要包含 1 到 120 个字符");
        if (patch.kind && !is_address_label_kind(*patch.kind)) issue.fields.emplace("kind", "标签类型无效");
        if (patch.note && patch.note->size() > 2000) issue.fields.emplace("note", "备注最多 2000 个字符");
        co_return std::unexpected(std::move(issue));
    }
    try { co_return co_await repository_->update_address_label(label_id, patch); }
    catch (const std::exception &exception) { co_return repository_failure<AddressLabelView>(exception); }
}

drogon::Task<ServiceResult<std::monostate>> JournalService::delete_address_label(std::string_view label_id) const {
    try { co_await repository_->delete_address_label(label_id); co_return std::monostate{}; }
    catch (const std::exception &exception) { co_return repository_failure<std::monostate>(exception); }
}

drogon::Task<ServiceResult<std::vector<ChainTransactionView>>>
JournalService::chain_transactions(std::string_view ledger_id, std::uint16_t limit) const {
    try { co_return co_await repository_->list_chain_transactions(ledger_id, std::clamp<std::uint16_t>(limit, 1, 250)); }
    catch (const std::exception &exception) { co_return repository_failure<std::vector<ChainTransactionView>>(exception); }
}

}  // namespace journalseed::application
