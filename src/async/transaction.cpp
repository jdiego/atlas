#include "atlas/async/transaction.hpp"
#include "atlas/async/pool.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <string>
#include <utility>

namespace atlas {

namespace {

asio::awaitable<void> rollback_connection(pool_connection conn)
{
    auto res = co_await conn.execute("ROLLBACK", std::span<const char* const>{});
    (void)res;
    co_return;
}

[[nodiscard]] auto make_finished_transaction_error() -> pg::error
{
    return pg::error{"transaction is already finished", pg::errc::invalid_state};
}

} // namespace

// ── Constructors / move ────────────────────────────────────────────────────

transaction::transaction(pool_connection conn, executor_type ex)
    : conn_{std::move(conn)}
    , ex_{std::move(ex)}
    , committed_{false}
{}

transaction::transaction(transaction&& other) noexcept
    : conn_{std::move(other.conn_)}
    , ex_{std::move(other.ex_)}
    , committed_{std::exchange(other.committed_, true)}
    // Mark other as committed to prevent its destructor from firing a rollback.
{}

transaction& transaction::operator=(transaction&& other) noexcept
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Move-assigns; if this transaction is uncommitted, spawns a rollback first.
     *
     * Step 1 — Guard: if (this == &other) return *this.
     * Step 2 — If !committed_:
     *             asio::co_spawn(ex_, do_rollback(), asio::detached).
     * Step 3 — conn_      = std::move(other.conn_).
     *           ex_        = std::move(other.ex_).
     *           committed_ = std::exchange(other.committed_, true).
     * Step 4 — Return *this.
     *
     * Pitfalls:
     *   - do_rollback() consumes the OLD conn_; step 3 then moves the NEW connection.
     *   - After step 2, do not access the old conn_ or ex_ (they were moved into the
     *     coroutine). The move in step 3 brings in other's members.
     *   - Setting other.committed_ = true prevents other's destructor from also
     *     issuing a rollback.
     */
    if (this == &other) {
        return *this;
    }

    if (!committed_ && conn_.is_alive()) {
        asio::co_spawn(ex_, rollback_connection(std::move(conn_)), asio::detached);
    }

    conn_ = std::move(other.conn_);
    ex_ = std::move(other.ex_);
    committed_ = std::exchange(other.committed_, true);
    return *this;
}

// ── Destructor ─────────────────────────────────────────────────────────────

transaction::~transaction()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Automatically issues ROLLBACK on scope exit when commit() was never called.
     *
     * Step 1 — If committed_ == true, return immediately.
     * Step 2 — asio::co_spawn(ex_, do_rollback(), asio::detached).
     *
     * Preconditions:
     *   - ex_ executor is still running (io_context not stopped).
     *
     * Postconditions:
     *   - A ROLLBACK command is enqueued fire-and-forget.
     *
     * Pitfalls:
     *   - co_spawn with detached is fire-and-forget; no way to observe if
     *     ROLLBACK succeeds. The server cleans up when the connection drops anyway.
     *   - If io_context is stopped before the coroutine runs, the ROLLBACK is lost.
     *     This is acceptable; the server-side transaction will time out.
     *
     * Hint:
     *   asio::co_spawn(ex_, do_rollback(), asio::detached)
     */
    if (!committed_ && conn_.is_alive()) {
        asio::co_spawn(ex_, rollback_connection(std::move(conn_)), asio::detached);
        committed_ = true;
    }
}

// ── Commit / Rollback ──────────────────────────────────────────────────────

asio::awaitable<std::expected<void, pg::error>>
transaction::commit()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Sends COMMIT and marks the transaction done on success.
     *
     * Step 1 — auto res = co_await conn_.execute("COMMIT", {}).
     * Step 2 — If res is an error: co_return std::unexpected(res.error()).
     *           Do NOT set committed_ = true on failure.
     * Step 3 — committed_ = true.
     * Step 4 — co_return {} (success, void).
     *
     * Preconditions:
     *   - committed_ == false; BEGIN has been issued (guaranteed by pool::begin()).
     *
     * Postconditions:
     *   - On success: committed_ == true; server transaction is closed.
     *   - On failure: committed_ == false; destructor will attempt ROLLBACK.
     *
     * Pitfalls:
     *   - Do NOT set committed_ = true before the COMMIT result is confirmed.
     *     A network error during COMMIT leaves the transaction state ambiguous.
     *   - A failed COMMIT means PostgreSQL already aborted the transaction;
     *     the destructor's ROLLBACK is harmless in that case.
     *
     * Hint:
     *   PostgreSQL guarantees that a successfully acknowledged COMMIT is durable.
     *   Any error reply to COMMIT means the transaction was rolled back.
     */
    if (committed_) {
        co_return std::unexpected(make_finished_transaction_error());
    }

    auto res = co_await conn_.execute("COMMIT", std::span<const char* const>{});
    if (!res) {
        co_return std::unexpected(res.error());
    }

    committed_ = true;
    co_return std::expected<void, pg::error>{};
}

asio::awaitable<std::expected<void, pg::error>>
transaction::rollback()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Explicitly rolls back and suppresses the destructor's auto-rollback.
     *
     * Step 1 — co_await do_rollback() (ROLLBACK sent; errors discarded).
     * Step 2 — committed_ = true (suppresses ~transaction() rollback attempt).
     * Step 3 — co_return {} (success).
     *
     * Pitfalls:
     *   - Errors from ROLLBACK are intentionally discarded because the server
     *     discards the transaction anyway when the connection closes.
     *   - Setting committed_ = true is safe here even if ROLLBACK errored.
     */
    if (!committed_) {
        co_await do_rollback();
        committed_ = true;
    }

    co_return std::expected<void, pg::error>{};
}

