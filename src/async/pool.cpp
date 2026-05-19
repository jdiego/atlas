#include "atlas/async/pool.hpp"
#include "atlas/async/transaction.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <memory>
#include <utility>

namespace atlas {

namespace {

struct acquire_waiter {
    explicit acquire_waiter(executor_type ex)
        : timer{std::move(ex)}
    {}

    asio::steady_timer timer;
    async_connection*  conn = nullptr;
    bool               expired = false;
};

} // namespace

// ── pool_connection ────────────────────────────────────────────────────────

pool_connection::pool_connection(async_connection* conn, pool* owner) noexcept
    : conn_{conn}
    , owner_{owner}
{}

pool_connection::pool_connection(pool_connection&& other) noexcept
    : conn_{std::exchange(other.conn_, nullptr)}
    , owner_{std::exchange(other.owner_, nullptr)}
{}

pool_connection& pool_connection::operator=(pool_connection&& other) noexcept
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Move-assigns, first returning the currently held connection to its pool.
     *
     * Step 1 — Guard: if (this == &other) return *this.
     * Step 2 — If conn_ != nullptr: owner_->release(conn_).
     * Step 3 — conn_  = std::exchange(other.conn_, nullptr).
     *           owner_ = std::exchange(other.owner_, nullptr).
     * Step 4 — Return *this.
     *
     * Pitfalls:
     *   - release() must be called on the OLD owner_ before overwriting it.
     *   - After release(), do not dereference conn_ (pool now owns it).
     *
     * Hint:
     *   std::exchange safely moves and clears each member atomically.
     */
    if (this == &other) {
        return *this;
    }

    if (conn_ != nullptr) {
        owner_->release(conn_);
    }

    conn_ = std::exchange(other.conn_, nullptr);
    owner_ = std::exchange(other.owner_, nullptr);
    return *this;
}

pool_connection::~pool_connection()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Returns the leased connection to the pool when this RAII guard expires.
     *
     * Step 1 — If conn_ == nullptr (moved-from state), do nothing.
     * Step 2 — owner_->release(conn_).
     *
     * Postconditions:
     *   - conn_ is no longer owned by this object; pool manages its lifetime.
     *
     * Pitfalls:
     *   - Do NOT call PQfinish here; the pool owns the connection lifetime.
     *   - owner_ must not be null when conn_ is not null (invariant maintained
     *     by all constructors and the move operator).
     */
    if (conn_ != nullptr) {
        owner_->release(conn_);
    }
}

std::expected<void, pg::error>
pool_connection::send_query(std::string_view sql,
                            std::span<const char* const> params)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Forwards send_query to the underlying async_connection.
     *
     * Step 1 — If conn_ == nullptr:
     *             return std::unexpected(pg::error{pg::errc::invalid_state,
     *                                             "pool_connection is in moved-from state"});
     * Step 2 — return conn_->send_query(sql, params).
     */
    if (conn_ == nullptr) {
        return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    return conn_->send_query(sql, params);
}

asio::awaitable<std::expected<std::optional<pg::result>, pg::error>>
pool_connection::receive()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Forwards receive() to the underlying async_connection.
     *
     * Step 1 — If conn_ == nullptr:
     *             co_return std::unexpected(pg::error{pg::errc::invalid_state, ...});
     * Step 2 — co_return co_await conn_->receive().
     */
    if (conn_ == nullptr) {
        co_return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    co_return co_await conn_->receive();
}

asio::awaitable<std::expected<pg::result, pg::error>>
pool_connection::execute(std::string_view sql,
                         std::span<const char* const> params)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Forwards execute() to the underlying async_connection.
     *
     * Step 1 — If conn_ == nullptr:
     *             co_return std::unexpected(pg::error{pg::errc::invalid_state, ...});
     * Step 2 — co_return co_await conn_->execute(sql, params).
     */
    if (conn_ == nullptr) {
        co_return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    co_return co_await conn_->execute(sql, params);
}

