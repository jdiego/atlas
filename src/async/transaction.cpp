#include "atlas/async/transaction.hpp"
#include "atlas/async/pool.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <string>
#include <utility>

namespace atlas {

namespace {

asio::awaitable<void> rollback_connection(pool_connection conn) {
    auto res = co_await conn.execute("ROLLBACK", std::span<const char *const>{});
    (void)res;
    co_return;
}

[[nodiscard]] auto make_finished_transaction_error() -> pg::error {
    return pg::error{"transaction is already finished", pg::errc::invalid_state};
}

// Savepoint names cannot be bound as parameters, so they are interpolated into
// the statement. Emitting them as a quoted identifier (with embedded quotes
// doubled) keeps any caller-supplied name from escaping into SQL syntax.
// Quoting also makes the name case-sensitive — savepoint(), rollback_to() and
// release_savepoint() all quote, so they stay consistent with each other.
[[nodiscard]] auto quote_identifier(std::string_view name) -> std::expected<std::string, pg::error> {
    if (name.empty()) {
        return std::unexpected(pg::error{"savepoint name must not be empty", pg::errc::invalid_argument});
    }
    if (name.find('\0') != std::string_view::npos) {
        return std::unexpected(pg::error{"savepoint name must not contain a null byte", pg::errc::invalid_argument});
    }

    std::string quoted;
    quoted.reserve(name.size() + 2);
    quoted.push_back('"');
    for (const char c : name) {
        if (c == '"') {
            quoted.push_back('"');
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

// Runs a savepoint statement of the form "<verb> <quoted name>".
[[nodiscard]] auto savepoint_command(pool_connection &conn, std::string_view verb, std::string_view name)
    -> asio::awaitable<std::expected<void, pg::error>> {
    auto quoted = quote_identifier(name);
    if (!quoted) {
        co_return std::unexpected(quoted.error());
    }

    std::string sql{verb};
    sql.push_back(' ');
    sql.append(*quoted);

    auto res = co_await conn.execute(sql, std::span<const char *const>{});
    if (!res) {
        co_return std::unexpected(res.error());
    }

    co_return std::expected<void, pg::error>{};
}

} // namespace

// ── Constructors / move ────────────────────────────────────────────────────

transaction::transaction(pool_connection conn, executor_type ex) : conn_{std::move(conn)}, ex_{std::move(ex)} {
}

transaction::transaction(transaction &&other) noexcept
    : conn_{std::move(other.conn_)}, ex_{std::move(other.ex_)}, state_{std::exchange(other.state_, state::finished)} {
    other.conn_.reset();
}

transaction &transaction::operator=(transaction &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (state_ == state::active && conn_) {
        asio::co_spawn(ex_, rollback_connection(std::move(*conn_)), asio::detached);
        conn_.reset();
    }

    conn_ = std::move(other.conn_);
    ex_ = std::move(other.ex_);
    state_ = std::exchange(other.state_, state::finished);
    other.conn_.reset();
    return *this;
}

// ── Destructor ─────────────────────────────────────────────────────────────

transaction::~transaction() {
    if (state_ == state::active && conn_) {
        asio::co_spawn(ex_, rollback_connection(std::move(*conn_)), asio::detached);
        conn_.reset();
        state_ = state::finished;
    }
}

auto transaction::active_connection() -> std::expected<pool_connection *, pg::error> {
    if (state_ != state::active || !conn_) {
        return std::unexpected(make_finished_transaction_error());
    }
    return &*conn_;
}

void transaction::finish() noexcept {
    state_ = state::finished;
    conn_.reset();
}

// ── Commit / Rollback ──────────────────────────────────────────────────────

asio::awaitable<std::expected<void, pg::error>> transaction::commit() {
    auto conn = active_connection();
    if (!conn) {
        co_return std::unexpected(conn.error());
    }

    auto result = co_await (*conn)->execute("COMMIT", std::span<const char *const>{});
    if (!result) {
        co_return std::unexpected(result.error());
    }

    const auto tag = result->command_tag();
    if (tag == "COMMIT") {
        finish();
        co_return std::expected<void, pg::error>{};
    }
    if (tag == "ROLLBACK") {
        finish();
        co_return std::unexpected(pg::error{"transaction was rolled back", pg::errc::transaction_aborted});
    }
    co_return std::unexpected(pg::error{"unexpected COMMIT command tag", pg::errc::invalid_state});
}

asio::awaitable<std::expected<void, pg::error>> transaction::rollback() {
    auto conn = active_connection();
    if (!conn) {
        co_return std::unexpected(conn.error());
    }

    auto result = co_await (*conn)->execute("ROLLBACK", std::span<const char *const>{});
    if (!result) {
        co_return std::unexpected(result.error());
    }

    finish();
    co_return std::expected<void, pg::error>{};
}

// ── Query execution ────────────────────────────────────────────────────────

asio::awaitable<std::expected<pg::result, pg::error>> transaction::execute(std::string_view sql,
                                                                           std::span<const char *const> params) {
    auto conn = active_connection();
    if (!conn) {
        co_return std::unexpected(conn.error());
    }

    co_return co_await (*conn)->execute(sql, params);
}

// ── Savepoints ─────────────────────────────────────────────────────────────

asio::awaitable<std::expected<void, pg::error>> transaction::savepoint(std::string_view name) {
    auto conn = active_connection();
    if (!conn) {
        co_return std::unexpected(conn.error());
    }

    co_return co_await savepoint_command(**conn, "SAVEPOINT", name);
}

asio::awaitable<std::expected<void, pg::error>> transaction::rollback_to(std::string_view name) {
    auto conn = active_connection();
    if (!conn) {
        co_return std::unexpected(conn.error());
    }

    // The savepoint survives the rollback, unlike RELEASE, and the transaction
    // leaves its aborted state so further statements can be issued.
    co_return co_await savepoint_command(**conn, "ROLLBACK TO SAVEPOINT", name);
}

asio::awaitable<std::expected<void, pg::error>> transaction::release_savepoint(std::string_view name) {
    auto conn = active_connection();
    if (!conn) {
        co_return std::unexpected(conn.error());
    }

    // Work done since the savepoint is kept; only the rollback target is dropped.
    co_return co_await savepoint_command(**conn, "RELEASE SAVEPOINT", name);
}

} // namespace atlas
