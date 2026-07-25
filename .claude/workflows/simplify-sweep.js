export const meta = {
  name: 'simplify-sweep',
  description: 'Dual-model review of all first-party C++ under modules/ and entry/, producing a verified simplification worklist (no source edits)',
  whenToUse: 'When you want a repo-wide quality sweep (reuse, simplification, altitude, over-engineering, cpp-coding rule drift) across modules/ and entry/ without touching source files. Report-only: writes docs/reviews/<date>-simplify-sweep.md.',
  phases: [
    { title: 'Sweep', detail: 'per group: one Claude reviewer + one Codex/GPT-5.6 reviewer, independent' },
    { title: 'Verify', detail: 'per group: one adversarial verifier, refute-by-default, scores 0-100' },
    { title: 'Report', detail: 'cross-group pattern merge, writes docs/reviews/<date>-simplify-sweep.md' },
  ],
}

// ---------------------------------------------------------------------------
// Scope. `external/` holds vendored luau + imgui and is never in scope.
// Groups are sized so one agent can read a whole group (~3.5k-7.8k lines).
// ---------------------------------------------------------------------------

const GROUPS = [
  {
    key: 'core-domain',
    paths: ['modules/core', 'modules/domain'],
    note: 'The reusable foundation plus the domain vocabulary. Highest bar: anything here is vocabulary every other module reads. Watch for facilities that duplicate C++23 stdlib, and for domain types that leaked policy.',
  },
  {
    key: 'engine-image-vision-script',
    paths: ['modules/engine', 'modules/image', 'modules/vision', 'modules/script'],
    note: 'Four small subsystems. Watch for the same helper re-implemented in two of them, and for platform or third-party types (luau, image codecs) escaping their owning module.',
  },
  {
    key: 'annotation',
    paths: ['modules/annotation'],
    note: 'Largest module. Watch for over-abstraction (one-implementation interfaces), long functions that are really three, and duplicated geometry/serialization logic.',
  },
  {
    key: 'controller',
    paths: ['modules/controller'],
    note: 'Most files of any module. Watch for near-identical sibling files, dispatch tables that could be data, and state machines spread across files.',
  },
  {
    key: 'm0-demo',
    paths: ['entry/m0-demo'],
    note: 'Largest entry. Demo code drifts: watch for copy-pasted setup blocks, dead experiments, and logic that belongs in a module rather than an entry.',
  },
  {
    key: 'workbench-cli',
    paths: ['entry/workbench', 'entry/cli'],
    note: 'The two shipping entries. The workbench panel sources are the longest files here, so check whether any has grown past the point where one file is still the right unit. Watch for ImGui panel code duplicated across panels, and for CLI argument plumbing that reinvents core facilities.',
  },
]

const CATEGORIES = [
  'reuse',            // re-implements C++23 stdlib or an existing core/module facility
  'simplification',   // same behaviour, materially fewer lines or branches
  'dead-code',        // unreachable, unused, or a speculative feature with no caller
  'over-engineering', // one-implementation abstraction, single-caller layer, unset config
  'altitude',         // logic sitting at the wrong layer, or a type escaping its owning module
  'efficiency',       // needless copies or allocations on a path that runs often
  'rules',            // a cpp-coding mandatory-rule violation the repo gates do not catch
]

const FINDINGS_SCHEMA = {
  type: 'object',
  properties: {
    findings: {
      type: 'array',
      maxItems: 12,
      items: {
        type: 'object',
        properties: {
          file:       { type: 'string', description: 'repo-relative path, e.g. modules/core/source/core/types/integer.hpp' },
          line:       { type: 'integer', description: '1-indexed anchor line' },
          symbol:     { type: 'string', description: 'enclosing function/type name, or "" if none' },
          category:   { type: 'string', enum: CATEGORIES },
          title:      { type: 'string', description: 'one compressed line, <=80 chars, the claim alone' },
          current:    { type: 'string', description: 'what the code does today, <=2 sentences' },
          proposal:   { type: 'string', description: 'the concrete change: name the stdlib/core facility, the lines to delete, or the split. <=3 sentences' },
          rule:       { type: 'string', description: 'cpp-coding mandatory rule number if category is "rules", else ""' },
          linesSaved: { type: 'integer', description: 'best estimate of net lines removed, 0 if neutral' },
          risk:       { type: 'string', enum: ['mechanical', 'local', 'behavioural'] },
        },
        required: ['file', 'line', 'category', 'title', 'current', 'proposal', 'linesSaved', 'risk'],
      },
    },
  },
  required: ['findings'],
}

