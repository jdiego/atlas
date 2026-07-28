# Pool Hygiene and Waiters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recirculate only clean PostgreSQL sessions and ensure timed-out acquires leave no stale queue entries.

**Architecture:** `async_connection` will expose one internal reuse predicate combining transport, query-stream, and transaction state. The pool's waiter storage will move from deferred callbacks in `std::queue` to explicit waiter objects in a strand-owned `std::list`, allowing timeout to erase its own node in constant time.

**Tech Stack:** C++23, Boost.Asio coroutines/timers/strand, libpq 18, Boost.UT, PostgreSQL 18.

## Global Constraints

- Reuse requires `CONNECTION_OK`, no transport failure, no active result stream, and `PQTRANS_IDLE`.
- Replace rather than clean caller-owned transaction or protocol state.
- Preserve FIFO ordering among live waiters.
- A timer-versus-hand-off boundary must neither lose nor duplicate a connection.
- Write and observe each regression test failing before production changes.

---

### Task 1: Reject live but dirty sessions at check-in

**Files:**
- Modify: `include/atlas/async/async_connection.hpp`
- Modify: `src/async/async_connection.cpp`
- Modify: `src/async/pool.cpp`
- Modify: `test/async/test_pool.cpp`

**Interfaces:**
- Produces: `async_connection::is_reusable() const noexcept -> bool`.
- Changes: `pool_state::release()` hands off only reusable connections.

- [ ] **Step 1: Write failing open-transaction and aborted-transaction tests**

For each case, capture the first backend PID, drop the lease without cleanup,
then acquire again and assert both a working query and a different PID:

```cpp
auto acquired = co_await db.acquire();
expect(acquired.has_value() >> fatal);
std::optional<atlas::pool_connection> first{std::move(*acquired)};
auto pid_result = co_await first->execute(
    "SELECT pg_backend_pid()", no_params);
expect(pid_result.has_value() >> fatal);
auto pid_field = pid_result->get(0, 0);
expect(pid_field.has_value() >> fatal);
expect(pid_field->has_value() >> fatal);
const std::string first_pid{pid_field->value()};
expect((co_await first->execute("BEGIN", no_params)).has_value());
first.reset();

auto second = co_await db.acquire();
expect(second.has_value() >> fatal);
auto second_pid_result =
    co_await second->execute("SELECT pg_backend_pid()", no_params);
expect(second_pid_result.has_value() >> fatal);
auto second_pid_field = second_pid_result->get(0, 0);
expect(second_pid_field.has_value() >> fatal);
expect(second_pid_field->has_value() >> fatal);
expect(std::string{second_pid_field->value()} != first_pid);
```

For the aborted variant, issue `SELECT 1 / 0` after `BEGIN` before dropping the
lease.

- [ ] **Step 2: Run to verify RED**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: the pool returns the same live backend, leaking `INTRANS` or
`INERROR` state.

- [ ] **Step 3: Write the unfinished-result regression**

Send a multi-result query, consume only the first result, drop the lease, then
assert the next borrower receives a replacement and `SELECT 1` succeeds:

```cpp
auto acquired = co_await db.acquire();
expect(acquired.has_value() >> fatal);
std::optional<atlas::pool_connection> first{std::move(*acquired)};
expect(first->send_query("SELECT 1; SELECT 2", no_params).has_value());
auto one = co_await first->receive();
expect(one.has_value());
first.reset();

auto second = co_await db.acquire();
expect(second.has_value() >> fatal);
expect((co_await second->execute("SELECT 1", no_params)).has_value());
```

- [ ] **Step 4: Run to verify RED**

Expected: the current `is_alive()` check recirculates the connection with
`query_in_progress_ == true`.

- [ ] **Step 5: Implement the reuse predicate**

Declare and implement:

```cpp
bool async_connection::is_reusable() const noexcept {
    return pg_conn_ != nullptr && !transport_failed_ &&
           !query_in_progress_ && PQstatus(pg_conn_) == CONNECTION_OK &&
           PQtransactionStatus(pg_conn_) == PQTRANS_IDLE;
}
```

Change release:

```cpp
if (conn->is_reusable()) {
    self->hand_off(conn);
    return;
}
conn->invalidate();
asio::co_spawn(
    self->strand,
    [self, conn]() -> asio::awaitable<void> {
        co_await self->revive(conn);
    },
    asio::detached);
```

- [ ] **Step 6: Verify GREEN**

Run the full CTest command and expect all three new regressions to pass.

- [ ] **Step 7: Commit**

```bash
git add include/atlas/async/async_connection.hpp src/async/async_connection.cpp src/async/pool.cpp test/async/test_pool.cpp
git commit -m "fix: recirculate only clean pool sessions"
```

### Task 2: Make waiter eviction immediate and deterministic