bool pool_connection::is_alive() const noexcept
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * Step 1 — If conn_ == nullptr, return false.
     * Step 2 — return conn_->is_alive().
     */
    return conn_ != nullptr && conn_->is_alive();
}

// ── pool ──────────────────────────────────────────────────────────────────

pool::pool(executor_type ex, pool_config cfg)
    : ex_{ex}
    , strand_{ex}
    , cfg_{std::move(cfg)}
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Initialises member storage and spawns the async connection setup.
     *
     * Step 1 — connections_.reserve(cfg_.max_size).
     *           free_.reserve(cfg_.max_size).
     * Step 2 — asio::co_spawn(strand_, initialise(), asio::detached).
     *
     * Preconditions:
     *   - ex is bound to a running io_context.
     *
     * Pitfalls:
     *   - Connections are not yet available synchronously; callers must co_await acquire().
     *   - Reserving before initialise() prevents reallocation that would invalidate
     *     the raw pointers stored in free_.
     *
     * Hint:
     *   connections_.reserve(cfg_.max_size) must happen BEFORE co_spawn so that the
     *   reserve is visible to initialise() when it runs on the strand.
     */
    connections_.reserve(cfg_.max_size);
    free_.reserve(cfg_.max_size);
    asio::co_spawn(strand_, initialise(), asio::detached);
}

pool::~pool()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Destroys all connections and signals pending waiters with nullptr.
     *
     * Step 1 — While !waiters_.empty():
     *             auto handler = std::move(waiters_.front());
     *             waiters_.pop();
     *             handler(nullptr);  // nullptr signals: pool is shutting down
     * Step 2 — free_.clear().
     * Step 3 — connections_.clear() — async_connection destructors close sockets.
     *
     * Preconditions:
     *   - io_context is stopped or no coroutines are pending on this pool.
     *
     * Pitfalls:
     *   - Handlers receiving nullptr must check for it and return
     *     pg::error{pg::errc::connection_failure, "pool destroyed"}.
     *   - Do not call release() from the destructor; it dispatches to the strand
     *     which may no longer be running.
     */
    while (!waiters_.empty()) {
        auto handler = std::move(waiters_.front());
        waiters_.pop();
        handler(nullptr);
    }
    free_.clear();
    connections_.clear();
}

asio::awaitable<std::expected<pool_connection, pg::error>>
pool::acquire()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Returns a RAII connection lease, suspending the coroutine if all
     *   connections are in use until one is released or timeout expires.
     *
     * Step 1 — Dispatch to strand_ to safely check free_:
     *             auto conn = co_await asio::dispatch(strand_, asio::use_awaitable);
     *             (use asio::bind_executor to run the check on the strand)
     * Step 2 — If !free_.empty():
     *             auto* ptr = free_.back(); free_.pop_back();
     *             co_return pool_connection{ptr, this}.
     * Step 3 — Otherwise, set up a race between a waiter channel and a timeout timer:
     *   a. Declare async_connection* received = nullptr.
     *   b. Create asio::steady_timer timer{ex_}; timer.expires_after(cfg_.timeout).
     *   c. Push a waiter onto waiters_:
     *        waiters_.push([&received, &timer](async_connection* c) {
     *            received = c;
     *            timer.cancel();
     *        });
     *   d. co_await timer.async_wait(asio::use_awaitable).
     *      (completes either on timeout or when timer.cancel() is called by waiter)
     *   e. If received == nullptr:
     *        co_return std::unexpected(pg::error{pg::errc::query_canceled, "acquire timed out"});
     *   f. co_return pool_connection{received, this}.
     *
     * Key types involved:
     *   - asio::strand: serialises access to free_ and waiters_
     *   - asio::steady_timer: implements the acquisition timeout
     *   - std::function<void(async_connection*)>: waiter callback type
     *
     * Preconditions:
     *   - Pool has been constructed and initialise() has been co_spawned.
     *
     * Postconditions:
     *   - On success: one connection is removed from free_.
     *   - On timeout: no connection is acquired; error returned.
     *
     * Pitfalls:
     *   - All free_/waiters_ mutations must happen inside the strand.
     *   - timer.async_wait error_code is asio::error::operation_aborted when
     *     cancelled by the waiter — this means success, not error.
     *   - If timer fires first, the waiter lambda is still in waiters_ and
     *     must be removed to avoid dangling reference to local variables.
     *
     * Hint:
     *   After timer fires check received == nullptr to distinguish timeout from
     *   wakeup. Use asio::error::operation_aborted to detect cancellation.
     */
    co_await asio::dispatch(strand_, asio::use_awaitable);

    if (!free_.empty()) {
        auto* ptr = free_.back();
        free_.pop_back();
        co_return pool_connection{ptr, this};
    }

    auto waiter = std::make_shared<acquire_waiter>(strand_);
    waiter->timer.expires_after(cfg_.timeout);
    waiters_.push([waiter](async_connection* conn) {
        if (waiter->expired) {
            return false;
        }

        waiter->conn = conn;
        waiter->timer.cancel();
        return true;
    });

    boost::system::error_code ec;
    co_await waiter->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    if (ec == asio::error::operation_aborted) {
        if (waiter->conn == nullptr) {
            co_return std::unexpected(pg::error{"pool destroyed", pg::errc::connection_failure});
        }

        co_return pool_connection{waiter->conn, this};
    }

    waiter->expired = true;
    co_return std::unexpected(pg::error{"acquire timed out", pg::errc::query_canceled});
}

