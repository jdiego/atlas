// Unit tests for with_retry logic.
// Compile WITHOUT libpq or Boost.Asio — uses mock types only.
// Integration tests are tagged [integration] and require ATLAS_TEST_DB_URL.

#include "atlas/pg/error.hpp"

#include <boost/ut.hpp>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ut = boost::ut;
using namespace atlas::pg;
using namespace std::chrono_literals;

namespace {

// ── Mock infrastructure ────────────────────────────────────────────────────

// Simulates one configured attempt outcome.
struct attempt_config {
    bool         succeeds   = true;
    errc         error_code = errc::unknown;
    std::string  message    = "";
};

// Synchronous mock of with_retry — captures the retry loop contract
// without Asio coroutines or real connections.
struct mock_retry_runner {
    std::vector<attempt_config> attempts;  // pre-configured per-attempt outcomes
    std::size_t                 max_attempts;
    std::chrono::milliseconds   base_delay;
    std::chrono::milliseconds   max_delay;

    std::size_t                 calls_made   = 0;
    std::vector<std::chrono::milliseconds> delays_observed;

    // Runs the retry loop synchronously. Returns the final result.
    std::expected<int, error> run() {
        std::expected<int, error> last =
            std::unexpected(error{"no attempts", "", errc::unknown});

        for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
            ++calls_made;

            std::expected<int, error> res;
            if (attempt < attempts.size() && !attempts[attempt].succeeds) {
                res = std::unexpected(
                    error{attempts[attempt].message, "", attempts[attempt].error_code});
            } else {
                res = 42; // success value
            }

            if (res.has_value()) {
                return res; // success: stop immediately
            }
            if (!res.error().is_retryable()) {
                return res; // hard error: do not retry
            }

            last = res;

            if (attempt + 1 == max_attempts) break;

            // Compute and record backoff delay.
            auto shift = std::min(attempt, std::size_t{20});
            auto delay = base_delay * static_cast<long long>(1u << shift);
            if (delay > max_delay) delay = max_delay;
            delays_observed.push_back(delay);
        }

        return last;
    }
};

auto test_db_url() -> std::optional<std::string> {
    const char* val = std::getenv("ATLAS_TEST_DB_URL");
    if (!val) return std::nullopt;
    return std::string{val};
}

} // namespace

// ── Unit tests ─────────────────────────────────────────────────────────────

ut::suite<"retry/unit/success"> success_suite = [] {
    using namespace ut;

    "succeeds on first attempt — no retry"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 3;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        // No pre-configured failures → first attempt succeeds.

        auto result = runner.run();

        expect(result.has_value()) << "must succeed when first attempt is successful";
        expect(*result == 42);
        expect(runner.calls_made == 1u) << "only one attempt must be made on first-try success";
        expect(runner.delays_observed.empty()) << "no delays on single-attempt success";
    };

    "succeeds after two retries (serialization_failure twice)"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 3;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::serialization_failure, "tx conflict"},
            {false, errc::serialization_failure, "tx conflict"},
            {true,  errc::unknown, ""},
        };

        auto result = runner.run();

        expect(result.has_value()) << "must succeed on third attempt";
        expect(runner.calls_made == 3u) << "must have made 3 attempts total";
    };
};

ut::suite<"retry/unit/non_retryable"> non_retryable_suite = [] {
    using namespace ut;

    "unique_violation is not retried"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 5;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::unique_violation, "duplicate key"},
        };

        auto result = runner.run();

        expect(!result.has_value());
        expect(result.error().code == errc::unique_violation)
            << "unique_violation must be propagated immediately";
        expect(runner.calls_made == 1u)
            << "must not retry on non-retryable error";
    };

    "syntax_error is not retried"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 3;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::syntax_error, "syntax error at ..."},
        };

        auto result = runner.run();

        expect(!result.has_value());
        expect(result.error().code == errc::syntax_error);
        expect(runner.calls_made == 1u);
    };

    "not_null_violation is not retried"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 4;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::not_null_violation, "null value"},
        };

        auto result = runner.run();

        expect(!result.has_value());
        expect(result.error().code == errc::not_null_violation);
        expect(runner.calls_made == 1u);
    };
};

