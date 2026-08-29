# 2026-08-29 — Effort and workflows are chosen, never inherited

## Decision

- **Effort follows the task.** Choose and strictly control each agent's effort
  from the actual complexity, risk and uncertainty of what it is being asked to
  do. Use the lowest effort that can reliably complete it. High and maximum are
  not defaults and are not a safety margin; state the concrete need before
  raising effort.
- **`fable` is never a default.** Do not dispatch Claude Code's `fable` agent
  casually. Its use requires a concrete task requirement that a lighter agent
  cannot meet, and that requirement is explained before the dispatch.
- **A Claude Code workflow needs explicit approval.** Before starting,
  enabling, dispatching or rerunning one, explain the intended workflow and its
  scope, ask, and wait for an affirmative answer. General task authorization is
  not workflow approval.

`workflow` here means Claude Code's own workflows. Nothing in this ruling
changes the authorization required for GitHub Actions or any other operation.

## Context

The rule was written from an incident, not in the abstract. A review agent was
started with no model named, so it came up on the account default — which an
earlier `/model Fable` had saved for new sessions — and it was handed a large,
open-ended review brief. Being a full Claude Code session, it answered the brief
by running a workflow: twelve Fable review agents, roughly 1.48M tokens between
them, followed by a verify stage that reached 84 of 182 agents. The weekly
budget moved thirteen points against a ten-point ceiling for the entire task.

Two assumptions failed at once, and both are cheap to check. The dispatcher read
the standing "no workflows unless asked" guidance as binding its own hands only,
having missed that **starting an agent authorizes whatever that agent decides to
run**. And it never looked at the pane's footer, which states the model and the
remaining budget, so a review priced as an ordinary pass ran on the most
expensive tier available, twelve times over.

## Consequences

The live instructions are in [CLAUDE.md](../../CLAUDE.md), under `Agent dispatch
economy` and `Workflow red lines`. The existing guidance there about splitting
batches and controlling validation cost does not waive these limits.

Because a dispatch hands over the whole harness and not just the task, both
prohibitions belong in the brief given to every dispatched agent, beside the
other constraints it is handed — and the model and effort of a new agent are
read back and set deliberately before it is prompted.
