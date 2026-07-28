# Runtime Correctness and Recovery

## Goal

Make the async runtime preserve the two guarantees expected from a database
abstraction:

1. a successful operation reports what PostgreSQL actually did; and
2. a pooled lease is handed to another caller only when its session is clean.

The work covers transaction completion, pool check-in, timed-out acquire
cleanup, recovery from transient connection failures, and connection-string
validation.

## Scope

This design includes:

- detecting PostgreSQL's implicit rollback of an aborted transaction;
- returning a transaction's lease immediately after explicit completion;
- replacing live but non-reusable sessions instead of recirculating them;
- removing timed-out acquire waiters immediately;
- retrying unavailable pool capacity with bounded backoff until recovery or
  shutdown;
- parsing `sslmode` through libpq and rejecting embedded null bytes; and
- focused unit and PostgreSQL-backed regression coverage for those behaviors.

Windows support, the minimum Boost version, macOS PostgreSQL integration,
workflow hermeticity, and aggregate-initialization compatibility for
`pg::error` remain outside this runtime-focused change.

## Runtime Invariants

### Transaction completion is observable and final

A transaction has two internal states: `active` and `finished`. Its lease is
stored in an optional owner so finishing the transaction can destroy the lease
without destroying the C++ transaction object.

`commit()` behaves as follows:

1. reject a second completion attempt with `errc::invalid_state`;
2. execute `COMMIT`;
3. accept success only when the returned PostgreSQL command tag is exactly
   `COMMIT`;
4. if PostgreSQL returns the `ROLLBACK` command tag, return
   `errc::transaction_aborted`; and
5. for either acknowledged terminal tag, mark the transaction finished and
   release its pool lease immediately.

PostgreSQL accepts `COMMIT` while a transaction is aborted but completes it as
a rollback. Checking only `PGRES_COMMAND_OK` therefore cannot prove that a
commit occurred. Atlas will expose `pg::result::command_tag()` as a
non-owning `std::string_view` backed by the result and use `PQcmdStatus()` for
this distinction.

The SQLSTATE mapping will add `25P02` as `errc::transaction_aborted`. The new
enumerator is appended so existing numeric values do not move.

`rollback()` also marks the transaction finished and releases the lease as soon
as PostgreSQL acknowledges it. A failed commit or rollback leaves the
transaction active because the server state is not known to be clean; its
destructor then performs the existing best-effort cleanup.

Every query and savepoint method rejects a finished or moved-from transaction.
Move construction and move assignment transfer the optional lease and leave
the source finished. Replacing an active transaction through move assignment
first schedules best-effort rollback of the old lease.

### Pool check-in requires a clean session

Transport liveness is necessary but not sufficient for reuse. An
`async_connection` is reusable only when all of these conditions hold:

- it owns a `PGconn`;
- no transport failure has been recorded;
- libpq reports `CONNECTION_OK`;
- Atlas has no result stream in progress; and
- `PQtransactionStatus()` reports `PQTRANS_IDLE`.

The check remains an internal pool invariant rather than a promise clients
need to reason about. Returning a lease in `PQTRANS_INTRANS`,
`PQTRANS_INERROR`, `PQTRANS_ACTIVE`, or `PQTRANS_UNKNOWN` invalidates that
slot and starts recovery. This deliberately prefers replacing a session over
guessing how to clean caller-owned state.

This rule also covers callers that:

- drop a lease after sending only part of a query;
- drop a lease with an open transaction;
- time out while cleanup leaves the transaction aborted; or
- encounter a protocol or transport failure that has not yet changed
  `PQstatus()`.

### Timed-out waiters do not remain queued

The waiter queue becomes a strand-owned `std::list` of waiter objects. An
acquire operation stores its own list iterator while suspended.

There are only two serialized outcomes:

- hand-off removes the waiter from the front, marks it handled, and cancels its
  timer; or
- timeout removes that exact waiter immediately and marks it expired.

Shutdown removes and signals every remaining waiter. The `handled` flag remains
authoritative for the timer-versus-hand-off boundary, so a timer expiring in the
same strand turn cannot lose a connection. Queue memory and future hand-off
work are therefore proportional to currently waiting callers, not historical
timeouts.

## Recovery State Machine

A dead or non-reusable existing slot is unavailable while one recovery
coroutine owns it. A connection that failed during initial pool creation is
represented by one missing-capacity recovery coroutine. Recovery work runs on
the pool strand, and the connection vector keeps its full `max_size` reserve so
later successful insertions cannot invalidate existing lease pointers.

One recovery cycle makes `max(max_retries, 1)` connection attempts. If every
attempt fails, the cycle waits before trying again. The delay starts at
`reconnect_initial_delay` and doubles after each failed cycle, saturating at
`reconnect_max_delay`. Successful recovery resets the delay for that slot.

The new defaults are:

- `reconnect_initial_delay = 100ms`; and
- `reconnect_max_delay = 5s`.

Zero delay is allowed for deterministic tests. A negative delay or a maximum
smaller than the initial delay is a terminal `errc::invalid_argument`
configuration error.

