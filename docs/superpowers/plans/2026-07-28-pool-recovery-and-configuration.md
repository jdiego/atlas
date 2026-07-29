# Pool Recovery and Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate connection strings through libpq and restore unavailable pool capacity after transient outages.

**Architecture:** The pool will parse and store its effective connection string once. Initial failures and dirty returned slots will each own one recovery coroutine that retries in bounded exponential cycles until success or shutdown; unavailable slots reduce `size()` temporarily but are never permanently retired.

**Tech Stack:** C++23, Boost.Asio coroutines/timers/strand, libpq 18 `PQconninfoParse`, Boost.UT, PostgreSQL 18.

## Global Constraints

- `apply_ssl_mode()` returns `std::expected<std::string, pg::error>`.
- Explicit `sslmode` always wins over `pool_config::ssl`.
- Embedded null bytes and malformed conninfo return `errc::invalid_argument`.
- Defaults are `reconnect_initial_delay = 100ms` and `reconnect_max_delay = 5s`.
- Recovery retries until success, shutdown, or terminal configuration error.
- `pool::size()` counts usable connections; `available()` counts free connections.
- Write and observe each regression test failing before production changes.

---

### Task 1: Make connection-string handling libpq-authoritative

**Files:**
- Modify: `include/atlas/async/pool_config.hpp`
- Modify: `src/async/pool_config.cpp`
- Modify: `src/async/pool.cpp`
- Modify: `test/async/test_pool.cpp`

**Interfaces:**
- Changes: `apply_ssl_mode(std::string, ssl_mode) -> std::expected<std::string, pg::error>`.
- Produces: one validated effective conninfo stored by `pool_state`.

- [ ] **Step 1: Write failing parsing tests**

Add unit cases:

```cpp
auto keyword = atlas::apply_ssl_mode(
    "host=localhost application_name='sslmode=require'",
    atlas::ssl_mode::disable);
expect(keyword.has_value() >> fatal);
expect(keyword->find("sslmode=disable") != std::string::npos);

auto explicit_uri = atlas::apply_ssl_mode(
    "postgresql://localhost/atlas?sslmode=require",
    atlas::ssl_mode::disable);
expect(explicit_uri.has_value() >> fatal);
expect(*explicit_uri ==
       "postgresql://localhost/atlas?sslmode=require");

using namespace std::string_literals;
auto nul = atlas::apply_ssl_mode(
    std::string{"host=local\0host", 15},
    atlas::ssl_mode::prefer);
expect(!nul.has_value());
expect(nul.error().code == errc::invalid_argument);

auto malformed = atlas::apply_ssl_mode(
    "host='unterminated", atlas::ssl_mode::prefer);
expect(!malformed.has_value());
expect(malformed.error().code == errc::invalid_argument);
```

- [ ] **Step 2: Build to verify RED**

Run:

```bash
cmake --build --preset dev -j4
```

Expected: compilation fails because the function still returns `std::string`;
after adapting the calls, the false-positive `application_name` case fails.

- [ ] **Step 3: Implement parsing with RAII**

Include `<libpq-fe.h>` and define:

```cpp
struct conninfo_deleter {
    void operator()(PQconninfoOption *value) const noexcept {
        if (value != nullptr) PQconninfoFree(value);
    }
};
struct pq_memory_deleter {
    void operator()(char *value) const noexcept {
        if (value != nullptr) PQfreemem(value);
    }
};
```

Reject `'\0'`, then parse:

```cpp
char *raw_error = nullptr;
std::unique_ptr<char, pq_memory_deleter> parse_error;
std::unique_ptr<PQconninfoOption, conninfo_deleter> options{
    PQconninfoParse(url.c_str(), &raw_error)};
parse_error.reset(raw_error);
if (!options) {
    const std::string message =
        parse_error ? parse_error.get() : "could not parse connection string";
    return std::unexpected(
        pg::error{message, pg::errc::invalid_argument});
}
for (auto *option = options.get(); option->keyword != nullptr; ++option) {
    if (std::string_view{option->keyword} == "sslmode" &&
        option->val != nullptr) {
        return url;
    }
}
```

Append the configured mode using the existing URI-versus-keyword formatting and
return the resulting `expected`.

- [ ] **Step 4: Parse once in pool state**

Add:

```cpp
std::expected<std::string, pg::error> connstr;
```