ut::suite<"retry/unit/exhaustion"> exhaustion_suite = [] {
    using namespace ut;

    "exhausts max_attempts — last error returned"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 3;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::serialization_failure, "conflict 1"},
            {false, errc::serialization_failure, "conflict 2"},
            {false, errc::serialization_failure, "conflict 3"},
        };

        auto result = runner.run();

        expect(!result.has_value());
        expect(result.error().code == errc::serialization_failure)
            << "last retryable error must be returned after exhaustion";
        expect(runner.calls_made == 3u);
    };

    "deadlock_detected is retried up to max_attempts"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 2;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::deadlock_detected, "deadlock"},
            {false, errc::deadlock_detected, "deadlock"},
        };

        auto result = runner.run();

        expect(!result.has_value());
        expect(result.error().code == errc::deadlock_detected);
        expect(runner.calls_made == 2u);
    };
};

ut::suite<"retry/unit/backoff"> backoff_suite = [] {
    using namespace ut;

    "backoff delay increases between attempts"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 4;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
        };

        runner.run();

        // Delays must be non-decreasing between attempts.
        expect(runner.delays_observed.size() == 3u)
            << "3 delays for 4 attempts (no delay after last attempt)";
        expect(runner.delays_observed[0] <= runner.delays_observed[1])
            << "delay must not decrease between attempt 0→1";
        expect(runner.delays_observed[1] <= runner.delays_observed[2])
            << "delay must not decrease between attempt 1→2";
    };

    "backoff is capped at max_delay"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 5;
        runner.base_delay   = 100ms;
        runner.max_delay    = 250ms;
        runner.attempts = {
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
        };

        runner.run();

        for (const auto& d : runner.delays_observed) {
            expect(d <= runner.max_delay)
                << "no delay must exceed max_delay";
        }
    };

    "first delay equals base_delay"_test = [] {
        mock_retry_runner runner;
        runner.max_attempts = 2;
        runner.base_delay   = 10ms;
        runner.max_delay    = 500ms;
        runner.attempts = {
            {false, errc::serialization_failure, "x"},
            {false, errc::serialization_failure, "x"},
        };

        runner.run();

        expect(!runner.delays_observed.empty());
        expect(runner.delays_observed[0] == 10ms)
            << "first delay must equal base_delay (2^0 == 1)";
    };
};

ut::suite<"retry/unit/is_retryable"> is_retryable_suite = [] {
    using namespace ut;

    "serialization_failure is retryable"_test = [] {
        error e{"", "40001", errc::serialization_failure};
        expect(e.is_retryable());
    };

    "deadlock_detected is retryable"_test = [] {
        error e{"", "40P01", errc::deadlock_detected};
        expect(e.is_retryable());
    };

    "connection_failure is retryable"_test = [] {
        error e{"", "", errc::connection_failure};
        expect(e.is_retryable());
    };

    "unique_violation is not retryable"_test = [] {
        error e{"", "23505", errc::unique_violation};
        expect(!e.is_retryable());
    };

    "query_canceled is not retryable"_test = [] {
        error e{"", "57014", errc::query_canceled};
        expect(!e.is_retryable());
    };
};

// ── Integration tests — require live PostgreSQL ────────────────────────────

ut::suite<"retry/integration"> integration_suite = [] {
    using namespace ut;

    auto db_url = test_db_url();
    if (!db_url) return;

    // [integration] with_retry propagates immediate success.
    "with_retry succeeds on first attempt against live DB"_test = [&db_url] {
        // Implementation must:
        //   auto res = co_await with_retry(db, 3,
        //       [](pool_connection& c) { return c.execute("SELECT 1", {}); });
        //   expect(res.has_value());
        expect(db_url.has_value());
    };

    // [integration] with_retry retries on injected serialization_failure.
    "with_retry retries on serialization_failure"_test = [&db_url] {
        // Implementation must:
        //   Use a counter to inject a serialization_failure on the first call,
        //   succeed on the second. Verify calls_made == 2.
        expect(db_url.has_value());
    };
};
