#include "atlas/async/pool.hpp"
#include "async/detail/acquire_waiter_queue.hpp"
#include "async/detail/pool_test_access.hpp"
#include "async/detail/reconnect_backoff.hpp"
#include "atlas/async/transaction.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace atlas {

namespace detail {

namespace {

// One suspended acquire(). Both the queue and coroutine retain shared ownership
// until either hand-off/failure or timeout removes the waiter.
struct acquire_waiter {
    explicit acquire_waiter(executor_type ex) : timer{std::move(ex)} {
    }

    asio::steady_timer timer;
    async_connection *conn = nullptr;
    std::optional<pg::error> failure;
    bool handled = false; // a connection (or a shutdown signal) was delivered
    bool expired = false; // the timeout won and erased this waiter
};

[[nodiscard]] auto validated_connstr(const pool_config &config) -> std::expected<std::string, pg::error> {
    if (config.reconnect_initial_delay.count() < 0 || config.reconnect_max_delay.count() < 0) {
        return std::unexpected(pg::error{"reconnect delays must be non-negative", pg::errc::invalid_argument});
    }
    if (config.reconnect_max_delay < config.reconnect_initial_delay) {
        return std::unexpected(
            pg::error{"reconnect maximum delay must not be smaller than initial delay", pg::errc::invalid_argument});
    }
    return apply_ssl_mode(config.url, config.ssl);
}

} // namespace

struct pool_state : std::enable_shared_from_this<pool_state> {
    pool_state(executor_type executor, pool_config config)
        : ex{std::move(executor)}, strand{ex}, cfg{std::move(config)}, connstr{validated_connstr(cfg)} {
        // Leases hand out raw pointers into `connections`; reserving up front
        // guarantees no reallocation can invalidate them later.
        connections.reserve(cfg.max_size);
        free.reserve(cfg.max_size);
    }

    executor_type ex;
    asio::strand<executor_type> strand;
    pool_config cfg;
    std::expected<std::string, pg::error> connstr;

    // Everything below is touched only from `strand`.
    std::vector<async_connection> connections;
    std::vector<async_connection *> free;
    waiter_queue<std::shared_ptr<acquire_waiter>> waiters;
    std::size_t unavailable = 0; // existing slots with one recovery loop
    std::vector<std::shared_ptr<asio::steady_timer>> recovery_timers;
    std::optional<pg::error> last_connect_error;
    bool shutting_down = false;
    bool initialised = false; // initialise() has run to completion
    bool initial_failure_pending = false;

    // Mirror of the two vectors above, for size() and available(). Those are
    // callable from any thread, and reading a vector's size while the strand
    // pushes to it is undefined behaviour rather than a benign race.
    std::atomic<std::size_t> live_count{0};
    std::atomic<std::size_t> free_count{0};

    [[nodiscard]] asio::awaitable<std::expected<async_connection *, pg::error>> acquire_slot();
    asio::awaitable<void> initialise();
    asio::awaitable<void> revive(async_connection *slot);
    asio::awaitable<void> recover_missing_slot();
    [[nodiscard]] asio::awaitable<std::expected<async_connection, pg::error>> connect_cycle();
    [[nodiscard]] asio::awaitable<bool> wait_before_reconnect(std::chrono::milliseconds delay);

