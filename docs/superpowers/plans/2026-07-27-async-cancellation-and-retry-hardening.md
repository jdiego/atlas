# Async Cancellation and Retry Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Require libpq 18, cancel PostgreSQL queries without blocking the Asio executor, and make retry cleanup and backoff safe.

**Architecture:** `async_connection` will drive libpq 18's `PGcancelConn` state machine with an Asio descriptor and expose transaction-abort and invalidation state through `pool_connection`. `with_timeout` will either drain a successfully cancelled query or invalidate a connection that cannot be made reusable. Retry delay calculation will be a small saturating helper, and rollback will depend on `PQtransactionStatus`.

**Tech Stack:** C++23, Boost.Asio coroutines, libpq 18, CMake/pkg-config, Boost.UT, PostgreSQL 18.

## Global Constraints

- Require libpq 18 or newer at CMake configure time.
- Use PostgreSQL 18 for Ubuntu CI integration tests.
- Preserve `with_timeout`'s `query_canceled` result contract.
- Never return a connection to the pool unless its result stream is drained or it has been invalidated.
- Preserve all pre-existing uncommitted user changes; do not create implementation commits from overlapping dirty files.

---

### Task 1: Enforce the libpq 18 baseline

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/ubuntu.yml`

**Interfaces:**
- Consumes: pkg-config module `libpq`.
- Produces: configure-time requirement `libpq>=18` and PostgreSQL 18 CI coverage.

- [ ] **Step 1: Verify the current configuration accepts a simulated old libpq**

Run:

```bash
rg -n "pkg_check_modules\\(LIBPQ.*libpq>=18" CMakeLists.txt
```

Expected: no match, proving the version floor is not yet declared.

- [ ] **Step 2: Add the minimum version**

Replace the libpq lookup with:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET "libpq>=18")
```

- [ ] **Step 3: Upgrade Ubuntu CI**

Use `postgres:18` for the service. Install the PostgreSQL Apt repository before
`libpq-dev`, then assert the selected client version:

```yaml
- name: Install Dependencies
  run: |
    sudo apt-get update
    sudo apt-get install -y ca-certificates postgresql-common gcc-14 g++-14
    sudo /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y
    sudo apt-get install -y libboost-all-dev libpq-dev pkg-config
    pkg-config --atleast-version=18 libpq
```

- [ ] **Step 4: Verify local configuration**

Run:

```bash
cmake --preset dev
```

Expected: configuration succeeds and reports the locally installed libpq 18.

### Task 2: Add transaction-aware, overflow-safe retry behavior

**Files:**
- Modify: `include/atlas/async/async_connection.hpp`
- Modify: `include/atlas/async/pool.hpp`
- Modify: `include/atlas/async/retry.hpp`
- Modify: `src/async/async_connection.cpp`
- Modify: `src/async/pool.cpp`
- Modify: `test/async/test_async_connection.cpp`
- Modify: `test/async/test_retry.cpp`

**Interfaces:**
- Produces: `async_connection::transaction_aborted() const noexcept -> bool`.
- Produces: `pool_connection::transaction_aborted() const noexcept -> bool`.
- Produces: `detail::retry_delay(attempt, base_delay, max_delay) -> std::chrono::milliseconds`.

- [ ] **Step 1: Write the failing transaction-state integration test**

Add a test that connects, verifies `transaction_aborted()` is false, runs
`BEGIN`, raises SQLSTATE `40001`, verifies the method is true, runs `ROLLBACK`,
and verifies it is false again:

```cpp
auto begin = co_await conn->execute("BEGIN", no_params);
expect(begin.has_value());
expect(!conn->transaction_aborted());

auto failed = co_await conn->execute(
    "DO $$ BEGIN RAISE EXCEPTION 'retry' USING ERRCODE = '40001'; END $$",
    no_params);
expect(!failed.has_value());
expect(failed.error().code == errc::serialization_failure);
expect(conn->transaction_aborted());

auto rollback = co_await conn->execute("ROLLBACK", no_params);
expect(rollback.has_value());
expect(!conn->transaction_aborted());
```

- [ ] **Step 2: Build to verify RED**

Run:

```bash
cmake --build --preset dev -j4
```

Expected: compilation fails because `transaction_aborted()` does not exist.

- [ ] **Step 3: Implement transaction state**

Declare and forward:

```cpp
[[nodiscard]] bool transaction_aborted() const noexcept;
```

Implement on `async_connection`:

```cpp
return pg_conn_ != nullptr && PQtransactionStatus(pg_conn_) == PQTRANS_INERROR;
```

Implement on `pool_connection` by checking `conn_` and forwarding.

- [ ] **Step 4: Verify the transaction-state test passes**

Run the PostgreSQL-backed suite with `ATLAS_TEST_CONNINFO`.

Expected: the new integration test passes.

- [ ] **Step 5: Write the failing overflow test**

Add a unit test using the maximum `milliseconds::rep`:

```cpp
constexpr auto huge = std::chrono::milliseconds{
    std::numeric_limits<std::chrono::milliseconds::rep>::max() / 2 + 1};
constexpr auto cap = std::chrono::milliseconds{500};
expect(atlas::detail::retry_delay(8, huge, cap) == cap);
expect(atlas::detail::retry_delay(8, -1ms, cap) == 0ms);
```

- [ ] **Step 6: Build to verify RED**

Expected: compilation fails because `detail::retry_delay` does not exist.

- [ ] **Step 7: Implement saturating delay calculation**

Add:

