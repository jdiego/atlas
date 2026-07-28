# CI Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the macOS and Ubuntu workflows deterministic while preserving strict maintainer checks and fixing the dead-transport query hang exposed by the pool test.

**Architecture:** Keep maintainer build policy private to source-tree targets, make the nested package-contract build resource-bounded, and stop result draining as soon as libpq reports a transport failure. Tests cover both the installed CMake interface and the real PostgreSQL 18 connection path.

**Tech Stack:** C++23, Boost.Asio, libpq 18, CMake 3.25+, CTest, Boost.UT, GitHub Actions.

## Global Constraints

- The minimum supported libpq version remains 18.
- The shared 30-second coroutine watchdog remains unchanged.
- Installed consumers must not inherit `-Werror`, `/WX`, or Atlas maintainer warning flags.
- The full installed-consumer suite remains in place.
- The nested consumer build defaults to one job but permits an explicit positive job count.
- PostgreSQL integration tests use `ATLAS_TEST_CONNINFO`.
- No workflow may be declared fixed until its fresh GitHub Actions run passes.

---

### Task 1: Keep Maintainer Warnings Out of the Installed Interface

**Files:**
- Modify: `test/cmake/run_installed_consumer.cmake`
- Modify: `test/CMakeLists.txt`
- Modify: `cmake/CompilerWarnings.cmake`

**Interfaces:**
- Consumes: the installed CMake export under `ATLAS_PACKAGE_STAGE`.
- Produces: a package contract that fails when `-Werror` or `/WX` appears in an installed Atlas target.

- [ ] **Step 1: Add the failing installed-interface assertion**

Immediately after the install succeeds in
`test/cmake/run_installed_consumer.cmake`, inspect every installed target export:

```cmake
file(
    GLOB_RECURSE atlas_installed_target_files
    "${ATLAS_PACKAGE_STAGE}/*[Tt]argets.cmake"
)
if(NOT atlas_installed_target_files)
    message(FATAL_ERROR "Atlas install produced no CMake target export")
endif()

foreach(atlas_target_file IN LISTS atlas_installed_target_files)
    file(READ "${atlas_target_file}" atlas_target_contents)
    if(atlas_target_contents MATCHES "(^|[ ;\"])(-Werror|/WX)([ ;\"]|$)")
        message(FATAL_ERROR
            "Installed Atlas target exports maintainer warnings-as-errors: "
            "${atlas_target_file}"
        )
    endif()
endforeach()
```

- [ ] **Step 2: Run the package contract and verify RED**

Run:

```bash
ctest --test-dir build/dev -R '^atlas_installed_consumer_contract$' --output-on-failure
```

Expected: FAIL with `Installed Atlas target exports maintainer warnings-as-errors`.

- [ ] **Step 3: Make warnings private to maintained targets**

In `test/CMakeLists.txt`, apply the same strict warning set independently to the
library and test executable:

```cmake
if(NOT TEST_USE_INSTALLED_VERSION)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${MODULE_NAME} PRIVATE -Wall -Wpedantic -Wextra -Werror)
        target_compile_options(${TEST_SUITE} PRIVATE -Wall -Wpedantic -Wextra -Werror)
    elseif(MSVC)
        target_compile_options(${MODULE_NAME} PRIVATE /W4 /WX)
        target_compile_options(${TEST_SUITE} PRIVATE /W4 /WX)
    endif()
endif()
```

In `cmake/CompilerWarnings.cmake`, keep the forbidden header-only branch
unchanged and change the concrete-library scope from `PUBLIC` to `PRIVATE`:

```cmake
if(${PROJECT_NAME_UPPERCASE}_BUILD_HEADERS_ONLY)
    target_compile_options(${PROJECT_NAME} INTERFACE ${PROJECT_WARNINGS})
else()
    target_compile_options(${PROJECT_NAME} PRIVATE ${PROJECT_WARNINGS})
endif()
```

