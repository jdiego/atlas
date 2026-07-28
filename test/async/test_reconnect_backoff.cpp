// Exercises the bounded reconnect-delay configuration and calculation.

#include "async_test_support.hpp"

#include "async/detail/reconnect_backoff.hpp"
#include "atlas/async/pool.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <chrono>
#include <limits>
#include <string>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] auto acquire_rejects(atlas::pool_config cfg) -> bool {
    asio::io_context ctx;
    atlas::pool db{ctx.get_executor(), std::move(cfg)};

    return atlas_test::run_on(ctx, [&]() -> asio::awaitable<bool> {
        const auto acquired = co_await db.acquire();
        co_return !acquired && acquired.error().code == atlas::pg::errc::invalid_argument;
    }());
}

} // namespace

ut::suite<"async/reconnect_backoff/unit"> reconnect_backoff_unit_suite = [] {
    using namespace ut;

    "reconnect delay defaults are bounded"_test = [] {
        const atlas::pool_config cfg;
        expect(cfg.reconnect_initial_delay == 100ms);
        expect(cfg.reconnect_max_delay == 5s);
    };

    "reconnect delay doubles below its maximum"_test = [] {
        expect(atlas::detail::next_reconnect_delay(100ms, 5s) == 200ms);
    };

    "reconnect delay saturates before its maximum"_test = [] {
        expect(atlas::detail::next_reconnect_delay(4s, 5s) == 5s);
    };

    "reconnect delay remains at its maximum"_test = [] { expect(atlas::detail::next_reconnect_delay(5s, 5s) == 5s); };

    "zero reconnect delay remains zero"_test = [] { expect(atlas::detail::next_reconnect_delay(0ms, 5s) == 0ms); };

    "reconnect delay saturates without overflowing"_test = [] {
        expect(atlas::detail::next_reconnect_delay(std::chrono::milliseconds::max(), 5s) == 5s);
    };

    "negative initial reconnect delay is terminal"_test = [] {
        atlas::pool_config cfg;
        cfg.reconnect_initial_delay = -1ms;
        expect(acquire_rejects(std::move(cfg)));
    };

    "negative maximum reconnect delay is terminal"_test = [] {
        atlas::pool_config cfg;
        cfg.reconnect_max_delay = -1ms;
        expect(acquire_rejects(std::move(cfg)));
    };

    "maximum reconnect delay below initial delay is terminal"_test = [] {
        atlas::pool_config cfg;
        cfg.reconnect_initial_delay = 2ms;
        cfg.reconnect_max_delay = 1ms;
        expect(acquire_rejects(std::move(cfg)));
    };
};
