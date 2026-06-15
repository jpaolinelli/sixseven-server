---
name: jira-workflow
description: Use when you need to interact with Jira — fetching tickets, reading acceptance criteria, transitioning issue status, or finding subtasks. Provides the exact procedures for working with the SixSevenDB Jira project.
user-invocable: false
---

# Jira Workflow

## Cloud ID

The Atlassian cloud ID for the SixSevenDB project is: `d1c81655-b174-4ffc-9c84-3c76752eb094`

Use this for all Jira API calls. It is constant — never call `getAccessibleAtlassianResources` to rediscover it.

## Do trivial Jira ops inline — do not spawn a subagent for them

A status read or a transition is two or three MCP calls. Spawning a separate agent to do it costs far more in fixed overhead (system prompt, tool-schema loading, skill text) than the work itself. If you already have the Jira MCP tools available in your context (e.g. the `/start` orchestrator), make these calls directly. Only delegate to a subagent when the task also needs code reasoning.

## Standard queue / search JQL (copy verbatim)

- Next To Do ticket in the queue:
  `project = GDB AND assignee = currentUser() AND status = "To Do" AND issuetype NOT IN (Epic, Subtask) ORDER BY rank ASC`
- My In-Progress tickets (resume check):
  `project = GDB AND assignee = currentUser() AND status = "In Progress" AND issuetype NOT IN (Epic, Subtask) ORDER BY rank ASC`
- Tickets awaiting QA:
  `project = GDB AND status = "QA" ORDER BY key ASC`

## Known transition IDs (verify with getTransitionsForJiraIssue if a call fails)

These have been stable for the GDB workflow; use them to skip a lookup, but fall back to `getTransitionsForJiraIssue` if a transition errors (board config can change):
- To "In Progress": transition ID `21`

## Fetching a Ticket

1. Extract the ticket key from user input (e.g. `GDB-22` from a URL or bare key).
2. Fetch the parent ticket using `getJiraIssue` with the cloud ID and issue key.
3. Find subtasks by searching with JQL: `parent = <TICKET-KEY> ORDER BY key ASC` using `searchJiraIssuesUsingJql`.
4. Fetch each subtask's full details with `getJiraIssue`.

## Reading Acceptance Criteria

Acceptance criteria are found in the ticket description under these headings:
- "Acceptance Criteria"
- "AC"
- "Definition of Done"
- Bullet-pointed requirements

Extract every criterion as a discrete checkable item. Never skip or summarize criteria.

## Transitioning Ticket Status

1. First get available transitions: `getTransitionsForJiraIssue` with the issue key.
2. Find the transition ID matching the target status name (e.g. "In Progress", "In Review", "Done").
3. Execute the transition: `transitionJiraIssue` with the issue key and transition ID.

Common status flow: `To Do` → `In Progress` → `In Review` → `Done`

**A ticket must not move to `Done` until its PR is merged.** "Done" means merged-to-main, not just QA-passed. Only the merge step (the `/start` orchestrator after a successful squash-merge) transitions a ticket to `Done`. Subagents in the pipeline (implementer, reviewer, QA) must never set `Done` themselves.

## Displaying Ticket Information

When presenting ticket information, use this format:

```
# <TICKET-KEY> — <Summary>
**Status**: <status>

## Acceptance Criteria
- [ ] criterion 1
- [ ] criterion 2

## Subtasks
### <SUBTASK-KEY> — <Summary> [<status>]
**Acceptance Criteria:**
- [ ] criterion 1
```
