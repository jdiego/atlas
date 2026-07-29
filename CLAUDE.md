# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Atlas is an async C++23 client library for PostgreSQL built on Boost.Asio and libpq. It exposes three layers: a thin synchronous RAII wrapper over libpq (`atlas::pg`), an async Boost.Asio coroutine layer (`atlas::async`), and a compile-time schema/query DSL (`atlas::schema`, `atlas::query`).

## Build

Requires: CMake ≥ 3.25, a C++23 compiler (GCC 13 / Clang 16+), Boost (headers + Asio), libpq, and pkg-config. CPM fetches remaining deps on first configure.

```bash
# Configure + build + test (development)
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

# Other presets: release, asan, tsan, coverage, warnings-as-errors
# Each writes to build/<preset-name>/

# Makefile shortcuts
make test          # clean build + ctest (Release)
make fix-format    # auto-fix clang-format + cmake-format
make format        # check formatting without fixing
make check         # run cppcheck
make docs          # Doxygen HTML → build/docs/html/index.html
make coverage      # gcov report
```

Speed up clean builds by setting `CPM_SOURCE_CACHE=~/.cache/cpm`.

## Running a single test

Tests use boost::ut (expression-based, no macros). Run specific tests via ctest's `-R` filter:

```bash
ctest --preset dev -R test_retry       # matches test binary or test name
ctest --preset dev -R "schema" -V      # verbose output
```

Or build just the test binary and run it directly:

```bash
cmake --build --preset dev --target atlas_test_suite
./build/dev/test/atlas_test_suite
```

## Static analysis

```bash
# clang-tidy (configure with -DATLAS_ENABLE_CLANG_TIDY=ON)
cmake --preset dev -DATLAS_ENABLE_CLANG_TIDY=ON
cmake --build --preset dev

# cppcheck (via Makefile)
make check
```

## Architecture

### Layer 1 — `atlas::pg` (synchronous libpq wrapper)

`include/atlas/pg/` and `src/pg/`. Provides `connection`, `result`, `error`, and `types`. All operations return `std::expected<T, pg::error>`. `pg::error` carries a `pg::errc` enum (mapped from SQLSTATE), a message string, and `is_retryable()` (true for `serialization_failure` / `deadlock_detected` / `connection_failure`). Both `connection` and `result` use PIMPL to keep libpq types out of public headers — only `async_connection.hpp` forward-declares `PGconn`/`PGresult` directly.

### Layer 2 — `atlas::async` (Boost.Asio coroutine layer)

`include/atlas/async/` and `src/async/`. All async operations return `asio::awaitable<std::expected<T, pg::error>>`. The convenience alias `pg_awaitable<T>` is defined in `async_connection.hpp`.

Key types and their roles:
- **`async_connection`** — wraps a single `PGconn*` with `asio::posix::stream_descriptor`. Non-blocking connect via `PQconnectStart` + poll. Sends queries with `PQsendQueryParams`; drives the read loop with `async_wait` + `PQconsumeInput`.
- **`pool`** — manages a fixed-size vector of `async_connection`s, serialised by an `asio::strand`. `acquire()` suspends the coroutine if no connection is free; `release()` dispatches to the strand and hands off to waiters if any are queued.
- **`pool_connection`** — RAII lease returned by `pool::acquire()`. Destructor calls `pool::release()` automatically; never call release manually.
- **`transaction`** — RAII transaction wrapping a `pool_connection`. Destructor fire-and-forgets `ROLLBACK` via `co_spawn(..., detached)` if `commit()` was never called.
- **`with_timeout`** — races an awaitable against `asio::steady_timer` using `awaitable_operators::||`. Returns `pg::errc::query_canceled` if the timer wins. Has a two-arg overload and a three-arg overload that accepts an external `cancellation_slot`.
- **`with_retry`** — retries an operation up to `max_attempts` times on retryable errors, acquiring a fresh connection per attempt (because a serialization failure leaves the connection in an aborted transaction). Uses exponential backoff: `base_delay * 2^attempt`, capped at `max_delay`. Acquires shift via `min(attempt, 20)` to prevent overflow.
- **`async_engine.hpp`** — single convenience re-export header for the entire async layer.

### Layer 3 — `atlas::schema` + `atlas::query` (compile-time DSL)

`include/atlas/schema/` and `include/atlas/query/`. Zero heap allocation — all descriptors are `constexpr`-constructible.

- **`column_t<Entity, MemberPtr, ...Constraints>`** — maps a struct field to a SQL column. `member_ptr` drives both DDL generation and serde. `has_constraint<C>()` is a fold expression over the constraint pack.
- **`table_t<Entity, ...Columns>`** — aggregates columns. `for_each_column(fn)` visits columns in declaration order, which is the same order used by DDL and serde — preserving this invariant is critical.
- **`storage_t<...Tables>`** — compile-time registry. `get_table<Entity>()` resolves to the matching `table_t` via `consteval` index lookup; static_asserts if the entity is unregistered.
- **`ddl.hpp`** — generates `CREATE TABLE` SQL. Has two overloads: one without storage (emits a TODO comment for `REFERENCES` clauses) and one with storage (resolves foreign key table/column names via `find_column`).
- **`serde.hpp`** — serialises/deserialises entity structs to/from `pg::result` rows using `for_each_column`.

### Error handling convention

Use `std::expected<T, pg::error>` for all fallible operations. Propagate with `if (!res) { co_return std::unexpected(res.error()); }`. Never throw; never silently discard errors.

## Code style

Formatting is enforced by `.clang-format` (LLVM-based) and `.clang-tidy` (strict, `*` minus fuchsia/google/zircon/abseil/altera/llvm families). Run `make fix-format` before committing. The PostToolUse hook in `.claude/settings.json` auto-formats C/C++ files on every edit.

All new public API functions must use `[[nodiscard]]`. Prefer `std::string_view` over `const std::string&` for read-only string parameters. Keep libpq types (`PGconn*`, `PGresult*`) out of public headers via PIMPL or forward declarations.
