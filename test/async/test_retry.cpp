// Exercises the real atlas::with_retry template. The retry policy is driven by
// the error the operation reports, so the operations here are lambdas whose
// outcome the test controls; the pool, the backoff timer and the retry loop
// itself are the production ones.

#include "async_test_support.hpp"

#include "atlas/async/retry.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

using atlas::pg::errc;
using atlas_test::conninfo;
using atlas_test::run_on;

using int_result = std::expected<int, atlas::pg::error>;
constexpr std::span<const char *const> no_params{};

[[nodiscard]] auto config_for(std::string url, std::size_t max_size, std::chrono::milliseconds timeout)
    -> atlas::pool_config {
    atlas::pool_config cfg;
    cfg.url = std::move(url);
    cfg.max_size = max_size;
    cfg.timeout = timeout;
    return cfg;
}

[[nodiscard]] auto failure(errc code, std::string message) -> int_result {
    return std::unexpected(atlas::pg::error{std::move(message), code});
}

auto sample_pool_during_backoff(atlas::pool &db, const int &calls, std::size_t &available_during_backoff)
    -> asio::awaitable<void> {
    for (int sample_attempt = 0; calls < 1 && sample_attempt < 10; ++sample_attempt) {
        co_await atlas_test::sleep_for(5ms);
    }
    if (calls < 1) {
        co_return;
    }
    co_await atlas_test::sleep_for(50ms); // well inside the 300ms backoff
    available_during_backoff = db.available();
}

} // namespace

ut::suite<"async/retry/unit"> retry_unit_suite = [] {
    using namespace ut;

    "a pool that cannot connect is reported without running the operation"_test = [] {
        asio::io_context ctx;
        auto db =
            std::make_unique<atlas::pool>(ctx.get_executor(), config_for(atlas_test::unreachable_conninfo, 1, 5s));

        int calls = 0;
        auto op = [&calls](atlas::pool_connection &) -> asio::awaitable<int_result> {
            ++calls;
            co_return 1;
        };

        auto res = run_on(ctx, [&]() -> asio::awaitable<int_result> {
            auto result = co_await atlas::with_retry<int>(*db, 3, op, 1ms, 5ms);
            db.reset(); // perpetual recovery ends only when the pool shuts down
            co_return result;
        }());

        expect(!res.has_value());
        expect(res.error().code == errc::connection_failure);
        expect(calls == 0_i) << "the operation ran without a connection";
    };

    "backoff saturates before duration multiplication can overflow"_test = [] {
        constexpr auto huge =
            std::chrono::milliseconds{std::numeric_limits<std::chrono::milliseconds::rep>::max() / 2 + 1};
        constexpr auto cap = 500ms;

        expect(atlas::detail::retry_delay(8, huge, cap) == cap);
        expect(atlas::detail::retry_delay(8, -1ms, cap) == 0ms);
    };
};

