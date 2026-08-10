# Third adversarial round — `37296d7..cec8898` (2026-08-10)

Eight commits, reviewed as landed rather than as they sit in the working tree
(two agents held uncommitted edits during this review; every file below was read
through `git show <commit>:<path>`). No build was run.

Chronological order of the range, which matters for several findings below:

```
f0b351b  fix: make clang-tidy see the headers it was configured to check
603b0b0  fix: clear the clang-analysis diagnostics outside the operator module
25520a3  feat: implement the Replay Bundle and both publication gates
eadaef9  docs: specify the four remaining work items, and reconcile them
5bb281d  docs: correct the claims that no gate was enforcing
6b20c3d  feat: add a JCS serializer to the trusted Luau framework
dcc43b5  feat: export a consumable contract suite, and make every gate say what it proves
cec8898  refactor: keep one confined path walk, and compile its POSIX half   (HEAD)
```

**Verdict: FAIL.** Two findings are the repository's own recurring defect in its
newest clothes — a `contract-` gate that stays green when the behaviour it names
is deleted, and a whole protection mechanism (`runtime_publications`) that no
test can observe. One is a documented rule the block broke while writing the
commit that was supposed to stop that class of breakage.

> **Disposition note, 2026-08-11. The findings below are left exactly as
> recorded; this says only what has since answered two of them.** R3-F2 was
> answered by deletion rather than by a test: `848e390` removed
> `PublicationHold` and the `runtime_publications` table after verifying the
> unreachability this finding could not decide between — reclamation has no
> non-test caller, `OperatorCoordinator` appears in no entry point or other
> module, and `open()` holds `PRAGMA locking_mode=EXCLUSIVE` for the
> connection's lifetime. The finding's second reading was the right one: a
> second publisher cannot exist, so no test could have been written, and the
> mechanism went. R3-F3's 14 wrong rows were repaired on 2026-08-10, and the
> same violation recurred twice on 2026-08-11 in `4b955de` and `848e390`, which
> registered five new gates before updating the report; the report was repaired
> again rather than the rule relaxed. Dispositions belonging to the block rather
> than to this record live in
> [the next block](../plans/2026-08-10-next-block.md) §7.

## Findings

| ID | Severity | What it is | Where |
|---|---|---|---|
| R3-F1 | high | `contract-state-s05`'s only executed assertion is that manifest minting is deterministic; seven of the eight fields can leave `SessionManifest`'s canonical form with every gate green | `tests/operator/test-state-contract.cpp:251`, `modules/operator/source/operator/manifest.cpp:88` |
| R3-F2 | high | `PublicationHold` / `runtime_publications` — the third leg of the reclamation refcount — is observed by no test, and the design claim that protects it is not supported by the code | `modules/operator/source/operator/ledger.cpp:924`, `:1052`, `:1411`, `tests/operator/test-ledger.cpp:473` |
| R3-F3 | high | 14 requirement rows name CTest IDs that no longer exist, in violation of the report's own stop condition 2; the commit whose job was to correct unenforced claims left them | `docs/plans/2026-08-09-runtime-migration-report.md:41`, `dcc43b5`, `5bb281d` |
| R3-F4 | medium-high | a registered per-case gate whose `TEST_CASE` is compiled out passes forever; the configure-time guard is textual and doctest exits 0 on an empty selection | `tests/CMakeLists.txt`, `cmake/operator-contract-suite.cmake` |
| R3-F5 | medium | the `contract-`/`schema-` split is enforced only by the spelling of the name; the list that claims to forbid a shape-only `contract-` gate is a list of names | `tests/CMakeLists.txt:47` |
| R3-F6 | medium | `dcc43b5`'s "one landing, cannot be split" claim is false for at least two of its parts | `dcc43b5` |
| R3-F7 | medium | the Python workspace SQLite schema root hash is pinned nowhere; its only test cannot fail, and the cross-boundary check compares Python against itself | `tools/annotate/store.py:480` and `:2247`, `tools/annotate/tests/test_backend.py:588` |
| R3-F8 | medium | two canonicalisers in one Python package, both feeding SHA-256 identities, disagreeing on key order and on numbers; `25520a3` uses both in one function | `tools/annotate/store.py:109`, `tools/annotate/jcs.py` |
| R3-F9 | medium | the JCS transform's spellings diverge on concrete inputs, and the claimed cross-check left no artifact | `modules/task/runtime/jcs.luau`, `tools/annotate/jcs.py`, `modules/core/source/core/text/json-text.cpp` |
| R3-F10 | medium | `requireChildName`'s `.` and `..` clauses cannot be falsified on Windows; on POSIX their removal empties the whole root before returning an error | `modules/task/source/task/platform/confined-file.cpp:56`, `tests/task/test-confined-file.cpp` |
| R3-F11 | medium-low | W11's stated scope was written after `603b0b0` cleared most of it and is not reconciled with it | `docs/plans/2026-08-10-next-block.md:155`, `.claude/skills/cpp-coding/references/coding-standard.md:20` |
| R3-F12 | low | `test_frameless_bundle_audits_but_cannot_stand_in_for_a_frame_replay` proves nothing about framelessness | `tools/annotate/tests/test_backend.py` |
| R3-F13 | low | `fix_format.py` rewrites bytes inside raw string literals, and the documented remedy for the resulting red fingerprint is the action that silently breaks every database | `scripts/fix_format.py`, `modules/operator/source/operator/ledger.cpp:340` |
| R3-F14 | low | `f0b351b`'s closing claim names a gate the same block documents as not compiling; the `static_assert` proof was not committed, and the suppressions are per-record | `f0b351b`, `.clang-tidy` |
| R3-F15 | low | `.clang-tidy` and the Python gates disagree on the vendored vocabulary, and `ExcludeHeaderFilterRegex` is version-gated on the unpinned Windows lane | `.clang-tidy` |
| R3-F16 | low | `uf_add_operator_contract_suite` does not enforce the CASES↔declared exact match its sibling does — the property `dcc43b5` cites as the reason for one landing | `cmake/operator-contract-suite.cmake` |
| R3-F17 | low | the depth-ceiling case's comment states an all-or-nothing removal property that does not hold once a tree has siblings | `tests/task/test-confined-file.cpp` |