const VERDICTS_SCHEMA = {
  type: 'object',
  properties: {
    verdicts: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id:      { type: 'integer', description: 'the candidate id you were given' },
          score:   { type: 'integer', description: '0-100 per the rubric' },
          keep:    { type: 'boolean', description: 'true only when score >= 80' },
          reason:  { type: 'string', description: 'one sentence: the evidence that settled it' },
          correction: { type: 'string', description: 'if the finding is real but its file/line/proposal is wrong, the corrected version; else ""' },
        },
        required: ['id', 'score', 'keep', 'reason'],
      },
    },
  },
  required: ['verdicts'],
}

// ---------------------------------------------------------------------------
// Shared prompt fragments. Both models get the same mandate so their findings
// are comparable; only the enumeration mechanics differ.
// ---------------------------------------------------------------------------

function mandate(group) {
  return `You are reviewing one group of a C++23 repository at the repository root (this is your current working directory) for SIMPLIFICATION ONLY.

GROUP: ${group.key}
IN SCOPE: ${group.paths.join(', ')}
GROUP NOTE: ${group.note}

Enumerate the files in scope with:
  git ls-files ${group.paths.join(' ')} | grep -E '\\.(cpp|hpp)$' | grep -v '/external/'
Never review anything under an external/ directory — that is vendored luau and imgui.

READ THESE RULE SOURCES FIRST (they are the standard you judge against):
  CLAUDE.md
  .claude/skills/cpp-coding/SKILL.md
  .claude/skills/cpp-coding/references/coding-standard.md
  .claude/skills/cpp-coding/references/core-reuse.md
Also skim docs/ARCHITECTURE.md for the module boundaries, so an "altitude"
finding cites the documented layering rather than your own taste.

YOUR MANDATE — quality only, in these categories: ${CATEGORIES.join(', ')}.
This is NOT a bug hunt. Do not report correctness defects, races, or missing error
handling; a different review owns those. Report code that is more complicated than
the job requires.

HARD RULES:
- Every finding needs a real file path and a line number you actually read.
- Every finding needs a CONCRETE proposal. "Consider refactoring" is not a finding.
  Name the stdlib facility, name the core function, name the lines to delete, or
  name the split and its seams.
- A \`TODO(cpp-debt):\` marker is a deliberate, ledgered shortcut. Never flag it.
- Do not flag anything the repo's own gates already enforce (byte formatting,
  module dependency graph, [[nodiscard]] on Result/Status/optional, unsafe-boundary
  rules). Those are checked by scripts/fix_format.py, scripts/check_cpp_format.py,
  scripts/check_modules.py and scripts/check_safety.py.
- Prefer few high-value findings over many small ones. Cap at 12. If the group is
  genuinely clean, return fewer, or an empty list. An empty list is a valid answer
  and is better than padding.
- Rank the list yourself: highest value first, where value is (lines removed +
  clarity gained) / risk.`
}

function claudePrompt(group) {
  return `${mandate(group)}

Work efficiently: use Grep to locate patterns across the group before you Read whole
files, and Read only the regions you need. Do not dump entire files into your context.

Return the JSON object the schema requires. Nothing else.`
}

function codexPrompt(group) {
  return `${mandate(group)}

You are running as \`codex exec --sandbox read-only\` with the repository root as the
working directory. Use ripgrep and targeted file reads. Do not modify any file.

Answer with EXACTLY one JSON object and no prose, no markdown fence:
{"findings":[{"file":"...","line":123,"symbol":"...","category":"one of ${CATEGORIES.join('|')}","title":"...","current":"...","proposal":"...","rule":"","linesSaved":0,"risk":"mechanical|local|behavioural"}]}
An empty findings array is a valid answer.`
}

