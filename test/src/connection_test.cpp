#include "atlas/pg/connection.hpp"
#include "atlas/pg/error.hpp"
#include "atlas/pg/result.hpp"

#include <array>
#include <boost/ut.hpp>
#include <cstdlib>
#include <optional>
#include <sys/poll.h>
#include <string>
#include <string_view>

namespace ut = boost::ut;
using namespace atlas::pg;
using namespace std::string_view_literals;

namespace {

auto test_conninfo() -> std::optional<std::string> {
    const auto *val = std::getenv("ATLAS_TEST_CONNINFO");
    if (val == nullptr) {
        return std::nullopt;
    }
    return std::string{val};
}

// Wait for the connection socket to become readable or writable based on
// poll_state. Returns false on timeout (5 s) or poll error.
auto wait_for_socket(int fd, poll_state state) -> bool {
    if (state == poll_state::active || fd < 0) {
        return true;
    }
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = (state == poll_state::reading) ? POLLIN : POLLOUT;
    return ::poll(&pfd, 1, 5000) > 0;
}

} // namespace

// ============================================================
// Unit tests — no database required
// ============================================================

ut::suite<"connection/unit/default_state"> default_state_suite = [] {
    using namespace ut;

    "default-constructed is not alive"_test = [] {
        connection conn{};
        expect(!conn.is_alive());
    };

    "default-constructed status is bad"_test = [] {
        connection conn{};
        expect(conn.status() == connection_status::bad);
    };

    "default-constructed is_busy returns false"_test = [] {
        connection conn{};
        expect(!conn.is_busy());
    };

    "default-constructed backend_pid returns 0"_test = [] {
        connection conn{};
        expect(conn.backend_pid() == 0);
    };

    "default-constructed socket_fd returns -1"_test = [] {
        connection conn{};
        expect(conn.socket_fd() == -1);
    };
};

ut::suite<"connection/unit/null_handle_errors"> null_handle_suite = [] {
    using namespace ut;

    "exec on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.exec("SELECT 1");
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "exec_params on null handle returns invalid_state"_test = [] {
        connection conn{};
        std::array<text_param, 0> params{};
        auto r = conn.exec_params("SELECT 1", params);
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "send_query on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.send_query("SELECT 1");
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "send_query_params on null handle returns invalid_state"_test = [] {
        connection conn{};
        std::array<text_param, 0> params{};
        auto r = conn.send_query_params("SELECT 1", params);
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "flush on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.flush();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "consume_input on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.consume_input();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "next_result on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.next_result();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "reset on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.reset();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "reset_start on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.reset_start();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "poll_connect on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.poll_connect();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };

    "poll_reset on null handle returns invalid_state"_test = [] {
        connection conn{};
        auto r = conn.poll_reset();
        expect(!r);
        expect(r.error().code == errc::invalid_state);
    };
};

ut::suite<"connection/unit/validation"> validation_suite = [] {
    using namespace ut;

    "connect with NUL byte in conninfo returns invalid_argument"_test = [] {
        constexpr std::string_view bad{"host=loc\0alhost", 15};
        auto r = connection::connect(bad);
        expect(!r);
        expect(r.error().code == errc::invalid_argument);
    };

    "connect_start with NUL byte in conninfo returns invalid_argument"_test = [] {
        constexpr std::string_view bad{"host=loc\0alhost", 15};
        auto r = connection::connect_start(bad);
        expect(!r);
        expect(r.error().code == errc::invalid_argument);
    };
};

ut::suite<"connection/unit/move_semantics"> move_suite = [] {
    using namespace ut;

    "move-constructed — source is not alive"_test = [] {
        connection a{};
        connection b = std::move(a);
        expect(!a.is_alive());
        expect(!b.is_alive());
    };

    "move-assigned — source has bad status"_test = [] {
        connection a{};
        connection b{};
        b = std::move(a);
        expect(a.status() == connection_status::bad);
        expect(b.status() == connection_status::bad);
    };
};

// ============================================================
// Integration tests — require ATLAS_TEST_CONNINFO env var
// ============================================================

ut::suite<"connection/integration/connect"> connect_suite = [] {
    using namespace ut;

    "connect succeeds and connection is alive"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);
        expect(conn->is_alive());
        expect(conn->status() == connection_status::ok);
    };

    "connect sets valid backend_pid and socket_fd"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);
        expect(conn->backend_pid() != 0);
        expect(conn->socket_fd() >= 0);
    };

    "connect_start and poll_connect reaches ready state"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect_start(*ci);
        expect(conn.has_value() >> fatal);

        auto state = poll_state::writing;
        while (state != poll_state::ready) {
            expect(wait_for_socket(conn->socket_fd(), state) >> fatal);
            auto poll = conn->poll_connect();
            expect(poll.has_value() >> fatal);
            state = *poll;
        }

        expect(state == poll_state::ready);
        expect(conn->is_alive());
    };

    "move-constructed from live connection preserves state"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto a = connection::connect(*ci);
        expect(a.has_value() >> fatal);

        connection b = std::move(*a);
        expect(b.is_alive());
        expect(!a->is_alive());

        auto r = b.exec("SELECT 1");
        expect(r.has_value() >> fatal);
    };
};

