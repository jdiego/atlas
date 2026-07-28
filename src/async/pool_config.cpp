#include "atlas/async/pool_config.hpp"

#include <libpq-fe.h>

#include <memory>
#include <string_view>

namespace atlas {

namespace {

struct conninfo_deleter {
    void operator()(PQconninfoOption *value) const noexcept {
        if (value != nullptr) {
            PQconninfoFree(value);
        }
    }
};

struct pq_memory_deleter {
    void operator()(char *value) const noexcept {
        if (value != nullptr) {
            PQfreemem(value);
        }
    }
};

} // namespace

std::expected<std::string, pg::error> apply_ssl_mode(std::string url, ssl_mode mode) {
    if (url.find('\0') != std::string::npos) {
        return std::unexpected(
            pg::error{"connection string contains an embedded NUL byte", pg::errc::invalid_argument});
    }

    char *raw_error = nullptr;
    std::unique_ptr<char, pq_memory_deleter> parse_error;
    std::unique_ptr<PQconninfoOption, conninfo_deleter> options{PQconninfoParse(url.c_str(), &raw_error)};
    parse_error.reset(raw_error);

    if (!options) {
        const std::string message = parse_error ? parse_error.get() : "could not parse connection string";
        return std::unexpected(pg::error{message, pg::errc::invalid_argument});
    }

    for (auto *option = options.get(); option->keyword != nullptr; ++option) {
        if (std::string_view{option->keyword} == "sslmode" && option->val != nullptr) {
            return url;
        }
    }

    const char *mode_str = "prefer";
    switch (mode) {
    case ssl_mode::disable:
        mode_str = "disable";
        break;
    case ssl_mode::allow:
        mode_str = "allow";
        break;
    case ssl_mode::prefer:
        mode_str = "prefer";
        break;
    case ssl_mode::require:
        mode_str = "require";
        break;
    case ssl_mode::verify_ca:
        mode_str = "verify-ca";
        break;
    case ssl_mode::verify_full:
        mode_str = "verify-full";
        break;
    }

    const bool is_uri = url.starts_with("postgresql://") || url.starts_with("postgres://");
    if (is_uri) {
        url += (url.find('?') == std::string::npos) ? "?sslmode=" : "&sslmode=";
    } else {
        url += " sslmode=";
    }

    url += mode_str;
    return url;
}

} // namespace atlas
