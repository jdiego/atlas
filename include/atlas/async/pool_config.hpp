#pragma once

#include <chrono>
#include <string>

namespace atlas {

enum class ssl_mode {
    disable,
    allow,
    prefer,
    require,
    verify_ca,
    verify_full
};

struct pool_config {
    std::string               url;
    std::size_t               max_size    = 4;
    std::chrono::milliseconds timeout     = std::chrono::seconds{30};
    ssl_mode                  ssl         = ssl_mode::prefer;
    std::size_t               max_retries = 3;
};

// Appends sslmode=<value> to a libpq connection string (keyword=value or URI form).
// Returns url unchanged if sslmode is already present.
[[nodiscard]] std::string apply_ssl_mode(std::string url, ssl_mode mode);

} // namespace atlas
