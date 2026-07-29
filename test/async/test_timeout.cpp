// Exercises the real atlas::with_timeout templates. These suites need no
// server: the operation being raced is a plain Asio timer, and the connection
// handed to the cancelling overload is a test double that records what
// with_timeout asks of it.

#include "async_test_support.hpp"

#include "atlas/async/timeout.hpp"

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/ut.hpp>

#include <chrono>
#include <expected>
#include <optional>

namespace ut = boost::ut;
using namespace std::chrono_literals;

namespace {

using atlas::pg::errc;
using atlas_test::run;
using atlas_test::sleep_for;

// Satisfies atlas::cancellable_connection, reproducing what a real backend
// sends after a cancel: an error result carrying SQLSTATE 57014, and only then
// the sentinel that ends the stream. A double that reported end-of-stream
// straight away would hide a drain that stops at the first error.
struct recording_connection {
    int cancels = 0;
    int receives = 0;
    bool error_reported = false;
    bool cancel_fails = false;
    bool drain_fails = false;
    bool invalidated = false;

    // Stalls stand in for a server that accepted the socket and then stopped
    // answering — the usual reason a query ran out of time to begin with.
    bool cancel_stalls = false;
    bool drain_stalls = false;
    mutable int budget_reads = 0;

    [[nodiscard]] boost::asio::awaitable<std::expected<void, atlas::pg::error>> request_cancel() {
        ++cancels;
        if (cancel_stalls) {
            co_await sleep_for(60s);
        }
        if (cancel_fails) {
            co_return std::unexpected(atlas::pg::error{"cancel dispatch failed", errc::connection_failure});
        }
        co_return std::expected<void, atlas::pg::error>{};
    }

    void invalidate() noexcept {
        invalidated = true;
    }

    [[nodiscard]] std::chrono::milliseconds cleanup_budget() const noexcept {
        ++budget_reads;
        return budget;
    }

    std::chrono::milliseconds budget{100};

    [[nodiscard]] boost::asio::awaitable<std::expected<std::optional<atlas::pg::result>, atlas::pg::error>> receive() {
        ++receives;

        if (drain_stalls) {
            co_await sleep_for(60s);
        }
        if (drain_fails) {
            co_return std::unexpected(atlas::pg::error{"drain failed", errc::connection_failure});
        }
        if (!error_reported) {
            error_reported = true;
            co_return std::unexpected(
                atlas::pg::error{"canceling statement due to user request", "57014", errc::query_canceled});
        }
        co_return std::optional<atlas::pg::result>{};
    }
};

static_assert(atlas::cancellable_connection<recording_connection>);

struct minimal_connection {
    int receives = 0;
    bool invalidated = false;

    [[nodiscard]] boost::asio::awaitable<std::expected<void, atlas::pg::error>> request_cancel() {
        co_return std::expected<void, atlas::pg::error>{};
    }

    [[nodiscard]] boost::asio::awaitable<std::expected<std::optional<atlas::pg::result>, atlas::pg::error>> receive() {
        ++receives;
        if (receives == 1) {
            co_return std::unexpected(
                atlas::pg::error{"canceling statement due to user request", "57014", errc::query_canceled});
        }
        co_return std::optional<atlas::pg::result>{};
    }

    void invalidate() noexcept {
        invalidated = true;
    }
};

static_assert(atlas::cancellable_connection<minimal_connection>);
static_assert(!atlas::cleanup_budget_provider<minimal_connection>);
static_assert(atlas::default_cleanup_budget == 5s);

// An operation that takes `delay` and then reports `value`.
auto slow_operation(std::chrono::milliseconds delay, int value)
    -> boost::asio::awaitable<std::expected<int, atlas::pg::error>> {
    co_await sleep_for(delay);
    co_return value;
}

} // namespace

