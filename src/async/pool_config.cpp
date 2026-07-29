#include "atlas/async/pool_config.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
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

[[nodiscard]] constexpr auto ssl_mode_name(ssl_mode mode) noexcept -> std::string_view {
    switch (mode) {
    case ssl_mode::disable:
        return "disable";
    case ssl_mode::allow:
        return "allow";
    case ssl_mode::prefer:
        return "prefer";
    case ssl_mode::require:
        return "require";
    case ssl_mode::verify_ca:
        return "verify-ca";
    case ssl_mode::verify_full:
        return "verify-full";
    }
    return "prefer";
}

[[nodiscard]] auto option_value(PQconninfoOption *options, std::string_view keyword) -> const char * {
    for (auto *option = options; option->keyword != nullptr; ++option) {
        if (keyword == option->keyword) {
            return option->val;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr auto valid_ssl_mode(std::string_view value) noexcept -> bool {
    constexpr std::array allowed{"disable", "allow", "prefer", "require", "verify-ca", "verify-full"};
    return std::ranges::find(allowed, value) != allowed.end();
}

// libpq accepts one numeric port for every host, one port shared by all hosts,
// and empty list elements to request the compiled-in default port.
[[nodiscard]] constexpr auto valid_port_list(std::string_view value) noexcept -> bool {
    while (true) {
        const auto comma = value.find(',');
        const auto port = value.substr(0, comma);
        if (!port.empty() &&
            !std::ranges::all_of(port, [](char character) { return character >= '0' && character <= '9'; })) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        value.remove_prefix(comma + 1);
    }
}

void append_keyword_value(std::string &conninfo, std::string_view keyword, std::string_view value) {
    if (!conninfo.empty()) {
        conninfo.push_back(' ');
    }
    conninfo.append(keyword).append("='");
    for (const char character : value) {
        if (character == '\'' || character == '\\') {
            conninfo.push_back('\\');
        }
        conninfo.push_back(character);
    }
    conninfo.push_back('\'');
}

[[nodiscard]] auto parse_conninfo(const std::string &url)
    -> std::expected<std::unique_ptr<PQconninfoOption, conninfo_deleter>, pg::error> {
    char *raw_error = nullptr;
    std::unique_ptr<char, pq_memory_deleter> parse_error;
    std::unique_ptr<PQconninfoOption, conninfo_deleter> options{PQconninfoParse(url.c_str(), &raw_error)};
    parse_error.reset(raw_error);

    if (!options) {
        const std::string message = parse_error ? parse_error.get() : "could not parse connection string";
        return std::unexpected(pg::error{message, pg::errc::invalid_argument});
    }
    return options;
}

[[nodiscard]] auto validate_selected_options(PQconninfoOption *options) -> std::expected<void, pg::error> {
    if (const char *sslmode = option_value(options, "sslmode");
        sslmode != nullptr && !valid_ssl_mode(std::string_view{sslmode})) {
        return std::unexpected(pg::error{"invalid sslmode value", pg::errc::invalid_argument});
    }
    if (const char *port = option_value(options, "port"); port != nullptr && !valid_port_list(std::string_view{port})) {
        return std::unexpected(
            pg::error{"port must be a comma-separated list of numeric values", pg::errc::invalid_argument});
    }
    return {};
}

} // namespace

std::expected<std::string, pg::error> apply_ssl_mode(std::string url, ssl_mode mode) {
    if (url.find('\0') != std::string::npos) {
        return std::unexpected(
            pg::error{"connection string contains an embedded NUL byte", pg::errc::invalid_argument});
    }

    auto parsed = parse_conninfo(url);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    auto &options = *parsed;
    if (auto validated = validate_selected_options(options.get()); !validated) {
        return std::unexpected(std::move(validated.error()));
    }

    const char *explicit_sslmode = option_value(options.get(), "sslmode");
    if (explicit_sslmode != nullptr) {
        return url;
    }

    const auto mode_name = ssl_mode_name(mode);
    const bool is_uri = url.starts_with("postgresql://") || url.starts_with("postgres://");
    if (is_uri) {
        std::string canonical;
        for (auto *option = options.get(); option->keyword != nullptr; ++option) {
            if (option->val != nullptr) {
                append_keyword_value(canonical, option->keyword, option->val);
            }
        }
        append_keyword_value(canonical, "sslmode", mode_name);
        url = std::move(canonical);
    } else {
        url += " sslmode=";
        url += mode_name;
    }

    auto verified = parse_conninfo(url);
    if (!verified) {
        return std::unexpected(std::move(verified.error()));
    }
    if (auto validated = validate_selected_options(verified->get()); !validated) {
        return std::unexpected(std::move(validated.error()));
    }
    const char *effective_sslmode = option_value(verified->get(), "sslmode");
    if (effective_sslmode == nullptr || std::string_view{effective_sslmode} != mode_name) {
        return std::unexpected(pg::error{"configured sslmode was not applied", pg::errc::invalid_argument});
    }

    return url;
}

} // namespace atlas
