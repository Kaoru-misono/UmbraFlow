---
name: diagnose
description: Diagnose difficult bugs and performance regressions with a reproducible feedback loop. Use when the user asks to debug or diagnose a failure, crash, flake, unexpected behavior, or performance regression.
---

# Diagnose

Use `pitfall-lookup` first. Do not replace evidence with speculation.

## Procedure

1. Define the exact user-observed failure and construct the fastest deterministic
   agent-runnable reproduction. For a nondeterministic bug, loop or stress the
   trigger until the reproduction rate is useful.
2. Confirm the reproduction matches the reported symptom and capture its exact
   output or timing.
3. Generate three to five ranked, falsifiable hypotheses. Inspect the relevant
   code and history before changing anything.
4. Test one variable at a time with a debugger, focused instrumentation, or a
   profiler. Tag temporary logs with a unique `[DEBUG-...]` prefix.
5. When the requested scope includes a fix and a correct test seam exists, turn
   the minimized reproduction into a failing regression test, observe it fail,
   apply the smallest fix, and observe it pass.
6. Rerun the original reproduction and the affected validation gates.
7. Remove tagged instrumentation and temporary harnesses before completion.

If human interaction is unavoidable, copy
`scripts/hitl-loop.template.sh` to a temporary directory and adapt it there.
If no usable feedback loop can be built, state what was attempted and request
the missing environment, captured artifact, or instrumentation authority. Do
not implement a fix when the user requested diagnosis only.
