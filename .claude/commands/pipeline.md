Run the full implement → review → QA pipeline for ticket: $ARGUMENTS

Each phase runs as an isolated subagent so the main conversation stays clean.

## Workflow

### Phase 1: Implementation
Delegate to the **implementer** subagent: `@agent-implementer $ARGUMENTS`

The implementer runs in an isolated git worktree, creates the branch, writes code and tests, runs quality gates, commits, and opens a PR. Wait for it to complete and capture its structured summary.

If the implementer reports issues or quality-gate failures that block progress, **stop the pipeline** and surface to the user.

### Phase 2: Code Review
Delegate to the **reviewer** subagent: `@agent-reviewer $ARGUMENTS`

The reviewer runs build, tests, clang-format, clang-tidy, cross-checks acceptance criteria, and returns an APPROVED or CHANGES REQUESTED verdict.

**If verdict is CHANGES REQUESTED:**
- Re-delegate to the implementer with the review feedback as context: `@agent-implementer fix the following review issues on branch <branch>: <issues>`
- Re-run the reviewer (v2)
- Maximum 2 fix-and-review cycles. If still CHANGES REQUESTED after 2 cycles, stop and surface to the user.

**If verdict is APPROVED:** continue to Phase 3.

### Phase 3: QA
Delegate to the **qa-engineer** subagent: `@agent-qa-engineer $ARGUMENTS`

The QA engineer writes adversarial tests, runs them under AddressSanitizer, files Bug tickets for Critical/High findings, and returns a QA PASS or QA FAIL verdict.

### Phase 4: Final Report
Present a consolidated summary to the user:

```
## Pipeline Report — $ARGUMENTS

### Implementation
- Branch: <name>
- PR: <url>
- Status: <complete/issues>

### Review
- Verdict: APPROVED (v<N>)
- Issues fixed: <N>

### QA
- Verdict: QA PASS / QA FAIL
- Bugs filed: <list>

### Final Status
<all phases passed / action needed>
```

## Notes

- Run phases sequentially by default. The user may request `--parallel` to run review and QA simultaneously after implementation.
- Each subagent operates in its own context window; only its compact return line flows back here. Detailed reviews and QA reports are posted to the PR and Jira, never surfaced into this conversation. Do not re-read PRs, diffs, or files a subagent already summarized; trust the compact block.
- If any subagent asks a clarifying question, surface it to the user immediately and pause the pipeline.
