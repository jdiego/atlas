#include "atlas/async/pool_config.hpp"

namespace atlas {

std::string apply_ssl_mode(std::string url, ssl_mode mode)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Appends a sslmode=<value> key-value pair to a libpq connection string,
     *   choosing the correct string representation for the ssl_mode enum value.
     *
     * Step 1 — Map ssl_mode enum to its libpq string name using a switch:
     *           disable→"disable", allow→"allow", prefer→"prefer",
     *           require→"require", verify_ca→"verify-ca",
     *           verify_full→"verify-full".
     * Step 2 — If url.find("sslmode=") != std::string::npos, return url unchanged
     *           to avoid duplicate parameters.
     * Step 3 — Detect format:
     *             is_uri = url starts with "postgresql://" or "postgres://".
     * Step 4 — For keyword=value format: append " sslmode=<mode_str>" (leading space).
     * Step 5 — For URI format: append "?sslmode=<mode_str>" if '?' is absent in url,
     *           otherwise "&sslmode=<mode_str>".
     * Step 6 — Return the modified url.
     *
     * Key types involved:
     *   - ssl_mode: enum class defined in pool_config.hpp
     *   - std::string: owns the connection string being built
     *
     * Preconditions:
     *   - url is a valid libpq connection string (keyword=value or URI form).
     *
     * Postconditions:
     *   - The returned string has exactly one "sslmode=<mode>" token.
     *   - If url already contained "sslmode=", it is returned unmodified.
     *
     * Pitfalls:
     *   - verify-ca and verify-full use hyphens in libpq, not underscores.
     *   - URI form requires "?" before the first query parameter and "&" for
     *     subsequent ones; check with url.find('?').
     *   - Do not append twice; the early-return guard prevents duplicates.
     *
     * Hint:
     *   std::string_view mode_names[] indexed by static_cast<int>(mode).
     */
    const char* mode_str = "prefer";
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

    if (url.find("sslmode=") != std::string::npos) {
        return url;
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
