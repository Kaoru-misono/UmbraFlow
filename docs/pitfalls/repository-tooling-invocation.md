# Running the repository's own tooling

Two ways a repository command does something other than what you asked. Both
are quiet: one writes files you never named, the other runs nothing at all and
exits zero.

## A repo-wide formatter rewrites files you do not own

### Symptom

Several agents are working one worktree with an agreed path split. One of them
finishes a change and runs the format gate. Afterwards `git status` shows
modifications in files that agent never opened, and another agent's next edit
fails to apply because the text it matched has moved.

The rewrites are invisible in review terms — alignment inside a struct
initializer, a wrapped argument list, a normalized quote — which is exactly why
nobody notices until someone else's edit misses.

### Root cause

`scripts/fix_format.py` defaults to "all first-party text" when given no path
arguments, and `scripts/check_cpp_format.py --fix` walks its own fixed
`SOURCE_ROOTS` — `modules`, `entry`, `tests`, `conformance`. Neither knows
about a path split; there is nothing to know, because the split lives in the
agents' instructions and not in the repository.

The gate documented in `CLAUDE.md` is a whole-repository gate, and that is
correct for a single writer finishing a change. It is wrong for one of several
concurrent writers, and nothing in the command says so.

This happened on 2026-08-10: a coverage agent's format pass rewrote alignment
inside `modules/operator/source/operator/ledger.cpp` and normalized
`tools/annotate/store.py`, both owned by other agents at that moment. Ignoring
whitespace, roughly 26 of the 55 deleted lines in `ledger.cpp` and 30 of the 55
in `store.py` were that pass rather than either author's work.

### Fix

While other agents are writing, run the checkers in check mode and hand-fix
only what you own:

```bash
python scripts/fix_format.py --check
python scripts/check_cpp_format.py
```

Both list offending paths. Fix the ones in your own set; leave the rest for
their owner. If a formatter must write, name your paths explicitly —
`fix_format.py` takes them as positional arguments.

Run the unrestricted `--fix` form only when you are the sole writer, which for
this repository means the final gate before handing the tree back.

### Regression check

With another agent's file deliberately misformatted, run the check-mode
commands: they report it and change nothing. Run the `--fix` form and it is
rewritten. `git diff -w --stat` against the same file separates a formatter's
whitespace pass from real edits, and is the fastest way to tell after the fact
which of the two happened.

## MSVC activation does not survive being invoked from the Bash tool

### Symptom

The documented Windows build command, run through the Bash tool:

```bash
cmd /c "call .claude\skills\build-project\script\windows\build-env.bat && cmake --build --preset x64-debug"
```

prints the `cmd.exe` copyright banner and a bare prompt line, then exits 0. No
compiler is activated, no build runs, and the exit status says everything
succeeded. Two agents independently lost time to this on 2026-08-10.

### Root cause

Not MSVC, and not the batch file. The Bash tool is Git Bash, whose MSYS layer
rewrites arguments that look like POSIX paths before handing them to a native
Windows program. `/c` looks like an absolute path, so `cmd.exe` never receives
its `/c` switch. Without a switch it starts interactively, reads EOF from a
stdin that is not a terminal, and exits cleanly.

Everything downstream is consistent with success because nothing downstream
ran.

### Fix

Suppress the conversion. Either form works, verified on this machine:

```bash
cmd //c "call .claude\skills\build-project\script\windows\build-env.bat && cmake --build --preset x64-debug"
MSYS_NO_PATHCONV=1 cmd /c "call .claude\skills\build-project\script\windows\build-env.bat && cmake --build --preset x64-debug"
```

MSYS collapses a leading `//` to `/` and leaves the argument alone. Writing a
small `.bat` wrapper in the scratchpad and running that also works, for the
same reason: no hand-written `/c` argument exists to be converted.

The command as written in `CLAUDE.md` and in the build skill is correct — it is
a PowerShell command, and PowerShell passes it through unchanged. Do not
"fix" the documented form; fix the invocation.

### Regression check

```bash
cmd /c "echo probe"    # prints the cmd banner, no output, exit 0
cmd //c "echo probe"   # prints: probe
```

If the first form ever prints `probe`, the path conversion is off in that shell
and the workaround is unnecessary there — but check rather than assume, because
the failing form's exit status cannot tell you.
