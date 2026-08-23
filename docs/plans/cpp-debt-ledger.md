# C++ debt ledger

Date: 2026-08-23
Scope: `umbraflow-cpp` only.

This ledger is a harvest, not an analysis. Each row is one `TODO(cpp-debt)`
marker found in source; the marker's own comment is the only argument for why
the shortcut is acceptable. A row closes when its marker is deleted from
source, never by editing this file in place.

Harvested with:

```
rg -n "TODO\(cpp-debt\)" --glob '!**/.worktrees/**' modules/ entry/ tests/ cmake/
```

## Current markers

| Location | Marker |
|---|---|
| `modules/operator/source/operator/agent-profile.hpp:49` | `riskUnits` has no caller and `k_agentNoProgressCeiling` has no enforcement site. `remaining_risk_units` was never charged — that predates the Operation cut — but the no-progress ceiling was enforced inside `submitCommand`, which the cut deleted, so today an Agent asking the same thing of the same world is stopped by nothing. Both ceilings, their `agent_budgets` columns and the `budget_snapshot` members that report them stay declared rather than being deleted quietly, because deleting them means rewriting recorded `budget_snapshot` audit JSON under a registered migration and re-enforcing them means choosing what a Tool call's state fingerprint is. Neither is this change's to decide; what is this change's is to say plainly that the ceilings below are declared and unenforced. |
| `modules/service/source/service/product-lifecycle.cpp:1049` | `localSemanticTargets` is the RuntimeModel's declared `ui_target` vocabulary rather than the targets THIS resolution reported, because the StateResolution document publishes readings only for declared Readers and this generation's models may declare none. Narrowing it needs the resolution to publish the targets it resolved; until it does, the reference is snapshot-scoped by its frame identity and model-scoped by its target vocabulary. |
| `modules/service/source/service/product-lifecycle.cpp:1298` | A bare coordinate has no Receipt to present, so it posts nothing; `k_unmeasuredInputVerdict` states why and what the Tool would have to carry for that to change. |
