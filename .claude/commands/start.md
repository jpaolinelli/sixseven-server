Start the autonomous ticket-queue worker: drain every TODO Jira ticket assigned to Joseph through /pipeline, merging approved work, until the queue is empty.

Invoke the Skill tool now with `skill: "loop"` and **no interval** (self-paced), passing the entire **Loop Prompt** section below, verbatim, as the args. Do not paraphrase or shorten it.

## Loop Prompt

You are the autonomous ticket-queue worker for SixSevenDB. Each time this prompt fires, advance the queue by exactly one step, then either schedule the next wakeup or end the loop. Every iteration must be self-contained: re-derive the current state, do not assume memory of prior iterations.

### Step 0 — State check (cheap, do this first)
1. If a /pipeline run or its subagents from a previous iteration are still in flight, do NOT start anything new. Schedule a long wakeup (1800s) and end this iteration.
2. Otherwise check for a ticket left "In Progress" by a prior iteration with an open PR: resume it at Step 4 instead of pulling new work.

### Step 1 — Pick the next ticket
Use the jira-workflow skill. Search with JQL:

```
project = GDB AND assignee = currentUser() AND status = "To Do" AND issuetype NOT IN (Epic, Subtask) ORDER BY rank ASC
```

Take the first result. If no tickets remain, post a final summary of everything merged this run and END THE LOOP (do not schedule another wakeup).

### Step 2 — Start the ticket
Transition the ticket to "In Progress" (jira-workflow skill: getTransitionsForJiraIssue, then transitionJiraIssue).

### Step 3 — Run the pipeline
Run /pipeline <TICKET-KEY> and wait for it to finish completely. While waiting, sleep with long wakeups (1800s); never poll on short intervals.

### Step 4 — Merge gate
Merge ONLY if all of these are true from the pipeline report:
- Review verdict: APPROVED
- QA verdict: QA PASS
- The PR exists and GitHub reports it mergeable

If all gates pass:
1. `gh pr merge <PR-number> --squash --delete-branch`
2. Verify both the remote and local ticket branches are deleted
3. `git checkout main && git pull`
4. Confirm the ticket ends in "Done"; transition it if the pipeline did not
5. Report: ticket key, PR link, merge commit, branches cleaned, tickets remaining in queue

If any gate fails (CHANGES REQUESTED after the pipeline's retry budget, QA FAIL, merge conflict, unmergeable PR): do NOT merge. Leave the ticket "In Progress", report exactly what failed and why, ask me how to proceed, and do not pull new tickets until I answer.

### Step 5 — Continue
After a successful merge, schedule the next wakeup (60s is fine here since the next iteration starts immediately) and repeat from Step 0.

### Token discipline (hard requirements)
- One ticket and one pipeline at a time. Never parallelize tickets.
- Delegate mechanical work (Jira reads and transitions, PR status checks, branch cleanup verification) to the cheapest model that can do it via the Agent tool `model` override: prefer `haiku`, then `sonnet`. Reserve `fable` or `opus` for genuinely complex implementation or debugging only.
- Keep orchestration turns short: no exploratory reading, no redundant builds, no re-verifying what the pipeline already verified. Trust the pipeline's structured report.
- While anything is running in the background, wakeups are 1800s minimum.
- If you notice signs of rate limiting or usage limits, stop immediately and tell me rather than retrying.

### Safety rules
- Never force push. Never merge with a failing gate. Never amend commits.
- If main is broken after a merge (build or tests fail), stop the loop and tell me immediately.
- If anything is ambiguous (ticket selection, missing Jira transition, unexpected repo state, flaky test), stop and ask me a question instead of guessing.
