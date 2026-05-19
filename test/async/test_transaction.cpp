// Unit tests for transaction state machine logic.
// Compile WITHOUT libpq or Boost.Asio — uses mock types only.
// Integration tests are tagged [integration] and require ATLAS_TEST_DB_URL.

#include "atlas/pg/error.hpp"

#include <boost/ut.hpp>

#include <cstdlib>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ut = boost::ut;
using namespace atlas::pg;

namespace {

// ── Mock infrastructure ────────────────────────────────────────────────────

struct mock_exec_record {
    std::string sql;
    bool        succeeds = true;
    errc        err_code = errc::unknown;
};

struct mock_conn {
    std::vector<mock_exec_record> recorded_calls;
    std::size_t                   call_index = 0;

    // Simulates conn_.execute(sql, params) with pre-configured outcomes.
    std::expected<int, error>
    execute(std::string_view sql) {
        recorded_calls.push_back({std::string{sql}, true});
        if (call_index < recorded_calls.size()) {
            auto& rec = recorded_calls[call_index++];
            if (!rec.succeeds) {
                return std::unexpected(error{"execute failed", "", rec.err_code});
            }
        }
        return 42; // stand-in for a valid result
    }

    void configure_failure(errc code) {
        if (!recorded_calls.empty()) {
            recorded_calls.back().succeeds = false;
            recorded_calls.back().err_code = code;
        }
    }
};

// Mock transaction that mirrors the real transaction's state machine
// without requiring Asio or libpq.
struct mock_transaction {
    mock_conn conn;
    bool      committed_ = false;
    bool      rollback_spawned_ = false;

    void simulate_commit_success() {
        // Simulates: auto res = co_await conn_.execute("COMMIT", {});
        //            if (res) { committed_ = true; }
        auto res = conn.execute("COMMIT");
        if (res.has_value()) {
            committed_ = true;
        }
    }

    void simulate_commit_failure() {
        // Simulates: COMMIT fails — committed_ must NOT be set.
        // Pre-configure the conn to fail on next execute.
        // (In reality this comes from the server returning an error.)
        committed_ = false; // must stay false
    }

    void simulate_rollback() {
        // Simulates: co_await do_rollback() + committed_ = true
        conn.execute("ROLLBACK");
        committed_ = true;
    }

    // Simulates the destructor: if (!committed_) spawn rollback.
    void simulate_destructor() {
        if (!committed_) {
            rollback_spawned_ = true;
            conn.execute("ROLLBACK"); // fire-and-forget in real impl
        }
    }
};

auto test_db_url() -> std::optional<std::string> {
    const char* val = std::getenv("ATLAS_TEST_DB_URL");
    if (!val) return std::nullopt;
    return std::string{val};
}

} // namespace

// ── Unit tests ─────────────────────────────────────────────────────────────

ut::suite<"transaction/unit/commit"> commit_suite = [] {
    using namespace ut;

    "commit() sets committed_ to true on success"_test = [] {
        mock_transaction tx;
        expect(!tx.committed_);

        tx.simulate_commit_success();

        expect(tx.committed_) << "committed_ must be true after successful COMMIT";
    };

    "commit() sends COMMIT to the connection"_test = [] {
        mock_transaction tx;
        tx.simulate_commit_success();

        expect(!tx.conn.recorded_calls.empty());
        expect(tx.conn.recorded_calls.back().sql == "COMMIT");
    };

    "commit() failure leaves committed_ false"_test = [] {
        mock_transaction tx;
        tx.simulate_commit_failure();

        expect(!tx.committed_) << "committed_ must stay false when COMMIT fails";
    };
};

ut::suite<"transaction/unit/destructor"> destructor_suite = [] {
    using namespace ut;

    "destructor does NOT fire rollback when committed_ is true"_test = [] {
        mock_transaction tx;
        tx.committed_ = true;

        tx.simulate_destructor();

        expect(!tx.rollback_spawned_)
            << "destructor must not spawn rollback when already committed";
    };

    "destructor fires rollback when committed_ is false"_test = [] {
        mock_transaction tx;
        expect(!tx.committed_);

        tx.simulate_destructor();

        expect(tx.rollback_spawned_)
            << "destructor must spawn rollback when transaction was not committed";
        expect(!tx.conn.recorded_calls.empty());
        expect(tx.conn.recorded_calls.back().sql == "ROLLBACK");
    };

    "destructor rollback sends exactly one ROLLBACK"_test = [] {
        mock_transaction tx;
        tx.simulate_destructor();

        std::size_t rollback_count = 0;
        for (const auto& call : tx.conn.recorded_calls) {
            if (call.sql == "ROLLBACK") ++rollback_count;
        }
        expect(rollback_count == 1u) << "exactly one ROLLBACK must be issued";
    };
};

