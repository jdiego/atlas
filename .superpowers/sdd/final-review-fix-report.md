# Final Review Fix Report

Date: 2026-07-27

Branch: `feat/async-engine`

Review baseline: `26bfaed05a677f29fd132a51bef2648f88cd696a`

## Outcome

All Critical, Important, and Minor findings in
`.superpowers/sdd/final-review-findings.md` were addressed. The timeout cleanup
policy remains intact: timed-out operations still cancel, drain through the
terminal sentinel within the configured cleanup budget, and invalidate the
connection when cleanup cannot complete safely.

## Corrections

### Async transport

- A successfully connected `PGconn` is switched to libpq nonblocking mode with
  `PQsetnonblocking`.
- Connect and cancel polling now detach and re-register the Asio descriptor
  observer on every poll, including same-numeric-fd reuse. The observer never
  owns or closes libpq's socket.
- Flush backpressure waits for either read or write readiness. Read readiness
  calls `PQconsumeInput` before retrying `PQflush`.
- Deterministic socketpair coverage exercises read readiness under write
  backpressure and same-numeric-fd re-registration.

### Package contract

- The generated and installed CMake package now requires `libpq>=18`, matching
  the source build.
- The install workflow enables the PGDG repository and asserts libpq 18 before
  building the installed-package consumer.
- A CTest guard verifies that the generated package configuration retains the
  version floor.

### Result and error handling

- `PGRES_COPY_IN`, `PGRES_COPY_OUT`, and `PGRES_COPY_BOTH` are rejected as
  unsupported; the connection is invalidated so it cannot return to the pool in
  COPY mode.
- `PGRES_PIPELINE_ABORTED` is classified as an error instead of an ordinary
  successful result.
- `send_query` distinguishes an already-active command (`invalid_state`) from a
  dead transport (`connection_failure`), while retaining `unknown` only for
  otherwise unclassified libpq rejection.
- Explicit `transaction::rollback()` propagates server/transport errors and
  marks the transaction finished only after a successful `ROLLBACK`.
  Destructor rollback remains best-effort.

## TDD Evidence

The behavior-changing fixes were driven through focused RED/GREEN cycles:

- Nonblocking connection: RED failed because `is_nonblocking` did not exist;
  GREEN passed the live-connect test after `PQsetnonblocking`.
- Descriptor observer: RED failed on the missing observer header; GREEN passed
  the socket backpressure and fd-reuse tests.
- Package floor: RED CTest reported that the installed config did not require
  libpq 18; GREEN passed after applying the versioned dependency.
- COPY handling: RED exposed COPY as an ordinary result and left the connection
  unusable; GREEN rejected COPY, invalidated it, and acquired a working
  replacement from the pool.
- Result status classification: RED failed on the missing classifier; GREEN
  classified every COPY state as unsupported and pipeline abort as error.
- Send failure classification: both focused tests failed with `unknown` in RED;
  GREEN reported `invalid_state` for overlap and `connection_failure` for a
  terminated backend.
- Explicit rollback: RED showed the failure was suppressed; GREEN propagated
  the PostgreSQL termination error and preserved its message.

## Recommendation Evaluation

- A real PostgreSQL test now forces SQLSTATE `40001` inside `BEGIN`; the existing
  `with_retry` implementation rolls back the aborted transaction before the
  clean retry. No production change was required.
- `static_assert(atlas::default_cleanup_budget == 5s)` locks the documented
  default.
- Neither the design nor implementation claims jitter, so no jitter change was
  needed.

## Verification

- `clang-format --dry-run --Werror` passed for every touched C++ source/header.
- `git diff 26bfaed05a677f29fd132a51bef2648f88cd696a..HEAD --check` passed.
- `cmake --build build/dev --target atlas_test_suite atlas_verify_interface_header_sets -j4`
  passed.
- Installed-package consumer configuration found `libpq 18.4` and its
  `atlas_test_suite` target built successfully.
- With
  `ATLAS_TEST_CONNINFO='host=/tmp/atlas-review-pg.3Ia3Gl port=55432 user=atlas dbname=atlas_test'`,
  Debug CTest passed `2/2` tests, including the full real PostgreSQL suite and
  package-config floor guard.

## Commits

- `b02c7e5` — `fix: harden async socket readiness`
- `676ed73` — `fix: preserve libpq floor for consumers`
- `cf77486` — `fix: reject unsupported async states`

No unresolved review concern remains.
