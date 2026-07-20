---
name: cpp-debt
description: Collect TODO(cpp-debt) markers into docs/plans/cpp-debt-ledger.md.
---

# C++ debt ledger

Find deliberate shortcuts with:

```bash
rg -n "TODO\(cpp-debt\)" --glob '!**/.worktrees/**' modules/ entry/ tests/ cmake/
```

Write the derived ledger to `docs/plans/cpp-debt-ledger.md` with source
location, shortcut, ceiling, and upgrade path. Do not modify source while
harvesting debt.