Initialize it from `apply_ssl_mode(cfg.url, cfg.ssl)`. `acquire_slot()` returns
`connstr.error()` promptly when invalid; `initialise()` and recovery use
`*connstr` and never parse again. Adapt all existing callers and tests to the
expected return type.

- [ ] **Step 5: Verify GREEN**

Run:

```bash
cmake --build --preset dev -j4
ctest --preset dev --output-on-failure
```

Expected: all parsing tests and the full suite pass.

- [ ] **Step 6: Commit**

```bash
git add include/atlas/async/pool_config.hpp src/async/pool_config.cpp src/async/pool.cpp test/async/test_pool.cpp
git commit -m "fix: validate pool connection strings"
```

### Task 2: Define and test bounded recovery backoff

**Files:**
- Create: `src/async/detail/reconnect_backoff.hpp`
- Create: `test/async/test_reconnect_backoff.cpp`
- Modify: `include/atlas/async/pool_config.hpp`
- Modify: `src/async/pool.cpp`

**Interfaces:**
- Produces: `pool_config::reconnect_initial_delay`.
- Produces: `pool_config::reconnect_max_delay`.
- Produces: `detail::next_reconnect_delay(current, maximum)`.
- Produces: terminal validation for negative or inverted delay ranges.

- [ ] **Step 1: Write failing backoff tests**

```cpp
expect(atlas::detail::next_reconnect_delay(100ms, 5s) == 200ms);
expect(atlas::detail::next_reconnect_delay(4s, 5s) == 5s);
expect(atlas::detail::next_reconnect_delay(5s, 5s) == 5s);
expect(atlas::detail::next_reconnect_delay(0ms, 5s) == 0ms);
expect(atlas::detail::next_reconnect_delay(
           std::chrono::milliseconds::max(), 5s) == 5s);
```

Add pool tests asserting negative initial delay and maximum smaller than initial
return `invalid_argument` from `acquire()`.

- [ ] **Step 2: Build/run to verify RED**

Run:

```bash
cmake --build --preset dev -j4
ctest --preset dev --output-on-failure
```

Expected: compilation fails because the fields and helper do not exist.

- [ ] **Step 3: Add configuration defaults and saturating helper**

Add:

```cpp
std::chrono::milliseconds reconnect_initial_delay{100};
std::chrono::milliseconds reconnect_max_delay{5000};
```

Implement without multiplication overflow:

```cpp
constexpr auto next_reconnect_delay(
    std::chrono::milliseconds current,
    std::chrono::milliseconds maximum) noexcept
    -> std::chrono::milliseconds {
    if (current.count() <= 0 || maximum.count() <= 0) {
        return 0ms;
    }
    if (current >= maximum || current > maximum - current) {
        return maximum;
    }
    return current + current;
}
```

Before initialisation, store `invalid_argument` when either delay is negative or
the maximum is smaller than the initial delay.

- [ ] **Step 4: Verify GREEN and commit**

Run:

```bash
cmake --build --preset dev -j4
ctest --preset dev --output-on-failure
```

Expected: helper and validation tests pass.

Commit:

```bash
git add include/atlas/async/pool_config.hpp src/async/detail/reconnect_backoff.hpp test/async/test_reconnect_backoff.cpp src/async/pool.cpp
git commit -m "feat: configure bounded pool recovery"
```

### Task 3: Recover unavailable capacity until success or shutdown

**Files:**
- Modify: `src/async/pool.cpp`
- Modify: `test/async/test_pool.cpp`

**Interfaces:**
- Consumes: validated `pool_state::connstr`.
- Consumes: `detail::next_reconnect_delay`.
- Changes: replaces permanent `retired` slots with unavailable/recovering slots.
- Produces: one recovery loop per failed existing or missing slot.

- [ ] **Step 1: Write the failing transient-initialisation test**

Create a unique database name from the process ID. Add a test helper that
replaces a URI path or appends a final keyword value:

```cpp
auto with_database(std::string conninfo, std::string_view database)
    -> std::string {
    if (conninfo.starts_with("postgresql://") ||
        conninfo.starts_with("postgres://")) {
        const auto scheme = conninfo.find("://") + 3;
        const auto query = conninfo.find('?', scheme);
        const auto slash = conninfo.find('/', scheme);
        const auto end = query == std::string::npos ? conninfo.size() : query;
        if (slash == std::string::npos || slash > end) {
            conninfo.insert(end, "/" + std::string{database});
        } else {
            conninfo.replace(
                slash + 1, end - slash - 1, std::string{database});
        }
        return conninfo;
    }
    conninfo.append(" dbname=").append(database);
    return conninfo;
}
```