ut::suite<"connection/integration/exec"> exec_suite = [] {
    using namespace ut;

    "exec SELECT 1 returns one row with correct value"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        auto r = conn->exec("SELECT 1 AS n");
        expect(r.has_value() >> fatal);
        expect(r->rows() == std::size_t{1});
        expect(r->columns() == std::size_t{1});
        auto cell = r->get(0, 0);
        expect(cell.has_value() >> fatal);
        expect(cell->has_value() >> fatal);
        expect(cell->value() == "1"sv);
    };

    "exec SELECT multiple rows"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        auto r = conn->exec("SELECT generate_series(1, 3) AS n");
        expect(r.has_value() >> fatal);
        expect(r->rows() == std::size_t{3});
        expect(r->get(0, 0)->value() == "1"sv);
        expect(r->get(1, 0)->value() == "2"sv);
        expect(r->get(2, 0)->value() == "3"sv);
    };

    "exec returns empty result set"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        auto r = conn->exec("SELECT 1 WHERE false");
        expect(r.has_value() >> fatal);
        expect(r->rows() == std::size_t{0});
        expect(r->empty());
    };

    "exec with invalid SQL returns syntax_error"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        auto r = conn->exec("NOT VALID SQL");
        expect(!r);
        expect(r.error().code == errc::syntax_error);
        expect(!r.error().message.empty());
    };

    "exec with undefined table returns undefined_table"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        auto r = conn->exec("SELECT * FROM _atlas_nonexistent_table_xyz");
        expect(!r);
        expect(r.error().code == errc::undefined_table);
        expect(!r.error().sqlstate.empty());
    };

    "exec with NUL byte in SQL returns invalid_argument"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        constexpr std::string_view bad_sql{"SELECT 1\0DROP TABLE users", 25};
        auto r = conn->exec(bad_sql);
        expect(!r);
        expect(r.error().code == errc::invalid_argument);
    };

    "connection remains usable after a query error"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        expect(!conn->exec("NOT VALID SQL"));
        expect(conn->is_alive());

        auto r = conn->exec("SELECT 1");
        expect(r.has_value() >> fatal);
        expect(r->get(0, 0)->value() == "1"sv);
    };
};

