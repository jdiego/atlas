// Exercises the bounded reconnect-delay configuration and calculation.

#include "async_test_support.hpp"

#include "async/detail/reconnect_backoff.hpp"
#include "atlas/async/pool.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <string>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] auto acquire_fails_with(atlas::pool_config cfg, atlas::pg::errc expected) -> bool {
    asio::io_context ctx;
    auto db = std::make_unique<atlas::pool>(ctx.get_executor(), std::move(cfg));

    return atlas_test::run_on(ctx, [&]() -> asio::awaitable<bool> {
        const auto acquired = co_await db->acquire();
        const bool matched = !acquired && acquired.error().code == expected;
        db.reset(); // perpetual recovery ends only when the pool shuts down
        co_return matched;
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
        expect(acquire_fails_with(std::move(cfg), atlas::pg::errc::invalid_argument));
    };

    "negative maximum reconnect delay is terminal"_test = [] {
        atlas::pool_config cfg;
        cfg.reconnect_max_delay = -1ms;
        expect(acquire_fails_with(std::move(cfg), atlas::pg::errc::invalid_argument));
    };

    "maximum reconnect delay below initial delay is terminal"_test = [] {
        atlas::pool_config cfg;
        cfg.reconnect_initial_delay = 2ms;
        cfg.reconnect_max_delay = 1ms;
        expect(acquire_fails_with(std::move(cfg), atlas::pg::errc::invalid_argument));
    };

    "zero initial reconnect delay is accepted"_test = [] {
        atlas::pool_config cfg;
        cfg.url = atlas_test::unreachable_conninfo;
        cfg.reconnect_initial_delay = 0ms;
        cfg.reconnect_max_delay = 5s;
        expect(acquire_fails_with(std::move(cfg), atlas::pg::errc::connection_failure));
    };

    "zero reconnect delay range is accepted"_test = [] {
        atlas::pool_config cfg;
        cfg.url = atlas_test::unreachable_conninfo;
        cfg.reconnect_initial_delay = 0ms;
        cfg.reconnect_max_delay = 0ms;
        expect(acquire_fails_with(std::move(cfg), atlas::pg::errc::connection_failure));
    };

    "zero maximum below positive initial delay is terminal"_test = [] {
        atlas::pool_config cfg;
        cfg.reconnect_initial_delay = 1ms;
        cfg.reconnect_max_delay = 0ms;
        expect(acquire_fails_with(std::move(cfg), atlas::pg::errc::invalid_argument));
    };
};
