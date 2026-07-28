// Exercises atlas::pool. The unit suite runs without a server; the integration
// suite needs ATLAS_TEST_CONNINFO.

#include "async_test_support.hpp"

#include "async/detail/pool_test_access.hpp"
#include "atlas/async/pool.hpp"
#include "atlas/async/timeout.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

using atlas::pg::errc;
using atlas_test::conninfo;
using atlas_test::run_on;
using atlas_test::sleep_for;

constexpr std::span<const char *const> no_params{};

[[nodiscard]] auto config_for(std::string url, std::size_t max_size, std::chrono::milliseconds timeout)
    -> atlas::pool_config {
    atlas::pool_config cfg;
    cfg.url = std::move(url);
    cfg.max_size = max_size;
    cfg.timeout = timeout;
    return cfg;
}

} // namespace

ut::suite<"async/pool/unit"> pool_unit_suite = [] {
    using namespace ut;

    "sslmode is appended to a connection string"_test = [] {
        const auto url = atlas::apply_ssl_mode("host=localhost dbname=atlas", atlas::ssl_mode::require);
        expect(url.has_value() >> fatal);
        expect(url->find("sslmode=require") != std::string::npos);
    };

    "sslmode text inside a keyword value does not suppress the configured mode"_test = [] {
        const auto url =
            atlas::apply_ssl_mode("host=localhost application_name='sslmode=require'", atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(url->find("sslmode=disable") != std::string::npos);
    };

    "an explicit URI sslmode is preserved"_test = [] {
        const auto url =
            atlas::apply_ssl_mode("postgresql://localhost/atlas?sslmode=require", atlas::ssl_mode::disable);
        expect(url.has_value() >> fatal);
        expect(*url == "postgresql://localhost/atlas?sslmode=require");
    };

    "an embedded NUL in conninfo is rejected"_test = [] {
        using namespace std::string_literals;

        const auto url =
            atlas::apply_ssl_mode(std::string{"host=local\0host", 15}, atlas::ssl_mode::prefer);
        expect(!url.has_value());
        expect(url.error().code == errc::invalid_argument);
    };

    "malformed conninfo is rejected"_test = [] {
        const auto url = atlas::apply_ssl_mode("host='unterminated", atlas::ssl_mode::prefer);
        expect(!url.has_value());
        expect(url.error().code == errc::invalid_argument);
    };

    "invalid pool config fails every acquire promptly without reconnecting"_test = [] {
        asio::io_context ctx;
        auto cfg = config_for("host='unterminated", 1, 5s);
        cfg.max_retries = 1000;
        atlas::pool db{ctx.get_executor(), cfg};

        const auto started = std::chrono::steady_clock::now();
        const bool rejected = run_on(ctx, [&]() -> asio::awaitable<bool> {
            for (int request = 0; request < 3; ++request) {
                auto acquired = co_await db.acquire();
                expect(!acquired.has_value());
                if (acquired) {
                    co_return false;
                }
                expect(acquired.error().code == errc::invalid_argument);
            }
            co_return true;
        }());

        expect(rejected);
        expect(std::chrono::steady_clock::now() - started < 1s)
            << "invalid pool configuration waited or entered a reconnect loop";
    };

    "an unreachable server yields a failure, not a hang"_test = [] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(atlas_test::unreachable_conninfo, 1, 5s)};

        // pool_connection has no default constructor, so the lease cannot cross
        // co_spawn's completion handler; report the error code instead.
        const auto code = run_on(ctx, [&]() -> asio::awaitable<std::optional<errc>> {
            auto res = co_await db.acquire();
            if (res) {
                co_return std::nullopt;
            }
            co_return res.error().code;
        }());

        expect(code.has_value()) << "a lease was handed out with no server behind the pool";
        if (code) {
            expect(*code == errc::connection_failure);
        }
        expect(db.size() == 0_ul);
    };

    // Regression: initialisation used to be co_spawned with a raw `this`, so a
    // pool destroyed mid-initialisation left the coroutine on freed memory.
    "destroying the pool during initialisation is safe"_test = [] {
        asio::io_context ctx;

        {
            atlas::pool db{ctx.get_executor(), config_for(atlas_test::unreachable_conninfo, 4, 5s)};
            ctx.poll(); // let initialise() start and suspend on the first connect
        }

        ctx.run(); // the detached coroutine finishes after the pool is gone
        expect(true);
    };
};

