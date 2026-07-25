export const meta = {
  name: 'simplify-apply',
  description: 'Apply a verified refactor worklist as source edits, gating each wave and repairing gate failures',
  whenToUse: 'Second stage after simplify-sweep (or any review that produced a worklist with concrete per-item instructions). Give it items whose file sets are pairwise disjoint so they can be applied in parallel. Never commits.',
  phases: [
    { title: 'Apply', detail: 'one agent per item, in parallel, disjoint file sets' },
    { title: 'Gate', detail: 'format / modules / safety / build / ctest -L CI' },
    { title: 'Repair', detail: 'fix or revert whatever the gate reported, then re-gate' },
    { title: 'Final item', detail: 'the risky item, applied alone after the rest is green' },
    { title: 'Final gate', detail: 're-run the full gate after the risky item' },
  ],
}

// ---------------------------------------------------------------------------
// Input. Nothing about a particular worklist is baked into this script:
//
//   Workflow({ name: 'simplify-apply', args: {
//     preset: 'x64-debug',
//     items: [ { key: 'drop-dead-guard', files: ['modules/vision/.../sad.cpp'],
//                task: 'Delete lines 35-45 because ...' } ],
//     finalItem: { key, files, task },   // optional; applied alone after the rest is green
//   }})
//
// items[].files MUST be pairwise disjoint across items — that is what makes the
// parallel apply safe. The script checks and refuses otherwise rather than
// letting two agents corrupt each other's edits.
// ---------------------------------------------------------------------------

const PRESET = args?.preset ?? 'x64-debug'
const ITEMS = args?.items ?? []
const FINAL_ITEM = args?.finalItem ?? null

if (ITEMS.length === 0)
{
  throw new Error(
    'simplify-apply needs a worklist: Workflow({ name: "simplify-apply", args: { items: [{ key, files, task }] } })',
  )
}

const seen = new Map()
for (const item of ITEMS)
{
  if (!item?.key || !Array.isArray(item.files) || item.files.length === 0 || !item.task)
  {
    throw new Error(`simplify-apply: every item needs key, files[] and task — got ${JSON.stringify(item)}`)
  }
  for (const file of item.files)
  {
    if (seen.has(file))
    {
      throw new Error(
        `simplify-apply: items "${seen.get(file)}" and "${item.key}" both edit ${file}. ` +
        'File sets must be disjoint, because items are applied in parallel. Merge them into one item.',
      )
    }
    seen.set(file, item.key)
  }
}

// ---------------------------------------------------------------------------
// Every agent that edits source gets this. The repo gates enforce byte-level
// normalization, so agents must not invent a local wrapping style, and must not
// build — the Gate phase owns the toolchain so parallel editors never contend
// for it.
// ---------------------------------------------------------------------------

const EDIT_RULES = `You are editing C++23 sources in this repository (your working directory is its root).

READ FIRST, before your first edit:
  CLAUDE.md
  .claude/skills/cpp-coding/SKILL.md
  .claude/skills/cpp-coding/references/coding-standard.md
Read .claude/skills/cpp-coding/references/core-reuse.md as well if your item replaces
local code with a core facility, and error-handling.md if it touches Result, Status,
fail(), or UF_TRY*.

HARD CONSTRAINTS:
- Touch ONLY the files listed in your item. Other agents are editing other files in
  this same tree concurrently. Editing a file outside your list will corrupt their work.
- Do NOT build, do NOT run cmake, do NOT run ctest, do NOT run any formatter. A later
  phase owns the toolchain. Running a build here contends with other agents and wastes
  minutes.
- Do NOT commit, stage, stash, or run any mutating git command. The working tree may
  contain unrelated in-flight work that must not be disturbed.
- Behaviour must be preserved exactly unless your item says otherwise. Same observable
  outputs, same error messages, same diagnostics text.
- Follow the wrapping, Allman braces, trailing return types, east const, AAA locals and
  alignment conventions as they appear in the file you are editing. Match the surrounding
  code; do not introduce a new local style.
- Use the project integer aliases from <core/types/integer.hpp> and include that header
  directly in any file where you introduce one.
- If you conclude the item is wrong, or that it cannot be done without changing
  behaviour, STOP and report that instead of forcing it. A truthful "not done, here is
  why" is worth more than a risky edit.

When you are done, report what you actually changed.`

