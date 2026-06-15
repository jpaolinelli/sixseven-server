---
name: jira-planning
description: Use when creating epics, stories, and subtasks in Jira from a technical plan. Provides the ticket hierarchy, naming conventions, description templates, and exact API calls for the SixSevenDB project.
user-invocable: false
---

# Jira Planning

## Cloud ID

`d1c81655-b174-4ffc-9c84-3c76752eb094`

## Ticket Hierarchy

```
Epic (feature-level)
├── Story 1 (logical work unit)
│   ├── Subtask 1a (implementation step)
│   ├── Subtask 1b
│   └── Subtask 1c
├── Story 2
│   ├── Subtask 2a
│   └── Subtask 2b
└── Story 3
    └── ...
```

## Issue Types

| Type | ID | Hierarchy | Parent Field |
|------|----|-----------|-------------|
| Epic | 10113 | 1 (top) | none |
| Story | 10116 | 0 | `parent: "<EPIC-KEY>"` |
| Task | 10115 | 0 | `parent: "<EPIC-KEY>"` |
| Subtask | 10114 | -1 | `parent: "<STORY-KEY>"` |
| Bug | 10149 | 0 | optional |

## Creating an Epic

```
createJiraIssue:
  cloudId: "d1c81655-b174-4ffc-9c84-3c76752eb094"
  projectKey: "GDB"
  issueTypeName: "Epic"
  summary: "<Feature Name>"
  description: (see Epic template below)
```

### Epic Description Template

Read the Epic template in [`templates.md`](templates.md) when writing the description.

## Creating Stories

Stories are linked to their parent Epic:

```
createJiraIssue:
  cloudId: "d1c81655-b174-4ffc-9c84-3c76752eb094"
  projectKey: "GDB"
  issueTypeName: "Story"
  summary: "<Story Summary>"
  description: (see Story template below)
  parent: "<EPIC-KEY>"
```

### Story Description Template

Read the Story template in [`templates.md`](templates.md) when writing the description.

## Creating Subtasks

Subtasks are linked to their parent Story:

```
createJiraIssue:
  cloudId: "d1c81655-b174-4ffc-9c84-3c76752eb094"
  projectKey: "GDB"
  issueTypeName: "Subtask"
  summary: "<Subtask Summary>"
  description: (see Subtask template below)
  parent: "<STORY-KEY>"
```

### Subtask Description Template

Read the Subtask template in [`templates.md`](templates.md) when writing the description.

## Naming Conventions

### Epic Summary
Feature-level name. Short and descriptive.
- Good: `PostgreSQL COPY Protocol Support`
- Good: `Real-Time Dashboard Metrics`
- Bad: `Implement the thing we discussed`

### Story Summary
Logical unit of work. Describes what is delivered.
- Good: `Implement COPY FROM parser and data ingestion`
- Good: `Add buffer pool hit rate chart to dashboard`
- Bad: `Part 1 of COPY`

### Subtask Summary
Individual implementation step. Specific and actionable.
- Good: `Add COPY FROM token to lexer and parser`
- Good: `Create BufferPoolChart component with recharts`
- Bad: `Write code`

## Sizing Guidelines

- **Epic**: 1-4 weeks of total work.
- **Story**: 1-3 days of work. Should be independently deliverable and testable.
- **Subtask**: A few hours of work. One focused implementation step.

If a story has more than 5-6 subtasks, consider splitting it into two stories.

## Workflow After Creation

After creating all tickets, present the full hierarchy to the user:

```
# <EPIC-KEY> — <Epic Summary>

## Stories
### <STORY-KEY> — <Story Summary>
  - <SUBTASK-KEY> — <Subtask Summary>
  - <SUBTASK-KEY> — <Subtask Summary>

### <STORY-KEY> — <Story Summary>
  - <SUBTASK-KEY> — <Subtask Summary>
```

Include links to each ticket for easy navigation.
