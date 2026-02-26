---
name: qa-process
description: Use when performing QA on a ticket. Provides the methodology for verifying implementations work correctly, adversarial testing to find bugs, sanitizer runs, and the process for filing bug tickets.
user-invocable: false
---

# QA Process

## Philosophy

QA is not code review. Code review reads the code and checks quality. QA **runs** the code and tries to **break** it. Your goal is to find every bug, crash, edge case failure, and correctness issue before the code reaches production.

Think like an adversary. Every function is guilty until proven innocent.

## Step 1: Understand the Ticket

Read the parent ticket and all subtasks. Extract every acceptance criterion as a discrete, testable claim. For each criterion, think about:

- What inputs would make this fail?
- What boundary conditions exist?
- What happens when things go wrong (disk full, null input, concurrent access)?

## Step 2: Build and Run Existing Tests

Run the full build and test suite:

```bash
export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset default && cmake --build build/debug --target giodb_unit_tests && ctest --test-dir build/debug --output-on-failure
```

Record the results. If existing tests fail, that is an immediate QA finding.

## Step 3: Read the Implementation

Read every file added or modified for the ticket:

- **Headers**: `include/giodb/<module>/*.h`
- **Implementations**: `src/<module>/*.cpp`
- **Tests**: `tests/unit/test_*.cpp`

Read with a tester's eye — look for:

- **Unvalidated inputs**: Functions that trust their arguments without checking.
- **Missing error paths**: `Result<T>` returns that assume success.
- **Off-by-one errors**: Loop bounds, index calculations, size comparisons.
- **Integer overflow/underflow**: Arithmetic on sizes, offsets, or counts.
- **Null/empty handling**: What happens with empty strings, empty vectors, nullopt?
- **Resource leaks**: Pins not unpinned, files not closed on error paths.
- **State corruption**: Partial writes that leave data structures inconsistent on failure.
- **Concurrency issues**: Shared state accessed without proper locking.

## Step 4: Write Adversarial Tests

This is the core of QA. Write new test cases designed to **break** the implementation. Create a test file `tests/unit/test_qa_<ticket_key>.cpp` (lowercase ticket key, e.g., `test_qa_gdb_42.cpp`).

### Categories of Adversarial Tests

**Boundary values:**
- Zero, one, max values for numeric inputs.
- Empty strings, single-character strings, very long strings.
- Empty containers, single-element containers.
- INT32_MIN, INT32_MAX, UINT64_MAX, etc.

**Null and missing data:**
- NULL values in every column position.
- Queries against empty tables.
- Operations on non-existent tables, columns, or indices.

**Type coercion edge cases:**
- Inserting strings where numbers are expected.
- Mixing signed and unsigned comparisons.
- Decimal precision loss.

**Error path verification:**
- Confirm the correct `StatusCode` is returned for each error case.
- Verify error messages are informative (not empty, not generic).
- Ensure errors don't leave side effects (partial inserts, corrupted state).

**Sequence and ordering:**
- Operations in unexpected order (close before open, next after close).
- Repeated operations (open twice, close twice).
- Interleaved operations across multiple iterators.

**Scale and stress:**
- Insert many rows (1000+), then query.
- Wide rows with many columns.
- Deeply nested expressions.
- Very long SQL statements.

**SQL edge cases (if applicable):**
- SELECT with no rows matching WHERE.
- UPDATE/DELETE with no matching rows.
- INSERT with duplicate keys (if constraints exist).
- Expressions with division by zero.
- Comparisons between incompatible types.

### Test Quality Rules

- Every test must have a clear name describing the adversarial scenario.
- Every test must assert a specific expected outcome — not just "doesn't crash."
- Group related adversarial tests under a descriptive suite name: `QA_<Component>`.

### Register the Test File

Add the new test file to `tests/unit/CMakeLists.txt`.

## Step 5: Run Tests with AddressSanitizer

Build and run with ASan to catch memory bugs:

```bash
export VCPKG_ROOT="$HOME/vcpkg" && cmake --preset asan
cmake --build build/asan --target giodb_unit_tests
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./build/asan/tests/unit/giodb_unit_tests
```

ASan detects: buffer overflows, use-after-free, memory leaks, stack overflows, double-free. Any ASan finding is a QA bug.

## Step 6: Verify Acceptance Criteria

For each acceptance criterion, trace through the code path that satisfies it:

1. Identify the test(s) that exercise this criterion.
2. Confirm the test actually validates the criterion (not just a superficial check).
3. If no test covers a criterion, write one.
4. Run the specific test and verify it passes.

Build the criteria table:

| Criterion | Test(s) | Verified | Notes |
|-----------|---------|----------|-------|
| description | test names | PASS / FAIL / UNTESTED | details |

## Step 7: Compile Findings

Classify every finding by severity:

- **Critical**: Crash, data corruption, memory safety violation (ASan), incorrect query results.
- **High**: Missing error handling that could cause silent data loss, acceptance criterion not met.
- **Medium**: Edge case failures, missing validation, inconsistent error messages.
- **Low**: Minor behavioral quirks, missing edge case tests, cosmetic issues.

## Step 8: File Bug Tickets

For every Critical or High finding, create a Jira bug ticket:

```
Project: GDB
Type: Bug
Summary: [QA] <Component>: <Short description of the bug>
Description:
  ## Found During
  QA of <PARENT-TICKET-KEY>

  ## Description
  <Clear description of the bug>

  ## Steps to Reproduce
  1. <step>
  2. <step>

  ## Expected Behavior
  <what should happen>

  ## Actual Behavior
  <what actually happens>

  ## Severity
  Critical / High / Medium

  ## Test Case
  <test name in test_qa_<ticket>.cpp that demonstrates the bug>
```

For Medium findings, include them in the QA report and let the user decide whether to file tickets.

## Step 9: QA Report Format

```
# <TICKET-ID> — <Summary> — QA Report

## Build & Test Status
- Build: PASS / FAIL
- Existing tests: X/Y pass
- ASan: CLEAN / <N> findings

## Adversarial Tests Written
| Test Suite | Test Name | Result | Category |
|------------|-----------|--------|----------|
| QA_Component | BoundaryZeroInput | PASS | boundary |
| QA_Component | NullInEveryColumn | FAIL | null handling |

## Acceptance Criteria Verification
| Criterion | Test(s) | Status | Notes |
|-----------|---------|--------|-------|
| ... | ... | PASS/FAIL/UNTESTED | ... |

## Findings
### 1. <Title> — **Critical/High/Medium/Low**
- **File**: `path/to/file.cpp:line`
- **Description**: What is wrong.
- **Reproduction**: Test name or steps.
- **Bug ticket**: GDB-XXX (if filed)

## Verdict: QA PASS / QA FAIL
- **QA PASS**: All acceptance criteria verified, no Critical/High findings, ASan clean.
- **QA FAIL**: Any Critical or High finding, or unverified acceptance criteria.

## Bug Tickets Filed
- GDB-XXX: <summary>
- GDB-YYY: <summary>
```

## Verdict Rules

- **QA PASS**: All acceptance criteria verified with passing tests. No Critical or High findings. ASan clean. Medium findings are noted but do not block.
- **QA FAIL**: Any Critical or High finding. Any acceptance criterion that cannot be verified. Any ASan violation.

## Ticket Transitions

- **QA PASS**: Transition the ticket to "Done".
- **QA FAIL**: Leave the ticket in its current status. The filed bug tickets and QA report communicate what needs fixing.