const APPLY_SCHEMA = {
  type: 'object',
  properties: {
    item: { type: 'string' },
    done: { type: 'boolean', description: 'true only if the edit was fully applied' },
    filesChanged: { type: 'array', items: { type: 'string' } },
    linesRemoved: { type: 'integer', description: 'net lines removed, may be negative' },
    summary: { type: 'string', description: 'what changed, <=3 sentences' },
    concerns: { type: 'string', description: 'anything the gate or a reviewer should look at, or "" if none' },
  },
  required: ['item', 'done', 'filesChanged', 'summary'],
}

const GATE_SCHEMA = {
  type: 'object',
  properties: {
    passed: { type: 'boolean', description: 'true only if every step below passed' },
    steps: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          name: { type: 'string' },
          passed: { type: 'boolean' },
          detail: { type: 'string', description: 'for a failure: the actual error text, trimmed to the useful part' },
        },
        required: ['name', 'passed'],
      },
    },
    failingFiles: { type: 'array', items: { type: 'string' }, description: 'files implicated by failures' },
    diagnosis: { type: 'string', description: 'what broke and which worklist item most likely caused it' },
  },
  required: ['passed', 'steps'],
}

function gateTask(what)
{
  return `Run the repository validation gate and report results precisely. ${what}

Follow the project's own procedure — read these and do what they say:
  .claude/skills/build-project/SKILL.md
  .claude/skills/post-change-validation/SKILL.md

On Windows, MSVC must be activated in the SAME shell as the build; the project provides
.claude/skills/build-project/script/windows/build-env.bat and the build-project skill
documents how to use it. Use "python" on Windows and "python3" on Linux or macOS.

Close any running application built from this tree before building. A leftover process
holds a write lock on its own executable and the link step then fails with LNK1168, which
looks like a code failure but is not one.

The minimum gate, in order — run every step even if an earlier one fails, so one report
covers everything:
  python scripts/fix_format.py            (apply normalization first)
  python scripts/fix_format.py --check
  python scripts/check_cpp_format.py
  python scripts/check_modules.py
  python scripts/check_safety.py
  cmake --build --preset ${PRESET}
  ctest --test-dir build/${PRESET} -L CI --output-on-failure

Rules:
- Do NOT edit any source file to make the gate pass. Your job is to measure and report.
  A later phase fixes things.
- Do NOT commit, stage, or stash. The tree may contain unrelated in-flight work.
- For each failure, capture the ACTUAL compiler or test error text, trimmed to the part
  that identifies the problem, and name the file and line. A summary like "build failed"
  is useless to the phase that has to fix it.

Report honestly. A passing report for a tree that does not build is the worst possible
outcome here.`
}

function itemPrompt(item)
{
  return `${EDIT_RULES}

=== YOUR ITEM: ${item.key} ===
Files you may touch: ${item.files.join(', ')}

${item.task}`
}

// ---------------------------------------------------------------------------
// Phase 1: apply every item in parallel.
// ---------------------------------------------------------------------------

log(`Applying ${ITEMS.length} item(s) across ${seen.size} file(s) (disjoint sets verified)`)

const applied = await parallel(
  ITEMS.map((item) => () =>
    agent(itemPrompt(item), { label: `apply:${item.key}`, phase: 'Apply', schema: APPLY_SCHEMA }),
  ),
)

