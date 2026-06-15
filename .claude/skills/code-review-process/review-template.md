# Code Review — Detailed Output Template

Read this file when you reach Step 5 of the `code-review-process` skill and are ready to write the review. It is kept out of `SKILL.md` so it loads only when you actually write the review.

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