- [ ] **Step 4: Reconfigure and verify GREEN**

Run:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --test-dir build/dev -R '^atlas_installed_consumer_contract$' --output-on-failure
```

Expected: configuration and build exit 0; the package contract passes.

- [ ] **Step 5: Commit the warning ownership fix**

```bash
git add test/cmake/run_installed_consumer.cmake test/CMakeLists.txt cmake/CompilerWarnings.cmake
git commit -m "fix: keep maintainer warnings private"
```

---

### Task 2: Bound Installed-Consumer Build Parallelism

**Files:**
- Modify: `test/cmake/run_installed_consumer.cmake`
- Modify: `test/CMakeLists.txt`
- Create: `test/cmake/check_consumer_build_jobs.cmake`

**Interfaces:**
- Consumes: optional CMake variable `ATLAS_CONSUMER_BUILD_JOBS`.
- Produces: a validated positive integer passed to `cmake --build --parallel`.

- [ ] **Step 1: Add a failing validation test**

Create `test/cmake/check_consumer_build_jobs.cmake`:

```cmake
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -DATLAS_CONSUMER_BUILD_JOBS=0
        -DATLAS_VALIDATE_CONSUMER_BUILD_JOBS_ONLY=ON
        -P "${ATLAS_CONSUMER_SCRIPT}"
    RESULT_VARIABLE validation_result
    OUTPUT_VARIABLE validation_output
    ERROR_VARIABLE validation_error
)

set(validation_log "${validation_output}\n${validation_error}")
if(validation_result EQUAL 0)
    message(FATAL_ERROR "Invalid consumer build job count was accepted")
endif()
if(NOT validation_log MATCHES
   "ATLAS_CONSUMER_BUILD_JOBS must be a positive integer")
    message(FATAL_ERROR
        "Invalid job count failed for the wrong reason:\n${validation_log}"
    )
endif()
```

Add a CTest entry beside `atlas_installed_consumer_contract`:

```cmake
add_test(
    NAME atlas_installed_consumer_rejects_invalid_jobs
    COMMAND
        ${CMAKE_COMMAND}
        -DATLAS_CONSUMER_SCRIPT=${CMAKE_CURRENT_LIST_DIR}/cmake/run_installed_consumer.cmake
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_consumer_build_jobs.cmake
)
```

- [ ] **Step 2: Run the validation test and verify RED**

Run:

```bash
cmake --preset dev
ctest --test-dir build/dev -R '^atlas_installed_consumer_rejects_invalid_jobs$' --output-on-failure
```

Expected: FAIL because the script does not yet validate
`ATLAS_CONSUMER_BUILD_JOBS`.

- [ ] **Step 3: Validate the option and use serial default**

At the beginning of `test/cmake/run_installed_consumer.cmake`, add:

```cmake
if(NOT DEFINED ATLAS_CONSUMER_BUILD_JOBS)
    set(ATLAS_CONSUMER_BUILD_JOBS 1)
endif()
if(NOT ATLAS_CONSUMER_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "ATLAS_CONSUMER_BUILD_JOBS must be a positive integer")
endif()
if(ATLAS_VALIDATE_CONSUMER_BUILD_JOBS_ONLY)
    return()
endif()
```

Replace the hard-coded `-j4` build argument with:

```cmake
--parallel "${ATLAS_CONSUMER_BUILD_JOBS}"
```

- [ ] **Step 4: Verify validation and the real serial contract**

Run:

```bash
cmake --preset dev
ctest --test-dir build/dev \
  -R '^atlas_installed_consumer_(rejects_invalid_jobs|contract)$' \
  --output-on-failure