```cpp
[[nodiscard]] constexpr auto retry_delay(
    std::size_t attempt,
    std::chrono::milliseconds base_delay,
    std::chrono::milliseconds max_delay) noexcept
    -> std::chrono::milliseconds {
    if (base_delay.count() <= 0 || max_delay.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    if (base_delay >= max_delay) {
        return max_delay;
    }
    if (attempt >= std::numeric_limits<unsigned long long>::digits) {
        return max_delay;
    }
    const auto factor = 1ULL << attempt;
    const auto base = static_cast<unsigned long long>(base_delay.count());
    const auto cap = static_cast<unsigned long long>(max_delay.count());
    if (base > cap / factor) {
        return max_delay;
    }
    return std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(base * factor)};
}
```

Use it from `with_retry()` and execute `ROLLBACK` only when
`transaction_aborted()` is true.

- [ ] **Step 8: Verify retry tests and clean output**

Run the test binary directly with `ATLAS_TEST_CONNINFO`.

Expected: retry suites pass and stderr contains no
`there is no transaction in progress` warnings.

### Task 3: Implement native asynchronous cancellation

**Files:**
- Modify: `include/atlas/async/async_connection.hpp`
- Modify: `include/atlas/async/pool.hpp`
- Modify: `include/atlas/async/timeout.hpp`
- Modify: `src/async/async_connection.cpp`
- Modify: `src/async/pool.cpp`
- Modify: `test/async/test_timeout.cpp`
- Modify: `test/async/test_async_connection.cpp`

**Interfaces:**
- Changes: `request_cancel() -> pg_awaitable<void>` on both connection types.
- Produces: `invalidate() noexcept -> void` on both connection types.
- Changes: `detail::drain_connection(Connection&) -> asio::awaitable<bool>`.

- [ ] **Step 1: Write a failing real cancellation regression**

First assert the desired public contract:

```cpp
static_assert(std::same_as<
              decltype(std::declval<atlas::async_connection &>().request_cancel()),
              atlas::pg_awaitable<void>>);
```

Then, on one live `async_connection`, run:

```cpp
const auto started = std::chrono::steady_clock::now();
auto timed_out = co_await atlas::with_timeout<atlas::pg::result>(
    50ms, *conn, conn->execute("SELECT pg_sleep(10)", no_params));
const auto elapsed = std::chrono::steady_clock::now() - started;

expect(!timed_out.has_value());
expect(timed_out.error().code == errc::query_canceled);
expect(elapsed < 2s);

auto reused = co_await conn->execute("SELECT 1", no_params);
expect(reused.has_value());
```

- [ ] **Step 2: Run to verify RED**

Expected: compilation fails because the current `request_cancel()` returns
`pg_expected<void>` instead of `pg_awaitable<void>`.

- [ ] **Step 3: Update the cancellation concept and test double**

Require:

```cpp
{ conn.request_cancel() }
    -> std::same_as<asio::awaitable<std::expected<void, pg::error>>>;
{ conn.invalidate() } -> std::same_as<void>;
```

Make the recording test double return its cancellation result from an
`asio::awaitable`.

- [ ] **Step 4: Implement the libpq 18 polling state machine**

Create a `PGcancelConn` with `PQcancelCreate`, own it with an RAII deleter that
calls `PQcancelFinish`, and call `PQcancelStart`. Treat the initial desired
readiness as write. For every iteration:

```cpp
const int socket = PQcancelSocket(cancel.get());
// Synchronize the Asio descriptor with this socket.
co_await descriptor.async_wait(wait_type, asio::as_tuple(asio::use_awaitable));
const auto status = PQcancelPoll(cancel.get());
```

Return success on `PGRES_POLLING_OK`, map `PGRES_POLLING_FAILED` through
`PQcancelErrorMessage`, and re-read `PQcancelSocket()` after every poll.
Detach the descriptor before `PQcancelFinish`.

- [ ] **Step 5: Add invalidation and safe timeout cleanup**

`async_connection::invalidate()` calls `cleanup()`. `pool_connection` forwards
it. Change `drain_connection` to return `true` only on the end-of-stream
sentinel.

In `with_timeout`:

```cpp
auto cancelled = co_await conn.request_cancel();
if (!cancelled) {
    conn.invalidate();
} else if (!co_await detail::drain_connection(conn)) {
    conn.invalidate();
}
```

- [ ] **Step 6: Verify cancellation tests pass**

Run the PostgreSQL-backed async timeout and connection suites.

Expected: `pg_sleep(10)` is cancelled in under two seconds and the same
connection successfully executes `SELECT 1`.

### Task 4: Full verification

**Files:**
- Verify all modified source, test, CMake, and workflow files.

**Interfaces:**
- Consumes: all previous tasks.
- Produces: merge-ready evidence without changing behavior.

- [ ] **Step 1: Run formatting/diff checks**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 2: Build normal, warning-clean, and header targets**

Run the `dev` and `warnings-as-errors` builds plus
`atlas_verify_interface_header_sets`.

Expected: every target succeeds.

- [ ] **Step 3: Run PostgreSQL-backed tests**

Start an isolated PostgreSQL 18 instance in `/tmp`, run `ctest --preset dev
--output-on-failure`, and execute the test binary directly to inspect stderr.

Expected: all suites pass and no rollback warnings are emitted.

- [ ] **Step 4: Run sanitizers**

Build and run ASan/UBSan and TSan against the same PostgreSQL instance.

Expected: all tests pass with no sanitizer findings or data-race reports.

- [ ] **Step 5: Inspect final scope**

Run:

```bash
git status --short
git diff --stat
git diff --check
```

Expected: only the approved branch files and plan are changed; no temporary or
generated files are tracked.
