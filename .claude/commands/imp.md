Implement a SixSevenDB Jira ticket end to end. Delegate to the **implementer** subagent — it owns the full implementation workflow, runs on its own model in an isolated worktree, and keeps its detailed work out of this conversation.

## Resolve the ticket

- If `$ARGUMENTS` names a ticket (`GDB-123`), implement it.
- If `$ARGUMENTS` is empty, derive the ticket from the current branch name (`git rev-parse --abbrev-ref HEAD`); branches are named after their ticket. If the branch is `main` or has no ticket key, ask the user which ticket to implement — do not guess.

## Delegate

Hand the resolved ticket to the implementer: `@agent-implementer <ticket>`.

The implementer creates the branch, writes the C++ code and unit tests, runs the quality gates, commits, opens a PR, and returns one compact line (`IMPL …`). The full description lives in the PR body. Surface the compact line plus the PR link; do not re-read the diff it already produced.
