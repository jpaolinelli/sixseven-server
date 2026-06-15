---
name: code-review-process
description: Use when performing a code review. Provides the structured review methodology — how to cross-check acceptance criteria, evaluate code quality, assess test coverage, and produce the final review verdict.
user-invocable: false
---

# Code Review Process

## Step 1: Read All Source Files

Get the exact list of files the ticket changed, then read only those:

```bash
git diff --name-only main...HEAD
```

The changed files will be among:

- **Headers**: `include/sixseven/<module>/*.h`
- **Implementations**: `src/<module>/*.cpp`
- **Tests**: `tests/unit/test_*.cpp`
- **Build files**: `CMakeLists.txt`

Read every changed file completely. Do not skim or skip. Do not read files outside the diff to "get context"; the diff is the review scope.

## Step 2: Acceptance Criteria Cross-Check

For the parent ticket and every subtask, create a table:

| Criterion | Status | Evidence |
|-----------|--------|----------|
| description | ✅ / ⚠️ Low / ⚠️ Medium / ❌ High | file, function, or test name |

**Severity levels:**
- **✅** — Fully met.
- **⚠️ Low** — Minor gap, cosmetic, or reasonable phase deferral.
- **⚠️ Medium** — Should be fixed but not blocking (duplication, missing edge case).
- **❌ High** — Criterion not met, correctness bug, or missing required functionality.

Every single criterion must appear in the table. Never omit one.

## Step 3: Code Quality Evaluation

Check for:

- **Correctness**: Logic errors, off-by-one, missing error handling, resource leaks.
- **Error handling**: All `Result<T>` returns checked. No silent failures.
- **Naming**: `snake_case` functions/variables, `PascalCase` types, `UPPER_CASE` constants.
- **Headers**: `#pragma once`, correct include order.
- **Duplication**: Identical logic that should be extracted to a shared utility.
- **NULL/edge cases**: Proper NULL propagation, empty input handling, boundaries.
- **Thread safety**: Proper locking for shared state.
- **Memory**: No raw owning pointers, no leaks in error paths.
- **Consistency**: New code matches existing codebase patterns.

## Step 4: Test Quality Evaluation

- **Coverage**: Every new public function has at least one test. Every branch in non-trivial logic is tested.
- **Meaningful assertions**: Every test asserts something substantive. Flag empty or trivial tests.
- **Edge cases**: Empty inputs, NULLs, error conditions, boundary values tested.
- **Integration**: At least one end-to-end test through the full SQL pipeline.
- **Tests** DO NOT RUN TESTS, just verify they exist and are of high quality.

## Step 5: Review Output Format (detailed, post to the PR)

This detailed review goes in a PR comment (and a Jira comment on approval). The pipeline receives only the compact one-line block defined in the reviewer agent, never this table.

```
# <TICKET-ID> — <Summary> — v<N> Review

## Build & Test Status
✅, formatting/tidy status

## Files Reviewed
| Category | Files | Lines |
|----------|-------|-------|
| Headers  | N     | N     |
| Implementation | N | N   |
| Tests    | N     | N     |
| Total    | N     | N     |

## Acceptance Criteria Cross-Check

### <TICKET-ID> (Parent)
| Criterion | Status | Evidence |

### <SUBTASK-ID> — <Summary>
| Criterion | Status | Evidence |

## Architecture Assessment
Separation of concerns, extensibility, consistency with existing patterns.

## Issues Found
### 1. <Title> — **Severity**
Description, file/line location, suggested fix.

## Verdict: ✅ APPROVED / ❌ CHANGES REQUESTED
Summary justification.
```

## Verdict Rules

- **APPROVED**: All criteria ✅ or ⚠️ Low. No High or Medium issues.
- **CHANGES REQUESTED**: Any ❌ High, or multiple ⚠️ Medium that collectively warrant fixes.
- Low severity and reasonable phase deferrals do not block approval.

## Re-Review

- First review = **v1**, subsequent = **v2**, **v3**, etc.
- On re-review: verify previously reported issues are fixed, check for regressions.
- If APPROVED, transition ticket to QA with the PR summary as a comment.