ut::suite<"async/retry/integration"> retry_integration_suite = [] {
    using namespace ut;

    const auto url = conninfo();
    if (!url) {
        // Reported rather than silently contributing zero tests, so a CI run
        // without a server is visibly uncovered instead of looking green.
        skip / "requires a live server via ATLAS_TEST_CONNINFO"_test = [] {};
        return;
    }

    "a successful operation runs exactly once"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        int calls = 0;
        auto op = [&calls](atlas::pool_connection &) -> asio::awaitable<int_result> {
            ++calls;
            co_return 42;
        };

        auto res = run_on(ctx, atlas::with_retry<int>(db, 3, op, 1ms, 5ms));

        expect(res.has_value());
        expect(res.value() == 42_i);
        expect(calls == 1_i);
    };

    "a non-retryable error is not retried"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        int calls = 0;
        auto op = [&calls](atlas::pool_connection &) -> asio::awaitable<int_result> {
            ++calls;
            co_return failure(errc::unique_violation, "duplicate key");
        };

        auto res = run_on(ctx, atlas::with_retry<int>(db, 4, op, 1ms, 5ms));

        expect(!res.has_value());
        expect(res.error().code == errc::unique_violation);
        expect(calls == 1_i);
    };

    "a retryable error exhausts the attempt budget"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        int calls = 0;
        auto op = [&calls](atlas::pool_connection &) -> asio::awaitable<int_result> {
            ++calls;
            co_return failure(errc::serialization_failure, "could not serialize access");
        };

        auto res = run_on(ctx, atlas::with_retry<int>(db, 3, op, 1ms, 5ms));

        expect(!res.has_value());
        expect(res.error().code == errc::serialization_failure);
        expect(calls == 3_i);
    };

    "a transient failure is retried until it succeeds"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        int calls = 0;
        auto op = [&calls](atlas::pool_connection &) -> asio::awaitable<int_result> {
            ++calls;
            if (calls < 3) {
                co_return failure(errc::deadlock_detected, "deadlock detected");
            }
            co_return 7;
        };

        auto res = run_on(ctx, atlas::with_retry<int>(db, 5, op, 1ms, 5ms));

        expect(res.has_value());
        expect(res.value() == 7_i);
        expect(calls == 3_i);
    };

    "a serialization failure inside BEGIN is rolled back before retry"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        int calls = 0;
        auto op = [&calls](atlas::pool_connection &conn) -> asio::awaitable<int_result> {
            ++calls;

            auto begun = co_await conn.execute("BEGIN", no_params);
            if (!begun) {
                co_return std::unexpected(begun.error());
            }

            if (calls == 1) {
                auto failed = co_await conn.execute(
                    "DO $$ BEGIN RAISE EXCEPTION 'retry' USING ERRCODE = '40001'; END $$", no_params);
                if (failed) {
                    co_return failure(errc::unknown, "the serialization failure unexpectedly succeeded");
                }
                co_return std::unexpected(failed.error());
            }

            auto selected = co_await conn.execute("SELECT 42", no_params);
            if (!selected) {
                co_return std::unexpected(selected.error());
            }

            auto committed = co_await conn.execute("COMMIT", no_params);
            if (!committed) {
                co_return std::unexpected(committed.error());
            }

            co_return 42;
        };

        auto res = run_on(ctx, atlas::with_retry<int>(db, 2, op, 1ms, 5ms));

        expect(res.has_value()) << (res ? "" : res.error().message);
        expect(res.value_or(0) == 42_i);
        expect(calls == 2_i);
    };

    "backoff delays the retries"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        auto op = [](atlas::pool_connection &) -> asio::awaitable<int_result> {
            co_return failure(errc::serialization_failure, "could not serialize access");
        };

        const auto started = std::chrono::steady_clock::now();
        auto res = run_on(ctx, atlas::with_retry<int>(db, 3, op, 30ms, 200ms));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        expect(!res.has_value());
        // Two waits between three attempts: 30ms + 60ms.
        expect(elapsed >= 80ms) << "the retries did not back off";
    };

    // Regression: the lease used to stay in scope across the backoff timer, so a
    // retrying caller held a connection for the whole delay without using it.
    "the lease goes back to the pool before the backoff"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        int calls = 0;
        std::size_t available_during_backoff = 99;

        auto op = [&calls](atlas::pool_connection &) -> asio::awaitable<int_result> {
            ++calls;
            co_return failure(errc::serialization_failure, "could not serialize access");
        };

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            // Samples the pool while with_retry is waiting out its backoff.
            asio::co_spawn(co_await asio::this_coro::executor,
                           sample_pool_during_backoff(db, calls, available_during_backoff), asio::detached);

            auto res = co_await atlas::with_retry<int>(db, 2, op, 300ms, 300ms);
            expect(!res.has_value());
            expect(calls == 2_i);
            co_return true;
        }());

        expect(ran);
        expect(available_during_backoff == 1_ul) << "the connection was held across the backoff";
    };

    "the operation receives a usable connection"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        auto op = [](atlas::pool_connection &conn) -> asio::awaitable<int_result> {
            auto res = co_await conn.execute("SELECT 1", std::span<const char *const>{});
            if (!res) {
                co_return std::unexpected(res.error());
            }
            co_return static_cast<int>(res->rows());
        };

        auto res = run_on(ctx, atlas::with_retry<int>(db, 2, op, 1ms, 5ms));

        expect(res.has_value()) << (res ? "" : res.error().message);
        expect(res.value() == 1_i);
    };
};
