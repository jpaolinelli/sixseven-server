# Jira Planning — Description Templates

Read this file when you are ready to write epic/story/subtask descriptions in the `jira-planning` skill. These templates are kept out of `SKILL.md` so they load only when you actually create tickets.

## Epic Description Template

```markdown
## Summary
<One paragraph describing the feature and its purpose.>

## Requirements
- Requirement 1
- Requirement 2
- ...

## Non-Goals
- What this does NOT include.

## Technical Approach
<Brief description of the architecture and design decisions.>

## Acceptance Criteria
- [ ] Criterion 1
- [ ] Criterion 2
- [ ] All stories completed and passing tests
- [ ] No regressions in existing tests

## Stories
- <STORY-1-SUMMARY>
- <STORY-2-SUMMARY>
- ...
```

## Story Description Template

```markdown
## Summary
<What this story delivers. Should be independently testable.>

## Context
Part of [<EPIC-KEY>](https://undiscoveredtech.atlassian.net/browse/<EPIC-KEY>).

## Implementation Details
- What files/modules are affected
- Key design decisions for this story
- Dependencies on other stories (if any)

## Acceptance Criteria
- [ ] Criterion 1
- [ ] Criterion 2
- [ ] Unit tests written and passing
- [ ] No regressions

## Subtasks
- <Subtask 1 summary>
- <Subtask 2 summary>
```

## Subtask Description Template

```markdown
## Summary
<Specific implementation step.>

## Acceptance Criteria
- [ ] Criterion 1
- [ ] Criterion 2
```
