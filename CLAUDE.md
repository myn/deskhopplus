## Agent skills

### Issue tracker

Issues live in the `myn/deskhopplus` GitHub Issues, managed with the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, used verbatim as label strings. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

---

## Implementation Workflow

For every non-trivial implementation, follow this order:

1. `mattpocock-skills:implement`
2. `mattpocock-skills:tdd`
3. Implement the changes
4. `mattpocock-skills:code-review`
5. `deskhopplus-adversarial-review`
6. Run required validation/tests
7. Commit the changes
8. Push to GitHub
9. Verify the remote contains the expected commit
10. Wait for and verify required CI checks are green
11. Update and close the issue

`deskhopplus-adversarial-review` is the final review gate. It must run **after** `mattpocock-skills:code-review` and **before** the implementation is reported complete.

---

## Definition of Done

**A task is NOT complete until the changes are pushed to GitHub and the resulting required CI checks are green.**

The completion sequence is:

**Implement → Test → Review → Adversarial Review → Commit → Push → Verify Remote → Verify CI → Update Issue → Close**

### Required

Before closing an issue, verify all applicable items:

- Implementation is complete.
- Required tests and validation pass.
- Final diff has been reviewed.
- Changes are committed.
- Changes are pushed to GitHub.
- The remote branch contains the expected commit.
- Required CI checks have finished.
- All required CI checks are green.
- The issue contains relevant completion context.

### Never Close Prematurely

**Do not close or report an issue as complete if:**

- Changes have not been pushed.
- The push has not been verified.
- CI has not been checked.
- CI is still running.
- Any required CI check is failing.
- Required validation has not passed.
- Human validation is still required.

If CI fails, fix the problem and repeat:

**Commit → Push → Verify Remote → Verify CI**

If a required step cannot be completed, leave the issue open and document what remains.

### Human Validation

If human validation is required:

- Do not close the issue.
- Apply `needs-hardware-validation` when hardware/helper validation is required.
- Apply `ready-for-human`.
- Document exactly what the human must validate.
- Complete all agent-controlled work, including commit, push, and CI verification, before handing off.

### Issue Context

Before closing or handing off an issue, record important:

- Decisions and context
- Discoveries and learnings
- Non-obvious implementation details
- Tests and validation performed
- CI status
- Remaining human-validation requirements

Keep issue status accurate throughout the work.