Recovery continues until one of these terminal events:

- the slot reconnects and is handed to the oldest live waiter or parked free;
- pool shutdown begins; or
- the pool has a terminal connection-string configuration error.

The pool never permanently retires capacity after a short outage.
`pool::size()` continues to mean currently usable connections, and
`pool::available()` continues to mean immediately leasable connections.

Initialisation still reports a completed first pass. If that pass opens no
connections, callers already waiting are released with the last
`connection_failure` instead of waiting indefinitely. Background recovery
continues, and later callers may wait up to `pool_config::timeout` for restored
capacity. Once any recovery succeeds, it can directly satisfy the oldest
waiter.

Shutdown wakes queued acquires immediately. Recovery coroutines observe the
shutdown flag before every connection attempt and after every backoff wait.
Their timers and state are owned by the shared pool state, so detached work
cannot access a destroyed pool.

## Connection String Validation

`apply_ssl_mode()` changes to return
`std::expected<std::string, pg::error>`. This follows Atlas's error-return
philosophy and prevents malformed configuration from being silently converted
into a different connection string.

The function:

1. rejects any embedded null byte with `errc::invalid_argument`;
2. calls `PQconninfoParse()` to validate both keyword/value and URI forms;
3. checks the parsed `sslmode` option, whose value is null when the caller did
   not specify it;
4. returns the original string when `sslmode` is explicit; and
5. otherwise appends the configured value using the syntax of the original
   connection string.

Both the returned options and libpq's allocated error message use RAII
deleters. The pool applies this transformation once, stores either the validated
connection string or the terminal configuration error, and gives every initial
or replacement connection the same value.

An invalid connection string is not transient. `acquire()` therefore returns
the stored configuration error promptly and no reconnect loop is started.

## Error Precedence

- A PostgreSQL error returned while issuing `COMMIT` or `ROLLBACK` is preserved.
- An acknowledged `ROLLBACK` tag from `commit()` becomes
  `errc::transaction_aborted`.
- A terminal pool configuration error is returned unchanged by `acquire()`.
- A caller that waits for otherwise recoverable capacity until its acquisition
  deadline receives the existing `errc::query_canceled`.
- Recovery failures are retained as diagnostic context for the initial
  no-connection result but do not permanently disable the slot.

## Testing

Tests will be written before each production change and observed failing for
the intended reason.

### Transaction tests

- `pg::result::command_tag()` reports command tags without extending the
  result's lifetime.
- `sqlstate_to_errc("25P02")` maps to `transaction_aborted`.
- A statement error aborts a transaction; `commit()` then returns
  `transaction_aborted` and the attempted data change is absent.
- With `max_size = 1`, a successful `commit()` makes a new pool operation
  succeed while the finished transaction object is still alive.
- The same immediate-reuse assertion covers explicit `rollback()`.
- Every operation on a finished transaction returns `invalid_state`.

### Pool hygiene and waiter tests

- Dropping a live lease in `PQTRANS_INTRANS` replaces its session before the
  next borrower runs a query.
- Dropping a live lease in `PQTRANS_INERROR` does the same.
- Dropping a lease with an unfinished result stream never exposes that stream
  to the next borrower.
- A focused internal waiter-queue test times out many non-front waiters and
  proves they are removed immediately while preserving FIFO hand-off among the
  remaining waiters.
- Timer and hand-off boundary tests prove that a connection is neither lost nor
  delivered twice.

### Recovery and configuration tests

- An initially unavailable PostgreSQL database is created after the pool's
  first failed cycle; the existing pool recovers without reconstruction and
  reaches its configured capacity.
- Repeated recovery failures observe the configured delay and do not create
  duplicate recovery loops for one slot.
- Pool destruction during backoff is safe and wakes queued callers.
- Keyword/value and URI connection strings with explicit `sslmode` remain
  unchanged.
- Text such as `application_name=sslmode=require` no longer masquerades as an
  explicit SSL option.
- Malformed strings and embedded null bytes return `invalid_argument`.
- Valid strings without `sslmode` receive exactly one configured option.

The complete development, warnings-as-errors, standalone-header,
installed-consumer, ASan/UBSan, TSan, and PostgreSQL 18 integration suites must
remain green.

## Success Criteria

- `commit()` cannot report success for a server-side rollback;
- explicit transaction completion returns capacity before object destruction;
- only idle, fully drained sessions are recirculated;
- expired waiters consume no queue space or future hand-off work;
- transient outages reduce capacity temporarily but never retire it
  permanently;
- connection-string handling is libpq-authoritative and null-byte safe; and
- all existing public runtime behavior outside the documented
  `apply_ssl_mode()` return-type change remains compatible.

## libpq References

- [Connection parsing and `PQconninfoParse`](https://www.postgresql.org/docs/18/libpq-connect.html)
- [Transaction status and `PQtransactionStatus`](https://www.postgresql.org/docs/18/libpq-status.html)
- [Command tags and `PQcmdStatus`](https://www.postgresql.org/docs/18/libpq-exec.html)
