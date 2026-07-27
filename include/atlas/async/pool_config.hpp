#pragma once

#include <chrono>
#include <string>

namespace atlas {

enum class ssl_mode { disable, allow, prefer, require, verify_ca, verify_full };

// How long the cleanup that follows a timed-out query — cancelling it
// server-side, then reading its result stream to the end — may take before the
// connection is written off instead. Generous enough for a cancel round-trip,
// bounded so that a server which stopped answering cannot hold a caller past
// its own deadline.
inline constexpr std::chrono::milliseconds default_cleanup_budget{5000};

struct pool_config {
    std::string url;
    std::size_t max_size = 4;
    std::chrono::milliseconds timeout = std::chrono::seconds{30};
    ssl_mode ssl = ssl_mode::prefer;
    std::size_t max_retries = 3;
    std::chrono::milliseconds cleanup_budget = default_cleanup_budget;
};

// Appends sslmode=<value> to a libpq connection string (keyword=value or URI form).
// Returns url unchanged if sslmode is already present.
[[nodiscard]] std::string apply_ssl_mode(std::string url, ssl_mode mode);

} // namespace atlas
