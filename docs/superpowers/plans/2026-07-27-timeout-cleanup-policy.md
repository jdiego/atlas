# Timeout Cleanup Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep `cancellable_connection` minimal while resolving the cleanup budget from a per-call override, an optional connection provider, or the five-second library default.

**Architecture:** Cancellation capability and cleanup configuration remain separate structural contracts. `with_timeout()` resolves the effective budget lazily only after an operation times out, then passes the selected value to the existing bounded cancel-and-drain routine. PostgreSQL-backed coverage proves that a non-positive budget invalidates a pooled connection and that the pool revives the slot.

**Tech Stack:** C++23, Boost.Asio awaitables and cancellation, libpq 18, Boost.UT, CMake/CTest.

## Global Constraints

- Require libpq 18 or newer.
- Preserve `with_timeout()`'s public `query_canceled` result contract.
- Preserve all existing uncommitted work; edit only the files named by each task.
- Keep `cancellable_connection` limited to `request_cancel()`, `receive()`, and `invalidate()`.
- Resolve budgets in this order: per-call override, optional `conn.cleanup_budget()`, then `default_cleanup_budget`.
- Do not evaluate `conn.cleanup_budget()` when a per-call override exists.
- Keep the default cleanup budget at exactly five seconds.
- Treat zero and negative cleanup budgets as no cleanup grace period; invalidate the connection after the operation timeout.
- Do not detach cleanup work or let it outlive the connection.

---

### Task 1: Decouple cancellation capability from cleanup configuration

**Files:**
- Modify: `test/async/test_timeout.cpp:31-233`
- Modify: `include/atlas/async/timeout.hpp:3-189`

**Interfaces:**
- Consumes: `atlas::default_cleanup_budget` from `atlas/async/pool_config.hpp`.
- Produces: `atlas::cleanup_budget_provider<Connection>`.
- Produces: `atlas::detail::resolve_cleanup_budget(const Connection&, std::optional<std::chrono::milliseconds>) -> std::chrono::milliseconds`.
- Preserves: `atlas::with_timeout(Duration, Connection&, awaitable, optional<milliseconds>)`.

- [ ] **Step 1: Add a cancellable test adapter without a budget accessor**

Add this adapter beside `recording_connection` in `test/async/test_timeout.cpp`:

```cpp
struct minimal_connection {
    int receives = 0;
    bool invalidated = false;

    [[nodiscard]] boost::asio::awaitable<std::expected<void, atlas::pg::error>> request_cancel() {
        co_return std::expected<void, atlas::pg::error>{};
    }

    [[nodiscard]] boost::asio::awaitable<std::expected<std::optional<atlas::pg::result>, atlas::pg::error>> receive() {
        ++receives;
        if (receives == 1) {
            co_return std::unexpected(
                atlas::pg::error{"canceling statement due to user request", "57014", errc::query_canceled});
        }
        co_return std::optional<atlas::pg::result>{};
    }

    void invalidate() noexcept {
        invalidated = true;
    }
};

static_assert(atlas::cancellable_connection<minimal_connection>);
static_assert(!atlas::cleanup_budget_provider<minimal_connection>);
```

Add this case to the `async/timeout` suite:

```cpp
"a cancellable adapter does not need a cleanup budget accessor"_test = [] {
    minimal_connection conn;

    auto result = run(atlas::with_timeout<int>(20ms, conn, slow_operation(2s, 7)));

    expect(!result.has_value());
    expect(result.error().code == errc::query_canceled);
    expect(conn.receives == 2_i);
    expect(!conn.invalidated);
};
```

- [ ] **Step 2: Make budget-provider evaluation observable**

Extend `recording_connection`:

```cpp
mutable int budget_reads = 0;

[[nodiscard]] std::chrono::milliseconds cleanup_budget() const noexcept {
    ++budget_reads;
    return budget;
}
```

Extend the existing provider-default test with:

```cpp
expect(conn.budget_reads == 1_i) << "the connection budget was not read exactly once";
```

Extend the existing per-call override test with:

```cpp
expect(conn.budget_reads == 0_i) << "the override should bypass the connection budget accessor";
```

- [ ] **Step 3: Build to verify the new contract fails**