ut::suite<"connection/integration/exec_params"> exec_params_suite = [] {
    using namespace ut;

    "exec_params with single text parameter"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        std::array<text_param, 1> params{std::string_view{"hello"}};
        auto r = conn->exec_params("SELECT $1::text", params);
        expect(r.has_value() >> fatal);
        expect(r->get(0, 0)->value() == "hello"sv);
    };

    "exec_params with NULL parameter returns null cell"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        std::array<text_param, 1> params{std::nullopt};
        auto r = conn->exec_params("SELECT $1::text", params);
        expect(r.has_value() >> fatal);
        auto cell = r->get(0, 0);
        expect(cell.has_value() >> fatal);
        expect(!cell->has_value()); // NULL → empty optional
    };

    "exec_params with multiple parameters"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        std::array<text_param, 2> params{std::string_view{"3"}, std::string_view{"7"}};
        auto r = conn->exec_params("SELECT ($1::int + $2::int)::text", params);
        expect(r.has_value() >> fatal);
        expect(r->get(0, 0)->value() == "10"sv);
    };

    "exec_params with mixed NULL and non-NULL parameters"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        std::array<text_param, 2> params{std::string_view{"42"}, std::nullopt};
        auto r = conn->exec_params("SELECT $1::text, $2::text IS NULL", params);
        expect(r.has_value() >> fatal);
        expect(r->get(0, 0)->value() == "42"sv);
        expect(r->get(0, 1)->value() == "t"sv);
    };

    "exec_params with NUL byte in parameter returns invalid_argument"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        constexpr std::string_view bad_param{"hel\0lo", 6};
        std::array<text_param, 1> params{bad_param};
        auto r = conn->exec_params("SELECT $1", params);
        expect(!r);
        expect(r.error().code == errc::invalid_argument);
    };

    "exec_params with NUL byte in SQL returns invalid_argument"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        constexpr std::string_view bad_sql{"SELECT $1\0injected", 18};
        std::array<text_param, 1> params{std::string_view{"val"}};
        auto r = conn->exec_params(bad_sql, params);
        expect(!r);
        expect(r.error().code == errc::invalid_argument);
    };
};

ut::suite<"connection/integration/async"> async_suite = [] {
    using namespace ut;

    auto drain_results = [](connection &conn) -> std::optional<result> {
        for (;;) {
            auto f = conn.flush();
            if (!f.has_value() || !*f) break;
        }

        std::optional<result> got;
        for (;;) {
            if (!conn.consume_input().has_value()) break;
            if (conn.is_busy()) continue;

            auto res = conn.next_result();
            if (!res.has_value() || !res->has_value()) break;
            got = std::move(res->value());
        }
        return got;
    };

    "is_busy returns false on idle connection"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);
        expect(!conn->is_busy());
    };

    "send_query and next_result returns expected value"_test = [&drain_results] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        expect(conn->send_query("SELECT 42 AS val").has_value() >> fatal);

        auto got = drain_results(*conn);
        expect(got.has_value() >> fatal);
        expect(got->get(0, 0)->value() == "42"sv);
    };

    "send_query_params and next_result returns expected value"_test = [&drain_results] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        std::array<text_param, 1> params{std::string_view{"99"}};
        expect(conn->send_query_params("SELECT ($1::int * 2)::text", params).has_value() >> fatal);

        auto got = drain_results(*conn);
        expect(got.has_value() >> fatal);
        expect(got->get(0, 0)->value() == "198"sv);
    };

    "send_query with NUL byte in SQL returns invalid_argument"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        constexpr std::string_view bad{"SELECT 1\0injected", 17};
        auto r = conn->send_query(bad);
        expect(!r);
        expect(r.error().code == errc::invalid_argument);
    };
};

ut::suite<"connection/integration/reset"> reset_suite = [] {
    using namespace ut;

    "reset restores a live connection"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        expect(conn->reset().has_value() >> fatal);
        expect(conn->is_alive());
        expect(conn->status() == connection_status::ok);
    };

    "connection is usable after reset"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);
        expect(conn->reset().has_value() >> fatal);

        auto r = conn->exec("SELECT 1");
        expect(r.has_value() >> fatal);
        expect(r->get(0, 0)->value() == "1"sv);
    };

    "reset_start and poll_reset reaches ready state"_test = [] {
        auto ci = test_conninfo();
        if (!ci) return;

        auto conn = connection::connect(*ci);
        expect(conn.has_value() >> fatal);

        expect(conn->reset_start().has_value() >> fatal);

        auto state = poll_state::writing;
        while (state != poll_state::ready) {
            expect(wait_for_socket(conn->socket_fd(), state) >> fatal);
            auto poll = conn->poll_reset();
            expect(poll.has_value() >> fatal);
            state = *poll;
        }

        expect(state == poll_state::ready);
        expect(conn->is_alive());
    };
};
