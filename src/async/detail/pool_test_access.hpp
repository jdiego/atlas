#pragma once

#include <boost/asio/awaitable.hpp>

#include <cstddef>

namespace atlas {

class pool;

namespace detail {

struct pool_test_access {
    [[nodiscard]] static auto waiter_count(const pool &db) -> boost::asio::awaitable<std::size_t>;
};

} // namespace detail

} // namespace atlas
