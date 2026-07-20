---
name: cpp-coding-side-review
description: Auto-dispatch a parallel sub-agent that audits a C++ change against only the cpp-coding rules. Trigger for C++ reviews under modules/, entry/, or tests/; do not trigger for implementation tasks or non-C++ files.
---

# C++ coding rule check (side-channel)

## What this does and why

When the user asks for a C++ code review, you do **two complementary passes in parallel**:

- **Your own review** — open-ended: correctness, design, performance, architecture, anything that isn't in the rule sheet.
- **A side sub-agent** — a strict, mechanical checker that loads `cpp-coding` and applies *only* its rules.

The reason for splitting them: a single agent doing both tends to either skim the checklist (one rule per dozen lines of free-form analysis) or vice-versa. Separating them gives the user a comprehensive open review AND a guaranteed line-by-line rule check, and prevents the main reply from drowning the rule findings in unrelated discussion.

The side review is **not a replacement** for your own — it cannot reason about the change, only about conformance.

## Procedure

When the trigger fires:

1. **Start reading the diff / requested files** as you normally would.
2. **In the same message** (i.e. before sending text back to the user), use the `Agent` tool to dispatch the side review. Foreground only — do not pass `run_in_background`. Both reviews then return inline and you synthesize them.
3. When the sub-agent's findings come back, build your reply with the structure under **Synthesis** below.

Doing the dispatch in the same message as your reading tool calls keeps the sub-agent and your own analysis genuinely concurrent. Dispatching only after you've finished thinking serializes them — same latency for the user as not having this skill at all.

### Agent tool parameters

| field | value |
|------|------|
| `subagent_type` | `claude` (fallback order if not present: `code-review:code-review` → `general-purpose`) |
| `description` | `cpp-coding rule check on <files>` |
| `run_in_background` | omit / `false` |
| `prompt` | the template below, with `<scope>` filled in |

### Sub-agent prompt template

````
You are doing a narrow-scope C++ rule audit. The main agent is concurrently
doing the open-ended review (correctness, design, performance). Your job is
strictly the rule-conformance check — do NOT duplicate the main agent's work.

## Rule source

The complete rule set is the `cpp-coding` skill. Load it FIRST via
the Skill tool (skill: `cpp-coding`). When that skill references
its own bundled documents (e.g. `reference/coding-standard.md`,
`reference/core-reuse.md`, `reference/error-handling.md`,
`reference/logging-and-asserts.md`), those are PART OF the rule set —
read them as needed and cite from them.

Treat anything outside that skill (and its referenced docs) as out of
scope. No general correctness analysis. No architectural opinions. No
performance suggestions. No style preferences that aren't written down
in the rule set. The main agent is covering all of that.

For every changed C++ API and stored field, explicitly check the ownership and
lifetime rules: value-first ownership, raw-pointer non-ownership, stored borrows,
returned views, move-before-alias, shared mutability, asynchronous captures,
RAII cleanup, and construction/factory shape. Report a violation only when the
rule applies, but do not skip this lens when the diff looks mechanically simple.

## Scope

<files / commits / diff hunks under review — paste paths and any relevant
context from the user's request>

## Output (Markdown)

### Violations
One bullet per finding. Format:
- `<path>:<line>` — `<short quote or paraphrase of the specific rule>` —
  `<one-line suggested fix>`

If there are none, write: _none_.

### Clarifications needed
List any rule whose applicability you can't determine from the diff alone
— ownership intent, hot-path status, whether a TU has an established
local style that the rule set asks you to preserve, etc. Phrase each as
a direct question to the user; be specific about which file/line/decision
you can't classify.

If there are none, write: _none_.

### Verdict
One of: `clean`, `violations`, `clarifications-needed`,
`violations-and-clarifications`.

## Failure mode

If the `cpp-coding` skill is not available in this session, STOP
and return only:

> cpp-coding skill not available — cannot perform rule check.
````

## Synthesis

Compose your final reply to the user as three sections, in this order:

1. **Your own review** — correctness, design, performance, anything the rule sheet doesn't explicitly cover. Write this as you naturally would.
2. **`cpp-coding` rule check** — present the sub-agent's `Violations` and `Verdict` under a clear heading. Quote verbatim if cleanly formatted; lightly polish if not.
3. **Open clarifications** — if the sub-agent returned non-empty Clarifications, list them at the end as direct questions for the user. Do **not** answer them on the user's behalf. Wait for the user's reply before continuing.

When the user answers the clarifications, use your judgment:
- If the answer is trivial (one rule's applicability flips), amend the rule-check section inline in your follow-up reply — no need to re-dispatch.
- If multiple rules were affected, or the answer materially changes which files the audit applies to, re-dispatch with the clarifications baked into the new scope.

## When to skip the dispatch

Skip the side review (just do your normal review) when *any* of:

- The code is clearly **not C++** (Slang/HLSL only, CMake, JSON, Python, scripts, Markdown). The rule set doesn't apply.
- The user asked you to **write / modify / refactor** code, not review existing code. `cpp-coding` fires for those flows directly via its own trigger; a side dispatch would be redundant.
- The user explicitly says "skip the rule check", "just your review", "no second pass", "no side check", or similar.
- The review request is a **tiny conversational follow-up** to an in-progress review ("what about that line you mentioned?"). Don't break flow.
- The user invoked `/code-review` or any of the `code-review:*` plugin commands explicitly — those have their own protocol, and this skill should defer.

When in doubt, **trigger anyway**: the sub-agent returns `clean` fast if nothing is wrong, and the cost of a missed rule violation is higher than the cost of a redundant dispatch.

## Operational notes

- **One dispatch per review request.** Multi-file reviews go in a single sub-agent prompt — let it batch internally.
- **The sub-agent inherits the available-skills list.** It can invoke `cpp-coding` itself; you do not need to inline the rules or its reference docs into the dispatch prompt.
- **Foreground only.** The user expects both reviews back in the same reply turn. Background dispatch would deliver the rule check minutes later, out of context.
- **This skill is narrow on purpose.** It enforces conformance, not correctness. The main agent's review is the broader pass. If you find yourself adding general-purpose feedback to the sub-agent's prompt, you're using the wrong tool — let the main agent do that.
