# CI Stability Design

## Context

Pull request #6 currently passes the standalone and installation workflows but
fails on macOS and Ubuntu:

- the macOS installed-consumer build crashes Apple Clang 17 while compiling the
  test suite with four concurrent jobs;
- the Ubuntu pool-recovery integration test reaches the shared 30-second
  coroutine watchdog;
- the Ubuntu installed consumer inherits Atlas maintainer warnings, causing a
  GCC standard-library diagnostic to fail its build under `-Werror`.

The correction must preserve strict checks for Atlas itself, keep the
installed-package contract meaningful, and avoid masking a pool recovery defect
by merely increasing time limits.

## Chosen Design

### Warning ownership

Compiler warnings and warnings-as-errors belong to the targets maintained in
this source tree. They will be applied privately to the Atlas library and its
test executable. They must not appear in the installed target's interface or
be imposed on downstream consumers.

The installed-consumer contract will assert this property so that future CMake
changes cannot accidentally export `-Werror` again.

### Installed-consumer build

The nested installed-consumer build will use a configurable job count with a
default of one. This contract validates packaging rather than build throughput,
so deterministic resource usage is more valuable than maximum parallelism. A
caller may explicitly select a higher value when appropriate.

The nested configure must also use the same C++ compiler selected by the parent
build. This matters when CI explicitly selects a non-default toolchain: without
propagation, the package contract silently falls back to the platform compiler
and no longer tests the configuration that the workflow requested.

The test will retain its existing 180-second outer timeout.

### Pool recovery

The server-side termination test will continue to exercise the real PostgreSQL
18 connection path. The investigation will first determine whether the
30-second stall occurs while observing the terminated connection, reconnecting
the slot, acquiring it, or executing on the replacement.

The final test will wait for the pool's observable recovery condition within a
bounded deadline instead of sleeping for an assumed 500 milliseconds. The
global 30-second watchdog will remain unchanged. If investigation exposes an
implementation race, that race will be fixed at its source and covered by a
focused regression assertion.

## Alternatives Considered

### Increase the global coroutine budget

Rejected because the failing operation normally takes less than one second.
Increasing 30 seconds would lengthen CI while concealing a stalled recovery
path.

### Suppress GCC `-Warray-bounds`

Rejected because the diagnostic is emitted while compiling consumer code and
only becomes fatal because Atlas exports `-Werror`. Suppression would address
one compiler warning while preserving the incorrect package interface.

### Replace the installed-consumer suite with a minimal smoke program

Deferred. A smoke consumer would reduce compile cost, but the current full
suite also validates that all public headers and runtime behavior work from an
installed package. Serial compilation is the smaller change and preserves that
coverage.

## Testing

- inspect the generated installed target and verify it contains no `-Werror`;
- run the installed-consumer contract with the default serial build;
- run the pool recovery test repeatedly against PostgreSQL 18;
- build and run the complete development test suite;
- run the warnings-as-errors preset;
- run the ASan and TSan presets when supported locally;
- push the branch and verify the macOS, Ubuntu, standalone, and installation
  GitHub Actions workflows.

## Success Criteria

- no maintainer warning option is exported to installed consumers;
- the installed-consumer contract builds without compiler resource failures;
- the installed-consumer contract uses the parent's exact C++ compiler;
- pool recovery completes deterministically without raising the global test
  budget;
- all four GitHub Actions workflows pass.