const notDone = []
for (let i = 0; i < ITEMS.length; i += 1)
{
  const r = applied[i]
  if (!r)
  {
    notDone.push({ item: ITEMS[i].key, reason: 'agent produced no result' })
    log(`${ITEMS[i].key}: FAILED (no result)`)
  }
  else if (!r.done)
  {
    notDone.push({ item: ITEMS[i].key, reason: r.summary || r.concerns || 'reported not done' })
    log(`${ITEMS[i].key}: NOT APPLIED — ${(r.summary || '').slice(0, 120)}`)
  }
  else
  {
    log(`${ITEMS[i].key}: applied (${(r.filesChanged || []).length} file(s), ${r.linesRemoved ?? '?'} lines)`)
  }
}

// ---------------------------------------------------------------------------
// Phase 2 + 3: gate, then repair-and-regate. Bounded at two repair rounds — a
// third round means the change is wrong, not that the fix needs another try.
// ---------------------------------------------------------------------------

phase('Gate')

let gate = await agent(gateTask('This is the gate for the first wave of worklist items.'), {
  label: 'gate:first-wave',
  phase: 'Gate',
  schema: GATE_SCHEMA,
  effort: 'high',
})

const repairs = []

for (let round = 1; round <= 2; round += 1)
{
  if (!gate)
  {
    log('Gate produced no result; treating the first wave as unverified')
    break
  }
  if (gate.passed)
  {
    log(`Gate passed after ${round - 1} repair round(s)`)
    break
  }

  log(`Gate failed (${(gate.steps || []).filter((s) => !s.passed).map((s) => s.name).join(', ')}) — repair round ${round}`)

  phase('Repair')

  const repair = await agent(
    `The repository validation gate is failing after a batch of refactor edits was applied.
Fix the failures. Repair round ${round} of 2.

${EDIT_RULES}

The constraint against editing files outside a list does NOT apply to you — you are the
sole editor in this phase. But stay minimal: fix what the gate reported and nothing else.
You may run the gate commands yourself to check your work.

GATE DIAGNOSIS:
${gate.diagnosis || '(none given)'}

FAILING STEPS:
${(gate.steps || []).filter((s) => !s.passed).map((s) => `- ${s.name}: ${s.detail || '(no detail captured)'}`).join('\n')}

FILES IMPLICATED: ${(gate.failingFiles || []).join(', ') || '(none listed)'}

The edits that were just applied, so you can tell which one broke it:
${ITEMS.map((it, i) => `- ${it.key} [${applied[i]?.done ? 'applied' : 'NOT applied'}] files: ${it.files.join(', ')}${applied[i]?.concerns ? ` | concern: ${applied[i].concerns}` : ''}`).join('\n')}

Prefer fixing the edit. But if one of these items cannot be made to work without changing
behaviour, REVERT that single item back to its committed form — restore the original code
for just that file's affected region — and report which item you reverted and why.
Reverting one item cleanly is a better outcome than a tree that does not build.

Before reverting anything, run \`git status --porcelain\` and treat every file that was
already modified before this run as in-flight work belonging to someone else: never revert,
restore or reformat those beyond undoing this run's own edit to them.

Do NOT commit or stage anything.

Report what you changed and whether you reverted any item.`,
    { label: `repair:round-${round}`, phase: 'Repair', effort: 'high' },
  )

  repairs.push({ round, report: repair })

  gate = await agent(gateTask(`This is the re-gate after repair round ${round}.`), {
    label: `regate:round-${round}`,
    phase: 'Gate',
    schema: GATE_SCHEMA,
    effort: 'high',
  })
}

const firstWaveGreen = Boolean(gate?.passed)

// ---------------------------------------------------------------------------
// Phase 4: the risky item, alone. Gated separately so a failure here cannot
// cost the already-verified first wave, and skipped if the tree is not green —
// stacking a risky refactor on a broken build makes the failure unattributable.
// ---------------------------------------------------------------------------

let final = null
let finalGate = null