function verifyPrompt(group, candidates) {
  const listing = candidates
    .map((c) => `--- id ${c.id} [${c.source}] ${c.category} risk=${c.risk} saves~${c.linesSaved}
file: ${c.file}:${c.line}${c.symbol ? ` (${c.symbol})` : ''}
claim: ${c.title}
current: ${c.current}
proposal: ${c.proposal}${c.rule ? `\nrule: cpp-coding #${c.rule}` : ''}`)
    .join('\n')

  return `You are an adversarial verifier for a C++23 repository at the repository root (this is your current working directory).

Below are ${candidates.length} candidate simplification findings for the group
"${group.key}" (paths: ${group.paths.join(', ')}). They came from two independent
reviewers and are UNVERIFIED. Your job is to REFUTE them. Most flagged issues in
this kind of sweep are false positives, and a false positive that reaches the
worklist costs more than a missed finding.

For each candidate, open the cited file at the cited line and check, in this order:
1. Does the code actually say what the claim says? If the file/line is wrong or the
   code has moved, the claim is unverified — score it low unless you can locate the
   real site yourself and put the corrected location in "correction".
2. Would the proposal actually work here? Check that the named stdlib/core facility
   exists in this repo and has the right semantics — read
   .claude/skills/cpp-coding/references/core-reuse.md and the real core
   headers under modules/core rather than assuming.
3. Is there a reason the simple version was rejected? Look for a nearby comment, a
   \`TODO(cpp-debt):\` marker, a docs/pitfalls/ entry, or an ADR that explains the
   complexity. Complexity with a documented reason is not a finding.
4. Is it actually a simplification, or just a different shape of the same size?

SCORING RUBRIC — apply it literally:
  0   false positive, or the cited code does not exist as described
  25  maybe real, unverifiable from the code, or pure style not in CLAUDE.md
  50  real but a nitpick: trivial gain, or a site that barely matters
  75  verified real, meaningful gain, or directly called out in CLAUDE.md/cpp-coding
  100 verified real, large gain, evidence in the file directly confirms it

Set keep=true ONLY when score >= 80. When you are uncertain, score low — default to
refuted. Return a verdict for every id you were given.

CANDIDATES:
${listing}

Return the JSON object the schema requires. Nothing else.`
}

// ---------------------------------------------------------------------------
// Within-group merge of the two models' findings. Plain code, no agent.
// Same file + nearby line + same category is treated as one candidate, and the
// longer proposal wins (it usually carries more of the concrete detail).
// ---------------------------------------------------------------------------

function mergeGroupFindings(claudeResult, codexResult) {
  const tagged = []
  for (const f of claudeResult?.findings ?? []) tagged.push({ ...f, source: 'claude' })
  for (const f of codexResult?.findings ?? []) tagged.push({ ...f, source: 'codex' })

  const byKey = new Map()
  for (const f of tagged) {
    if (!f?.file || typeof f.line !== 'number') continue
    const bucket = Math.floor(f.line / 25)
    const key = `${f.file}|${f.category}|${bucket}`
    const prior = byKey.get(key)
    if (!prior) {
      byKey.set(key, f)
      continue
    }
    // Both models found it independently: that is corroboration, keep the richer text.
    const richer = (f.proposal ?? '').length > (prior.proposal ?? '').length ? f : prior
    byKey.set(key, { ...richer, source: 'both' })
  }

  return [...byKey.values()].map((f, i) => ({ ...f, id: i }))
}

// ---------------------------------------------------------------------------
// Phase 1 + 2, pipelined: a group verifies as soon as both its reviewers land,
// without waiting for slower groups.
// ---------------------------------------------------------------------------

log(`Sweeping ${GROUPS.length} groups with two models each (Claude + Codex/GPT-5.6)`)

const perGroup = await pipeline(
  GROUPS,

  (group) =>
    parallel([
      () => agent(claudePrompt(group), {
        label: `claude:${group.key}`,
        phase: 'Sweep',
        schema: FINDINGS_SCHEMA,
      }),
      () => agent(codexPrompt(group), {
        label: `codex:${group.key}`,
        phase: 'Sweep',
        agentType: 'codex-runner',
        schema: FINDINGS_SCHEMA,
      }),
    ]),

  (both, group) => {
    const [claudeResult, codexResult] = both ?? []
    const candidates = mergeGroupFindings(claudeResult, codexResult)

    const claudeCount = claudeResult?.findings?.length ?? 0
    const codexCount = codexResult?.findings?.length ?? 0
    if (!claudeResult) log(`${group.key}: Claude reviewer returned nothing`)
    if (!codexResult) log(`${group.key}: Codex reviewer returned nothing (check proxy)`)
    log(`${group.key}: claude=${claudeCount} codex=${codexCount} -> ${candidates.length} candidates`)

    if (candidates.length === 0) return { group: group.key, paths: group.paths, kept: [], candidates: 0 }

    return agent(verifyPrompt(group, candidates), {
      label: `verify:${group.key}`,
      phase: 'Verify',
      schema: VERDICTS_SCHEMA,
      effort: 'high',
    }).then((v) => {
      const verdicts = new Map((v?.verdicts ?? []).map((x) => [x.id, x]))
      const kept = candidates
        .filter((c) => {
          const verdict = verdicts.get(c.id)
          return verdict && verdict.keep === true && verdict.score >= 80
        })
        .map((c) => {
          const verdict = verdicts.get(c.id)
          return {
            ...c,
            score: verdict.score,
            evidence: verdict.reason,
            correction: verdict.correction || '',
          }
        })
        .sort((a, b) => b.score - a.score || b.linesSaved - a.linesSaved)

      const unjudged = candidates.length - (v?.verdicts?.length ?? 0)
      if (unjudged > 0) log(`${group.key}: ${unjudged} candidate(s) left unjudged by the verifier`)
      log(`${group.key}: ${kept.length}/${candidates.length} survived verification`)

      return { group: group.key, paths: group.paths, kept, candidates: candidates.length }
    })
  },
)

// ---------------------------------------------------------------------------
// Phase 3. Barrier is genuine here: the report merges patterns ACROSS groups
// ("this same hand-rolled span appears in four modules"), which needs every
// group's surviving findings at once.
// ---------------------------------------------------------------------------

const groups = perGroup.filter(Boolean)
const kept = groups.flatMap((g) => g.kept ?? [])
const totalCandidates = groups.reduce((n, g) => n + (g.candidates ?? 0), 0)
const droppedGroups = perGroup.filter((g) => !g).length

if (droppedGroups > 0) log(`WARNING: ${droppedGroups} group(s) failed outright and are NOT covered by this report`)

log(`Verified ${kept.length} findings out of ${totalCandidates} candidates across ${groups.length} groups`)

if (kept.length === 0) {
  return {
    status: 'clean',
    groupsReviewed: groups.length,
    groupsFailed: droppedGroups,
    totalCandidates,
    kept: 0,
    report: null,
    note: 'No candidate scored >= 80. Nothing written.',
  }
}

phase('Report')

const perGroupSummary = groups
  .map((g) => `- ${g.group} (${g.paths.join(', ')}): ${g.kept.length} kept of ${g.candidates} candidates`)
  .join('\n')

// Workflow scripts cannot call Date(), so the caller supplies today's date:
//   Workflow({ name: 'simplify-sweep', args: { date: '2026-08-01' } })
const SWEEP_DATE = args?.date
if (!SWEEP_DATE || !/^\d{4}-\d{2}-\d{2}$/.test(SWEEP_DATE))
{
  throw new Error(
    'simplify-sweep needs the current date: Workflow({ name: "simplify-sweep", args: { date: "YYYY-MM-DD" } })',
  )
}
const REPORT_PATH = `docs/reviews/${SWEEP_DATE}-simplify-sweep.md`

const reportSummary = await agent(
  `You are writing the single deliverable of a repo-wide C++ simplification sweep over
this repository (your working directory is its root).

${kept.length} findings survived adversarial verification (each scored >= 80 by a
verifier instructed to refute by default). They are given below as JSON.

COVERAGE:
${perGroupSummary}
${droppedGroups > 0 ? `\nWARNING: ${droppedGroups} group(s) failed and are NOT covered. Say so in the report.` : ''}

Write the report to ${REPORT_PATH}. Front-matter conventions to match: look at a
couple of existing files under docs/plans/ and follow their heading and date
style. Use the date ${SWEEP_DATE} in the title. Do NOT edit any C++ source file —
this sweep is report-only.

Structure the report so a person can work it top to bottom:

1. **Summary** — one paragraph: what was swept (every first-party .cpp/.hpp under
   modules/ and entry/, vendored external/ excluded), how (two independent models per
   group, adversarial verification, <80 dropped), and the headline: total findings,
   estimated net lines removable, and the 3 highest-value items.

2. **Cross-cutting patterns** — THIS IS THE MOST VALUABLE SECTION, so do it first and
   do it properly. Group the findings by shared root cause across modules, not by
   file. If the same hand-rolled helper, the same copy-paste block, or the same rule
   drift appears in several groups, that is one pattern with N sites, and fixing it
   once is worth more than N separate edits. Give each pattern a name, list its sites
   as \`file:line\`, and state the single change that resolves all of them.

3. **Worklist by group** — for each group, a table of its remaining
   (non-cross-cutting) findings: \`file:line\` | category | claim | proposal |
   lines saved | risk | score. Order by score then lines saved. Keep the proposal
   column concrete enough to act on without re-deriving it.

4. **Suggested execution order** — batches ordered so mechanical, zero-risk edits land
   first and behavioural ones last, noting which batches touch the same files and so
   must not run in parallel. Run \`git status --porcelain\` and, for any file on the
   worklist that already carries uncommitted work, flag that editing it will interleave
   with in-flight changes so the two cannot be told apart in one diff.

5. **Method and limits** — the two models used, the 0-100 rubric with the <80 cut, and
   an honest statement of what this sweep does not cover: correctness bugs, anything
   under external/, non-C++ files, and any group listed as failed above.

Use \`file:line\` form everywhere so paths are clickable. Where a finding carries a
"correction" field, the correction supersedes the original file/line.

FINDINGS JSON:
${JSON.stringify(kept, null, 1)}

After writing the file, return a plain-text summary of AT MOST 25 lines: the headline
numbers, the cross-cutting pattern names with their site counts, and the top 5
individual findings as \`file:line — claim\`. Do not restate the whole report.`,
  { label: 'synthesize-report', phase: 'Report', effort: 'high' },
)

return {
  status: 'reported',
  groupsReviewed: groups.length,
  groupsFailed: droppedGroups,
  totalCandidates,
  kept: kept.length,
  estimatedLinesRemovable: kept.reduce((n, f) => n + (f.linesSaved ?? 0), 0),
  corroboratedByBothModels: kept.filter((f) => f.source === 'both').length,
  report: REPORT_PATH,
  summary: reportSummary,
}