    void hand_off(async_connection *conn);
    void fail_all_waiters(const pg::error &failure);
    void release(async_connection *conn);
    void shutdown();
    void publish_counts();
};

// Republishes the vector sizes for the lock-free observers. Strand only.
void pool_state::publish_counts() {
    assert(unavailable <= connections.size());
    const auto usable = unavailable <= connections.size() ? connections.size() - unavailable : 0;
    live_count.store(usable, std::memory_order_relaxed);
    free_count.store(free.size(), std::memory_order_relaxed);
}

// Takes the next connection, or suspends until one is released. Runs on the
// strand, so the free list and waiter queue need no further synchronisation.
asio::awaitable<std::expected<async_connection *, pg::error>> pool_state::acquire_slot() {
    if (!connstr) {
        co_return std::unexpected(connstr.error());
    }

    if (shutting_down) {
        co_return std::unexpected(pg::error{"pool has no usable connections", pg::errc::connection_failure});
    }

    if (!free.empty()) {
        auto *conn = free.back();
        free.pop_back();
        publish_counts();
        co_return conn;
    }

    // initialise() is spawned by the constructor and may finish before the
    // caller's first acquire reaches the strand. Preserve one notification in
    // that scheduling case; once observed, later callers wait for recovery.
    if (initial_failure_pending && last_connect_error) {
        initial_failure_pending = false;
        co_return std::unexpected(*last_connect_error);
    }

    auto waiter = std::make_shared<acquire_waiter>(executor_type{strand});
    waiter->timer.expires_after(cfg.timeout);
    const auto ticket = waiters.push(waiter);

    boost::system::error_code ec;
    co_await waiter->timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    // `handled` is authoritative, not `ec`: a hand-off can land in the same
    // strand tick the timer expires in, and cancelling an already-expired timer
    // is a no-op. Trusting `ec` there would drop the connection on the floor —
    // taken out of the free list, never returned.
    if (waiter->handled) {
        if (waiter->failure) {
            co_return std::unexpected(std::move(*waiter->failure));
        }
        co_return waiter->conn;
    }

    waiters.erase(ticket);
    waiter->expired = true;
    co_return std::unexpected(pg::error{"acquire timed out", pg::errc::query_canceled});
}

// Gives the connection to the first waiter, or parks it.
void pool_state::hand_off(async_connection *conn) {
    if (!waiters.empty()) {
        auto waiter = waiters.pop_front();
        waiter->conn = conn;
        waiter->handled = true;
        waiter->timer.cancel();
        return;
    }

    free.push_back(conn);
    publish_counts();
}

// Signals every current waiter with the supplied terminal failure. Recovery
// can continue afterwards; only callers queued during the failed initial pass
// receive that pass's connection error.
void pool_state::fail_all_waiters(const pg::error &failure) {
    while (!waiters.empty()) {
        auto waiter = waiters.pop_front();
        waiter->failure = failure;
        waiter->handled = true;
        waiter->timer.cancel();
    }
}

auto pool_test_access::waiter_count(const pool &db) -> asio::awaitable<std::size_t> {
    auto state = db.state_;
    co_return co_await asio::co_spawn(
        state->strand, [state]() -> asio::awaitable<std::size_t> { co_return state->waiters.size(); },
        asio::use_awaitable);
}

void pool_state::release(async_connection *conn) {
    if (conn == nullptr) {
        return;
    }

    // The lease may be destroyed on any thread, and the pool may go away
    // between this dispatch and the handler running — hence the strong
    // reference rather than a raw `this`.
    asio::dispatch(strand, [self = shared_from_this(), conn] {
        if (self->shutting_down) {
            return;
        }

        // Only a live, idle connection with no unread results is safe to hand
        // to another borrower.
        if (conn->is_reusable()) {
            self->hand_off(conn);
            return;
        }

        conn->invalidate();
        ++self->unavailable;
        self->publish_counts();
        asio::co_spawn(
            self->strand, [self, conn]() -> asio::awaitable<void> { co_await self->revive(conn); }, asio::detached);
    });
}

// Makes one bounded batch of connection attempts. Recovery loops call this
// repeatedly with a cancellable delay between batches.
asio::awaitable<std::expected<async_connection, pg::error>> pool_state::connect_cycle() {
    const std::size_t attempts = std::max<std::size_t>(cfg.max_retries, 1);
    pg::error last{"pool connection failed", pg::errc::connection_failure};

    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        if (shutting_down) {
            co_return std::unexpected(std::move(last));
        }

        auto fresh = co_await async_connection::connect(ex, *connstr, cfg.cleanup_budget);
        if (fresh) {
            co_return std::move(*fresh);
        }
        last = std::move(fresh.error());
    }

    co_return std::unexpected(std::move(last));
}

// Registers the exact timer shutdown() must cancel. Both insertion and removal
// run on the strand, so clearing the registry during shutdown cannot race a
// recovering coroutine.
asio::awaitable<bool> pool_state::wait_before_reconnect(std::chrono::milliseconds delay) {
    if (shutting_down) {
        co_return false;
    }

    auto timer = std::make_shared<asio::steady_timer>(executor_type{strand});
    timer->expires_after(delay);
    recovery_timers.push_back(timer);

    boost::system::error_code ec;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
    std::erase(recovery_timers, timer);

    // Cancellation is the shutdown signal. Any other timer error is terminal
    // for this loop as well, avoiding an unbounded retry spin.
    co_return !shutting_down && !ec;
}