if (FINAL_ITEM)
{
  if (!firstWaveGreen)
  {
    log('Tree is NOT green after the first wave — skipping the final item rather than stacking onto a broken build')
  }
  else
  {
    phase('Final item')

    final = await agent(itemPrompt(FINAL_ITEM), {
      label: `apply:${FINAL_ITEM.key}`,
      phase: 'Final item',
      effort: 'high',
      schema: APPLY_SCHEMA,
    })

    log(final?.done ? `${FINAL_ITEM.key}: applied (${(final.filesChanged || []).length} files)` : `${FINAL_ITEM.key}: NOT applied`)

    phase('Final gate')

    finalGate = await agent(gateTask(`This is the gate for the final item "${FINAL_ITEM.key}".`), {
      label: `gate:${FINAL_ITEM.key}`,
      phase: 'Final gate',
      schema: GATE_SCHEMA,
      effort: 'high',
    })

    if (final?.done && finalGate && !finalGate.passed)
    {
      phase('Repair')

      const finalRepair = await agent(
        `The validation gate is failing after the final worklist item "${FINAL_ITEM.key}" was
applied. Everything else was already green before this change, so the cause is almost
certainly in the files that item touched: ${FINAL_ITEM.files.join(', ')}.

${EDIT_RULES}

You are the sole editor in this phase, so the file-list constraint does not bind you, but
confine yourself to that item's area unless the evidence points elsewhere.

GATE DIAGNOSIS:
${finalGate.diagnosis || '(none given)'}

FAILING STEPS:
${(finalGate.steps || []).filter((s) => !s.passed).map((s) => `- ${s.name}: ${s.detail || '(no detail captured)'}`).join('\n')}

WHAT THE ITEM CHANGED:
${final.summary || '(no summary)'}
Files: ${(final.filesChanged || []).join(', ')}
${final.concerns ? `Concerns the applier flagged: ${final.concerns}` : ''}

If you cannot make this item work while preserving behaviour, REVERT it entirely and
report that. The earlier wave was already verified green and must survive: do not revert
anything outside this item's files, and run \`git status --porcelain\` first so you can
recognise and preserve unrelated in-flight work.

Do NOT commit or stage anything. Report what you changed or reverted.`,
        { label: `repair:${FINAL_ITEM.key}`, phase: 'Repair', effort: 'high' },
      )

      repairs.push({ round: FINAL_ITEM.key, report: finalRepair })

      finalGate = await agent(gateTask(`This is the re-gate after repairing "${FINAL_ITEM.key}".`), {
        label: `regate:${FINAL_ITEM.key}`,
        phase: 'Final gate',
        schema: GATE_SCHEMA,
        effort: 'high',
      })
    }
  }
}

// ---------------------------------------------------------------------------

const closingGate = finalGate ?? gate

return {
  firstWave: {
    green: firstWaveGreen,
    applied: applied.filter((r) => r?.done).length,
    of: ITEMS.length,
    notDone,
    linesRemoved: applied.reduce((n, r) => n + (r?.linesRemoved ?? 0), 0),
  },
  finalItem: FINAL_ITEM
    ? (final
      ? { key: FINAL_ITEM.key, attempted: true, done: Boolean(final.done), summary: final.summary, concerns: final.concerns || '' }
      : { key: FINAL_ITEM.key, attempted: false, reason: 'skipped: tree not green after the first wave' })
    : null,
  gate: closingGate
    ? {
        passed: Boolean(closingGate.passed),
        failingSteps: (closingGate.steps || []).filter((s) => !s.passed).map((s) => ({ name: s.name, detail: (s.detail || '').slice(0, 600) })),
        diagnosis: closingGate.diagnosis || '',
      }
    : { passed: false, diagnosis: 'no gate result' },
  repairRounds: repairs.length,
  repairReports: repairs.map((r) => ({ round: r.round, report: (r.report || '').slice(0, 1200) })),
  committed: false,
}