Run:

```bash
cmake --build build/dev --target atlas_test_suite -j4
```

Expected: compilation fails because `cleanup_budget_provider` does not exist yet
and the current `cancellable_connection` rejects `minimal_connection` for not
having `cleanup_budget()`.

- [ ] **Step 4: Restore the minimal concept and add the optional provider**

Include the default policy explicitly in `include/atlas/async/timeout.hpp`:

```cpp
#include "atlas/async/pool_config.hpp"
```

Replace the concepts with:

```cpp
template <typename Connection>
concept cancellable_connection = requires(Connection &conn) {
    { conn.request_cancel() } -> std::same_as<asio::awaitable<std::expected<void, pg::error>>>;
    { conn.receive() } -> std::same_as<asio::awaitable<std::expected<std::optional<pg::result>, pg::error>>>;
    { conn.invalidate() } -> std::same_as<void>;
};

template <typename Connection>
concept cleanup_budget_provider = requires(const Connection &conn) {
    { conn.cleanup_budget() } -> std::same_as<std::chrono::milliseconds>;
};
```

- [ ] **Step 5: Build to expose the eager provider expression**

Run:

```bash
cmake --build build/dev --target atlas_test_suite -j4
build/dev/bin/atlas_test_suite
```

Expected: compilation fails while instantiating `with_timeout()` for
`minimal_connection`, because
`std::optional::value_or(conn.cleanup_budget())` still forms and evaluates the
provider expression even when an override may be present. The read-count
assertion remains the GREEN regression proving that an explicit override does
not invoke a provider that does exist.

- [ ] **Step 6: Resolve the effective budget lazily**

Add this helper inside `atlas::detail`:

```cpp
template <typename Connection>
[[nodiscard]] auto resolve_cleanup_budget(const Connection &conn,
                                          std::optional<std::chrono::milliseconds> override_budget)
    -> std::chrono::milliseconds {
    if (override_budget) {
        return *override_budget;
    }
    if constexpr (cleanup_budget_provider<Connection>) {
        return conn.cleanup_budget();
    }
    return default_cleanup_budget;
}
```

Replace the eager expression in `with_timeout()`:

```cpp
const auto effective_cleanup_budget = detail::resolve_cleanup_budget(conn, cleanup_budget);
if (!co_await detail::reclaim_connection(conn, effective_cleanup_budget)) {
    conn.invalidate();
}
```

Update the API comment to state the full precedence:

```cpp
// The cleanup budget comes from the per-call override, then from an optional
// connection provider, and finally from default_cleanup_budget.
```

- [ ] **Step 7: Build and run the timeout suite**

Run:

```bash
cmake --build build/dev --target atlas_test_suite atlas_verify_interface_header_sets -j4
build/dev/bin/atlas_test_suite
```

Expected: build succeeds; `async/timeout` reports all tests passed; standalone public-header verification succeeds.

- [ ] **Step 8: Commit the concept and precedence change**

```bash
git add include/atlas/async/timeout.hpp test/async/test_timeout.cpp
git commit -m "fix: keep timeout cancellation concept extensible"
```

---

### Task 2: Prove pool invalidation and revival when cleanup has no grace period

**Files:**
- Modify: `test/async/test_pool.cpp:4-250`

**Interfaces:**
- Consumes: `atlas::pool_config::cleanup_budget`.
- Consumes: `atlas::with_timeout<T>(Duration, Connection&, awaitable, optional<milliseconds>)`.
- Verifies: an invalidated lease is not recirculated and its pool slot is revived before serving the next acquire.

- [ ] **Step 1: Include the timeout API in the pool integration suite**

Add:

```cpp
#include "atlas/async/timeout.hpp"
```

- [ ] **Step 2: Add the PostgreSQL-backed revival regression**

Add this case to `async/pool/integration`:

