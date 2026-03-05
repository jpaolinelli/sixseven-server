---
name: jira-workflow
description: Use when you need to interact with Jira — fetching tickets, reading acceptance criteria, transitioning issue status, or finding subtasks. Provides the exact procedures for working with the SixSevenDB Jira project.
user-invocable: false
---

# Jira Workflow

## Cloud ID

The Atlassian cloud ID for the SixSevenDB project is: `d1c81655-b174-4ffc-9c84-3c76752eb094`

Use this for all Jira API calls.

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