// Reconnects a dead slot in place. Leases hand out pointers into `connections`,
// so the slot is reused rather than replaced — move assignment keeps the
// address stable for anyone still holding it.
asio::awaitable<void> pool_state::revive(async_connection *slot) {
    auto delay = cfg.reconnect_initial_delay;

    while (!shutting_down) {
        auto fresh = co_await connect_cycle();
        if (shutting_down) {
            co_return;
        }

        if (fresh) {
            *slot = std::move(*fresh);
            assert(unavailable > 0);
            if (unavailable > 0) {
                --unavailable;
            }
            publish_counts();
            hand_off(slot);
            co_return;
        }

        last_connect_error = std::move(fresh.error());
        if (!(co_await wait_before_reconnect(delay))) {
            co_return;
        }
        delay = detail::next_reconnect_delay(delay, cfg.reconnect_max_delay);
    }
}

// Recovers one slot that could not be created during the initial pass. Each
// failed initial slot starts exactly one instance of this coroutine.
asio::awaitable<void> pool_state::recover_missing_slot() {
    auto delay = cfg.reconnect_initial_delay;

    while (!shutting_down) {
        auto fresh = co_await connect_cycle();
        if (shutting_down) {
            co_return;
        }

        if (fresh) {
            // The constructor reserved max_size, and exactly one coroutine
            // exists per missing slot, so this push cannot reallocate or exceed
            // the configured capacity.
            assert(connections.size() < cfg.max_size);
            connections.push_back(std::move(*fresh));
            initial_failure_pending = false;
            publish_counts();
            hand_off(&connections.back());
            co_return;
        }

        last_connect_error = std::move(fresh.error());
        if (!(co_await wait_before_reconnect(delay))) {
            co_return;
        }
        delay = detail::next_reconnect_delay(delay, cfg.reconnect_max_delay);
    }
}

void pool_state::shutdown() {
    if (shutting_down) {
        return;
    }

    shutting_down = true;
    for (const auto &timer : recovery_timers) {
        static_cast<void>(timer->cancel());
    }
    recovery_timers.clear();
    fail_all_waiters(pg::error{"pool has no usable connections", pg::errc::connection_failure});
    free.clear();
    publish_counts();
    // `connections` is deliberately left alone: detached work may still hold a
    // reference to this state, and the connections die with it.
}

// Fills the pool sequentially. Failed slots are recorded and each receives one
// recovery coroutine after the initial pass has notified its waiters.
asio::awaitable<void> pool_state::initialise() {
    if (!connstr) {
        initialised = true;
        co_return;
    }

    std::size_t missing = 0;
    for (std::size_t i = 0; i < cfg.max_size; ++i) {
        if (shutting_down) {
            co_return;
        }

        auto res = co_await async_connection::connect(ex, *connstr, cfg.cleanup_budget);
        if (!res) {
            last_connect_error = std::move(res.error());
            ++missing;
            continue;
        }

        connections.push_back(std::move(*res));
        hand_off(&connections.back());
        publish_counts();
    }

    initialised = true;

    if (connections.empty() && last_connect_error) {
        initial_failure_pending = waiters.empty();
        fail_all_waiters(*last_connect_error);
    }

    for (std::size_t i = 0; i < missing; ++i) {
        asio::co_spawn(
            strand, [self = shared_from_this()]() -> asio::awaitable<void> { co_await self->recover_missing_slot(); },
            asio::detached);
    }
}

} // namespace detail

// ── pool_connection ────────────────────────────────────────────────────────

pool_connection::pool_connection(async_connection *conn, std::shared_ptr<detail::pool_state> owner) noexcept
    : conn_{conn}, owner_{std::move(owner)} {
}

pool_connection::pool_connection(pool_connection &&other) noexcept
    : conn_{std::exchange(other.conn_, nullptr)}, owner_{std::move(other.owner_)} {
}

pool_connection &pool_connection::operator=(pool_connection &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    // Return the connection currently held to its own pool before adopting the
    // new one; the owner may differ.
    if (conn_ != nullptr && owner_) {
        owner_->release(conn_);
    }

    conn_ = std::exchange(other.conn_, nullptr);
    owner_ = std::move(other.owner_);
    return *this;
}

