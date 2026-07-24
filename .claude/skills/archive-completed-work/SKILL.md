---
name: archive-completed-work
description: Archive completed plans to the docs/archive/plans/ directory. Use when a plan's implementation is fully complete and verified.
---

# Archive Completed Work

Move completed plan from `docs/plans/` into `docs/archive/plans/`.

## Steps

1. **Identify the plan to archive**

   If no plan is specified, list active (non-archived) plans:

   ```bash
   ls docs/plans/*.md
   ```

   Ask the user which plan to archive.

2. **Verify completion**

   Read the plan file. Check for incomplete task markers (`- [ ]`). If any exist, warn the user and ask for confirmation before proceeding.

3. **Move the file**

   Create `docs/archive/plans/` when needed, then move only the selected plan with a
   normal filesystem operation. Preserve unrelated files and existing user
   changes. Do not use `git mv`, because it stages the rename implicitly.

4. **Review the result**

   Show the old and new paths and inspect `git status`. Do not stage or commit as
   part of the archive operation.

5. **Report**

   ```
   ## Archived
   - Plan: <filename> → docs/archive/plans/
   ```

   If the user wants a commit, ask for explicit approval first. After approval,
   stage only the exact removed and added paths and follow the repository commit
   workflow.

## Notes

- If the plan's status field exists, update it to "Completed" before archiving.
- Closed review documents follow the same workflow into `docs/archive/reviews/`.
- Discussion point documents are not plans — leave them in place unless explicitly requested.
- Never stage or commit without explicit user approval.