ut::suite<"async/timeout"> timeout_suite = [] {
    using namespace ut;

    "operation finishing first passes its value through"_test = [] {
        auto result = run(atlas::with_timeout<int>(500ms, slow_operation(1ms, 7)));

        expect(result.has_value());
        expect(result.value() == 7_i);
    };

    "operation exceeding the deadline reports query_canceled"_test = [] {
        auto result = run(atlas::with_timeout<int>(20ms, slow_operation(2s, 7)));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
    };

    "the losing branch is cancelled rather than awaited"_test = [] {
        const auto started = std::chrono::steady_clock::now();
        auto result = run(atlas::with_timeout<int>(50ms, slow_operation(10s, 7)));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        expect(!result.has_value());
        expect(elapsed < 5s) << "with_timeout waited for the abandoned operation";
    };

    // Regression: the drain used to stop at the first error, which is exactly
    // what a cancelled query reports, leaving the sentinel unread. A server-side
    // error does not invalidate PQstatus, so the pool would take the connection
    // back mid-stream and hand it to the next caller.
    "timing out cancels the query server-side and drains past the error"_test = [] {
        recording_connection conn;

        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(conn.cancels == 1_i) << "request_cancel was never issued";
        expect(conn.receives == 2_i) << "the drain stopped at the cancellation error, before the sentinel";
        expect(!conn.invalidated);
    };

    "a cancellable adapter does not need a cleanup budget accessor"_test = [] {
        minimal_connection conn;

        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(conn.receives == 2_i);
        expect(!conn.invalidated);
    };

    "a zero cleanup budget invalidates even when cancellation completes immediately"_test = [] {
        minimal_connection conn;

        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7), 0ms));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(conn.invalidated) << "a zero cleanup budget must not allow connection reuse";
    };

    "a negative cleanup budget invalidates even when cancellation completes immediately"_test = [] {
        minimal_connection conn;

        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7), -1ms));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(conn.invalidated) << "a negative cleanup budget must not allow connection reuse";
    };

    "completing in time leaves the connection untouched"_test = [] {
        recording_connection conn;

        auto result = run(atlas::with_timeout<int>(500ms, conn, slow_operation(1ms, 7)));

        expect(result.has_value());
        expect(result.value() == 7_i);
        expect(conn.cancels == 0_i);
        expect(conn.receives == 0_i);
    };

    "a failed cancel invalidates instead of waiting for an uncancelled query"_test = [] {
        recording_connection conn;
        conn.cancel_fails = true;

        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(conn.cancels == 1_i);
        expect(conn.receives == 0_i);
        expect(conn.invalidated);
    };

    "an incomplete drain invalidates the connection"_test = [] {
        recording_connection conn;
        conn.drain_fails = true;

        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(conn.receives == 2_i);
        expect(conn.invalidated);
    };

    // Regression: the cleanup path used to have no deadline of its own, so a
    // server that accepted the cancel connection and then went quiet could hold
    // with_timeout for far longer than the timeout it was asked to enforce.
    "a stalled cancel cannot outlast the cleanup budget"_test = [] {
        recording_connection conn;
        conn.cancel_stalls = true;

        const auto started = std::chrono::steady_clock::now();
        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(elapsed < 2s) << "with_timeout blocked in its own cleanup path";
        expect(conn.invalidated) << "a connection left mid-cancel must not be reused";
    };

    "a stalled drain cannot outlast the cleanup budget"_test = [] {
        recording_connection conn;
        conn.drain_stalls = true;

        const auto started = std::chrono::steady_clock::now();
        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
        expect(elapsed < 2s) << "with_timeout blocked draining a connection that went quiet";
        expect(conn.invalidated) << "a connection left mid-stream must not be reused";
    };

    // The budget comes from the connection unless the call overrides it, so a
    // pool can set it once in pool_config instead of every call site passing it.
    "the connection's own budget is used by default"_test = [] {
        recording_connection conn;
        conn.cancel_stalls = true;
        conn.budget = 600ms;

        const auto started = std::chrono::steady_clock::now();
        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        expect(!result.has_value());
        expect(elapsed >= 500ms) << "the connection's budget was ignored in favour of a shorter one";
        expect(elapsed < 2s);
        expect(conn.invalidated);
        expect(conn.budget_reads == 1_i) << "the connection budget was not read exactly once";
    };

    "an explicit budget overrides the connection's"_test = [] {
        recording_connection conn;
        conn.cancel_stalls = true;
        conn.budget = 10s;

        const auto started = std::chrono::steady_clock::now();
        auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7), 100ms));
        const auto elapsed = std::chrono::steady_clock::now() - started;

        expect(!result.has_value());
        expect(elapsed < 2s) << "the per-call budget did not take precedence";
        expect(conn.invalidated);
        expect(conn.budget_reads == 0_i) << "the override should bypass the connection budget accessor";
    };
};

ut::suite<"async/timeout/cancellation_slot"> timeout_slot_suite = [] {
    using namespace ut;

    "an external slot aborts the operation"_test = [] {
        boost::asio::io_context ctx;
        boost::asio::cancellation_signal signal;

        auto op = [&]() -> boost::asio::awaitable<std::expected<int, atlas::pg::error>> {
            // Fire the signal once the race is under way.
            co_await sleep_for(10ms);
            signal.emit(boost::asio::cancellation_type::all);
            co_await sleep_for(5s);
            co_return 7;
        };

        auto result = atlas_test::run_on(ctx, atlas::with_timeout<int>(10s, op(), signal.slot()));

        expect(!result.has_value());
        expect(result.error().code == errc::query_canceled);
    };

    // Regression: the slot handler captures the timer by reference and the
    // timer dies with the with_timeout frame, so the handler must be gone by
    // the time with_timeout returns.
    "emitting after the operation returned is harmless"_test = [] {
        boost::asio::cancellation_signal signal;

        auto result = run(atlas::with_timeout<int>(500ms, slow_operation(1ms, 7), signal.slot()));
        expect(result.has_value());

        expect(!signal.slot().has_handler()) << "the handler outlived the timer it references";
        signal.emit(boost::asio::cancellation_type::all);
    };
};
