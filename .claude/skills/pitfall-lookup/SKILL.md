---
name: pitfall-lookup
description: Search the project pitfalls base before debugging. MUST be used whenever encountering a bug, test failure, runtime error, assertion failure, unexpected behavior, or any issue that requires investigation. Also use when the user reports a problem, pastes an error message, or asks "why is X happening". The pitfalls base at docs/pitfalls/ contains hard-won debugging insights organized by module — checking it first prevents hours of re-investigation on already-solved problems.
---

# Pitfall Lookup

## Why This Matters

The project pitfalls base (`docs/pitfalls/`) contains non-obvious issues that took significant debugging effort to discover. Each entry documents the root cause, symptoms, and fix for a specific class of problem. Checking the pitfalls base before starting a fresh investigation can save hours — many "new" bugs turn out to be known issues with documented solutions.

## When to Use

Before investigating ANY issue:
- Runtime errors or assertion failures
- Test failures
- Unexpected behavior (wrong output, silent failures, crashes)
- Build errors that seem architecture-related (not simple typos)
- User-reported problems

## Process

### Step 1: Read the pitfalls base

Glob all `.md` files in `docs/pitfalls/` and read them. The files are organized by module (e.g., `slang-reflection.md`, `material-system.md`). Each file may contain multiple entries.

```
docs/pitfalls/*.md
```

### Step 2: Match against current issue

For each pitfall entry, check whether its **Symptoms** section matches what you're observing. Look for:

- Matching error messages or assertion text
- Same function/file names appearing in the error
- Similar behavioral patterns (e.g., "fields not found", "silent failure", "returns null")
- Same module or subsystem involved

### Step 3: Report findings

**If a match is found:** Present the relevant entry to the user before doing any investigation. Include:
- The entry title and which pitfalls file it's from
- How the documented symptoms match the current issue
- The documented root cause and fix
- Whether the fix has already been applied (check the referenced commit) or if this is a regression

Then apply the known fix or verify it's already in place.

**If no match is found:** State briefly that the pitfalls base was checked and no matching entry was found, then proceed with normal debugging.

### Step 4: After resolving a NEW issue

If the investigation reveals a non-obvious root cause that is NOT already in the pitfalls base, add it:

1. Determine which module file it belongs to (create a new file if no existing module fits)
2. Add an entry following this structure:

```markdown
## Entry Title

### Symptom
Observable error messages, behaviors, or patterns that indicate this issue.

### Root cause
The actual underlying reason, with enough detail to understand without re-debugging.

### Fix
What was changed and where. Include a code snippet if the fix is non-trivial.

### Regression check
How to verify the issue stays fixed: the command, gate, or test that would
catch a recurrence.
```

## What NOT to add to the pitfalls base

- Simple typos or missing includes (obvious fixes)
- Issues specific to one person's environment
- Temporary workarounds that will be removed soon
- Build configuration issues documented elsewhere (e.g., CLAUDE.md)

The pitfalls base is for **non-obvious, hard-to-debug issues** where the symptoms don't obviously point to the root cause.
