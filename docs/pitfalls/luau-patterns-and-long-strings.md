# Luau patterns and long strings

Two ways that text which looks like a regular expression or a here-document
behaves differently in Luau, both of which fail quietly rather than loudly.

## A Lua pattern has no optional group

### Symptom

`project.parse` refused every RuntimeModel whose TOML carried a decimal:

```text
script error: line 17 has unsupported bare value '0.90'
```

Integers and exponent forms were accepted. No error came from the pattern
itself, so the value looked genuinely unsupported.

### Root cause

The number check was written with a regex habit:

```lua
string.match(token, "^[+-]?%d+%.%d+([eE][+-]?%d+)?$")
```

In a Lua pattern `?` applies to a single character class, and `(` `)` open a
capture rather than a group. `)?` is therefore not "the group is optional"; the
pattern simply never matches a plain decimal. `string.match` returns nil instead
of raising, so the mistake reads as a rejected value rather than a broken
matcher.

### Fix

Enumerate the accepted spellings; a Lua pattern cannot express the alternation
any other way.

```lua
if string.match(token, "^[+-]?%d+$") == nil
    and string.match(token, "^[+-]?%d+%.%d+$") == nil
    and string.match(token, "^[+-]?%d+%.%d+[eE][+-]?%d+$") == nil
    and string.match(token, "^[+-]?%d+[eE][+-]?%d+$") == nil
then
```

### Regression check

`ctest -R test-runtime-model-v2-luau`, whose TOML fixture carries
`threshold = 0.90`. Before the fix the case failed at parse time.

Grep for the shape when touching any Luau matcher:

```bash
rg -n '"\^?[^"]*\)[?*+]' modules/task/runtime/
```

## TOML array-of-tables closes a Luau long string

### Symptom

A Luau test that embeds a TOML fixture failed to load at all:

```text
luau_load failed: [string "test-runtime-model-v2.luau"]:17:
Expected identifier when parsing expression, got "ui_target"
```

The reported line was inside what looked like a string literal, and the fixture
was valid TOML.

### Root cause

The fixture opened with `local valid = [[` and the TOML declared array-of-tables
sections:

```toml
[[ui_target]]
```

The first `]]` inside the body closes the Luau long string, so everything after
it is parsed as code. Any TOML with an array-of-tables section is unquotable at
long-bracket level 0.

### Fix

Raise the long-bracket level so the delimiter cannot occur in the body:

```lua
local valid = [==[
[[ui_target]]
...
]==]
```

### Regression check

`ctest -R test-runtime-model-v2-luau`. Any Luau file embedding TOML that
declares `[[section]]` must use `[=[`/`]=]` or higher.