```

Expected: both tests pass, and the contract build log shows serialized
compilation.

- [ ] **Step 5: Commit the parallelism fix**

```bash
git add test/cmake/run_installed_consumer.cmake test/cmake/check_consumer_build_jobs.cmake test/CMakeLists.txt
git commit -m "test: bound installed consumer parallelism"
```

---

### Task 3: Stop Draining Results After a Dead Transport

**Files:**
- Modify: `test/async/test_async_connection.cpp`
- Modify: `src/async/async_connection.cpp`

**Interfaces:**
- Consumes: `pg::errc::connection_failure` returned by `receive()`.
- Produces: `async_connection::execute()` returns that transport error promptly instead of awaiting another result from a dead socket.

- [ ] **Step 1: Strengthen the real-server regression test**

In the existing
`"send_query classifies a dead transport as connection_failure"` test, bound
the self-terminating query independently of the suite watchdog and require the
original transport classification:

```cpp
const auto started = std::chrono::steady_clock::now();
auto terminated = co_await atlas::with_timeout<atlas::pg::result>(
    2s, *conn, conn->execute("SELECT pg_terminate_backend(pg_backend_pid())", no_params));
const auto elapsed = std::chrono::steady_clock::now() - started;

expect(!terminated.has_value()) << "the backend survived pg_terminate_backend";
if (!terminated) {
    expect(terminated.error().code == errc::connection_failure)
        << "dead transport was hidden by the timeout watchdog";
}
expect(elapsed < 2s) << "execute tried to drain a dead transport";
```

- [ ] **Step 2: Run repeatedly and verify RED**

Run against PostgreSQL 18:

```bash
for run in {1..20}; do
  ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" \
    build/dev/bin/atlas_test_suite \
    || exit 1
done
```

Expected on the unfixed implementation: a run may report
`dead transport was hidden by the timeout watchdog` or
`execute tried to drain a dead transport`. The fresh Ubuntu Actions failure,
where the self-terminating query remained suspended until the 30-second
watchdog, is the authoritative RED evidence if the local scheduler does not
trigger the race.

- [ ] **Step 3: Stop only on non-drainable transport failures**

In `async_connection::execute()`, retain draining for SQL/result errors but
return promptly when the transport has failed:

```cpp
if (!result) {
    if (failure) {
        break;
    }
    failure = result.error();
    if (failure->code == pg::errc::connection_failure) {
        break;
    }
    continue;
}
```

This preserves the terminating-sentinel drain for reusable connections while
avoiding a second `receive()` on a dead socket.

- [ ] **Step 4: Verify GREEN repeatedly**

Rebuild and run the same 20-run PostgreSQL 18 loop:

```bash
cmake --build --preset dev
for run in {1..20}; do
  ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" \
    build/dev/bin/atlas_test_suite \
    || exit 1
done
```

Expected: all 20 runs exit 0 and each self-termination is classified as
`connection_failure` before two seconds.

- [ ] **Step 5: Commit the dead-transport fix**

```bash
git add test/async/test_async_connection.cpp src/async/async_connection.cpp
git commit -m "fix: stop draining dead async transports"
```

---

### Task 4: Replace the Pool Recovery Sleep With a Condition

**Files:**
- Modify: `test/async/async_test_support.hpp`
- Modify: `test/async/test_pool.cpp`

**Interfaces:**
- Produces: `atlas_test::wait_until(Predicate, timeout, interval) -> asio::awaitable<bool>`.
- Consumes: the pool's thread-safe `available()` observer.

- [ ] **Step 1: Add the desired condition-based use**

Replace the fixed 500-millisecond sleep in the server-termination pool test
with:

```cpp
const bool revived = co_await atlas_test::wait_until(
    [&db] { return db.available() == 1; }, 5s, 10ms);
