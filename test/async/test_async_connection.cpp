// Exercises atlas::async_connection against a real server. Integration suites
// are skipped unless ATLAS_TEST_CONNINFO points at a reachable database.

#include "async_test_support.hpp"

#include "atlas/async/async_connection.hpp"
#include "atlas/async/timeout.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ut = boost::ut;
namespace asio = boost::asio;
using namespace std::chrono_literals;

namespace {

using atlas::pg::errc;
using atlas_test::conninfo;
using atlas_test::run_on;

constexpr std::span<const char *const> no_params{};

static_assert(
    std::same_as<decltype(std::declval<atlas::async_connection &>().request_cancel()), atlas::pg_awaitable<void>>);

// Every query below is wrapped in a deadline: a regression that leaves receive()
// waiting on a socket that will never become readable should fail the suite, not
// hang the whole test binary.
auto execute_with_deadline(atlas::async_connection &conn, std::string_view sql)
    -> asio::awaitable<std::expected<atlas::pg::result, atlas::pg::error>> {
    co_return co_await atlas::with_timeout<atlas::pg::result>(5s, conn.execute(sql, no_params));
}

// async_connection has no default constructor, so the expected<> it comes
// wrapped in cannot cross co_spawn's completion handler. Report just the error
// code instead; nullopt means the connection unexpectedly succeeded.
auto connect_failure(asio::io_context &ctx, std::string_view url) -> asio::awaitable<std::optional<errc>> {
    auto res = co_await atlas::async_connection::connect(ctx.get_executor(), url);
    if (res) {
        co_return std::nullopt;
    }
    co_return res.error().code;
}

} // namespace

ut::suite<"async/connection/unit"> async_connection_unit_suite = [] {
    using namespace ut;

    "connecting to a closed port fails instead of hanging"_test = [] {
        asio::io_context ctx;

        const auto code = run_on(ctx, connect_failure(ctx, atlas_test::unreachable_conninfo));

        expect(code.has_value()) << "a connection to a closed port succeeded";
        if (code) {
            expect(*code == errc::connection_failure);
        }
    };

    "an empty connection string fails cleanly"_test = [] {
        asio::io_context ctx;

        const auto code = run_on(ctx, connect_failure(ctx, "host= port=0"));

        expect(code.has_value());
    };
};

ut::suite<"async/connection/integration"> async_connection_integration_suite = [] {
    using namespace ut;

    const auto url = conninfo();
    if (!url) {
        // Reported rather than silently contributing zero tests, so a CI run
        // without a server is visibly uncovered instead of looking green.
        skip / "requires a live server via ATLAS_TEST_CONNINFO"_test = [] {};
        return;
    }

    "connect reports a live backend"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            expect(conn.has_value()) << (conn ? "" : conn.error().message);
            if (!conn) {
                co_return false;
            }

            expect(conn->is_alive());
            expect(conn->socket_fd() >= 0);
            expect(conn->backend_pid() > 0);
            co_return true;
        }());

        expect(ran);
    };

    "a single query returns its row"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            auto res = co_await execute_with_deadline(*conn, "SELECT 1");
            expect(res.has_value()) << (res ? "" : res.error().message);
            if (res) {
                expect(res->rows() == 1_ul);
                expect(res->columns() == 1_ul);
            }
            co_return true;
        }());

        expect(ran);
    };

    // Regression: receive() used to wait for readability before draining what
    // libpq had already buffered, so the terminating sentinel of the first query
    // was never read and the second query waited on a socket that stayed quiet.
    "consecutive queries reuse the same connection"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            for (int i = 0; i < 3; ++i) {
                auto res = co_await execute_with_deadline(*conn, "SELECT 1");
                expect(res.has_value()) << "query " << i << " did not complete";
                if (!res) {
                    co_return false;
                }
            }
            co_return true;
        }());

        expect(ran);
    };

    // Regression: execute() used to return on the first error result without
    // reading the sentinel, handing back a connection that could not be reused.
    "a failed query leaves the connection usable"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            auto bad = co_await execute_with_deadline(*conn, "SELECT * FROM table_that_does_not_exist");
            expect(!bad.has_value());
            expect(bad.error().code == errc::undefined_table);

            auto good = co_await execute_with_deadline(*conn, "SELECT 1");
            expect(good.has_value()) << "the connection was not drained after the error";
            co_return true;
        }());

        expect(ran);
    };

    "transaction state reports an aborted explicit transaction"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            expect(!conn->transaction_aborted());

            auto begin = co_await conn->execute("BEGIN", no_params);
            expect(begin.has_value());
            expect(!conn->transaction_aborted());

            auto failed = co_await conn->execute("DO $$ BEGIN RAISE EXCEPTION 'retry' USING ERRCODE = '40001'; END $$",
                                                 no_params);
            expect(!failed.has_value());
            if (!failed) {
                expect(failed.error().code == errc::serialization_failure);
            }
            expect(conn->transaction_aborted());

            auto rollback = co_await conn->execute("ROLLBACK", no_params);
            expect(rollback.has_value());
            expect(!conn->transaction_aborted());
            co_return true;
        }());

        expect(ran);
    };

    // Regression: send_query passed string_view::data() straight to libpq, which
    // reads until a NUL that a view does not promise.
    "a non-terminated string_view is not read past its end"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            const std::string buffer = "SELECT 1 AND THIS TRAILING TEXT IS NOT SQL";
            const std::string_view sql = std::string_view{buffer}.substr(0, 8); // "SELECT 1"

            auto res = co_await execute_with_deadline(*conn, sql);
            expect(res.has_value()) << "libpq read past the end of the view";
            if (res) {
                expect(res->rows() == 1_ul);
            }
            co_return true;
        }());

        expect(ran);
    };

    "parameters are bound rather than interpolated"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            const std::array<const char *, 1> params{"42"};
            auto res = co_await atlas::with_timeout<atlas::pg::result>(5s, conn->execute("SELECT $1::int", params));

            expect(res.has_value()) << (res ? "" : res.error().message);
            if (res) {
                auto field = res->get(0, 0);
                expect(field.has_value());
                expect(field->has_value());
                expect(**field == std::string_view{"42"});
            }
            co_return true;
        }());

        expect(ran);
    };

    "a timed out query is cancelled and the connection is reusable"_test = [&url] {
        asio::io_context ctx;

        const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
            auto conn = co_await atlas::async_connection::connect(ctx.get_executor(), *url);
            if (!conn) {
                expect(false) << conn.error().message;
                co_return false;
            }

            const auto started = std::chrono::steady_clock::now();
            auto timed_out = co_await atlas::with_timeout<atlas::pg::result>(
                50ms, *conn, conn->execute("SELECT pg_sleep(10)", no_params));
            const auto elapsed = std::chrono::steady_clock::now() - started;

            expect(!timed_out.has_value());
            if (!timed_out) {
                expect(timed_out.error().code == errc::query_canceled);
            }
            expect(elapsed < 2s) << "cancellation blocked the executor";

            auto reused = co_await conn->execute("SELECT 1", no_params);
            expect(reused.has_value()) << "the cancelled result stream was not drained";
            co_return true;
        }());

        expect(ran);
    };
};