---

## R3-F1 — high — `contract-state-s05` stays green when the binding it names is deleted

**VERIFIED by reading.**

`dcc43b5` renamed 19 gates to `schema-*` on the grounds that a `contract-` name
must mean "goes red when the behaviour is removed". `contract-state-s05` kept
the `contract-` name. Its body is
`tests/operator/test-state-contract.cpp:251-267`:

```cpp
TEST_CASE("contract-state-s05")
{
    auto const schema             = readSchema("umbraflow-operator-v1.schema.json");
    auto const manifestDefinition = definition(schema, "SessionManifest");
    checkStrictObject(manifestDefinition);
    CHECK(manifestDefinition.find("\"runtime_model_artifact_root_hash\"") != std::string::npos);
    CHECK(manifestDefinition.find("\"project_registration_hash\"") != std::string::npos);
    CHECK(manifestDefinition.find("\"policy_artifact_hash\"") != std::string::npos);
    CHECK(manifestDefinition.find("\"journal_envelope_schema_hash\"") != std::string::npos);

    auto const first  = SessionManifest::create(manifestSpec());
    auto const second = SessionManifest::create(manifestSpec());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->canonicalBytes() == second->canonicalBytes());
    CHECK(first->hash() == second->hash());
}
```

The four field assertions read `schema/umbraflow-operator-v1.schema.json`. The
executed half builds the same spec twice and asserts the two results are equal —
that is a determinism check, and nothing more. S-05 in the requirement matrix is
`Session coordinator | OP:SessionManifest + PR`; the "+ PR" half — that the
manifest actually binds the project registration hash — is asserted only against
schema text.

**Exact mutation.** In `modules/operator/source/operator/manifest.cpp`,
`canonicalSessionManifest` (≈ lines 88–108), delete these two lines:

```cpp
output += ",\"project_registration_hash\":";
appendHash(output, spec.projectRegistrationHash);
```

The manifest no longer binds the project registration at all. Both manifests in
`contract-state-s05` change identically, so both `CHECK`s still pass. The gate
is green. The same holds for `journal_envelope_schema_hash`,
`runtime_model_artifact_root_hash`, `runtime_model_schema_hash`,
`host_protocol_schema_hash`, `operator_protocol_schema_hash` and
`agent_profile_hash`.

**Nothing else catches it.** `git grep` over `tests/` and `contract-suite/` finds
exactly one place in the tree that varies a `SessionManifestSpec` field and
compares hashes — `tests/operator/test-product-contract.cpp:228-233`, which
varies `policyArtifactHash` only. So one of the eight fields is covered and
seven are not, and the one that is covered is covered by a *different*
requirement's gate.

This is precisely the shape `dcc43b5`'s comment in `tests/CMakeLists.txt:47`
says the vocabulary exists to forbid: "a `contract-` gate whose assertions only
read a schema file". S-05's assertions about S-05 do only that.

**Fix shape.** Give `contract-state-s05` the same treatment `contract-product-p06`
already has, once per field: perturb each of the eight spec fields in turn and
require the hash to move. Eight `CHECK`s, one loop.

---

## R3-F2 — high — the reclamation refcount's third leg is unfalsifiable, and its stated defence does not hold

**VERIFIED by reading.**

`dcc43b5`'s message: "The refcount is not a counter but the set of rows naming a
hash — installations, a new `runtime_publications` table, and the active root".
Two of those three legs are exercised. The third is not.

### (a) No test ever observes a non-empty `runtime_publications`

`git grep -n "runtime_publications\|PublicationHold" HEAD -- tests contract-suite entry`
returns **nothing**. The four reclamation cases
(`tests/operator/test-ledger.cpp:473, 514, 546, 568`) each drive a single
`OperatorCoordinator` and call `reclaimUnreferencedRuntimeArtifacts()` only after
every `installRuntimeArtifact` call has returned. A `PublicationHold` row is
inserted by `PublicationHold::take` and removed either by `discardPublication`
inside the installing transaction (`ledger.cpp:1374`) or by the destructor on
failure (`ledger.cpp:956-962`). It therefore never survives the call that made
it, and `runtime_publications` is empty at every point any test reads it.

**Exact mutations, both of which leave the whole suite green:**

1. In `ledger.cpp:1411-1424`, delete the clause

   ```sql
   artifact_root_hash NOT IN (SELECT artifact_root_hash FROM runtime_publications) AND
   ```

2. In `ledger.cpp:1479-1484`, delete

   ```cpp
   if (std::ranges::contains(claimed, name)) { continue; }
   ```

   (and, to keep `-Werror` quiet, the `claimedQuery` block above it).

After either mutation the "claim before write" protection is gone. `ctest -L CI`
is unchanged. Under the standard this repository already applies — a safety test
counts only once removing the property makes it red — the claim-before-write
mechanism currently has no test at all.

### (b) The comment that explains why it is safe is not supported by the code

`ledger.cpp:1048-1052`, in `beginSessionEpoch`:

```cpp
// A publication claim outlives its process only after a crash, and
// the epoch bump above has just fenced that process out. Dropping
// the claims here is what keeps a crash from pinning an artifact
// directory against reclamation forever.
UF_TRY(execute(database, "DELETE FROM runtime_publications"));
```

The epoch bump fences *sessions and control leases*: `beginSessionEpoch` deletes
`control_leases` and clears `sessions.active`. It does not fence installation.
`OperatorCoordinator::installRuntimeArtifact` (`ledger.cpp:1231-1381`) contains
no reference to the session epoch at all — verified by `awk 'NR>=1231 && NR<=1384'
| grep epoch`, which returns nothing. Its only compare-and-swap is on
`runtime_state.installed_generation`.