Use an admin connection to ensure the safe identifier
`atlas_recovery_<process-id>` is absent, build a pool whose conninfo selects
that database, observe the first `connection_failure`, create the database
through the admin connection, and wait for the existing pool:

```cpp
auto first = co_await recovering_pool.acquire();
expect(!first.has_value());
expect(first.error().code == errc::connection_failure);

expect(admin.exec("CREATE DATABASE " + quoted_name).has_value());
const bool recovered = co_await atlas_test::wait_until(
    [&recovering_pool] {
        return recovering_pool.size() == 1 &&
               recovering_pool.available() == 1;
    },
    10s,
    20ms);
expect(recovered);
expect((co_await recovering_pool.execute("SELECT 1", no_params)).has_value());
```

Drop the database during test cleanup after all pool leases and the pool are
destroyed.

- [ ] **Step 2: Run to verify RED**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: the pool remains at size zero because initial failures are skipped
permanently.

- [ ] **Step 3: Write shutdown-during-backoff regression**

Configure an unreachable pool with a 5-second reconnect delay, start one
acquire, destroy the pool after its first failure, and assert the acquire is
woken with `connection_failure` without waiting for the delay. This verifies
the recovery timer is cancellable from `shutdown()`.

- [ ] **Step 4: Implement recovery cycles**

Replace `retired` with:

```cpp
std::size_t unavailable = 0;
std::vector<std::shared_ptr<asio::steady_timer>> recovery_timers;
std::optional<pg::error> last_connect_error;
```

Publish usable size:

```cpp
live_count.store(
    connections.size() - unavailable,
    std::memory_order_relaxed);
```

Factor one bounded attempt cycle:

```cpp
asio::awaitable<std::expected<async_connection, pg::error>>
connect_cycle() {
    const std::size_t attempts = std::max<std::size_t>(cfg.max_retries, 1);
    pg::error last{"pool connection failed", pg::errc::connection_failure};
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        if (shutting_down) {
            co_return std::unexpected(last);
        }
        auto fresh =
            co_await async_connection::connect(ex, *connstr, cfg.cleanup_budget);
        if (fresh) co_return std::move(*fresh);
        last = std::move(fresh.error());
    }
    co_return std::unexpected(std::move(last));
}
```

Both `revive(slot)` and `recover_missing_slot()` loop over `connect_cycle()`.
After failure they retain `last_connect_error`, register a shared timer, wait
with `asio::redirect_error(asio::use_awaitable, ec)`, remove that exact timer
from the registry, and advance the saturating delay. Cancellation or
`shutting_down` exits the loop. On success, revive replaces the existing object
and decrements `unavailable`; missing recovery pushes a new connection into the
pre-reserved vector. Both publish counts and hand off the connection.

The initial pass starts exactly one missing recovery coroutine per failed slot,
sets `initialised`, and when zero connections opened calls
`fail_all_waiters(*last_connect_error)`. Later acquires queue normally and can
be satisfied by background recovery.

`shutdown()` cancels every registered timer, clears the timer registry, wakes
waiters, and makes every loop exit before its next attempt.

- [ ] **Step 5: Verify recovery and the full runtime**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: initial-outage recovery, existing dead-slot recovery,
shutdown-during-backoff, and all prior tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/async/pool.cpp test/async/test_pool.cpp
git commit -m "fix: recover pool capacity after outages"
```

### Task 4: Run complete branch verification

**Files:**
- Modify only if verification exposes a scoped defect.

**Interfaces:**
- Verifies all three runtime implementation plans together.

- [ ] **Step 1: Development and package contracts**

```bash
cmake --preset dev
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Warnings as errors**

```bash
cmake --preset warnings-as-errors
cmake --build --preset warnings-as-errors -j4
```

Expected: zero warnings promoted to errors.

- [ ] **Step 3: Sanitizers**

```bash
cmake --preset asan
cmake --build --preset asan -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --test-dir build/tsan --output-on-failure
```

Expected: all tests pass with no sanitizer report. If a scoped defect appears,
return to the relevant task's RED step, add the regression there, and repeat
that task's full RED-GREEN verification before committing the named files from
that task.
