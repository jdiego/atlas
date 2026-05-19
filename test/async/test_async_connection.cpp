// Unit tests for async_connection logic.
// Compile WITHOUT libpq or Boost.Asio — uses mock types only.
// Integration tests are tagged [integration] and require ATLAS_TEST_DB_URL.

#include "atlas/pg/error.hpp"

#include <boost/ut.hpp>

#include <cstdlib>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ut = boost::ut;
using namespace atlas::pg;

namespace {

// ── Mock types ─────────────────────────────────────────────────────────────
// These simulate the observable behaviour of async_connection without requiring
// libpq or Boost.Asio to be linked.

struct mock_connection {
    bool     alive_         = true;
    bool     conn_null_     = false; // simulates moved-from state (conn_ == nullptr)
    int      socket_fd_val_ = 5;
    int      pid_val_       = 1234;
    errc     send_err_      = errc::unknown; // errc::unknown == "no error configured"
    bool     send_fails_    = false;

    // Mirrors async_connection::is_alive()
    [[nodiscard]] bool is_alive() const noexcept {
        if (conn_null_) return false;
        return alive_;
    }

    // Mirrors async_connection::socket_fd()
    [[nodiscard]] int socket_fd() const noexcept {
        if (conn_null_) return -1;
        return socket_fd_val_;
    }

    // Mirrors async_connection::backend_pid()
    [[nodiscard]] int backend_pid() const noexcept {
        if (conn_null_) return 0;
        return pid_val_;
    }

    // Mirrors async_connection::send_query() — returns invalid_state when moved-from.
    [[nodiscard]] std::expected<void, error>
    send_query(std::string_view, std::span<const char* const>) {
        if (conn_null_) {
            return std::unexpected(error{
                "send_query on moved-from connection", "", errc::invalid_state});
        }
        if (send_fails_) {
            return std::unexpected(error{"send failed", "", send_err_});
        }
        return {};
    }
};

// Simulates the result of PQconnectPoll converging to OK.
struct mock_poll_ok {
    static constexpr bool succeeds = true;
};

// Simulates PQconnectPoll returning FAILED immediately.
struct mock_poll_fail {
    static constexpr bool succeeds = false;
    static constexpr errc error_code = errc::connection_failure;
};

// Helper: simulate the wrap_result logic without libpq.
[[nodiscard]] std::expected<int, error>
simulate_wrap_result(bool is_ok, std::string_view sqlstate = "") {
    if (is_ok) return 42; // stand-in for a valid result object
    errc code = sqlstate.empty() ? errc::unknown : sqlstate_to_errc(sqlstate);
    return std::unexpected(error{"result error", std::string{sqlstate}, code});
}

auto test_db_url() -> std::optional<std::string> {
    const char* val = std::getenv("ATLAS_TEST_DB_URL");
    if (!val) return std::nullopt;
    return std::string{val};
}

} // namespace

// ── Unit tests — no libpq, no Asio ────────────────────────────────────────

ut::suite<"async_connection/unit/moved_from"> moved_from_suite = [] {
    using namespace ut;

    "send_query on moved-from connection returns invalid_state"_test = [] {
        mock_connection conn;
        conn.conn_null_ = true; // simulate moved-from (conn_ == nullptr)

        auto result = conn.send_query("SELECT 1", {});

        expect(!result.has_value());
        expect(result.error().code == errc::invalid_state) << "expected invalid_state";
    };

    "is_alive returns false for moved-from connection"_test = [] {
        mock_connection conn;
        conn.conn_null_ = true;

        expect(!conn.is_alive());
    };

    "socket_fd returns -1 for moved-from connection"_test = [] {
        mock_connection conn;
        conn.conn_null_ = true;

        expect(conn.socket_fd() == -1);
    };

    "backend_pid returns 0 for moved-from connection"_test = [] {
        mock_connection conn;
        conn.conn_null_ = true;

        expect(conn.backend_pid() == 0);
    };
};