asio::awaitable<std::expected<transaction, pg::error>>
pool::begin()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Acquires a connection, issues BEGIN, and wraps it in a transaction.
     *
     * Step 1 — auto conn_res = co_await acquire();
     *           if (!conn_res) co_return std::unexpected(conn_res.error());
     * Step 2 — auto exec_res = co_await conn_res->execute("BEGIN", {});
     *           if (!exec_res) co_return std::unexpected(exec_res.error());
     *           (pool_connection RAII releases connection if BEGIN fails)
     * Step 3 — co_return transaction{std::move(*conn_res), ex_}.
     *
     * Postconditions:
     *   - Returned transaction holds an exclusive connection with an open server
     *     transaction (BEGIN has been acknowledged).
     *
     * Pitfalls:
     *   - If BEGIN fails, pool_connection destructor releases conn automatically.
     *     Do NOT call release() manually in the error path.
     *
     * Hint:
     *   transaction's private constructor is accessible via friend class pool.
     */
    auto conn_res = co_await acquire();
    if (!conn_res) {
        co_return std::unexpected(conn_res.error());
    }

    auto exec_res = co_await conn_res->execute("BEGIN", std::span<const char* const>{});
    if (!exec_res) {
        co_return std::unexpected(exec_res.error());
    }

    co_return transaction{std::move(*conn_res), ex_};
}

asio::awaitable<std::expected<pg::result, pg::error>>
pool::execute(std::string_view sql, std::span<const char* const> params)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Convenience: acquires a connection, executes, then releases automatically.
     *
     * Step 1 — auto conn_res = co_await acquire();
     *           if (!conn_res) co_return std::unexpected(conn_res.error());
     * Step 2 — co_return co_await conn_res->execute(sql, params).
     *           (pool_connection destructor releases conn when conn_res goes out of scope)
     *
     * Postconditions:
     *   - Connection is returned to the pool even if execute fails.
     *
     * Pitfalls:
     *   - Do not manually release; the pool_connection destructor handles it.
     */
    auto conn_res = co_await acquire();
    if (!conn_res) {
        co_return std::unexpected(conn_res.error());
    }

    co_return co_await conn_res->execute(sql, params);
}

std::size_t pool::size() const noexcept
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * Returns the total number of connections managed by the pool.
     *
     * Step 1 — return connections_.size().
     *
     * Pitfalls:
     *   - Not strand-guarded; benign race for informational use only.
     */
    return connections_.size();
}