Nothing serialises coordinators either: `OperatorCoordinator::open` takes no lock
file and does not set `PRAGMA locking_mode=EXCLUSIVE`; it opens the database
`SQLITE_OPEN_CREATE|READWRITE|FULLMUTEX|NOFOLLOW`.

So the following sequence is available:

1. Process A: `installRuntimeArtifact` → `PublicationHold::take` commits a row
   for `(token T, hash H)` → `publishRuntimeArtifact` begins materialising into
   `runtime-artifacts/.staging/T`. For a real artifact this takes time.
2. Process B: `OperatorCoordinator::open(...)` → `beginSessionEpoch` →
   `DELETE FROM runtime_publications`. A's live claim is gone. A is not stopped:
   it never looks at the epoch.
3. Process B: `reclaimUnreferencedRuntimeArtifacts()` → `claimed` is empty, so
   `.staging/T` is swept; `H` is in `runtime_artifacts` and in neither
   `runtime_installations` nor `runtime_publications` nor the active root, so
   `removeTree(H)` runs too.

On POSIX both removals succeed and A's in-flight tree is destroyed under it. On
Windows the confined root A holds over the staging tree is opened without
`FILE_SHARE_DELETE`, so B's `removeTree` returns `ioFailure` and aborts B's whole
reclamation instead — a different wrong answer, and one no test would see.

**Either way there is a finding.** If two coordinators over one runtime directory
are possible, this is a data-destruction path with no test. If they are *not*
possible, then a second publisher cannot exist, `PublicationHold` protects
against nothing, and no test could ever be written for it — which is the same
family as the six defects found earlier today. The code cannot be right on both
readings; the design intent needs to be stated and then made testable.

**Cheapest falsifying test.** Open a second `OperatorCoordinator` on the same
production root while a `PublicationHold` is outstanding — the hold is currently
unreachable from outside `ledger.cpp`, so this needs either a test-only seam or a
`reclaim` call driven from a second coordinator between a `take` and its
transaction. That the test is awkward to write is the finding, not an excuse.

---

## R3-F3 — high — 14 requirement rows name CTest IDs that do not exist

**VERIFIED by reading.**

`docs/plans/2026-08-09-runtime-migration-report.md:34-36` states the rule:

> Verification IDs marked `CTEST` are exact local CTest names.

and stop condition 2 (`:178` onward):

> 2. a schema path or test ID changes without updating this report first;

`dcc43b5` renamed 19 IDs from `contract-*` to `schema-*` and did not touch the
report. At HEAD, these 14 rows still name a CTest that `ctest -N` cannot produce,
because the registered name is now the `schema-` spelling:

`P-01`, `P-02`, `P-03`, `S-01`, `S-02`, `S-04`, `C-03`, `C-05`, `C-08`, `A-01`,
`A-02`, `A-03`, `A-05`, `A-07`.

(The other five renamed requirements — `C-09` … `C-13` — are unaffected, because
they gained a real `contract-` gate in the exported suite, so their rows remain
true.)

`5bb281d`, whose subject is *"docs: correct the claims that no gate was
enforcing"*, touched this file — it added `contract-suite-umbraflow` and
`contract-suite-arcana` to the retained-CTest list, with the justification
"recorded here because stop condition 2 requires this report to carry every local
CTest ID" — and left all 14 wrong rows in place. It also did not record
`test-contract-operator` and `test-contract-runtime`, the two new aggregate CTest
names created by the same landing, although its own added sentence says the
report must carry every local CTest ID.

**How to reproduce.** `git show HEAD:docs/plans/2026-08-09-runtime-migration-report.md | grep -n 'contract-product-p01\|contract-state-s01'`
against `tests/CMakeLists.txt`'s `UF_REQUIRED_DOCTEST_CONTRACTS`, which spells
them `schema-product-p01` and `schema-state-s01`.

This is the class of defect the review record already calls out as B-NEW-3 (a
false claim in a document) and treats as equal to a broken test.

---

## R3-F4 — medium-high — a registered gate whose case is compiled out is green forever

**Mechanism VERIFIED; the demonstration is INFERRED but mechanical.**

Both CMake helpers derive the set of declared cases with a regex over the source
*text*:

```cmake
string(REGEX MATCHALL
    "TEST_CASE[ \t\r\n]*\\([ \t\r\n]*\"(contract|schema)-[a-z0-9-]+\"[ \t\r\n]*\\)"
    ...
```

`cmake/operator-contract-suite.cmake` even names the hazard the check is meant to
close:

> A CASES entry outside this set would register a CTest whose `--test-case` filter
> matches nothing, and doctest reports an empty selection as success.

The check closes the *misspelling* case. It does not close the
*not-compiled* case, because a `TEST_CASE` inside `#if 0`, inside a
platform guard, or inside a block comment is still text.

doctest's behaviour on an empty selection is as described:
`tests/external/doctest/doctest/doctest.h` tracks
`numTestCasesPassingFilters` but uses it only for reporting (lines 6232–6276);
there is no zero-selection failure path and no Catch2-style `--warn NoTests`.
The process exit code is the failure count, which is 0.

**Exact mutation.** In `tests/operator/test-agent-audit-contract.cpp`, wrap
`TEST_CASE("contract-agent-a08") { ... }` in `#if defined(_WIN32) ... #endif`.
Configure still succeeds (the text matches). On Linux, CTest
`contract-agent-a08` runs the binary with `--test-case=contract-agent-a08`,
selects nothing, and exits 0. The aggregate `test-contract-operator` also passes,
because the case simply is not there. A REQUIRED_CORE requirement's only gate
now proves nothing, on the platform where CI runs, and the configure-time
coverage check reports full coverage.