ut::suite<"async/pool/integration"> pool_integration_suite = [] {
    using namespace ut;

    const auto url = conninfo();
    if (!url) {
        // Reported rather than silently contributing zero tests, so a CI run
        // without a server is visibly uncovered instead of looking green.
        skip / "requires a live server via ATLAS_TEST_CONNINFO"_test = [] {};
        return;
    }

    "acquire hands out a working connection and takes it back"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 2, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                expect(lease->is_alive());
                auto res = co_await lease->execute("SELECT 1", no_params);
                expect(res.has_value()) << (res ? "" : res.error().message);
            }

            // The lease destructor dispatches the release; give the strand a turn.
            co_await sleep_for(20ms);
            expect(db.available() == 2_ul) << "the lease was not returned to the pool";
            co_return true;
        }());

        expect(ran);
    };

    // pool_config is the single place the cleanup budget is set; with_timeout
    // reads it off the lease rather than having every call site pass it.
    "the configured cleanup budget reaches the lease"_test = [&url] {
        asio::io_context ctx;
        auto cfg = config_for(*url, 1, 5s);
        cfg.cleanup_budget = 750ms;
        atlas::pool db{ctx.get_executor(), cfg};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto lease = co_await db.acquire();
            expect(lease.has_value()) << (lease ? "" : lease.error().message);
            if (!lease) {
                co_return false;
            }

            expect(lease->cleanup_budget() == 750ms);
            co_return true;
        }());

        expect(ran);
    };

    "an exhausted cleanup budget invalidates and revives the pooled connection"_test = [&url] {
        asio::io_context ctx;
        auto cfg = config_for(*url, 1, 5s);
        cfg.cleanup_budget = 0ms;
        atlas::pool db{ctx.get_executor(), cfg};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                auto timed_out = co_await atlas::with_timeout<atlas::pg::result>(
                    20ms, *lease, lease->execute("SELECT pg_sleep(10)", no_params));

                expect(!timed_out.has_value());
                expect(timed_out.error().code == errc::query_canceled);
                expect(!lease->is_alive()) << "the lease survived an exhausted cleanup budget";
            }

            auto replacement = co_await db.acquire();
            expect(replacement.has_value()) << (replacement ? "" : replacement.error().message);
            if (!replacement) {
                co_return false;
            }

            auto result = co_await replacement->execute("SELECT 1", no_params);
            expect(result.has_value()) << (result ? "" : result.error().message);
            expect(replacement->is_alive());
            co_return result.has_value();
        }());

        expect(ran);
    };

    "a connection returned in an open transaction is replaced"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value() >> fatal);
            if (!acquired) {
                co_return false;
            }

            std::optional<atlas::pool_connection> first{std::move(*acquired)};
            auto pid_result = co_await first->execute("SELECT pg_backend_pid()", no_params);
            expect(pid_result.has_value() >> fatal);
            if (!pid_result) {
                co_return false;
            }

            auto pid_field = pid_result->get(0, 0);
            expect(pid_field.has_value() >> fatal);
            if (!pid_field || !pid_field->has_value()) {
                co_return false;
            }
            const std::string first_pid{pid_field->value()};

            expect((co_await first->execute("BEGIN", no_params)).has_value());
            first.reset();

            auto second = co_await db.acquire();
            expect(second.has_value() >> fatal);
            if (!second) {
                co_return false;
            }

            auto second_pid_result = co_await second->execute("SELECT pg_backend_pid()", no_params);
            expect(second_pid_result.has_value() >> fatal);
            if (!second_pid_result) {
                co_return false;
            }

            auto second_pid_field = second_pid_result->get(0, 0);
            expect(second_pid_field.has_value() >> fatal);
            if (!second_pid_field || !second_pid_field->has_value()) {
                co_return false;
            }

            expect(std::string{second_pid_field->value()} != first_pid)
                << "the pool recirculated a session with an open transaction";
            co_return true;
        }());

        expect(ran);
    };

    "a connection returned in an aborted transaction is replaced"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value() >> fatal);
            if (!acquired) {
                co_return false;
            }

            std::optional<atlas::pool_connection> first{std::move(*acquired)};
            auto pid_result = co_await first->execute("SELECT pg_backend_pid()", no_params);
            expect(pid_result.has_value() >> fatal);
            if (!pid_result) {
                co_return false;
            }

            auto pid_field = pid_result->get(0, 0);
            expect(pid_field.has_value() >> fatal);
            if (!pid_field || !pid_field->has_value()) {
                co_return false;
            }
            const std::string first_pid{pid_field->value()};

            expect((co_await first->execute("BEGIN", no_params)).has_value());
            expect(!(co_await first->execute("SELECT 1 / 0", no_params)).has_value());
            first.reset();

            auto second = co_await db.acquire();
            expect(second.has_value() >> fatal);
            if (!second) {
                co_return false;
            }

            auto second_pid_result = co_await second->execute("SELECT pg_backend_pid()", no_params);
            expect(second_pid_result.has_value() >> fatal);
            if (!second_pid_result) {
                co_return false;
            }

            auto second_pid_field = second_pid_result->get(0, 0);
            expect(second_pid_field.has_value() >> fatal);
            if (!second_pid_field || !second_pid_field->has_value()) {
                co_return false;
            }

            expect(std::string{second_pid_field->value()} != first_pid)
                << "the pool recirculated a session with an aborted transaction";
            co_return true;
        }());

        expect(ran);
    };

    "an unsupported COPY invalidates and revives the pooled connection"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                auto sent = lease->send_query("COPY (SELECT 1) TO STDOUT", no_params);
                expect(sent.has_value()) << (sent ? "" : sent.error().message);

                auto copy = co_await lease->receive();
                expect(!copy.has_value()) << "COPY was exposed as an ordinary query result";
                if (!copy) {
                    expect(copy.error().code == errc::invalid_state);
                }
                expect(!lease->is_alive()) << "a connection left in COPY mode remained reusable";
            }

            auto replacement = co_await db.acquire();
            expect(replacement.has_value()) << (replacement ? "" : replacement.error().message);
            if (!replacement) {
                co_return false;
            }

            auto result = co_await replacement->execute("SELECT 1", no_params);
            expect(result.has_value()) << (result ? "" : result.error().message);
            co_return result.has_value();
        }());

        expect(ran);
    };

    "a connection returned with an unfinished result stream is replaced"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto acquired = co_await db.acquire();
            expect(acquired.has_value() >> fatal);
            if (!acquired) {
                co_return false;
            }

            std::optional<atlas::pool_connection> first{std::move(*acquired)};
            auto pid_result = co_await first->execute("SELECT pg_backend_pid()", no_params);
            expect(pid_result.has_value() >> fatal);
            if (!pid_result) {
                co_return false;
            }

            auto pid_field = pid_result->get(0, 0);
            expect(pid_field.has_value() >> fatal);
            if (!pid_field || !pid_field->has_value()) {
                co_return false;
            }
            const std::string first_pid{pid_field->value()};

            expect(first->send_query("SELECT 1", no_params).has_value());
            auto one = co_await first->receive();
            expect(one.has_value());
            first.reset();

            auto second = co_await db.acquire();
            expect(second.has_value() >> fatal);
            if (!second) {
                co_return false;
            }

            auto second_pid_result = co_await second->execute("SELECT pg_backend_pid()", no_params);
            expect(second_pid_result.has_value() >> fatal);
            if (!second_pid_result) {
                co_return false;
            }

            auto second_pid_field = second_pid_result->get(0, 0);
            expect(second_pid_field.has_value() >> fatal);
            if (!second_pid_field || !second_pid_field->has_value()) {
                co_return false;
            }

            expect(std::string{second_pid_field->value()} != first_pid)
                << "the pool recirculated a session with unread results";
            co_return true;
        }());

        expect(ran);
    };

    "pool::execute leases and releases around the query"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto res = co_await db.execute("SELECT 1", no_params);
            expect(res.has_value()) << (res ? "" : res.error().message);

            co_await sleep_for(20ms);
            expect(db.available() == 1_ul);
            co_return true;
        }());

        expect(ran);
    };

    "a request beyond capacity waits for a release"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 5s)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto first = co_await db.acquire();
            expect(first.has_value()) << (first ? "" : first.error().message);
            if (!first) {
                co_return false;
            }

            std::optional<atlas::pool_connection> held;
            held.emplace(std::move(*first));

            bool second_served = false;

            // The closure must be a named local: a lambda coroutine reaches its
            // captures through the closure object, and a temporary one would be
            // gone by the time the coroutine first resumes.
            auto queued_request = [&]() -> asio::awaitable<void> {
                auto second = co_await db.acquire();
                expect(second.has_value()) << "the queued waiter was never served";
                second_served = second.has_value();
            };
            asio::co_spawn(co_await asio::this_coro::executor, queued_request(), asio::detached);

            co_await sleep_for(50ms);
            expect(!second_served) << "the second request did not wait for capacity";

            held.reset(); // releases the connection, waking the waiter
            co_await sleep_for(50ms);
            expect(second_served);
            co_return true;
        }());

        expect(ran);
    };

    // Regression: release() used to put any connection straight back into
    // circulation. A connection the server has dropped would then be handed to
    // the next waiter, fail their query, and come back again — one network drop
    // permanently costing the pool a slot.
    "a connection killed server-side is replaced, not recirculated"_test = [&url] {
        asio::io_context ctx;
        auto cfg = config_for(*url, 1, 5s);
        cfg.cleanup_budget = 250ms; // must survive the reconnect below
        atlas::pool db{ctx.get_executor(), cfg};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            {
                auto lease = co_await db.acquire();
                expect(lease.has_value()) << (lease ? "" : lease.error().message);
                if (!lease) {
                    co_return false;
                }

                // The backend terminates itself, so the lease is dead by the
                // time its destructor hands it back.
                static_cast<void>(co_await lease->execute("SELECT pg_terminate_backend(pg_backend_pid())", no_params));
                expect(!lease->is_alive()) << "the connection survived its own backend being terminated";
            }

            const bool revived = co_await atlas_test::wait_until(
                [&db] { return db.available() == 1; }, 5s, 10ms);
            expect(revived) << "the pool did not publish a replacement connection";
            if (!revived) {
                co_return false;
            }

            expect(db.size() == 1_ul) << "the slot was retired instead of reconnected";

            auto reused = co_await db.execute("SELECT 1", no_params);
            expect(reused.has_value()) << "the pool never recovered a usable connection";

            auto replacement = co_await db.acquire();
            expect(replacement.has_value());
            if (replacement) {
                expect(replacement->cleanup_budget() == 250ms) << "the replacement lost the configured budget";
            }
            co_return true;
        }());

        expect(ran);
    };

    // Drives the pool from several threads while size()/available() are read
    // from another. Those observers used to read vector::size() straight off
    // the containers the strand mutates, which is undefined behaviour rather
    // than a benign race. Run this under TSan for it to mean anything.
    "concurrent use and observation stay race-free"_test = [&url] {
        // Fewer workers than connections on purpose: a saturated pool hands
        // connections straight from one waiter to the next and never touches the
        // free list, so the observers would have nothing to race against.
        constexpr int pool_size = 4;
        constexpr int worker_count = 3;
        constexpr int thread_count = 4;

        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, pool_size, 10s)};

        std::atomic<int> completed{0};

        // Each worker churns the free list for a while, so the polling loop
        // below overlaps with a steady stream of acquire/release rather than a
        // burst that is over in microseconds.
        constexpr int queries_per_worker = 200;

        auto worker = [&]() -> asio::awaitable<void> {
            for (int i = 0; i < queries_per_worker; ++i) {
                auto res = co_await db.execute("SELECT 1", no_params);
                if (!res) {
                    co_return;
                }
            }
            completed.fetch_add(1, std::memory_order_relaxed);
        };
        for (int i = 0; i < worker_count; ++i) {
            asio::co_spawn(ctx, worker(), asio::detached);
        }

        std::vector<std::thread> threads;
        threads.reserve(thread_count);
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ctx] { ctx.run(); });
        }

        // Poll the observers while the strand hands connections around. The loop
        // deliberately touches no atomic: reading `completed` here would hand
        // ThreadSanitizer a happens-before edge and hide the very race this
        // test exists to catch.
        const auto until = std::chrono::steady_clock::now() + 300ms;
        std::size_t observations = 0;
        while (std::chrono::steady_clock::now() < until) {
            observations += db.size() + db.available();
        }

        for (auto &thread : threads) {
            thread.join();
        }

        expect(completed.load() == _i(worker_count)) << "not every concurrent query completed";
        expect(observations > 0_ul) << "the observers were never polled";
    };

    "acquire times out while every connection is leased"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 100ms)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto first = co_await db.acquire();
            expect(first.has_value()) << (first ? "" : first.error().message);
            if (!first) {
                co_return false;
            }

            auto second = co_await db.acquire();
            expect(!second.has_value()) << "a second lease was handed out from a pool of one";
            if (!second) {
                expect(second.error().code == errc::query_canceled);
            }
            co_return true;
        }());

        expect(ran);
    };

    "timed-out acquire is immediately removed from the waiter queue"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 10ms)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto held = co_await db.acquire();
            expect(held.has_value() >> fatal);
            if (!held) {
                co_return false;
            }

            auto timed_out = co_await db.acquire();
            expect(!timed_out.has_value());
            if (!timed_out) {
                expect(timed_out.error().code == errc::query_canceled);
            }

            const auto waiter_count = co_await atlas::detail::pool_test_access::waiter_count(db);
            expect(waiter_count == 0_ul) << "the timed-out acquire remained queued";
            co_return waiter_count == 0;
        }());

        expect(ran);
    };

    "timer and handoff boundary never loses the pool slot"_test = [&url] {
        asio::io_context ctx;
        atlas::pool db{ctx.get_executor(), config_for(*url, 1, 10ms)};

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            for (int iteration = 0; iteration < 100; ++iteration) {
                auto acquired = co_await db.acquire();
                expect(acquired.has_value() >> fatal);
                if (!acquired) {
                    co_return false;
                }
                std::optional<atlas::pool_connection> held{std::move(*acquired)};

                auto release = [&held]() -> asio::awaitable<void> {
                    co_await sleep_for(10ms);
                    held.reset();
                };
                asio::co_spawn(co_await asio::this_coro::executor, release(), asio::detached);

                {
                    auto boundary = co_await db.acquire();
                    if (!boundary) {
                        expect(boundary.error().code == errc::query_canceled);
                    }
                }

                const bool returned = co_await atlas_test::wait_until([&db] { return db.available() == 1; }, 1s);
                expect(returned) << "the only pool slot was lost at the timer/handoff boundary";
                if (!returned) {
                    co_return false;
                }
            }

            auto usable = co_await db.execute("SELECT 1", no_params);
            expect(usable.has_value()) << (usable ? "" : usable.error().message);
            co_return usable.has_value();
        }());

        expect(ran);
    };
};
