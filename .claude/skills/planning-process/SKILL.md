---
name: planning-process
description: Use when planning a new feature or project. Provides the step-by-step methodology for gathering requirements, designing a technical approach, iterating with stakeholders, and producing a detailed plan.
user-invocable: false
---

# Planning Process

## Philosophy

Good planning prevents wasted implementation effort. The goal is to produce a detailed, unambiguous plan that an implementer can follow without guessing. Every design decision should be explicitly stated, every trade-off acknowledged, and every acceptance criterion testable.

## Phase 1: Requirements Gathering

Ask the stakeholder (user) clarifying questions. Do not assume — ask. Cover:

- **What**: What should the feature do? What is the expected user-facing behavior?
- **Why**: What problem does this solve? What is the motivation?
- **Scope**: What is explicitly in scope? What is explicitly out of scope?
- **Constraints**: Performance requirements, compatibility needs, deadline pressures?
- **Dependencies**: Does this depend on other features or tickets? Does anything depend on this?
- **Non-goals**: What should this feature deliberately NOT do?

Use `AskUserQuestion` to get answers. Do not proceed to design until requirements are clear.

### Output

A requirements summary with:
- Feature name and one-sentence description
- Detailed requirements (bulleted)
- Constraints and non-goals
- Open questions (if any remain)

## Phase 2: Research

Explore the existing codebase to understand the landscape:

1. **Relevant modules**: Which modules will be affected? Read their current implementation.
2. **Existing patterns**: How does the codebase handle similar features? Reuse patterns, don't invent new ones.
3. **Prior art**: Has something similar been attempted before? Check git history and existing tickets.
4. **Integration points**: Where does the new feature connect to existing code? What interfaces exist?
5. **Test patterns**: How are similar features tested? What test infrastructure exists?

Use the `sixseven-architecture` skill to understand the module structure.

### Output

A research summary with:
- Affected modules and files
- Existing patterns to reuse
- Integration points and interfaces
- Risks or concerns discovered

## Phase 3: Design

Propose a technical approach. Include:

1. **Architecture**: High-level component design, data flow, and module interactions.
2. **API / Interface contracts**: Function signatures, types, data structures.
3. **Data model**: New tables, columns, types, or schema changes (if applicable).
4. **Error handling**: What can go wrong and how will it be handled?
5. **Testing strategy**: What tests will verify this works? What adversarial scenarios exist?
6. **Trade-offs**: What alternatives were considered? Why was this approach chosen?
7. **Risks**: What could go wrong during implementation? What are the unknowns?

### Design Principles

- **Minimize blast radius**: Prefer changes that touch fewer modules.
- **Follow existing patterns**: Don't introduce new abstractions when existing ones work.
- **Design for testability**: Every behavior should be testable in isolation.
- **Keep it simple**: The simplest design that meets requirements is the best design.
- **Explicit over implicit**: Prefer clear, verbose code over clever, compact code.

### Output

A design document with sections for each item above.

## Phase 4: Stakeholder Review

Present the design to the user. Specifically:

1. Summarize the approach in plain language.
2. Call out key design decisions and why they were made.
3. Highlight trade-offs and alternatives considered.
4. Flag any risks or concerns.
5. Ask for explicit approval or feedback.

Use `AskUserQuestion` for specific decision points. Iterate until the user approves the design.

## Phase 5: Plan Document

Write the final plan. Use this template:

```markdown
# <Feature Name> — Technical Plan

## Summary
One paragraph describing what this feature does and why.

## Requirements
- Requirement 1
- Requirement 2
- ...

## Non-Goals
- What this feature deliberately does NOT do.

## Technical Design

### Architecture
How the feature fits into the system. Component diagram or description.

### Components
| Component | Location | Purpose |
|-----------|----------|---------|
| name | file path | what it does |

### Data Flow
Step-by-step data flow through the system.

### API / Interfaces
Key function signatures, types, and contracts.

### Error Handling
How errors are handled at each layer.

## Work Breakdown
| Story | Description | Subtasks |
|-------|-------------|----------|
| Story 1 | What it delivers | Sub A, Sub B, Sub C |
| Story 2 | What it delivers | Sub D, Sub E |

## Testing Strategy
- Unit tests: what will be tested
- Integration tests: end-to-end scenarios
- Edge cases: adversarial scenarios

## Risks
- Risk 1: description and mitigation
- Risk 2: description and mitigation

## Acceptance Criteria
- [ ] Criterion 1
- [ ] Criterion 2
```

## Phase 6: Ticket Creation

Once the plan is approved, create Jira tickets using the `jira-planning` skill.

Break the plan into:
- **1 Epic** — the feature as a whole
- **N Stories** — logical units of work (each independently deliverable and testable)
- **M Subtasks per Story** — individual implementation steps

Every ticket must have acceptance criteria. See the `jira-planning` skill for structure and conventions.
