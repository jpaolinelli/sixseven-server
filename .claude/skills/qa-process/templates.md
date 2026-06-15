# QA Process — Templates

Read this file when you reach the bug-filing step (Step 9) or the report step (Step 11) of the `qa-process` skill. These templates are kept out of `SKILL.md` so they load only when you actually need them.

## Bug Ticket Template (Step 9)

For every Critical/High finding, create a Jira `Bug` ticket in the **same epic as the ticket under review**:

```
Project: GDB
Type: Bug
Epic: <same epic as the ticket under review>
Summary: [BUG][<Severity>] <Component>: <Short description of the bug>
Description:
  ## Found During
  QA of <TICKET-UNDER-REVIEW>

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

## QA Report Format (Step 11)

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

> This detailed report is posted to the PR and the Jira ticket. The pipeline receives only the compact one-line `QA …` block defined in the qa-engineer agent, never this report.
