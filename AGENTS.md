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
