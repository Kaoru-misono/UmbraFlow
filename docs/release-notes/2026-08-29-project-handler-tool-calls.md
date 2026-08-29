# 2026-08-29 — Project handlers may call Tools

This source change overturns the leaf-handler restriction introduced in
[the flat-call cut](2026-08-26-a-tool-is-a-flat-call.md). The reasoning and
precise superseded clauses live in
[the composition decision](../decisions/2026-08-29-project-handlers-compose-recorded-tool-calls.md).
The install's actual published surface is
[`PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

## What broke and what stayed removed

Registered Project handlers no longer refuse every Tool invocation. They may
compose Framework Tools and other registered Project Tools through the same
generated calling surface used by interactive code. Tests or handlers relying
on that unconditional refusal must change.

`child_effects` remains deleted. It was a separate declaration of child Tool
names, bounds and budgets; do not put it back into `tools[]`. `body` and
`workflow_limits` remain deleted too. No compatibility flag or replacement
mandatory child array is introduced.

The existing `required_capabilities`, `ui_action_bounds` and `effect_bounds`
must cover the composition. Each child still needs ordinary policy admission;
the outer Tool's admission does not authorize an otherwise forbidden child.
Actual calls are recorded beneath their parent for replay.
An Agent's direct surface remains semantic. Privileged primitives inside a
handler still require explicit Operator policy grants.

The scoped environment and Tool runtime protocol identities change. Rebuild
Project registrations and frozen releases with the new framework; old pinned
identities are not accepted under the new execution contract.

## Project upgrade

1. Consume a newly cut framework install containing this change; an existing
   immutable install cannot gain it from a source-tree edit.
2. Use that install's kit and generated contract to rebuild the Project's
   rendered SDK and registration artifacts.
3. Move the known composition into the registered handler using generated
   namespace functions. Keep screenshot and observation references explicit.
4. Update the existing descriptor bounds and policy grants to cover the calls
   the handler may make. Do not declare a parallel child Tool list.
5. Exercise the composed Tool through normal admission, including child failure
   and replay, before publishing the updated Project registration.

Recursive handler cycles are refused, and a whole nested invocation shares
bounded call, depth, time and VM allocation resources as specified by the
decision. A caught child error cannot turn an unresolved effect into a proven
success. Interactive calls remain independent roots.
