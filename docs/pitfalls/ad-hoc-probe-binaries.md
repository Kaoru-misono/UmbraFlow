# Ad-hoc probe binaries put modal dialogs on the developer's screen

## Symptom

Four Windows `应用程序错误` dialogs appeared during a migration, each blocking on
`要终止程序，请单击"确定"`. Nothing in the test output or the gate mentioned them,
and `WerFault` left no report in `%LOCALAPPDATA%\...\WER\ReportQueue` or the
Application log's crash channel. They are recorded only as **System log event
ID 26, provider `Application Popup`**:

```text
06:06:19  protocol-old.exe                    指令引用了 0x0000000000000009 内存。该内存不能为 read。
06:07:33  protocol-old.exe                    指令引用了 0x0000000000000009 内存。
06:08:05  protocol-old.exe                    指令引用了 0x0000000000000009 内存。
08:00:06  uf-framework-catalog-probe-old.exe  指令引用了 0x00000000000003F0 内存。
```

That event ID is the only channel that names them. Searching the Application log
for `Application Error` (1000) finds nothing, which makes the popups look like
they came from somewhere outside the repository.

## Root cause

Two things compounded.

**The probes existed at all.** They were compiled by hand, against the
pre-migration tree, to print the *previous* values of the published identity
hashes for a release note's "Before" column. Linked ad hoc, they ran framework
entry points without the static initialisation a real binary performs, so they
dereferenced a null object — `0x9` and `0x3F0` are member offsets from a null
`this`, not random addresses.

**Nothing suppressed the dialog.** The one place in this repository that turns
off the OS crash box is `tests/test-main.cpp`:

```cpp
_set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
```

A doctest target links it and crashes silently into a failed test. A hand-built
`cl.exe` probe links none of it, so an access violation reaches the default
Windows handler, which raises a modal dialog and **suspends the process until
somebody clicks it**. On a machine the developer is using, that is not a log
line, it is an interruption — and under `ctest --parallel` it can hold a worker
until the timeout fires, so the visible failure is a timeout with no cause.

## Fix

**Do not compile a probe to read a historical identity. Read it from history.**
Every value those probes were built to print already existed in the tree:

```bash
git grep -n '<hash>' $(git rev-parse HEAD) -- tests/ modules/   # what pinned it
git show HEAD:docs/release-notes/<previous-note>.md             # what published it
```

The previous release note carries the previously published identities by
definition — that is what a release note is for — and a pinned literal in a test
carries the rest.

For a value that genuinely is not written down anywhere, add a temporary
`MESSAGE(...)` to the doctest case that already computes it and run that one
case:

```cpp
MESSAGE("[DEBUG-identity] PROTOCOL=" << identity->hex()); // [DEBUG-identity]
```

```bash
./build/x64-debug/bin/test-deployment.exe -tc="<the case that computes it>"
```

The binary already links the suppression and the initialisation, the value is
computed the way production computes it, and the marker is removed the way any
other `[DEBUG-...]` instrumentation is. A file digest needs no binary at all:
`python -c "import hashlib,pathlib; print(hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest())"`.

If a standalone binary is genuinely unavoidable, call `SetErrorMode` on its
first line before anything else runs.

## Regression check

There is no gate for this, and one should not be added — the failure is in
hand-run commands the gate never sees. The check is the log, after any session
that built something by hand:

```powershell
Get-WinEvent -FilterHashtable @{LogName='System'; Id=26; StartTime=(Get-Date).AddHours(-12)} |
  Select-Object TimeCreated, Message
```

An empty result is the property. A non-empty one names the executable, and that
executable should not have existed.

Leftover probe binaries are findable directly, and are worth sweeping before
calling a session finished:

```bash
find . -maxdepth 3 -name "*-old.exe" -o -maxdepth 3 -name "*probe*.exe" | grep -v "build/x64-"
```
