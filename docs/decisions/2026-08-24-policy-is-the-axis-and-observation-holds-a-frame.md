# 2026-08-24 — Policy is the axis, and an observation holds its frame

This **supersedes one sentence** of
[`2026-08-24-there-is-no-annotation-phase.md`](2026-08-24-there-is-no-annotation-phase.md),
which keeps its bytes. Everything else that document rules stands, including the
seal, the deletion of the generation kind, and the two integrity principles.

The superseded sentence is:

> "The entire difference between annotating and running the product lives in
> `tool_closure`."

**It is false**, and the same document also named policy as the axis, so it named
two mechanisms as *the* one. Verified in the tree: framework Tools are dispatched
by namespace —

```cpp
auto const framework = validateToolNameOwnership(toolName, k_frameworkToolNamespace);
return framework.has_value()
    ? m_framework.validate(std::move(toolName), std::move(canonicalArgs))
    : m_project.validate(std::move(toolName), std::move(canonicalArgs));
```

— and project generation pins **the whole framework catalog** into every
project's resources. The closure gates project Tools and nothing else.

## V1 — The axis is policy

> The difference between an annotating session and a production session lives
> entirely in **the policy artifact its manifest pins**. `tool_closure` defines
> which *project* Tools a generation holds, and does not vary by session mode.

Making `framework.*` per-registration selectable was rejected, and not narrowly:

- **The framework catalog is the floor, not an à-la-carte menu.** Per-registration
  selection means every framework Tool ever added becomes an explicit opt-in for
  every project — the onboarding floor rises, and each framework advance becomes
  a migration for everyone. It also splits the catalog hash per project, so
  recorded identity baselines stop being comparable.
- **It buys no verification.** Refusing an unauthorised call is what policy
  already does.
- **Two mechanisms for one job are two spellings of one thing.**

It also does not revive the phase. The framework never learns what "annotating"
means; it knows only which effects this session's policy granted. That is the
owner's line landed: all of it is calling Tools, and some sessions carry
different grants.

Framework-side code changes: **none**. Namespace dispatch and whole-catalog
delivery both stay.

## V2 — An observation holds its frame, and that needs hold-with-children first

`explore.cycle(callback)` performs crop, read-lines and colour-census **on one
frame** and then acts on that frame. That sameness is the verbs' meaning, not an
implementation detail.

**`framework.screen.observe` is redefined as a holding call**: engage opens the
cycle and resolves state, the measuring verbs run as child calls against that
same frame, disengage closes it. An observe with no children is a hold with zero
children, identical to today's single-shot behaviour — one shape, with no
compatibility reading of the old one.

The reason this is legitimate where a `framework.screen.capture` Tool was
**deleted** rather than completed: **pixels never cross a hash boundary.** A child
call does not recover a frame from a reference. It executes inside the engaged
hold; the issuing context — the holding call's active-run frame — carries the
frame, whose lifetime is the hold's and which dies when the cycle closes. The
minted reference still carries only identity hashes and a state resolution. This
is a provider attachment growing inside an already-ruled mechanism, not a new
capability.

Rejected: letting each measuring Tool capture its own frame, because measuring
across several captures of a moving screen silently changes what these verbs mean
— an accident dressed as a method. Also rejected: a separate frame-holding Tool,
which is either the capture sin again or hold-with-children under another name.

**Consequence for ordering: hold-with-children is built first.**

## V3 — One input Tool, a closed enumeration, tagged arms

The six-verb vocabulary — `click`, `key`, `drag`, `hold`, `scroll`, `move` —
lands in **one** Tool's contract, not six Tools. The declared model already
spells the vocabulary as one union; six Tools would be its second spelling.

- `action` becomes a **closed enumeration** of the six verbs.
- The contract is a **tagged union**: the fields of each arm are decided by the
  tag and are **required** — `drag` carries its endpoint, `hold` its duration,
  `scroll` its notches, `key` its name. This is a sum type, not an
  absent-means-the-old-behaviour reading.
- `x` and `y` **descend from the top level into the aiming arms**. `key` carries
  no coordinates; forcing it to carry meaningless ones would be an accident
  dressed as a method.
- **The free-string `action` must die.** A verb the framework cannot validate is
  an unbound call it cannot refuse by name, and refusing an unbound Tool is
  squarely inside the framework's remit. A closed enumeration is also what lets
  effect bounds be declared per verb.
- The placeholder verdict is replaced **in the same change**. A contract widened
  while delivery stays a placeholder is one more unbound contract.

## V4 — All three effect lines are ruled now

Rejected: landing the read-only measuring Tools first and leaving writes for a
fuller policy grammar. That leaves the exploration session with one foot in the
old mechanism, which is the bridge this repository forbids. A missing policy
resolves to deny-all, so these lines are not polish — they are whether an
annotating session can exist at all. The policy grammar must be shaped by its
first real consumer, and annotation is that consumer.