pool_connection::~pool_connection() {
    if (conn_ != nullptr && owner_) {
        owner_->release(conn_);
    }
}

std::expected<void, pg::error> pool_connection::send_query(std::string_view sql, std::span<const char *const> params) {
    if (conn_ == nullptr) {
        return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    return conn_->send_query(sql, params);
}

asio::awaitable<std::expected<std::optional<pg::result>, pg::error>> pool_connection::receive() {
    if (conn_ == nullptr) {
        co_return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    co_return co_await conn_->receive();
}

asio::awaitable<std::expected<pg::result, pg::error>> pool_connection::execute(std::string_view sql,
                                                                               std::span<const char *const> params) {
    if (conn_ == nullptr) {
        co_return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    co_return co_await conn_->execute(sql, params);
}

pg_awaitable<void> pool_connection::request_cancel() {
    if (conn_ == nullptr) {
        co_return std::unexpected(pg::error{"pool_connection is in moved-from state", pg::errc::invalid_state});
    }

    co_return co_await conn_->request_cancel();
}

void pool_connection::invalidate() noexcept {
    if (conn_ != nullptr) {
        conn_->invalidate();
    }
}

std::chrono::milliseconds pool_connection::cleanup_budget() const noexcept {
    // A moved-from lease has no connection to clean up; the library default is
    // as good an answer as any and keeps the accessor total.
    return conn_ != nullptr ? conn_->cleanup_budget() : default_cleanup_budget;
}

bool pool_connection::is_alive() const noexcept {
    return conn_ != nullptr && conn_->is_alive();
}

bool pool_connection::transaction_aborted() const noexcept {
    return conn_ != nullptr && conn_->transaction_aborted();
}

// ── pool ──────────────────────────────────────────────────────────────────

pool::pool(executor_type ex, pool_config cfg)
    : state_{std::make_shared<detail::pool_state>(std::move(ex), std::move(cfg))} {
    // The coroutine holds its own reference to the state, so destroying the
    // pool mid-initialisation cannot pull the memory out from under it.
    asio::co_spawn(
        state_->strand, [state = state_]() -> asio::awaitable<void> { co_await state->initialise(); }, asio::detached);
}

pool::~pool() {
    // Dispatched rather than run inline so the state is only ever touched on the
    // strand. If the io_context has already stopped there is nobody left to
    // wake, and the state dies with the last outstanding reference.
    asio::dispatch(state_->strand, [state = state_] { state->shutdown(); });
}

asio::awaitable<std::expected<pool_connection, pg::error>> pool::acquire() {
    auto state = state_;

    // The bookkeeping runs as a child coroutine bound to the strand.
    // `co_await asio::dispatch(strand, use_awaitable)` would not be enough: it
    // resumes *this* coroutine on its own executor, leaving the free list and
    // waiter queue unsynchronised on a multi-threaded io_context.
    auto slot = co_await asio::co_spawn(state->strand, state->acquire_slot(), asio::use_awaitable);
    if (!slot) {
        co_return std::unexpected(slot.error());
    }

    co_return pool_connection{*slot, std::move(state)};
}

asio::awaitable<std::expected<transaction, pg::error>> pool::begin() {
    auto conn_res = co_await acquire();
    if (!conn_res) {
        co_return std::unexpected(conn_res.error());
    }

    auto exec_res = co_await conn_res->execute("BEGIN", std::span<const char *const>{});
    if (!exec_res) {
        // The lease destructor returns the connection to the pool.
        co_return std::unexpected(exec_res.error());
    }

    co_return transaction{std::move(*conn_res), state_->ex};
}

asio::awaitable<std::expected<pg::result, pg::error>> pool::execute(std::string_view sql,
                                                                    std::span<const char *const> params) {
    auto conn_res = co_await acquire();
    if (!conn_res) {
        co_return std::unexpected(conn_res.error());
    }

    co_return co_await conn_res->execute(sql, params);
}

// Read off the atomic mirrors rather than the vectors, which only the strand may
// touch. The values are a snapshot and may be stale by the time they are used.
std::size_t pool::size() const noexcept {
    return state_->live_count.load(std::memory_order_relaxed);
}

std::size_t pool::available() const noexcept {
    return state_->free_count.load(std::memory_order_relaxed);
}

} // namespace atlas
