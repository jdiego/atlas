#pragma once

// Shared helpers for the atlas::async test suites.

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace atlas_test {

namespace asio = boost::asio;

// Connection string for the suites that need a live server, on the same
// environment variable the synchronous pg-layer tests use.
[[nodiscard]] inline auto conninfo() -> std::optional<std::string> {
    const char *val = std::getenv("ATLAS_TEST_CONNINFO");
    if (val == nullptr) {
        return std::nullopt;
    }
    return std::string{val};
}

// Points at a port nothing listens on, so the failure paths can be exercised
// without a server. Port 1 is refused immediately rather than timing out.
inline constexpr const char *unreachable_conninfo = "host=127.0.0.1 port=1 dbname=atlas_absent user=atlas_absent";

// How long any single test coroutine may run before the watchdog gives up.
inline constexpr auto test_budget = std::chrono::seconds{30};

// Runs one coroutine to completion on the given context and returns its value.
// Exceptions escaping the coroutine are rethrown to the caller.
//
// A watchdog stops the context if the coroutine overruns its budget, and the
// overrun is reported as an exception. Without it a regression that leaves a
// coroutine waiting on a socket would hang the whole binary instead of failing
// the test that caught it.
template <typename T>
[[nodiscard]] auto run_on(asio::io_context &ctx, asio::awaitable<T> op) -> T {
    std::optional<T> out;
    std::exception_ptr failure;

    asio::steady_timer watchdog{ctx};
    watchdog.expires_after(test_budget);
    watchdog.async_wait([&ctx](const boost::system::error_code &ec) {
        if (!ec) {
            ctx.stop();
        }
    });

    asio::co_spawn(ctx, std::move(op), [&](std::exception_ptr err, T value) {
        watchdog.cancel();
        failure = err;
        if (!err) {
            out = std::move(value);
        }
    });
    ctx.run();

    if (failure) {
        std::rethrow_exception(failure);
    }
    if (!out) {
        throw std::runtime_error{"the test coroutine did not finish within its budget"};
    }
    return std::move(*out);
}

// Same, on a context private to this call.
template <typename T>
[[nodiscard]] auto run(asio::awaitable<T> op) -> T {
    asio::io_context ctx;
    return run_on(ctx, std::move(op));
}

// Suspends the calling coroutine for `delay`.
[[nodiscard]] inline auto sleep_for(std::chrono::milliseconds delay) -> asio::awaitable<void> {
    asio::steady_timer timer{co_await asio::this_coro::executor};
    timer.expires_after(delay);
    co_await timer.async_wait(asio::use_awaitable);
}

} // namespace atlas_test