// ── Query execution ────────────────────────────────────────────────────────

asio::awaitable<std::expected<pg::result, pg::error>>
transaction::execute(std::string_view sql, std::span<const char* const> params)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Executes a SQL statement within the open transaction.
     *
     * Step 1 — co_return co_await conn_.execute(sql, params).
     *
     * Preconditions:
     *   - committed_ == false (transaction is still open).
     *   - Connection is alive.
     *
     * Pitfalls:
     *   - If the query fails, PostgreSQL puts the transaction in an aborted state.
     *     All subsequent execute() calls will return errors until rollback() is called.
     *     The caller is responsible for handling aborted-transaction errors.
     */
    if (committed_) {
        co_return std::unexpected(make_finished_transaction_error());
    }

    co_return co_await conn_.execute(sql, params);
}

// ── Savepoints ─────────────────────────────────────────────────────────────

asio::awaitable<std::expected<void, pg::error>>
transaction::savepoint(std::string_view name)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Creates a named savepoint within the current transaction.
     *
     * Step 1 — auto sql = std::format("SAVEPOINT {}", name).
     * Step 2 — auto res = co_await conn_.execute(sql, {}).
     * Step 3 — If res is error: co_return std::unexpected(res.error()).
     * Step 4 — co_return {}.
     *
     * Pitfalls:
     *   - name must be a valid PostgreSQL identifier to avoid SQL injection.
     *     Consider validating that name matches [A-Za-z_][A-Za-z0-9_]*.
     *     Alternatively use quoted identifiers: SAVEPOINT "<name>" with escaping.
     *
     * Hint:
     *   std::format (C++23) builds the SQL string without manual concatenation.
     */
    std::string sql{"SAVEPOINT "};
    sql.append(name);

    auto res = co_await conn_.execute(sql, std::span<const char* const>{});
    if (!res) {
        co_return std::unexpected(res.error());
    }

    co_return std::expected<void, pg::error>{};
}

asio::awaitable<std::expected<void, pg::error>>
transaction::rollback_to(std::string_view name)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Rolls back to a named savepoint, undoing changes since that savepoint.
     *   The transaction remains open; only the savepoint's scope is undone.
     *
     * Step 1 — auto sql = std::format("ROLLBACK TO SAVEPOINT {}", name).
     * Step 2 — auto res = co_await conn_.execute(sql, {}).
     * Step 3 — If error: co_return std::unexpected(res.error()).
     * Step 4 — co_return {}.
     *
     * Postconditions:
     *   - The named savepoint still exists after rollback_to (unlike RELEASE).
     *   - Changes since the savepoint are undone; the transaction is still open.
     *
     * Pitfalls:
     *   - Do not confuse with ROLLBACK (which closes the entire transaction).
     *   - After rollback_to, the transaction is no longer in an aborted state;
     *     further queries can be issued.
     */
    std::string sql{"ROLLBACK TO SAVEPOINT "};
    sql.append(name);

    auto res = co_await conn_.execute(sql, std::span<const char* const>{});
    if (!res) {
        co_return std::unexpected(res.error());
    }

    co_return std::expected<void, pg::error>{};
}

asio::awaitable<std::expected<void, pg::error>>
transaction::release_savepoint(std::string_view name)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Releases (destroys) a named savepoint, merging its work into the parent
     *   transaction scope.
     *
     * Step 1 — auto sql = std::format("RELEASE SAVEPOINT {}", name).
     * Step 2 — auto res = co_await conn_.execute(sql, {}).
     * Step 3 — If error: co_return std::unexpected(res.error()).
     * Step 4 — co_return {}.
     *
     * Postconditions:
     *   - The named savepoint no longer exists.
     *   - Work done since the savepoint is retained (it cannot be rolled back to).
     */
    std::string sql{"RELEASE SAVEPOINT "};
    sql.append(name);

    auto res = co_await conn_.execute(sql, std::span<const char* const>{});
    if (!res) {
        co_return std::unexpected(res.error());
    }

    co_return std::expected<void, pg::error>{};
}

// ── Internal rollback ──────────────────────────────────────────────────────

asio::awaitable<void> transaction::do_rollback()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Sends ROLLBACK; used by rollback() and ~transaction(). Errors are silenced.
     *
     * Step 1 — std::ignore = co_await conn_.execute("ROLLBACK", {}).
     *
     * Preconditions:
     *   - conn_ is valid (not moved-from). If conn_ has been moved out by
     *     move-assignment, this coroutine must not be spawned — the move operator
     *     ensures committed_ == true prevents the destructor from spawning it.
     *
     * Pitfalls:
     *   - In the destructor path this is spawned with detached; if the executor is
     *     stopped the coroutine is never scheduled. This is acceptable.
     *   - Errors are intentionally discarded here; the server handles cleanup
     *     when the connection is eventually closed.
     *
     * Hint:
     *   std::ignore = co_await conn_.execute("ROLLBACK", {});
     *   The ignore suppresses [[nodiscard]] warnings on the expected return.
     */
    auto res = co_await conn_.execute("ROLLBACK", std::span<const char* const>{});
    (void)res;
    co_return;
}

} // namespace atlas
