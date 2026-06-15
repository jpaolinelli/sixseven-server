Run adversarial QA on a SixSevenDB ticket or the current branch. Delegate to the **qa-engineer** subagent — it owns the full methodology, runs on its own model, and keeps its detailed work out of this conversation.

## Resolve the target

- If `$ARGUMENTS` names a ticket (`GDB-123`), QA that ticket.
- If `$ARGUMENTS` is `column` / `drain` (or asks to work the QA column), drain the QA column: `project = GDB AND status = "QA" ORDER BY key ASC`, one ticket at a time.
- If `$ARGUMENTS` is empty, target the current branch:
  - Get the branch: `git rev-parse --abbrev-ref HEAD`.
  - If it matches a ticket key (e.g. `GDB-123`), QA that ticket.
  - Otherwise tell the user QA needs a ticket key (it files bugs into the ticket's epic) and ask which ticket the branch belongs to.

## Delegate

Hand the resolved target to the QA engineer: `@agent-qa-engineer <target>`.

The QA engineer reads only the PR diff, writes adversarial tests, runs them under ASan, files Bug tickets for Critical/High findings, posts the detailed QA report to the PR and Jira (or returns it inline if no PR exists), and returns one compact verdict line. Surface that verdict line plus any bug keys; do not re-read files or re-run tests the QA engineer already covered.
