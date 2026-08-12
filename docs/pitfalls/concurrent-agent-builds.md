# Concurrent agent builds in one worktree

Several agents working the same worktree at once share three things they do not
declare: one build directory, one set of source files on disk, and one MSVC
toolchain. Each is a resource with single-writer semantics that nothing in the
build system enforces, so a collision does not produce a clear error. It
produces a stall, and a stall reads exactly like slow work.

A fourth shared resource joined the list on 2026-08-10: the repository's own
repo-wide tooling. `scripts/fix_format.py` and
`scripts/check_cpp_format.py --fix` write across the whole tree by default, so
one agent's format pass edits files another agent owns. That one is in
[Running the repository's own tooling](repository-tooling-invocation.md),
because it bites a single writer too.

The per-agent build directories prescribed below were in use throughout the
2026-08-10 parallel run — five of them — and no collision of the kind described
here occurred.

## Concurrent builds into one build directory deadlock every compiler

### Symptom

Two or more `cmake --build --preset x64-debug` runs are active at once. Every
`cl.exe` in the process table sits at a total CPU time of roughly 0.02-0.06
seconds and does not move; sampling the same processes ten minutes later returns
the identical figures. The parent `ninja` processes are equally still. Nothing
times out, no error is printed, and the builds never finish.

The same collision also surfaces as `LNK1168` on the output binary, as
`premature end of file` on an object, and as a ninja internal assertion — all of
which name the file that two runs reached at the same moment.

Killing `mspdbsrv.exe` does not clear it. That is worth stating because the
near-zero CPU makes the shared PDB server the obvious suspect, and ruling it out
costs another few minutes.

### Root cause

The preset fixes `binaryDir` to `${sourceDir}/build/${presetName}`, so every
agent that builds a given preset writes the same object files, the same import
libraries, and the same executables. Ninja takes no lock on its build
directory. When two runs schedule the same edge, one holds the output file open
and the other blocks on it; when each holds an output the other needs, neither
proceeds. It is a mutual file-lock deadlock, and it does not resolve on its own.

### Fix

Give every concurrent agent its own build directory. `build/` is gitignored, so
a sibling directory beside the preset's costs nothing:

```bash
cmake -S . -B build/<agent>-debug -G Ninja \
    -DCMAKE_CXX_COMPILER=cl.exe \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/<agent>-debug --target <target>
```

Those are the settings `x64-debug` inherits from `ninja-base`, so the resulting
tree matches the preset's in everything but its path.

Run the final gate against `build/x64-debug` as the gate specifies, but run it
when no other agent is building, and treat a `test-...(Timeout)` or a link error
naming a file you did not touch as a suspected collision rather than as a
finding about your own change.

To recover a deadlock already in progress, kill the stalled `cl.exe` processes.
Their parent `ninja` then reports a failure and exits, and no source is lost;
the owning agent reruns the build. Nothing else clears it.

### Regression check

Start two builds of the same preset from separate shells and watch the process
table. If `cl.exe` CPU time is unchanged across two samples a minute apart, the
runs have collided. Repeat with separate build directories: both complete.

## A stalled compiler pins the source file it was reading

### Symptom

Writing to a first-party `.cpp` fails with `PermissionError: [Errno 13]`
(or `Access is denied` from PowerShell). The file is not read-only and no editor
holds it. Retrying for sixty seconds does not help.

### Root cause

`cl.exe` opens its translation unit with `FILE_SHARE_READ` and no share-write, so
for as long as the compiler lives, nothing can write to that source. Under the
deadlock above the compiler lives indefinitely, which turns a lock that should
last milliseconds into one that lasts until the process is killed.

### Fix

Kill the stalled `cl.exe`, then write. Plan for the lock rather than fighting
it: any tooling that rewrites a source file needs a bounded retry and a loud
failure if the retry is exhausted, because the silent case is the dangerous one.

### Regression check

While a build is compiling a known file, attempt to write it. The write fails.
After the build finishes or its compiler is killed, the write succeeds.

## Mutation testing edits a file every other agent compiles

### Symptom

An agent's gate goes red on a test target it has no changes in, and the failure
is a timeout or an assertion inside another agent's new test. The failure
disappears on a rerun minutes later and cannot be reproduced from the committed
tree.

### Root cause

Proving a test red by mutating the code under test means the mutated source
exists on disk for as long as the proof takes. In a shared worktree that source
is compiled by everyone, so a deliberate defect is briefly indistinguishable
from a real one. The window is not as short as it looks: the revert can be
blocked by the file lock above, and a mutation whose whole purpose is to remove
an exit condition can leave a test hanging rather than failing, which stretches
every downstream gate to its timeout.

### Fix

Mutate a copy, never the shared tree. The source is small enough to duplicate in
seconds; only `build/` is large:

```bash
robocopy <worktree> <scratch> /MIR /XD build install /XF .git
```

Configure and build in the copy and run every mutation there. The shared
worktree then only ever holds the version that is meant to pass.

Put the copy at a short absolute path such as `E:\uf-<agent>`, not under the
session scratchpad. The scratchpad path is about 130 characters before the
mirror name, and MSVC's intermediate files (`.obj.modmap`, `.obj.ddi`, the
per-target `.pdb`) push the total past `MAX_PATH`. The build then fails as
`D8022 cannot open ... .modmap`, `C1083 Cannot open compiler generated file`,
and `C1041 cannot open program database`, none of which name a path problem;
CMake's only warning is a generic "cannot be safely placed under this
directory" at configure time. Measured 2026-08-12.

Design the harness so no mutation can hang. A test that drives a loop needs an
escape that survives the mutation being proved: if the escape is the very exit
the mutation removes, the case does not fail, it runs forever. Open every exit
at the bound — and keep a hard ceiling that fails the case outright, since
`REQUIRE` unwinds out of the loop where a `CHECK` would let it continue.

### Regression check

Apply each mutation in the copy and confirm two things: the intended case is
red, and the run terminates. A mutation whose run has to be killed has not
proved anything, and its harness needs the escape widened before the result
counts.

## Restoring the copy with robocopy leaves the mutation in the object

### Symptom

A falsification run reddens the case it aimed at *and* a case belonging to an
earlier mutation that was already restored. The extra red persists across every
later run in the campaign and disappears only after an unrelated rebuild, so it
reads as a real defect that one mutation exposed.

### Root cause

`robocopy /MIR` preserves the source file's timestamp. Restoring a mutated file
therefore replaces a file whose mtime is *now* with one whose mtime is older
than the object built from the mutation, and ninja rebuilds only when an input
is newer than its output. The mutated object stays, and the check the mutation
deleted stays deleted, for every run until something else touches that source.
Measured 2026-08-11: one deleted check survived four later runs this way, and
the four results it contaminated had to be discarded and re-measured.

The same shape catches a harness that rewrites source with Python's
`write_text`: the default newline translation converts the whole file to CRLF,
which compiles fine but breaks any test that searches a repository document for
`"\n"`-delimited text. That one produced a red whose message named an assertion
the mutation had nothing to do with.

### Fix

Touch every source the campaign mutates after restoring it, so the restored
file is newer than any object built from a mutation:

```python
for source in (mirror / "modules" / "<module>").rglob("*"):
    if source.is_file():
        source.touch()
```

Read and write mutated files as bytes, so no newline is rewritten.

### Regression check

Restore the copy, rebuild, and confirm the build actually recompiles the file
that was mutated -- a build that reports only the link step has restored
nothing. Then run the suite: it must be fully green before the next mutation is
applied. A campaign that never observes green between mutations cannot tell
which run its reds came from.
