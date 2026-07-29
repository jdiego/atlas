#pragma once

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>

namespace atlas {

class pool;

namespace detail {

struct pool_recovery_snapshot {
    std::size_t timer_count = 0;
    std::size_t connect_attempt_count = 0;
    std::size_t missing_loop_count = 0;
};

struct pool_test_access {
    [[nodiscard]] static auto waiter_count(const pool &db) -> boost::asio::awaitable<std::size_t>;
    [[nodiscard]] static auto recovery_snapshot(const pool &db) -> boost::asio::awaitable<pool_recovery_snapshot>;
    static auto set_waiter_enqueued_hook(const pool &db, std::function<void()> hook) -> boost::asio::awaitable<void>;
};

} // namespace detail

} // namespace atlas
