---
name: manage-git-changes
description: Prepare and execute safe, semantic Git workspace changes, pull requests, and CI follow-up. Use when the user asks to stage files, create or reorganize commits, soft-reset or recombine unpublished commits, run or unwind a bisect, push a branch, open or update a pull request, or inspect a workspace specifically in preparation for those actions. Do not use for read-only status, log, blame, or code-review history inspection.
---

# Manage Git Changes

Treat the workflow red lines in `CLAUDE.md` as mandatory.

## Procedure

1. Confirm the requested Git mutation is authorized. Never infer permission to
   commit, create a worktree, discard work, force-push, or rewrite published
   history.
2. Inspect `git status` and `git log --oneline -20`. Preserve unfamiliar and
   unrelated changes.
3. Group changes by task semantics. Keep architecture, runtime behavior, tests,
   documentation, and assets separate unless they form one minimal behavior
   change.
4. Stage only explicit paths. Never use `git add .` or `git add -A`.
5. Review `git diff --cached --check`, the staged file list, staged statistics,
   and the complete staged diff before committing.
6. Follow the repository's recent commit-message style. Do not add
   `Co-Authored-By` trailers.
7. After each commit, inspect status and the resulting history. Before every
   push, fetch the remote, verify the branch has not diverged unexpectedly,
   inspect the repository CI configuration, and run the corresponding local
   gates. Never push while a required local gate is failing.
8. Push without force to a non-protected branch, verify the remote ref matches
   the intended local commit, and create or update the pull request for that
   branch. Deliver every pushed fix through a pull request; never push a fix
   directly to the default or protected branch.
9. After every push, inspect the pull request's remote CI status and wait for
   all required checks to finish. If a check fails, read its logs and diagnose
   the failure. When the user has explicitly authorized CI repair commits, fix
   it within that scope, rerun the local gates, commit and push the repair to
   the same pull request, and inspect CI again until it passes. Otherwise,
   report the diagnosis and request authorization before changing or committing
   code.
10. Do not report a push complete while required CI is pending or failing. If
    CI status or pull-request creation cannot be accessed, report that as a
    blocker. Never merge the pull request without explicit authorization.

When the user asks to recombine unpublished commits, preserve semantic commit
boundaries and review the final staged diff and commit list before reporting
completion.
