---
name: manage-git-changes
description: Prepare and execute safe, semantic Git workspace changes, tags, pull requests, and CI follow-up. Use when the user asks to stage files, create or reorganize commits, soft-reset or recombine unpublished commits, run or unwind a bisect, create a session or milestone tag, push a branch, open or update a pull request, or inspect a workspace specifically in preparation for those actions. Also use at the end of a substantial working session to create the session review tag. Do not use for read-only status, log, blame, or code-review history inspection.
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
8. Push without force directly to the branch requested by the user and verify
   that its remote ref matches the intended local commit. An ordinary push does
   not require a pull request, including a push to the default branch. Create a
   pull request only when the user asks for one, the remote rejects a direct
   push, or step 9 requires a CI repair pull request.
9. After every push, inspect the remote CI status and wait for all required
   checks to finish. If a check fails, read its logs and diagnose the failure.
   Never push a CI repair commit directly to the failed branch. When the user
   has explicitly authorized CI repair commits, create a non-protected repair
   branch from the failed commit, fix it within that scope, rerun the local
   gates, commit and push the repair, open or update a pull request targeting
   the failed branch, and inspect CI again until it passes. Otherwise, report
   the diagnosis and request authorization before changing or committing code.
10. Do not report a push complete while required CI is pending or failing. If
    CI status or a required repair pull request cannot be accessed, report that
    as a blocker. Never merge a pull request without explicit authorization.

When the user asks to recombine unpublished commits, preserve semantic commit
boundaries and review the final staged diff and commit list before reporting
completion.

## Tagging

Always use annotated tags (`git tag -a`). Two kinds exist:

- **Session review tags** — `session/YYYY-MM-DD-<topic>` on the final commit of
  each substantial working session, so the user can review one session's work
  as a unit. Creating one at session end is standing-approved. When the name
  is already taken that day, append a numeric suffix. The tag message must
  contain: the review range `<base>..<tag>` where base is the parent of the
  session's first commit, commit bullets grouped by phase or topic, and every
  item still awaiting the user's acceptance. The user browses sessions with
  `git tag -n99 --list 'session/*'` and reviews one with
  `git log --reverse <base>..<tag>` and `git diff <base>..<tag>`.
- **Milestone tags** — flat names in the project's milestone vocabulary
  (for example `m0-acceptance`), created only on a state whose verification
  actually happened and is recorded, such as a documented real-machine
  acceptance. Never tag a merely code-complete or CI-green state as a
  milestone, and confirm the verification evidence with the user before
  creating one. Do not use semver names while no packaged release exists.

Never move, re-create, or delete an existing tag. `git push` does not transfer
tags, and `--follow-tags` pushes the branch plus every reachable annotated tag
rather than one tag. Push exactly the requested tag with
`git push origin <tag>`, only when the user asks and after the local gates
pass, then verify the remote tag points at the intended commit.
