# Async Cancellation and Retry Hardening

## Goal

Finish hardening the async engine by making query cancellation non-blocking,
making retry backoff overflow-safe, and avoiding unnecessary `ROLLBACK`
statements outside failed transactions.

## Compatibility

Atlas will require libpq 17 or newer. Version 17 introduced the encrypted,
non-blocking cancellation API used by this design:

- `PQcancelCreate`
- `PQcancelStart`
- `PQcancelPoll`
- `PQcancelSocket`
- `PQcancelErrorMessage`
- `PQcancelFinish`

CMake will reject older libpq versions during configuration. Ubuntu CI will
install libpq 17 explicitly and use PostgreSQL 17 for integration tests.

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
5. Build, standalone-header verification, warnings-as-errors, ASan/UBSan, TSan,
   and PostgreSQL-backed integration tests will remain green.

## Error handling and resource ownership

Every non-null `PGcancelConn` will be released exactly once with
`PQcancelFinish()`. Socket ownership remains with libpq; Asio descriptors only
observe readiness and are detached before libpq cleanup.

All libpq status failures are mapped to `pg::error`. Connections whose
cancellation or draining fails are invalidated before they can return to a pool.
No detached cancellation work will outlive the owning `async_connection`.