**Fix shape.** Compare `ctest -N` output — or the binary's own
`--list-test-cases` — against `UF_REQUIRED_DOCTEST_CONTRACTS` at build time, so
the set is taken from the linked binary rather than from source text. That check
runs after compilation and cannot be fooled by a preprocessor.

---

## R3-F5 — medium — the contract/schema split is a naming convention with no enforcement

**VERIFIED by reading.**

`tests/CMakeLists.txt:47-66` documents the vocabulary and ends:

> What this list forbids is the case the ruling was written against — a
> `contract-` gate whose assertions only read a schema file.

The list forbids no such thing. It is a list of 47 strings. What the CMake code
actually enforces is:

* registered names == `UF_REQUIRED_DOCTEST_CONTRACTS` (string set equality);
* requirement IDs derived from those names == `UF_REQUIRED_CORE_REQUIREMENTS`;
* every requested case name matches `^(contract|schema)-`.

None of these looks at what a case does. Whether a `contract-` gate executes any
production code is decided entirely by whoever typed the name. R3-F1 is an
instance that already got through in the same commit that introduced the rule.

I checked the coverage check itself for the property the commit claims for it
("dropping both of a requirement's gates has to fail here") and it *does* hold:
`UF_REQUIRED_CORE_REQUIREMENTS` is written out rather than derived, and the
`LENGTH ... EQUAL 42` guard blocks the obvious way to silence it. That part is
sound. The unsound part is that the classification it pins is unverifiable.

**Fix shape.** The nearest mechanical proxy is cheap: assert at configure time
that every `contract-*` case's source region contains at least one identifier
from the product namespace (or, better, that its translation unit is not
`readSchema`-only). A weaker but honest alternative is to stop claiming the list
forbids it and record the classification as reviewer-enforced.

---

## R3-F6 — medium — `dcc43b5`'s "cannot be split" is not true

**VERIFIED by reading.**

> One landing, because `tests/CMakeLists.txt` enforces that the registered case
> names exactly match the declared `TEST_CASE`s. Splitting this would leave an
> intermediate commit where configure fails for the whole tree, not just for one
> target.

That coupling is real for the gate rename and the exported suite. It does not
reach two other parts of the same commit:

* **`scripts/check_safety.py` + `tests/test-check-safety.py`** — renaming ten
  rule names from `ADR-011 forbidden ...` to `background_only forbidden ...`,
  removing `external` from `UNSAFE_DIRECTORY_NAMES`, and adding
  `test_a_vendored_directory_is_never_a_boundary_directory`. None of this touches
  a `TEST_CASE` name, a CMake list, or a CTest registration. It compiles and
  passes on its own.
* **Artifact reclamation by database refcount** — `ledger.cpp` (+349),
  `runtime-installation.{hpp,cpp}` (+78), the `runtime_publications` table, the
  `k_exactSchemaV1Fingerprint` move, and four new cases in `test-ledger.cpp`.
  `test-operator` is registered by `cpp_add_test`, not `cpp_add_contract_suite`,
  so the case-name enforcement does not apply to it. Its only dependency in the
  range is `ConfinedRoot::removeTree`, which arrives in `cec8898` — a *later*
  commit, so at `dcc43b5` this half already stands alone.

Only `SOURCE_ROOTS += "contract-suite"` in the two Python gates genuinely needs
the new directory to exist.

This matters beyond tidiness: a 3,888-line commit with a stated
impossibility-of-splitting is exactly the commit that a reviewer reads least
carefully, and R3-F1 and R3-F2 both live inside it.

---

## R3-F7 — medium — the workspace SQLite schema root hash is pinned nowhere

**VERIFIED by reading.**

`25520a3`: "the Python workspace SQLite schema root moves to 72fa0c39 with four
new tables, and existing workspaces stop opening rather than migrating."

`tools/annotate/store.py:480`:

```python
SCHEMA_ROOT_HASH = _sha256(jcs_bytes({ "application_id": ..., "objects": [...], "user_version": ... }))
```

It is *derived*, never compared with a checked-in literal. The only assertion on
it in the whole tree is `tools/annotate/tests/test_backend.py:588`:

```python
self.assertRegex(SCHEMA_ROOT_HASH, r"^[0-9a-f]{64}$")
```

which is true of any SHA-256 and cannot fail. The value `72fa0c39…` appears in
`docs/plans/2026-08-09-claude-handoff.md:229` and
`docs/plans/2026-08-10-next-block.md:246` and in no executable check.

The cross-boundary check does not help either. `store.py:2247`:

```python
or release_manifest["workspace_sqlite_schema_hash"] != SCHEMA_ROOT_HASH
```

compares the stamp Python wrote (`publication.py:499`) against the value Python
just computed. On the C++ side, `runtime-installation.cpp:281` only
`consume()`s the field *name* in the canonical byte order; it never checks the
value. So the field travels the trust boundary and is verified by nobody.

Contrast the sibling that *is* done right:
`k_annotationWorkspaceSchemaHash` is a literal in
`modules/operator/source/operator/runtime-installation.hpp:21`, and
`tests/test-runtime-surface.py` pins it to the checked-in schema file so the two
cannot drift. `SCHEMA_ROOT_HASH` deserves the same.

**Exact mutation demonstrating the hole.** Delete any `CHECK` from any `CREATE
TABLE` in `_SCHEMA_OBJECTS` — say the `passed = 1` constraint on
`project_operation_replay_intents`. `SCHEMA_ROOT_HASH` changes silently, the
release manifests stamp the new value, and every test still passes;
`test_schema_and_application_drift_are_rejected_without_migration` only proves
that a database whose DDL differs from the *current* module is refused, which
remains true.

---

## R3-F8 — medium — two canonicalisers in one package, both hashed, disagreeing

**VERIFIED by reading and by executing both.**

