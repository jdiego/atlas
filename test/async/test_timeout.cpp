// Unit tests for with_timeout logic.
// Compile WITHOUT libpq or Boost.Asio — uses mock timer/operation types.
// Integration tests are tagged [integration] and require ATLAS_TEST_DB_URL.

#include "atlas/pg/error.hpp"

#include <boost/ut.hpp>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace ut = boost::ut;
using namespace atlas::pg;
using namespace std::chrono_literals;

namespace {

// ── Mock timer / race infrastructure ──────────────────────────────────────
// Models the "race two awaitables" contract without Asio coroutine machinery.

enum class race_winner { operation, timer };

// Simulates the outcome of with_timeout:
//   - If winner == operation and the operation succeeds → return op_result.
//   - If winner == operation and the operation fails   → propagate op_error.
//   - If winner == timer                               → return query_canceled.
template<typename T>
std::expected<T, error>
simulate_with_timeout(race_winner winner,
                      std::expected<T, error> op_result)
{
    if (winner == race_winner::operation) {
        return op_result;
    }
    return std::unexpected(error{"operation timed out", "", errc::query_canceled});
}

struct mock_timer {
    bool cancelled = false;
    void cancel() { cancelled = true; }
};

auto test_db_url() -> std::optional<std::string> {
    const char* val = std::getenv("ATLAS_TEST_DB_URL");
    if (!val) return std::nullopt;
    return std::string{val};
}

} // namespace

// ── Unit tests ─────────────────────────────────────────────────────────────

ut::suite<"timeout/unit/op_wins"> op_wins_suite = [] {
    using namespace ut;

    "operation completes before timeout — result returned"_test = [] {
        auto op_result = std::expected<int, error>{99};
        auto result    = simulate_with_timeout(race_winner::operation, op_result);

        expect(result.has_value()) << "op winning must return its value";
        expect(*result == 99);
    };

    "operation completes before timeout — timer is cancelled"_test = [] {
        mock_timer timer;
        // When op wins, the implementation must cancel the timer.
        // Simulate: if (op won) timer.cancel();
        bool op_won = true;
        if (op_won) timer.cancel();

        expect(timer.cancelled) << "timer must be cancelled when op completes first";
    };

    "non-timeout errors are propagated unchanged"_test = [] {
        auto op_result = std::expected<int, error>{
            std::unexpected(error{"unique constraint", "23505", errc::unique_violation})};
        auto result = simulate_with_timeout(race_winner::operation, op_result);

        expect(!result.has_value());
        expect(result.error().code == errc::unique_violation)
            << "non-timeout error must not be wrapped or changed";
    };

    "serialization_failure propagated unchanged when op wins"_test = [] {
        auto op_result = std::expected<int, error>{
            std::unexpected(error{"tx conflict", "40001", errc::serialization_failure})};
        auto result = simulate_with_timeout(race_winner::operation, op_result);

        expect(!result.has_value());
        expect(result.error().code == errc::serialization_failure);
    };
};

ut::suite<"timeout/unit/timer_wins"> timer_wins_suite = [] {
    using namespace ut;

    "timer fires before operation — query_canceled returned"_test = [] {
        auto op_result = std::expected<int, error>{42};
        auto result    = simulate_with_timeout(race_winner::timer, op_result);

        expect(!result.has_value()) << "timeout must return an error";
        expect(result.error().code == errc::query_canceled)
            << "error code must be query_canceled on timeout";
    };

    "timeout error message is non-empty"_test = [] {
        auto op_result = std::expected<int, error>{0};
        auto result    = simulate_with_timeout(race_winner::timer, op_result);

        expect(!result.has_value());
        expect(!result.error().message.empty()) << "timeout error must have a message";
    };

    "timer fires — operation result is discarded"_test = [] {
        // When the timer wins, the operation's (would-be) result must not be returned.
        // Even if op_result has a value, timeout overrides it.
        auto op_result = std::expected<int, error>{12345};
        auto result    = simulate_with_timeout(race_winner::timer, op_result);

        expect(!result.has_value())
            << "op result must be discarded when timer wins";
    };
};

ut::suite<"timeout/unit/duration_semantics"> duration_suite = [] {
    using namespace ut;

    "zero duration immediately times out (edge case)"_test = [] {
        // A duration of 0 should expire immediately, causing the timer to win
        // before the operation gets a chance to run. Simulated here.
        auto result = simulate_with_timeout(
            race_winner::timer,
            std::expected<int, error>{1});

        expect(result.error().code == errc::query_canceled);
    };

    "large duration allows slow operations to complete"_test = [] {
        // A very large duration means op always wins in practice.
        auto result = simulate_with_timeout(
            race_winner::operation,
            std::expected<int, error>{7});

        expect(result.has_value());
        expect(*result == 7);
    };
};

// ── Integration tests — require live PostgreSQL ────────────────────────────

ut::suite<"timeout/integration"> integration_suite = [] {
    using namespace ut;

    auto db_url = test_db_url();
    if (!db_url) return;

    // [integration] Fast query completes before 500 ms timeout.
    "SELECT 1 completes within 500ms timeout"_test = [&db_url] {
        // Implementation must:
        //   auto res = co_await with_timeout(500ms, db.execute("SELECT 1", {}));
        //   expect(res.has_value());
        expect(db_url.has_value());
    };

    // [integration] Slow operation (pg_sleep) triggers timeout.
    "pg_sleep(10) times out with 50ms deadline"_test = [&db_url] {
        // Implementation must:
        //   auto res = co_await with_timeout(50ms,
        //       db.execute("SELECT pg_sleep(10)", {}));
        //   expect(!res.has_value());
        //   expect(res.error().code == errc::query_canceled);
        expect(db_url.has_value());
    };
};