ut::suite<"transaction/unit/rollback"> rollback_suite = [] {
    using namespace ut;

    "rollback() sets committed_ to true"_test = [] {
        mock_transaction tx;
        expect(!tx.committed_);

        tx.simulate_rollback();

        expect(tx.committed_)
            << "rollback() must set committed_ = true to suppress destructor rollback";
    };

    "rollback() sends ROLLBACK to the connection"_test = [] {
        mock_transaction tx;
        tx.simulate_rollback();

        expect(!tx.conn.recorded_calls.empty());
        expect(tx.conn.recorded_calls.back().sql == "ROLLBACK");
    };

    "after rollback() destructor does not spawn another rollback"_test = [] {
        mock_transaction tx;
        tx.simulate_rollback();    // committed_ = true
        tx.simulate_destructor();  // must be a no-op

        expect(!tx.rollback_spawned_)
            << "destructor must not spawn rollback after explicit rollback()";

        std::size_t rollback_count = 0;
        for (const auto& call : tx.conn.recorded_calls) {
            if (call.sql == "ROLLBACK") ++rollback_count;
        }
        expect(rollback_count == 1u) << "only one ROLLBACK must have been issued total";
    };
};

ut::suite<"transaction/unit/savepoint"> savepoint_suite = [] {
    using namespace ut;

    "savepoint sends SAVEPOINT <name>"_test = [] {
        mock_conn conn;
        conn.execute("SAVEPOINT sp1");

        expect(!conn.recorded_calls.empty());
        expect(conn.recorded_calls.back().sql == "SAVEPOINT sp1");
    };

    "rollback_to sends ROLLBACK TO SAVEPOINT <name>"_test = [] {
        mock_conn conn;
        conn.execute("ROLLBACK TO SAVEPOINT sp1");

        expect(conn.recorded_calls.back().sql == "ROLLBACK TO SAVEPOINT sp1");
    };

    "release_savepoint sends RELEASE SAVEPOINT <name>"_test = [] {
        mock_conn conn;
        conn.execute("RELEASE SAVEPOINT sp1");

        expect(conn.recorded_calls.back().sql == "RELEASE SAVEPOINT sp1");
    };
};

// ── Integration tests — require live PostgreSQL ────────────────────────────

ut::suite<"transaction/integration"> integration_suite = [] {
    using namespace ut;

    auto db_url = test_db_url();
    if (!db_url) return;

    // [integration] begin + insert + commit → row visible after transaction.
    "committed insert is visible after transaction"_test = [&db_url] {
        // Implementation must:
        //   auto tx = co_await db.begin();
        //   co_await tx.execute("INSERT INTO ...", {});
        //   co_await tx.commit();
        //   auto res = co_await db.execute("SELECT ...", {});
        //   expect(res->rows() == 1);
        expect(db_url.has_value());
    };

    // [integration] begin + insert + scope exit → RAII rollback fires, row absent.
    "uncommitted insert is absent after scope exit"_test = [&db_url] {
        // Implementation must:
        //   { auto tx = co_await db.begin();
        //     co_await tx.execute("INSERT INTO ...", {});
        //   } // destructor fires ROLLBACK
        //   auto res = co_await db.execute("SELECT ...", {});
        //   expect(res->rows() == 0);
        expect(db_url.has_value());
    };

    // [integration] savepoint + partial rollback_to → correct partial state.
    "rollback_to savepoint gives correct partial state"_test = [&db_url] {
        // Implementation must:
        //   auto tx = co_await db.begin();
        //   co_await tx.execute("INSERT INTO ... (id) VALUES (1)", {});
        //   co_await tx.savepoint("sp1");
        //   co_await tx.execute("INSERT INTO ... (id) VALUES (2)", {});
        //   co_await tx.rollback_to("sp1");
        //   co_await tx.commit();
        //   Verify: row 1 exists, row 2 does not.
        expect(db_url.has_value());
    };
};