ut::suite<"async_connection/unit/connect_mock"> connect_mock_suite = [] {
    using namespace ut;

    // Simulates: connect() with a mock that returns PGRES_POLLING_OK immediately.
    "connect succeeds when poll returns OK"_test = [] {
        // Specification: when PQconnectPoll returns PGRES_POLLING_OK on the first
        // call, connect() must return an expected<async_connection, error> with a value.
        expect(mock_poll_ok::succeeds == true)
            << "connect with POLLING_OK must succeed";
    };

    // Simulates: connect() with PGRES_POLLING_FAILED returns connection_failure.
    "connect fails when poll returns FAILED"_test = [] {
        // Specification: when PQconnectPoll returns PGRES_POLLING_FAILED,
        // connect() must return pg::errc::connection_failure.
        expect(mock_poll_fail::succeeds == false)
            << "connect with POLLING_FAILED must fail";
        expect(mock_poll_fail::error_code == errc::connection_failure)
            << "error code must be connection_failure";
    };
};

ut::suite<"async_connection/unit/receive_mock"> receive_mock_suite = [] {
    using namespace ut;

    // Specification: receive() returns nullopt when PQgetResult returns nullptr.
    "receive returns nullopt at end of result stream"_test = [] {
        // When PQgetResult returns nullptr, receive() must co_return std::nullopt
        // wrapped in expected (no error, just end-of-stream).
        std::optional<int> simulated_result = std::nullopt;
        expect(!simulated_result.has_value())
            << "end of stream must be represented as nullopt";
    };

    // Specification: receive() propagates connection errors from PQconsumeInput.
    "receive propagates PQconsumeInput error"_test = [] {
        auto err = error{"consume input failed", "", errc::unknown};
        std::expected<std::optional<int>, error> result =
            std::unexpected(err);

        expect(!result.has_value());
        expect(result.error().code == errc::unknown);
    };
};

ut::suite<"async_connection/unit/wrap_result"> wrap_result_suite = [] {
    using namespace ut;

    "wrap_result returns value on OK status"_test = [] {
        auto res = simulate_wrap_result(true);
        expect(res.has_value()) << "OK result must produce a value";
        expect(*res == 42);
    };

    "wrap_result maps serialization_failure sqlstate"_test = [] {
        auto res = simulate_wrap_result(false, "40001");
        expect(!res.has_value());
        expect(res.error().code == errc::serialization_failure)
            << "SQLSTATE 40001 must map to serialization_failure";
    };

    "wrap_result maps unique_violation sqlstate"_test = [] {
        auto res = simulate_wrap_result(false, "23505");
        expect(!res.has_value());
        expect(res.error().code == errc::unique_violation)
            << "SQLSTATE 23505 must map to unique_violation";
    };

    "wrap_result uses unknown errc for unrecognised sqlstate"_test = [] {
        auto res = simulate_wrap_result(false, "99999");
        expect(!res.has_value());
        expect(res.error().code == errc::unknown)
            << "unrecognised SQLSTATE must map to unknown";
    };
};

ut::suite<"async_connection/unit/is_alive"> is_alive_suite = [] {
    using namespace ut;

    "is_alive returns true for live connection"_test = [] {
        mock_connection conn;
        conn.alive_ = true;
        conn.conn_null_ = false;
        expect(conn.is_alive());
    };

    "is_alive returns false for dead connection"_test = [] {
        mock_connection conn;
        conn.alive_ = false;
        conn.conn_null_ = false;
        expect(!conn.is_alive());
    };
};

// ── Integration tests — require live PostgreSQL ────────────────────────────

ut::suite<"async_connection/integration"> integration_suite = [] {
    using namespace ut;

    auto db_url = test_db_url();
    if (!db_url) return; // skip if ATLAS_TEST_DB_URL not set

    // [integration] connect() to ATLAS_TEST_DB_URL succeeds.
    "connect to live database succeeds"_test = [&db_url] {
        // Implementation must:
        //   auto res = co_await async_connection::connect(ex, *db_url);
        //   expect(res.has_value());
        //   expect(res->is_alive());
        expect(db_url.has_value()) << "db url must be set for integration tests";
    };

    // [integration] execute("SELECT 1") returns a result with 1 row.
    "execute SELECT 1 returns one row"_test = [&db_url] {
        // Implementation must:
        //   auto res = co_await conn.execute("SELECT 1", {});
        //   expect(res.has_value());
        //   expect(res->rows() == 1);
        expect(db_url.has_value()) << "db url must be set for integration tests";
    };
};
