Review a SixSevenDB ticket or the current branch. Delegate to the **reviewer** subagent — it owns the full methodology, runs on its own model, and keeps its detailed work out of this conversation.

## Resolve the target

- If `$ARGUMENTS` names a ticket (`GDB-123`) or a PR, that is the target.
- If `$ARGUMENTS` is empty, target the current branch:
  - Get the branch: `git rev-parse --abbrev-ref HEAD`.
  - If it matches a ticket key (e.g. `GDB-123`), review that ticket against its Jira acceptance criteria.
  - Otherwise review the raw diff (`git diff --name-only main...HEAD`) on general code quality, with no Jira criteria. If the branch is `main` with no diff, tell the user there is nothing to review.

## Delegate

Hand the resolved target to the reviewer: `@agent-reviewer <target>`.

The reviewer reads only the PR diff, posts the detailed review to the PR (or returns it inline if no PR exists), and returns one compact verdict line. Surface that verdict line plus a link to the posted review; do not re-read the diff or files the reviewer already covered.