expect(revived) << "the pool did not publish a replacement connection";
if (!revived) {
    co_return false;
}
```

- [ ] **Step 2: Build and verify RED**

Run:

```bash
cmake --build --preset dev
```

Expected: compilation fails because `atlas_test::wait_until` is not defined.

- [ ] **Step 3: Implement the minimal bounded polling helper**

Add to `test/async/async_test_support.hpp`:

```cpp
template <typename Predicate>
[[nodiscard]] auto wait_until(
    Predicate condition,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds interval = std::chrono::milliseconds{10})
    -> asio::awaitable<bool> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    asio::steady_timer timer{co_await asio::this_coro::executor};

    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return false;
        }
        timer.expires_after(interval);
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return true;
}
```

- [ ] **Step 4: Verify pool recovery repeatedly**

Run:

```bash
cmake --build --preset dev
for run in {1..20}; do
  ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" \
    build/dev/bin/atlas_test_suite \
    || exit 1
done
```

Expected: all 20 runs exit 0; the recovery test no longer relies on a fixed
500-millisecond delay.

- [ ] **Step 5: Commit the condition-based test**

```bash
git add test/async/async_test_support.hpp test/async/test_pool.cpp
git commit -m "test: wait for observable pool recovery"
```

---

### Task 5: Run the Local Verification Matrix

**Files:**
- Verify only; modify files only if a command exposes a defect covered by this plan.

**Interfaces:**
- Consumes: all changes from Tasks 1–4.
- Produces: fresh evidence for build, tests, formatting, package contract, sanitizers, and repository cleanliness.

- [ ] **Step 1: Verify the standard build and all CTest contracts**

```bash
cmake --preset dev
cmake --build --preset dev
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" \
  ctest --preset dev --output-on-failure
```

Expected: build exits 0 and CTest reports zero failures.

- [ ] **Step 2: Verify strict warnings**

```bash
cmake --preset warnings-as-errors
cmake --build --preset warnings-as-errors
```

Expected: both commands exit 0 with no compiler warnings promoted to errors.

- [ ] **Step 3: Verify sanitizers supported by the local platform**

```bash
cmake --preset asan
cmake --build --preset asan
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" \
  ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan
ATLAS_TEST_CONNINFO="$ATLAS_TEST_CONNINFO" \
  ctest --test-dir build/tsan --output-on-failure
```

Expected: supported configurations report zero failures. Any unsupported
platform/toolchain result is recorded verbatim rather than reported as passing.

- [ ] **Step 4: Verify formatting and the diff**

```bash
git diff --check origin/feat/async-engine...HEAD
git status --short
```

Run the repository's configured formatting target if its tool versions accept
the checked-in configuration:

```bash
cmake --build build/dev --target format
```

Expected: no whitespace errors and only intentional files are present.

- [ ] **Step 5: Review the complete patch**

```bash
git diff --stat origin/feat/async-engine...HEAD
git diff origin/feat/async-engine...HEAD
```

Expected: the patch contains only the design, package-policy, parallelism,
dead-transport, and condition-wait changes described above.

---

### Task 6: Publish and Verify GitHub Actions

**Files:**
- No new source files expected.

**Interfaces:**
- Consumes: verified commits on `feat/async-engine`.
- Produces: updated PR #6 with four passing workflows.

- [ ] **Step 1: Push the branch**

```bash
git push origin feat/async-engine
```

Expected: GitHub accepts the commits and updates PR #6.

- [ ] **Step 2: Watch all PR checks**

```bash
gh pr checks 6 --watch --interval 10
```

Expected: macOS, Ubuntu, Standalone, and Install all pass.

- [ ] **Step 3: Inspect any non-passing workflow before further edits**

```bash
gh pr checks 6
gh run view "$(gh run list --branch feat/async-engine --status failure --limit 1 --json databaseId --jq '.[0].databaseId')" --log-failed
```

Expected: no failed workflow. If one fails, return to systematic root-cause
investigation and do not stack speculative changes.

- [ ] **Step 4: Confirm final branch state**

```bash
git status --short --branch
git log --oneline origin/main..HEAD
gh pr view 6 --json url,title,body,headRefName,baseRefName
```

Expected: clean `feat/async-engine`, English PR title/body, base `main`, and the
CI-fix commits present.