- **Screen observation** (and its held child measurements): read-only, no effect
  bounds, callable under deny-all. It is the framework's own verification eating;
  its risk is zero.
- **Input injection**: effect is a change to the external world; scope is **the
  target surface the registration declares**, no wider — refusing an injection
  outside it is the framework enforcing a limit it was given; risk is the highest
  band, external and irreversible.
- **Authoring write**: effect is a change to the project's own state; scope is
  that project's **unsealed, in-progress generation's authoring store**; the risk
  is contained by the seal itself, since the install door and the bind door
  already refuse an unsealed hash by name. Authoring writes therefore cannot leak
  into production admission, and no extra containment layer needs inventing.

## V5 — One door, and a project still writes nothing

`explore` acquires everything the production door requires — an operator root, a
production project, an installed and sealed artifact, a ledger, a policy
artifact, a registration, and a session manifest pinning policy and profile —
because there is only one door. The exploration session's private route to its
own engine session is deleted in the same change.

What it is **given** rather than required to author:

- the **genesis artifact**, which is exactly why an empty RuntimeModel was made
  representable and scaffolded into every new project;
- an **annotation policy artifact**, written into the project tree by the
  scaffold — visible, editable, and owned by the project. This does not violate
  the framework's line: the limit lives in the project's own file and is handed
  to the framework to enforce, rather than chosen silently on the project's
  behalf;
- the **registration**, created at init.

Onboarding floor from nothing to a first exploration session: `init` then
`explore`, with **zero hand-written files**.

## V6 — The fitted-layout search needs a bound that was given, not derived

A collection's fitted-layout search walks upward from the detected count looking
for a larger layout with missing slots. Its old terminator was the search
rectangle; now that a span saturates at the declared extent, the rectangle no
longer bounds it, and the remaining bound is the ambiguity floor — roughly
`extent / (2 * tolerance + 1) + 1`. At `tolerance = 0` that is `extent + 1`, so
four items measured one pixel off can resolve as a partial of a 559-item layout.

**This is not the declaration being enforced faithfully.** The project declared an
extent in pixels and a tolerance in pixels. **It never said anything about a slot
count.** A slot-count bound derived from pixel declarations is arithmetic the
framework *invented*, and the old rectangular terminator was equally incidental.

The collection declaration therefore gains a **required maximum slot count**. A
project knows its own collection; a four-item declaration says eight or fewer.
The search stops at the bound, and a detected layout consistent only beyond it
gets a **specific signal** — the candidate count and the declared bound — rather
than a generic failure. `tolerance = 0` keeps its original meaning of exact even
spacing. Where several layouts remain consistent inside the bound, the existing
deterministic preference stands (the first consistent layout at or above the
detected count), because it is a recorded and reproducible resolution rule rather
than an on-the-spot judgement.

## The order the remaining work must be done in

1. **hold-with-children.** The only piece with no dependencies, already fully
   ruled, and it does not move the catalog hash. It must come first because
   observe's descriptor in step 2 has to be written against the real mechanism
   rather than guessed — and a guessed field moves every recorded baseline twice.
2. **One catalog-hash move, packaging every descriptor change**: V3's input
   contract, V4's three effect bounds, V2's holding observe, and V6's declaration
   field. Every descriptor field enters a pinned identity, so landing them
   separately moves the baselines N times and landing them together moves them
   once. There is no consumer outside this repository, so this is the cheapest
   this hash will ever be to move.
3. **One door (V5)**: `explore` through the product lifecycle's admission, the
   scaffold writing the annotation policy, and the private engine-session route
   deleted in the same change. It must precede step 4, because the natives' Tool
   calls need a session with a ledger and a policy to be recorded into.
4. **The 16 `explore_*` natives become Tools, and the private capability table is
   deleted in the same change.** It comes last because every call target it needs
   must already exist — and when it lands, **input issued while annotating is
   recorded**. That is the debt this ruling exists to repay, and its payment
   completes the final third.

Anything off this four-step chain — conformance breadth, release-facing gates —
is deferred, and the deferral is stated rather than silently taken.

## Deliberately left unresolved

- Whether the three input Tool names (`coordinate`, `deliver`, `semantic_target`)
  finally merge. The shape is ruled — one vocabulary gets one spelling — and if
  `deliver` and `semantic_target` prove to be addressing variants of one delivery,
  they merge inside step 2's single hash move. The naming is settled while
  writing the descriptors, because that is expression, not solution.
- The policy grammar beyond these three grant lines, to be shaped by its first
  consumer rather than legislated in advance.
- The numeric workflow limits for hold-with-children. They are configurables the
  declaration supplies and the framework enforces.