`tools/annotate/store.py:109-120`:

```python
def _canonical_document(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False)
```

`tools/annotate/jcs.py:43-45` sorts by `key.encode("utf-16be")` and rejects every
float and every integer outside ±(2⁵³−1).

They disagree on two axes, and both feed SHA-256 identities
(`store.py:1274`, `:1482`, `:1716` use `_canonical_document`;
`_bundle_identity` and `result_id` use `jcs_bytes`):

| input | `_canonical_document` | `jcs_bytes` |
|---|---|---|
| `{"": 1, "\U00010000": 2}` | `{"":1,"𐀀":2}` (code point order) | `{"𐀀":2,"":1}` (UTF-16 order, RFC 8785 §3.2.3) |
| `{"v": 1e-7}` | `{"v":1e-07}` | raises `CanonicalJsonError` |
| `{"v": 100.0}` | `{"v":100.0}` | raises |

`25520a3` uses both inside one function.
`record_project_operation_replay_result` computes
`report = _canonical_document(document["report"])` and hashes it, then computes
`result_id = _sha256(jcs_bytes(identity_document))` — two different canonical
forms, one identity. `_bundle_document` does the same:
`content = _object(_canonical_document(dict(value)))` followed by
`_bundle_identity(content)` = `sha256(jcs_bytes(content))`.

That last pairing has a concrete consequence. `_canonical_document` **accepts**
floats (`allow_nan=False` only bans NaN/±Inf); `jcs_bytes` **rejects** them. So
`record_replay_bundle(cap, {..., "journal_prefix_length": 1.0})` reaches
`_bundle_identity` and raises a bare `CanonicalJsonError` — a `ValueError` that
is not a `StoreError` — out of the store's public API, before the `try/except
ValueError` that wraps `require_valid`. The CLI path is shielded only
incidentally, because `trusted.py::_load_exact_object` rejects floats at parse
time; the API the tests use is not.

This is "two spellings of one thing", which `CLAUDE.md` forbids outright. The
repository-wide fix is one canonicaliser; the minimum fix is to stop calling the
non-JCS one "canonical".

---

## R3-F9 — medium — where the JCS spellings actually diverge

**VERIFIED by reading all implementations and executing the Python ones; the
node/ECMAScript comparisons were executed against a reference interpreter.**

`6b20c3d`'s own report is right that the transform is spelled more than once and
untested across spellings. Two corrections and the concrete inputs:

* **There is no third C++ *serializer*.** `modules/core/source/core/text/json-text.cpp:15-45`
  implements the string escape only. Member ordering, number formatting and
  container framing have no C++ implementation; every C++ emitter hand-writes
  them inline (`manifest.cpp:95`, `ledger.cpp:870`, `trace/event.cpp:22`,
  `explore-protocol.cpp:392`). So the three-way overlap is one axis, and on that
  one axis all three agree (escape set enumerated over U+0000–U+02FF plus
  U+007F/2028/2029/FEFF/FFFD/10000/1F600: zero mismatches).
* **There is a fourth spelling**, `store.py::_canonical_document` — see R3-F8.

Named divergences:

| input | Luau `jcs.encode` | Python `jcs_bytes` | C++ |
|---|---|---|---|
| `9007199254740993` (2⁵³+1) | `9007199254740992` — the rounding is *pinned* at `tests/task/test-jcs.luau:173-177` | raises "JCS integer exceeds the exact I-JSON range" | `9007199254740993`, exact (`trace/event.cpp:36-45` via `std::format("{}", int64)`) |
| `1.5`, `1e-7`, `-0.0`, `1e21` | `1.5`, `1e-7`, `0`, `1e+21` | raises (integer-only) | n/a |
| `{"detections": {}}` | `{"detections":[]}` — a Lua table with no name-keys takes the array branch (`jcs.luau:230-233`, pinned at `test-jcs.luau:309`) | `{"detections":{}}`, and `load_exact_jcs` accepts those bytes as canonical | n/a |
| the byte string `\xFF` | rejects, "is not valid UTF-8" | rejects | **passes through verbatim** — `json-text.hpp:11-13` documents that the caller must have validated |
| CESU-8 `\xED\xA0\x80` | rejects | rejects | passes through |
| `0.1` on the CLI protocol line | `0.1` | n/a | `0.10000000000000001` — `entry/cli/explore-protocol.cpp:405` uses `{:.17g}`, not the shortest-round-trip rule |

Two of those have teeth today. The empty-object one is a silent, hash-changing
asymmetry: no Luau value can round-trip a Python-authored document containing
`{}`, and such documents are legal on the Python side. The `\xFF` one is
reachable: `explore-protocol.cpp:485` takes a raw Luau string straight to
`appendJsonString` with no `isValidUtf8` call (the only one in that file is on
the parse path at `:381`).

`tests/task/test-jcs.luau`'s header says the expected bytes "were produced by an
ECMAScript reference … and cross-checked against `tools/annotate/jcs.py`". That
was a manual act; nothing in the tree re-runs it. `git grep -rln "jcs|8785" HEAD
-- tests contract-suite` returns three files, none of which invokes another
implementation. The remedy the commit itself names — one shared vector file read
by all three — is not implemented, and cannot be until C++ gains a value-tree
entry point.

Also worth recording, since it is not a divergence but is surprising:
`modules/trace/source/trace/event.cpp:151-177` emits members in a deliberately
non-JCS order, while `json-text.hpp:16-19` lists that file as one of the "three
components". A future reader will try to hash that stream.

---

## R3-F10 — medium — the `.` and `..` clauses of `requireChildName` cannot be falsified on Windows

**VERIFIED by reading.**

`modules/task/source/task/platform/confined-file.cpp:56-70`:

```cpp
if (name.empty() || name == "." || name == ".."
    || name.contains('/') || name.contains('\\'))
