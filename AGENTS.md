## Agent skills

### Issue tracker

Issues live in the `myn/deskhopplus` GitHub Issues, managed with the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, used verbatim as label strings. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

## Post-Implementation Adversarial Review

For every non-trivial implementation, `deskhopplus-adversarial-review` is the final review gate.

Run it only after `mattpocock-skills:code-review` has completed, and before reporting the implementation complete. It supplements TDD and normal code review; it does not replace or repeat either.

Required order:

1. `mattpocock-skills:implement`
2. `mattpocock-skills:tdd`
3. implementation
4. `mattpocock-skills:code-review`
5. `deskhopplus-adversarial-review`

## Keeping Track of the Work

- If an issue is **100% complete**, including all required validation and testing including a green CI build, **close the ticket**.
- **Before closing a ticket**, document any important context, decisions, discoveries, or learnings from the work that could help another agent session understand what was done or pick up where you left off.
- If the work is technically complete but requires **human validation** (for example, hardware testing, visual verification, or helper proofs), **do not close the ticket**. Instead, tag it as **needs-hardware-validation** and **ready-for-human** and clearly document what needs to be validated.
- Keep ticket status accurate throughout the work so that another agent can quickly determine what remains to be done.
