# Async Cancellation and Retry Hardening

## Goal

Finish hardening the async engine by making query cancellation non-blocking,
bounding the cleanup that follows a timeout, making retry backoff overflow-safe,
and avoiding unnecessary `ROLLBACK` statements outside failed transactions.

## Compatibility

Atlas will require libpq 18 or newer. The encrypted, non-blocking cancellation
API used by this design was introduced in version 17 and remains the appropriate
API in version 18:

- `PQcancelCreate`
- `PQcancelStart`
- `PQcancelPoll`
- `PQcancelSocket`
- `PQcancelErrorMessage`
- `PQcancelFinish`

CMake will reject older libpq versions during configuration. Ubuntu CI will
install libpq 18 explicitly and use PostgreSQL 18 for integration tests.

## Non-blocking cancellation

`async_connection::request_cancel()` will become an awaitable operation. It will
create a `PGcancelConn`, start cancellation, and drive `PQcancelPoll()` using an
Asio `posix::stream_descriptor`.

The descriptor will mirror `PQcancelSocket()` on every polling iteration because
libpq may replace the socket during connection setup. Atlas will detach the
descriptor before `PQcancelFinish()` so libpq remains the sole owner of the
socket.

The public forwarding method on `pool_connection` and the
`cancellable_connection` concept will adopt the awaitable contract.
`with_timeout()` will await cancellation before draining the original query's
result stream.

Cancellation failures will be returned by `request_cancel()`. If dispatching the
cancel request fails, `with_timeout()` will mark the original connection
unusable and skip draining it: waiting for a query that was never cancelled
would violate the deadline, while reusing that connection would corrupt the
next operation. A pooled connection will therefore be replaced on release; a
standalone connection will reject further operations.

The drain helper will report whether it reached the terminating sentinel. A
connection will also be marked unusable when draining cannot finish cleanly.
`with_timeout()` will continue returning its existing `query_canceled` timeout
error, preserving the public timeout contract.

## Bounded timeout cleanup

Cancelling and draining both perform network I/O, so the cleanup that follows a
query timeout will have its own budget. The default cleanup budget is five
seconds. A pool can set the value once in `pool_config`; both initially opened
connections and replacement connections will retain it.

The total time before `with_timeout()` returns may therefore be the requested
operation timeout plus the cleanup budget. If cancellation or draining does not
finish within that budget, Atlas will cancel the cleanup coroutine, wait for it
to stop, and invalidate the connection before returning `query_canceled`.
Non-positive budgets mean that no cleanup grace period is allowed, so the
connection is invalidated immediately after the operation timeout.

`cancellable_connection` remains a minimal structural concept. It requires only
the operations needed to reclaim a connection:

- an awaitable `request_cancel()`;
- an awaitable `receive()`; and
- `invalidate()`.

Supplying a default cleanup policy is a separate, optional capability detected
structurally through `cleanup_budget()`. This keeps `with_timeout()` usable with
external adapters and test doubles that satisfied the original cancellation
concept.

The effective cleanup budget follows this precedence:

1. the per-call override passed directly to `with_timeout()`;
2. `conn.cleanup_budget()` when the connection provides that accessor; or
3. `default_cleanup_budget`.

Atlas will select the value with explicit control flow rather than
`std::optional::value_or()`, ensuring that `conn.cleanup_budget()` is not
evaluated when a per-call override is present.

## Transaction-aware retry cleanup

`async_connection` will expose whether libpq reports `PQTRANS_INERROR`.
`pool_connection` will forward that state. `with_retry()` will issue `ROLLBACK`
only when the connection is alive and its transaction is aborted.

This preserves recovery for serialization failures and deadlocks inside explicit
transactions without generating PostgreSQL warnings for failures outside a
transaction.

## Overflow-safe backoff

Backoff calculation will be isolated in a small internal helper. It will compare
the base delay with the configured maximum before multiplication and saturate at
the maximum whenever the factor would overflow or exceed the cap.

`max_delay` remains the upper bound. Non-positive delay inputs will produce an
immediate retry rather than invoking signed-overflow behavior.

## Tests

Tests will be written before each production change and observed failing for the
intended reason.

1. A retry delay test will use a near-maximum duration and assert that the result
   saturates at `max_delay`.
2. A transaction-state integration test will distinguish an idle connection, a
   healthy explicit transaction, and an aborted transaction.
3. A real timeout integration test will cancel `SELECT pg_sleep(...)`, assert a
   prompt `query_canceled` result, and then run `SELECT 1` on the same connection
   to prove the result stream was drained.
4. Existing retry integration tests will run without PostgreSQL's
   "there is no transaction in progress" warnings.
5. Compile-time and runtime timeout tests will cover a cancellable adapter with
   no budget accessor, a connection-provided budget, and a per-call override.
6. A PostgreSQL-backed pool test will use a non-positive cleanup budget, prove
   that the timed-out lease is invalidated, and verify that the pool replaces it
   with a connection that can execute `SELECT 1`.
7. Build, standalone-header verification, warnings-as-errors, ASan/UBSan, TSan,
   and PostgreSQL-backed integration tests will remain green.

## Error handling and resource ownership

Every non-null `PGcancelConn` will be released exactly once with
`PQcancelFinish()`. Socket ownership remains with libpq; Asio descriptors only
observe readiness and are detached before libpq cleanup.

All libpq status failures are mapped to `pg::error`. Connections whose
cancellation or draining fails are invalidated before they can return to a pool.
No detached cancellation work will outlive the owning `async_connection`.