```

`cec8898`'s message lists seven mutations run on each platform; the `.`/`..`
clause is not among them. Reasoning through it:

**Mutation.** Delete `|| name == "." || name == ".."`.

* **Windows.** `removeTree(".")` builds `<root>\.` and calls
  `openNode(path, k_removalAccess)`, which asks for `DELETE`. The root handle
  `ConfinedRoot` already holds was opened with share mode `FILE_SHARE_READ` and
  no `FILE_SHARE_DELETE`, so the second open is a sharing violation against
  ourselves → `ioFailure`. `CHECK_FALSE(root->removeTree(".").has_value())`
  passes. `removeTree("..")` opens the *parent* of the root, which nothing holds,
  enumerates it, recurses into `root` — and that recursion hits the same sharing
  violation, so the call fails before anything under the fixture is unlinked.
  The final `CHECK(std::filesystem::exists(inside / "orphan" / "assets"))` also
  passes. **The case is green with the clause deleted.**
* **POSIX.** `removeTreeAt(rootfd, ".", 0)` stats the root, opens it, enumerates
  it, and recursively removes **every child of the root**, then fails on
  `unlinkat(rootfd, ".", AT_REMOVEDIR)` with `EINVAL`. `CHECK_FALSE(...)` passes;
  the trailing `CHECK(exists(inside/"orphan"/"assets"))` goes red — but only
  after the root has been emptied.

So the clause is guarded on exactly one platform, the refusal on the other comes
from an unrelated mechanism, and the failure mode on the guarded platform is
"delete everything, then report an error". Note the test's own comment already
narrows the blast radius on purpose ("The root sits one level down, so a name
that escaped it could still only reach this test's own directory"), which is what
makes the Windows mutation unobservable.

Today `removeTree` is only ever called with names that came out of
`childNames()`, which filters `.` and `..`, so this is defence in depth rather
than a live bug. It is on the list because the test claims to guard it.

**Fix shape.** Assert the *reason*: `CHECK` that `removeTree(".")` fails with
`AutomationErrorKind::InvalidResource` (the `refuse` kind) rather than
`IoFailure`. That distinguishes the clause from the sharing violation and turns
red on Windows when the clause is deleted.

---

## R3-F11 — medium-low — W11's scope was written after most of it was fixed

**VERIFIED by reading.**

Order within the block: `603b0b0` lands second, `eadaef9` fifth, `5bb281d` sixth.

`603b0b0` reports: "106 unique clang-tidy sites over 137 objects, of which 77
were in scope here and are now 0 … The remaining 29 sites and 11 compile errors
are all in `modules/operator`, `modules/task/.../platform` and `contract-suite`."

`eadaef9` then writes W11 (`docs/plans/2026-08-10-next-block.md:155` and
`:190-196`): "about 14 `-Werror` failures … and roughly 90 fatal clang-tidy
diagnostics", and `5bb281d` copies the same two numbers into
`.claude/skills/cpp-coding/references/coding-standard.md:20-24`.

`git grep` over `docs/plans/2026-08-10-next-block.md` for `137`, `106`, `77` or
`29 sites` returns nothing: the plan never mentions `603b0b0`'s result. The plan
*does* explain that its own figure came from a run that "analyzed 92 objects and
never reached `modules/task`, `modules/operator` or `entry/`, so the 90 is a
floor and not a total" — which is honest about the measurement but does not
reconcile it with the fix that landed two commits earlier and cleared 77 sites
over 137 objects.

The effect is that the block's most-read planning document states as open work a
number that its own earlier commit largely retired, and states it as the current
condition. `5bb281d`'s subject is "correct the claims that no gate was
enforcing"; this is the same class it set out to fix.

I cannot rule out that "90 diagnostics" and "29 sites" count different things —
one site can emit several diagnostics. The text does not say so, and the two
figures sit in the same block with no bridge between them. Either reconcile them
or state the units.

---

## R3-F12 — low — the frameless-bundle case does not test framelessness

**VERIFIED by reading.**

`tools/annotate/tests/test_backend.py`,
`test_frameless_bundle_audits_but_cannot_stand_in_for_a_frame_replay`:

```python
project_id = self.workspace.attest_project(bundle_id=recorded["bundle_id"])[1]
with self.assertRaises(NotFound):
    self.workspace.publisher().publish("candidate-1", 1, None, (project_id, ids[1]), project_id)
self.workspace.publisher().publish("candidate-1", 1, None, ids, project_id)
```

The `NotFound` comes from `_replay_rows` looking `project_id` up in
`replay_result_intents`, where it is not — i.e. from the table separation, which
has nothing to do with frames. The bundle's frame count never enters either
assertion.

**Exact mutation.** Change `self.workspace.add_bundle(with_frames=False)` to
`with_frames=True` and drop the two `frame_count`/`frame_retention` assertions
at the top. Both `publish` outcomes are byte-identical. The property named in the
test's title — that a bundle with `frames: []` cannot satisfy a frame replay — is
not asserted anywhere, and in fact cannot be, because a Replay Bundle is not an
input to the UI gate at all.

Either rename the case to what it proves (project/operation evidence is not
accepted in a UI slot) or give it an assertion about frames.

---

## R3-F13 — low — `fix_format.py` rewrites bytes inside raw string literals

**Mechanism VERIFIED; the consequence is INFERRED.**

`ledger.cpp:337-348` documents that `k_exactSchemaV1Fingerprint` covers stored
DDL text and that "reindenting the `R"sql(...)"` block below changes it even when
the schema is identical".

`scripts/fix_format.py::normalize` operates on lines, with no notion of a raw
string:

```python
line = line.rstrip(" \t")
if replace_tabs:
    line = line.replace("\t", "    ")