std::size_t pool::available() const noexcept
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * Returns the number of idle connections ready for acquisition.
     *
     * Step 1 — return free_.size().
     *
     * Pitfalls:
     *   - Same strand-race caveat as size(); use for monitoring only.
     */
    return free_.size();
}

void pool::release(async_connection* conn)
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Returns a connection to the pool; if waiters are queued, hands off directly.
     *
     * Step 1 — asio::dispatch(strand_, [this, conn]() mutable {
     *   Step 2 — If !waiters_.empty():
     *               auto handler = std::move(waiters_.front());
     *               waiters_.pop();
     *               handler(conn);  // wake up the waiting acquire() coroutine
     *   Step 3 — Else:
     *               free_.push_back(conn);
     * });
     *
     * Preconditions:
     *   - conn was previously acquired from this pool and is not null.
     *   - conn is still alive (or caller has verified and accepts a dead connection
     *     being returned — pool may reconnect later).
     *
     * Postconditions:
     *   - conn is either given to a waiter handler or pushed back onto free_.
     *
     * Pitfalls:
     *   - Must run on the strand; called from pool_connection destructor which
     *     may execute on any thread/coroutine. Use asio::dispatch, not post.
     *   - Do not access free_ or waiters_ without being on the strand.
     *
     * Hint:
     *   asio::dispatch(strand_, lambda) ensures the lambda runs on the strand
     *   immediately if already on it, or posts it otherwise.
     */
    asio::dispatch(strand_, [this, conn]() mutable {
        if (conn == nullptr) {
            return;
        }

        while (!waiters_.empty()) {
            auto handler = std::move(waiters_.front());
            waiters_.pop();
            if (handler(conn)) {
                return;
            }
        }

        free_.push_back(conn);
    });
}

asio::awaitable<void> pool::initialise()
{
    /*
     * IMPLEMENTATION GUIDE:
     *
     * What this does:
     *   Sequentially connects cfg_.max_size connections and populates free_.
     *
     * Step 1 — std::string connstr = apply_ssl_mode(cfg_.url, cfg_.ssl).
     * Step 2 — for (std::size_t i = 0; i < cfg_.max_size; ++i):
     *             auto res = co_await async_connection::connect(ex_, connstr);
     *             if (res.has_value()):
     *               connections_.push_back(std::move(*res));
     *               free_.push_back(&connections_.back());
     *             else:
     *               // Log the error but continue; partial pool is acceptable.
     *               // (spdlog::warn or similar)
     * Step 3 — If free_.empty() after the loop, drain waiters_ with nullptr.
     *
     * Preconditions:
     *   - connections_.capacity() >= cfg_.max_size (set in pool constructor).
     *   - Coroutine runs on the strand_.
     *
     * Postconditions:
     *   - free_ contains up to cfg_.max_size valid pointers into connections_.
     *
     * Pitfalls:
     *   - MUST NOT push_back on connections_ once raw pointers into it are stored
     *     in free_ (reallocation invalidates pointers). The reserve() in the
     *     constructor prevents this.
     *   - For parallel connects, use co_spawn + atomic counter. Sequential is simpler
     *     and acceptable for moderate pool sizes (≤ 20).
     *
     * Hint:
     *   connections_.reserve(cfg_.max_size) in the constructor guarantees that
     *   no reallocation occurs during this loop.
     */
    std::string connstr = apply_ssl_mode(cfg_.url, cfg_.ssl);
    for (std::size_t i = 0; i < cfg_.max_size; ++i) {
        auto res = co_await async_connection::connect(ex_, connstr);
        if (!res) {
            continue;
        }

        connections_.push_back(std::move(*res));
        release(&connections_.back());
    }

    if (connections_.empty()) {
        while (!waiters_.empty()) {
            auto handler = std::move(waiters_.front());
            waiters_.pop();
            handler(nullptr);
        }
    }
}

} // namespace atlas
