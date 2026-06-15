Run the full implement → review → QA pipeline for ticket: $ARGUMENTS

Each phase runs as an isolated subagent so the main conversation stays clean.

## Workflow

### Phase 1: Implementation
Delegate to the **implementer** subagent: `@agent-implementer $ARGUMENTS`

The implementer runs in an isolated git worktree, creates the branch, writes code and tests, runs quality gates, commits, and opens a PR. Wait for it to complete and capture its structured summary **including the `HANDOFF:` line** (files touched, key functions, diff summary, new test names, risk areas).

If the implementer reports issues or quality-gate failures that block progress, **stop the pipeline** and surface to the user.

### Phase 2: Code Review
Delegate to the **reviewer** subagent: `@agent-reviewer $ARGUMENTS` — and **pass the implementer's `HANDOFF:` line in the delegation prompt** so the reviewer starts from the diff instead of re-exploring the tree.

The reviewer runs build, tests, clang-format, clang-tidy, cross-checks acceptance criteria, and returns an APPROVED or CHANGES REQUESTED verdict.

**If verdict is CHANGES REQUESTED:**
- Re-delegate to the implementer with the review feedback as context: `@agent-implementer fix the following review issues on branch <branch>: <issues>`
- Re-run the reviewer. The re-review is **scoped** (v2+): pass the prior reviewed SHA and the blocking-issue list so the reviewer only inspects the delta and re-runs tests, rather than a full from-scratch pass.
- Maximum 2 fix-and-review cycles. If still CHANGES REQUESTED after 2 cycles, stop and surface to the user.

**If verdict is APPROVED:** continue to Phase 3.

### Phase 3: QA
Delegate to the **qa-engineer** subagent: `@agent-qa-engineer $ARGUMENTS` — and **pass the implementer's `HANDOFF:` line** so QA targets the actual diff and attack surface.

The QA engineer writes adversarial tests, runs them under AddressSanitizer, files Bug tickets for Critical/High findings, and returns a QA PASS or QA FAIL verdict. QA does **not** transition the ticket to Done — that happens only after merge.

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

## Parallelizing Review + QA

After implementation, review and QA are independent reads of the same branch and can run concurrently. Default to running them **in parallel** when BOTH hold:
- The implementer reported `gate:PASS` with `blockers:none`, and
- The diff is small/low-risk (roughly a handful of files, no cross-module or architectural change per the HANDOFF `risk` field).

Launch both subagents in a single message (two tool calls). Roughly halves the review+QA wall-clock.

Run them **sequentially** (review first, then QA only if APPROVED) when the diff is large, risky, or touches multiple modules — there, a CHANGES REQUESTED verdict would waste the QA pass. If a parallel review comes back CHANGES REQUESTED, discard that round's QA result, run the fix-and-review cycle, then re-run QA on the fixed branch.

`--parallel` / `--sequential` flags in `$ARGUMENTS` force the choice and override the heuristic.

## Notes

- Each subagent operates in its own context window; only its compact return line (plus the implementer's HANDOFF line) flows back here. Detailed reviews and QA reports are posted to the PR and Jira, never surfaced into this conversation. Do not re-read PRs, diffs, or files a subagent already summarized; trust the compact block.
- Thread the implementer's HANDOFF line into both the review and QA delegations so they do not re-explore the tree.
- Build dirs are shared and incremental across phases — subagents must not wipe or re-configure `build/` (see the `quality-checks` skill).
- If any subagent asks a clarifying question, surface it to the user immediately and pause the pipeline.