```

`replace_tabs` is on for every `.cpp`, `modules/` is not excluded, and the tab
replacement is applied to the *whole line*, not only its indentation. So a tab or
a trailing space introduced anywhere inside that SQL block is silently rewritten
by the formatter that `scripts/ci-local.*` runs. `docs/pitfalls/repository-tooling-invocation.md`
(added in this block) already records that the repo-wide formatters ignore path
ownership and reformatted other agents' files twice today.

The immediate blast radius is contained: `initialize()` re-verifies right after
creating the schema, so a whitespace-only change turns every store-touching test
red at once. The residual risk is the *remedy*. The comment's instruction is to
recompute the constant "from a freshly created database"; doing that after a
formatter-induced change converts a cosmetic edit into a permanent refusal of
every `operator-runtime.sqlite` already on disk, and no gate distinguishes
"the schema changed" from "the whitespace changed". Nothing opens a
previously-created database in any test.

Given the repository's stated position (nothing is released, databases are
recreated rather than migrated) this is low. It is recorded because the comment
presents the fingerprint as self-defending and it is only half so: it defends
against forgetting to recompute, not against recomputing for the wrong reason.

There is no `.clang-format` in the tree and `.clang-tidy` sets `FormatStyle:
none`, so clang-format is not a second vector.

---

## R3-F14 — low — `f0b351b`'s closing claim, and what the suppressions actually guard

**VERIFIED, including a per-site check of the substantive claim.**

The substantive claim **holds**. All 11 `NOLINTNEXTLINE(...pro-type-member-init)`
sites were checked against their member types; only four distinct types are
involved and none is default-constructible:

| type | definition | why not default-constructible |
|---|---|---|
| `PixelRect` | `modules/domain/source/domain/space.hpp:128` | sole constructor is private, six `uint32`; public entry is `create(...) -> Result<PixelRect>` |
| `PixelPoint` | same file, `:110` | only `constexpr PixelPoint(uint32, uint32)` |
| `FrameId` | `modules/domain/source/domain/ids.hpp:36` → `StrongValue` | only `constexpr explicit StrongValue(Representation)` |
| `ContentHash` | `modules/domain/source/domain/content-hash.hpp:27` | only a private `explicit ContentHash(std::array<uint8,32>)` |

`{}` on any of them does not compile (checked against a standalone TU; the same
TU compiles `GrayTemplateImage i{}` cleanly, so the experiment can produce a
"compiles" answer). No suppression hides a real defect.

Three things around it are worth recording:

1. **The gate named in the closing sentence does not run.** "Removing one
   suppression turns the gate red" — clang-tidy runs only under
   `CPP_ENABLE_CLANG_TIDY`, i.e. the `*-analysis` presets and the
   `clang-analysis` CI job. It is in none of the four checks
   `scripts/ci-local.*` runs. And the same block's own documentation
   (`coding-standard.md:20-24`, added by `5bb281d`) states the job **does not
   compile**. So the claim is about a lane that is currently red for other
   reasons, which the commit message does not say. (`603b0b0`, by contrast, is
   scrupulous about this.)
2. **The `static_assert` proof was not committed.** `git show f0b351b
   --diff-filter=A` is empty and `git grep is_default_constructible` at HEAD
   finds no assertion about `PixelRect`, `PixelPoint`, `FrameId` or
   `ContentHash`. The proof was run and discarded, so nothing stops one of these
   four types from gaining a default constructor later and turning 11
   suppressions into 11 hidden defects. Six one-line `static_assert`s in an
   existing test would close it.
3. **The suppression is per record, not per member.** `NOLINTNEXTLINE` sits above
   `struct X final`, and the diagnostic is one warning per record listing every
   uninitialised field. Adding a bare `uint32 count;` to any of the 11 structs
   later is genuinely indeterminate and will be swallowed silently.
   `TemplateStore::Entry` already carries a third uninitialised member
   (`GrayTemplateImage image;`) that the check never named — harmless, because
   that type *is* default-constructible, but the site comment does not mention it.

Related, and outside the commit's claims: `scripts/member_init.py` — which *is*
in the local gate, via `check_safety.py:12` and `:288` — matches only members
named `m_\w+`. Every member at all 11 sites is a bare-named struct field and is
therefore invisible to it. Its docstring says the standard requires "every stored
data member" carry an initializer and that "this module reports both failures",
and its "deliberately skipped" list does not mention the `m_` restriction. So
between `member_init.py`'s naming restriction and the analysis lane not building,
no automated check currently reaches these members at all.

---

## R3-F15 — low — `.clang-tidy` and the Python gates disagree on what is vendored

**VERIFIED by reading.**

```yaml
HeaderFilterRegex: '[/\\](modules|entry|tests|contract-suite)[/\\]'
ExcludeHeaderFilterRegex: '[/\\](external|build|\.worktrees)[/\\]'
```

`scripts/check_safety.py` and `scripts/check_cpp_format.py` both use
`VENDORED_DIRECTORY_NAMES = {"external", "third_party"}`. `.clang-tidy` does not
know `third_party`. The comment claims "the vendored and generated trees, every
one of which lives under an `external` or `build` directory", which is true today
and is a statement about the current tree rather than a rule. The first
`third_party` directory under `modules/` gets linted with `WarningsAsErrors: '*'`
while both Python gates skip it.

Two smaller notes on the same block:

* `ExcludeHeaderFilterRegex` was added in LLVM 19. CI pins clang-tidy 23, so CI
  is fine. The `x64-analysis` lane uses "whatever clang-tidy is on `PATH`, in
  clang-cl mode" (`coding-standard.md:16-18`); on a host with clang-tidy 18 the
  key is ignored and `tests/external/doctest/doctest/doctest.h` enters the filter
  with errors-as-warnings on.
* `fix_format.py` excludes both `.worktrees` and `(".claude", "worktrees")`;
  `ExcludeHeaderFilterRegex` excludes only the former.

All binary directories are `${sourceDir}/build/${presetName}`, so the `build`
exclusion is currently sound — verified against `CMakePresets.json`.

---

## R3-F16 — low — the two suite helpers do not enforce the same property

**VERIFIED by reading.**

`cpp_add_contract_suite` (`tests/CMakeLists.txt`) requires
`SORTED_REQUESTED_CONTRACT_CASES STREQUAL SORTED_DECLARED_CONTRACT_CASES` — exact
set equality. `uf_add_operator_contract_suite`
(`cmake/operator-contract-suite.cmake`) only requires each requested case to be
*in* the declared set; there is no reverse check.

For a consumer that registers no CASES this is correct and deliberate. It is
recorded because `dcc43b5`'s stated reason for landing as one commit is exactly
the exact-match property, and that property holds for one of the two helpers it
introduces. A `contract-*` case added to `contract-suite/source/*.cpp` and never
registered runs only under the aggregates, which is the pre-`dcc43b5` situation
the commit set out to end.

---

## R3-F17 — low — the depth-ceiling case's comment overstates the removal property

**VERIFIED by reading.**

`tests/task/test-confined-file.cpp`, in "refuses a removal deeper than its
recursion ceiling":

```cpp
// Nothing is unlinked until every child call has returned, so the
// refusal leaves the tree standing rather than half removed.
CHECK(std::filesystem::exists(temporary.path() / "deep"));
```

That is true of the fixture, which is a 40-deep chain with exactly one child per
level, so no sibling is ever processed before the failure. It is not true in
general: `removeTreeAt` deletes each child as its own call returns (Windows: the
`FILE_DISPOSITION_INFO` takes effect when the child handle closes at the end of
its call; POSIX: `unlinkat` runs before the loop continues). Given a directory
`[a, b]` where `a` is ordinary and `b` contains a reparse point, `a` is gone by
the time `b` refuses, and `removeTree` returns an error over a half-removed tree.

No case exercises that shape. It is low because a partially removed orphan is
still an orphan and the next reclamation pass finishes it — but the comment
states an all-or-nothing property that the code does not have, and a later reader
will rely on it.

---

## Verified negatives

Recorded so the next round does not spend the time again.

* **The 19 `schema-*` gates do go red when their definition is removed.** Every
  name passed to `definition(schema, name)` — 38 distinct names across
  `umbraflow-operator-v1`, `umbraflow-journal-v1`, `umbraflow-policy-v1`,
  `umbraflow-annotation-workspace-v2` and `umbraflow-trace-v2` — occurs **exactly
  once** in its file (checked by counting `"<Name>"` occurrences in each file).
  So `definition()` cannot latch onto a `$ref` or a `required` entry, and
  deleting a definition makes `REQUIRE(namePosition != std::string::npos)` fire.
  The residual weakness is finer-grained: the field checks are substring
  searches over the whole definition text including nested objects, so a property
  removed from `properties` but left in `required` would still be found.
* **The Replay Bundle identity cannot be steered by a caller.**
  `_bundle_document` refuses a supplied `bundle_id` before anything else,
  the workspace schema's root is a `oneOf` over 13 definitions each with
  `additionalProperties: false` (so an extra field cannot ride into the hash —
  and that extra-field case is what makes the `require_valid` call load-bearing
  this time: `self.closure(seed_version=2)` is the only one of the subtests that
  would pass with the call deleted), and `_verify_bundle_file` recomputes the
  address from the stored bytes on every read.
  One design note rather than a defect: the address covers
  `frame_retention_expires_at`, a caller-chosen policy timestamp, so it is the
  content address of the closure *and its retention decision*. Two bundles over
  identical evidence that differ by one second of expiry are two permanent rows.
* **The two publication gates really do read from different tables on every path
  checked.** `_replay_rows` queries `replay_result_intents` only;
  `_project_operation_row` queries `project_operation_replay_intents` only; the
  error paths (`NotFound`, identity mismatch, already-consumed, expired bundle)
  all raise before `Publisher.publish` reaches `_export`, and
  `test_publication_refuses_a_bundle_whose_frame_retention_has_run_out` asserts
  the head generation is still 0 afterwards and then publishes for real — a
  genuine positive control. The `IntegrityError` branch in
  `record_project_operation_replay_result` re-reads by `result_id` and re-raises
  as a `Conflict` when the collision came from the
  `UNIQUE(candidate_id, candidate_revision, replay_bundle_id, report_hash)`
  constraint rather than from the primary key, so the idempotent-hit path does
  not swallow a different row.
* **The 42-requirement coverage check does catch a requirement losing every
  gate.** `UF_REQUIRED_CORE_REQUIREMENTS` is written out rather than derived, and
  the `LENGTH ... EQUAL 42` guard blocks the shortcut. What it cannot catch is
  R3-F5.
* **The 11 member-init suppressions are all correct** — see R3-F14.
* **`jcs.luau`'s number formatting is right.** `shortestDigits`
  (`jcs.luau:165-184`) was compared against a shortest-round-trip reference over
  400,000 random double bit patterns plus 19 hard cases (`1e23`,
  `9.999999999999999e22`, `4.35`, subnormals, `DBL_MAX`): zero mismatches. Its
  key ordering (`utf16Units`/`unitsLess`, `:102-128`) is genuinely UTF-16
  code-unit order and agrees with `jcs.py`'s `utf-16be` sort. Every hand-written
  member order on the C++ side (`manifest.cpp:95-111`, `ledger.cpp:870-892`,
  `runtime-installation.cpp:251-284`, `page-model-file.cpp:325-363`) was checked
  against that rule and is correct today.

## What would move this to PASS

R3-F1 and R3-F2 are the blocking pair: one is a `contract-` gate that does not
gate, the other is a protection mechanism with no test and a stated rationale the
code does not support. R3-F3 is blocking as a matter of the repository's own
stop condition. R3-F4 and R3-F5 are the structural half — the new gate machinery
can still be satisfied by a name — and are worth closing while the machinery is
fresh.