**Files:**
- Create: `src/async/detail/acquire_waiter_queue.hpp`
- Create: `test/async/test_acquire_waiter_queue.cpp`
- Modify: `src/async/pool.cpp`

**Interfaces:**
- Produces: `detail::waiter_queue<T>`.
- Produces: `push(waiter) -> ticket`, `pop_front()`, `erase(ticket)`, `empty()`,
  and `size()`.
- Changes: an acquire timeout erases its ticket before returning.

- [ ] **Step 1: Write the failing focused queue test**

Create a unit test that includes the internal header and asserts arbitrary
erasure plus FIFO:

```cpp
atlas::detail::waiter_queue<int> queue;
std::vector<atlas::detail::waiter_queue<int>::ticket> tickets;
for (int i = 0; i < 10'000; ++i) {
    tickets.push_back(queue.push(i));
}
for (std::size_t i = 1; i < tickets.size(); i += 2) {
    queue.erase(tickets[i]);
}
expect(queue.size() == 5'000_ul);
for (int expected = 0; expected < 10'000; expected += 2) {
    expect(queue.pop_front() == expected);
}
expect(queue.empty());
```

- [ ] **Step 2: Build to verify RED**

Run:

```bash
cmake --build --preset dev -j4
```

Expected: compilation fails because the queue type does not exist.

- [ ] **Step 3: Implement the focused container**

Use a stable list iterator as the ticket:

```cpp
template <typename T>
class waiter_queue {
public:
    using value_type = T;
    using storage_type = std::list<value_type>;
    using ticket = storage_type::iterator;

    auto push(value_type waiter) -> ticket {
        return waiters_.insert(waiters_.end(), std::move(waiter));
    }
    void erase(ticket position) { waiters_.erase(position); }
    auto pop_front() -> value_type {
        auto waiter = std::move(waiters_.front());
        waiters_.pop_front();
        return waiter;
    }
    [[nodiscard]] bool empty() const noexcept { return waiters_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return waiters_.size(); }

private:
    storage_type waiters_;
};
```

Production instantiates
`detail::waiter_queue<std::shared_ptr<acquire_waiter>>`; the focused unit test
uses `detail::waiter_queue<int>` and therefore needs no Asio timer fixture.

- [ ] **Step 4: Integrate explicit waiter objects**

Replace callback storage with:

```cpp
struct acquire_waiter {
    explicit acquire_waiter(executor_type ex) : timer{std::move(ex)} {}
    asio::steady_timer timer;
    async_connection *conn = nullptr;
    std::optional<pg::error> failure;
    bool handled = false;
    bool expired = false;
};
```

After pushing, retain the ticket in the acquiring coroutine. On timeout:

```cpp
if (waiter->handled) {
    if (waiter->failure) {
        co_return std::unexpected(std::move(*waiter->failure));
    }
    co_return waiter->conn;
}
waiters.erase(ticket);
waiter->expired = true;
co_return std::unexpected(
    pg::error{"acquire timed out", pg::errc::query_canceled});
```

`hand_off()` pops before setting `handled` and cancelling the timer. Shutdown
and terminal failure pop every waiter, set `failure`, set `handled`, and cancel.

- [ ] **Step 5: Add the timer-versus-hand-off boundary regression**

Repeat a near-deadline release 100 times. Either the queued acquire may win or
its timeout may win, but after both coroutines settle the sole connection must
always return to the pool and remain usable:

```cpp
asio::io_context ctx;
atlas::pool db{
    ctx.get_executor(), config_for(*url, 1, 10ms)};

const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto acquired = co_await db.acquire();
        expect(acquired.has_value() >> fatal);
        std::optional<atlas::pool_connection> held{std::move(*acquired)};

        auto release = [&held]() -> asio::awaitable<void> {
            co_await sleep_for(10ms);
            held.reset();
        };
        asio::co_spawn(
            co_await asio::this_coro::executor, release(), asio::detached);

        {
            auto boundary = co_await db.acquire();
            if (!boundary) {
                expect(boundary.error().code == errc::query_canceled);
            }
        }
        expect(co_await atlas_test::wait_until(
            [&db] { return db.available() == 1; }, 1s));
    }
    expect((co_await db.execute("SELECT 1", no_params)).has_value());
    co_return true;
}());
expect(ran);
```

The nested scope destroys a successful boundary lease before `wait_until()`.
This keeps both race outcomes acceptable while checking that neither loses the
slot.

- [ ] **Step 6: Verify timer-versus-hand-off behavior**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: queue unit tests, existing acquire timeout tests, FIFO hand-off tests,
and the full suite pass.

- [ ] **Step 7: Commit**

```bash
git add src/async/detail/acquire_waiter_queue.hpp test/async/test_acquire_waiter_queue.cpp src/async/pool.cpp
git commit -m "fix: evict expired pool waiters"
```
