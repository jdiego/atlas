# Transaction Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make transaction completion report PostgreSQL's actual outcome and release pool capacity immediately.

**Architecture:** `pg::result` will expose libpq's command tag, and `transaction` will own its lease through `std::optional`. An acknowledged `COMMIT` or `ROLLBACK` moves the transaction to a final state and resets the optional lease; a `ROLLBACK` tag returned for `COMMIT` is reported as `transaction_aborted`.

**Tech Stack:** C++23, Boost.Asio coroutines, libpq 18, Boost.UT, PostgreSQL 18.

## Global Constraints

- A successful `commit()` must mean PostgreSQL returned the `COMMIT` command tag.
- Append `errc::transaction_aborted`; do not move existing enumerator values.
- Release the pool lease immediately after an acknowledged terminal command.
- Preserve PostgreSQL or transport errors when completion is not acknowledged.
- Write and observe each regression test failing before production changes.

---

### Task 1: Expose command tags and aborted-transaction errors

**Files:**
- Modify: `include/atlas/pg/result.hpp`
- Modify: `include/atlas/pg/error.hpp`
- Modify: `src/pg/result.cpp`
- Modify: `test/src/connection_test.cpp`

**Interfaces:**
- Produces: `pg::result::command_tag() const noexcept -> std::string_view`.
- Produces: `pg::errc::transaction_aborted`.
- Produces: `sqlstate_to_errc("25P02") == errc::transaction_aborted`.

- [ ] **Step 1: Write failing public-contract tests**

Add to the result integration suite in `test/src/connection_test.cpp`:

```cpp
"command_tag exposes PostgreSQL's terminal command"_test = [] {
    auto ci = test_conninfo();
    if (!ci) return;

    auto conn = connection::connect(*ci);
    expect(conn.has_value() >> fatal);
    auto result = conn->exec("BEGIN");
    expect(result.has_value() >> fatal);
    expect(result->command_tag() == "BEGIN"sv);
};
```

Add to a unit suite in the same file:

```cpp
"25P02 maps to transaction_aborted"_test = [] {
    expect(sqlstate_to_errc("25P02") == errc::transaction_aborted);
};
```

- [ ] **Step 2: Build to verify RED**

Run:

```bash
cmake --build --preset dev -j4
```

Expected: compilation fails because `command_tag` and
`transaction_aborted` do not exist.

- [ ] **Step 3: Implement the minimal API**

Append the enum value and mapping:

```cpp
enum class errc {
    // existing values unchanged
    syntax_error,
    transaction_aborted,
};

if (sqlstate == "25P02") {
    return errc::transaction_aborted;
}
```

Declare:

```cpp
[[nodiscard]] auto command_tag() const noexcept -> std::string_view;
```

Implement:

```cpp
auto result::command_tag() const noexcept -> std::string_view {
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return {};
    }
    const char *tag = PQcmdStatus(impl_->handle.get());
    return tag == nullptr ? std::string_view{} : std::string_view{tag};
}
```

- [ ] **Step 4: Verify GREEN**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: build succeeds and all tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/atlas/pg/error.hpp include/atlas/pg/result.hpp src/pg/result.cpp test/src/connection_test.cpp
git commit -m "feat: expose PostgreSQL command tags"
```

### Task 2: Make commit outcome and lease ownership exact

**Files:**
- Modify: `include/atlas/async/transaction.hpp`
- Modify: `src/async/transaction.cpp`
- Modify: `test/async/test_transaction.cpp`

**Interfaces:**
- Consumes: `pg::result::command_tag()`.
- Consumes: `pg::errc::transaction_aborted`.
- Changes: `transaction` stores `std::optional<pool_connection> conn_`.
- Produces: all methods return `invalid_state` after transaction completion.

- [ ] **Step 1: Write the failing aborted-commit regression**

Add:

```cpp
"commit reports PostgreSQL rollback of an aborted transaction"_test = [&url] {
    asio::io_context ctx;
    atlas::pool db{ctx.get_executor(), config_for(*url, 1)};

    const bool ran = run_on(ctx, [&]() -> asio::awaitable<bool> {
        expect(co_await reset_table(db, "atlas_tx_aborted_commit"));
        auto tx = co_await db.begin();
        expect(tx.has_value() >> fatal);

        auto inserted = co_await tx->execute(
            "INSERT INTO atlas_tx_aborted_commit VALUES (1)", no_params);
        expect(inserted.has_value());
        auto failed = co_await tx->execute("SELECT 1 / 0", no_params);
        expect(!failed.has_value());

        auto committed = co_await tx->commit();
        expect(!committed.has_value());
        if (!committed) {
            expect(committed.error().code == errc::transaction_aborted);
        }
        expect(co_await count_rows(db, "atlas_tx_aborted_commit") == 0L);
        co_return !committed.has_value();
    }());
    expect(ran);
};
```

- [ ] **Step 2: Run the suite to verify RED**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: the new test fails because `commit()` returns success for the
`ROLLBACK` command tag.

- [ ] **Step 3: Add immediate-release regressions**

Use `max_size = 1`, keep the finished transaction object alive, and assert a
new pool query succeeds after both terminal methods:

```cpp
auto tx = co_await db.begin();
expect(tx.has_value() >> fatal);
expect((co_await tx->commit()).has_value());
auto after_commit = co_await db.execute("SELECT 1", no_params);
expect(after_commit.has_value());
```

Repeat with `rollback()`. Also call `execute()`, `savepoint()`, `rollback_to()`,
`release_savepoint()`, `commit()`, and `rollback()` after completion and assert
each returns `errc::invalid_state`.

- [ ] **Step 4: Run to verify the lease tests are RED**

Run the same CTest command.

Expected: the second query times out because the finished transaction still
owns the only lease; finished-operation assertions expose rollback's current
idempotent behavior.

- [ ] **Step 5: Implement explicit transaction state**

In the header, include `<optional>` and replace the members with:

```cpp
enum class state { active, finished };

std::optional<pool_connection> conn_;
executor_type ex_;
state state_ = state::active;

[[nodiscard]] auto active_connection() -> std::expected<pool_connection *, pg::error>;
void finish() noexcept;
```

Implement the helpers:

```cpp
auto transaction::active_connection()
    -> std::expected<pool_connection *, pg::error> {
    if (state_ != state::active || !conn_) {
        return std::unexpected(make_finished_transaction_error());
    }
    return &*conn_;
}

void transaction::finish() noexcept {
    state_ = state::finished;
    conn_.reset();
}
```

Make every public operation obtain `active_connection()` before use. Implement
commit's terminal-tag distinction:

```cpp
auto conn = active_connection();
if (!conn) {
    co_return std::unexpected(conn.error());
}
auto result = co_await (*conn)->execute("COMMIT", no_params);
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
    co_return std::unexpected(
        pg::error{"transaction was rolled back", pg::errc::transaction_aborted});
}
co_return std::unexpected(
    pg::error{"unexpected COMMIT command tag", pg::errc::invalid_state});
```

After an acknowledged `ROLLBACK`, call `finish()`. Move operations transfer
the optional and state; destructors and move assignment move the old lease into
`rollback_connection()` only when state is active and a lease exists.

- [ ] **Step 6: Verify GREEN**

Run:

```bash
cmake --build --preset dev -j4
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" ctest --preset dev --output-on-failure
```

Expected: all transaction regressions and the full suite pass.

- [ ] **Step 7: Commit**

```bash
git add include/atlas/async/transaction.hpp src/async/transaction.cpp test/async/test_transaction.cpp
git commit -m "fix: make transaction completion exact"
```
