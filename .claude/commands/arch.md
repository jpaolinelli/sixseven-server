Plan and scope a SixSevenDB feature into a Jira hierarchy. Delegate to the **architect** subagent — it owns requirements gathering, design, and ticket creation.

## Resolve the request

- If `$ARGUMENTS` describes a feature, hand it over as the starting brief.
- If `$ARGUMENTS` is empty, the architect opens by asking what to build (what, why, scope, constraints, non-goals) before designing anything.

## Delegate

Hand the request to the architect: `@agent-architect <request>`.

The architect asks clarifying questions, researches affected modules, proposes a design with trade-offs, gets your approval, then creates the epic → stories → subtasks with acceptance criteria and presents the linked hierarchy. It does not write implementation code or create branches.