```cpp
"an exhausted cleanup budget invalidates and revives the pooled connection"_test = [&url] {
    asio::io_context ctx;
    auto cfg = config_for(*url, 1, 5s);
    cfg.cleanup_budget = 0ms;
    atlas::pool db{ctx.get_executor(), cfg};

    const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
        {
            auto lease = co_await db.acquire();
            expect(lease.has_value()) << (lease ? "" : lease.error().message);
            if (!lease) {
                co_return false;
            }

            auto timed_out = co_await atlas::with_timeout<atlas::pg::result>(
                20ms, *lease, lease->execute("SELECT pg_sleep(10)", no_params));

            expect(!timed_out.has_value());
            expect(timed_out.error().code == errc::query_canceled);
            expect(!lease->is_alive()) << "the lease survived an exhausted cleanup budget";
        }

        auto replacement = co_await db.acquire();
        expect(replacement.has_value()) << (replacement ? "" : replacement.error().message);
        if (!replacement) {
            co_return false;
        }

        auto result = co_await replacement->execute("SELECT 1", no_params);
        expect(result.has_value()) << (result ? "" : result.error().message);
        expect(replacement->is_alive());
        co_return result.has_value();
    }());

    expect(ran);
};
```

- [ ] **Step 3: Run the real integration suite**

Start or provide a PostgreSQL 18 service with the same credentials as CI, then run:

```bash
cmake --build build/dev --target atlas_test_suite -j4
env ATLAS_TEST_CONNINFO="host=127.0.0.1 port=5432 user=atlas password=atlas dbname=atlas_test" \
    build/dev/bin/atlas_test_suite
```

Expected: `async/pool/integration` reports all tests passed; the timeout result is `query_canceled`; the expired lease is dead; the next acquire executes `SELECT 1`.

This is regression coverage for production behavior already introduced by the approved cleanup-budget work. If it passes before Task 1's implementation, retain it as characterization evidence rather than forcing an artificial production failure.

- [ ] **Step 4: Commit the integration regression**

```bash
git add test/async/test_pool.cpp
git commit -m "test: cover timeout cleanup pool revival"
```

---

### Task 3: Run the complete verification matrix

**Files:**
- Verify only: `include/atlas/async/timeout.hpp`
- Verify only: `test/async/test_timeout.cpp`
- Verify only: `test/async/test_pool.cpp`

**Interfaces:**
- Consumes: all production and test changes from Tasks 1 and 2.
- Produces: build, formatting, sanitizer, concurrency, and PostgreSQL integration evidence.

- [ ] **Step 1: Check formatting and patch hygiene**

Run:

```bash
clang-format --dry-run --Werror \
    include/atlas/async/timeout.hpp \
    test/async/test_timeout.cpp \
    test/async/test_pool.cpp
git diff --check
```

Expected: both commands exit successfully with no output.

- [ ] **Step 2: Build Debug, public headers, and warnings-as-errors**

Run:

```bash
cmake --build build/dev --target atlas_test_suite atlas_verify_interface_header_sets -j4
cmake --build --preset warnings-as-errors -j4
```

Expected: both builds exit successfully with `atlas_test_suite` and `atlas_verify_interface_header_sets` built.

- [ ] **Step 3: Run Debug integration tests against PostgreSQL 18**

Run:

```bash
env ATLAS_TEST_CONNINFO="host=127.0.0.1 port=5432 user=atlas password=atlas dbname=atlas_test" \
    ctest --preset dev --output-on-failure
```

Expected: 100% tests passed and zero failed tests.

- [ ] **Step 4: Run ASan/UBSan**

Run:

```bash
cmake --build --preset asan -j4
env ATLAS_TEST_CONNINFO="host=127.0.0.1 port=5432 user=atlas password=atlas dbname=atlas_test" \
    ctest --preset asan --output-on-failure
```

Expected: 100% tests passed with no AddressSanitizer or UndefinedBehaviorSanitizer report.

- [ ] **Step 5: Run TSan**

Run:

```bash
cmake --build --preset tsan -j4
env ATLAS_TEST_CONNINFO="host=127.0.0.1 port=5432 user=atlas password=atlas dbname=atlas_test" \
    build/tsan/bin/atlas_test_suite
```

Expected: every suite reports all tests passed with no ThreadSanitizer report.

- [ ] **Step 6: Confirm branch state**

Run:

```bash
git status --short --branch
git log -4 --oneline --decorate
```

Expected: the timeout-policy commits are on `feat/async-engine`; any unrelated pre-existing working-tree changes remain present and unmodified.
