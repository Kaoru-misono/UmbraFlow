--!strict
-- evidence: what counts as proof about the frame a run is looking at, who is
-- allowed to mint it, and which frame each piece of it is about.
--
-- WHY IT IS A FILE. The rule "an action is authorised by the frame it is
-- delivered against" used to live in C++, where the cycle ledger recorded a
-- page resolution and `cycle_click` read it back. Moving the page model into
-- Luau dissolved that (docs/plans/2026-08-01-three-layers-and-agent-operator.md
-- 2), and the rule was rebuilt out of two identity tables and four checks
-- scattered through `observe`. They are one subject -- what this framework will
-- accept as having come from the host, on this frame -- and a subject that
-- decides whether a click happens deserves to be readable in one place rather
-- than reconstructed from the verbs that consult it.
--
-- WHAT THE TABLES ARE FOR. A Luau table a script wrote is not evidence of
-- anything: layer three can build a table with any fields it likes, so a check
-- that read `hit.interact` off a caller-supplied table would be asking the
-- caller for permission. Both ledgers are therefore keyed by IDENTITY -- the
-- value has to be the very object this module minted -- and neither is
-- reachable from any environment a script runs in.
--
-- WHY THE HIT LEDGER'S VALUE IS A TICKET AND NOT `true`. A hit whose position
-- came from the FRAME is a claim about one frame and about no other. Two of the
-- three kinds hid that fact behind something else: a template hit carries a
-- match handle whose cycle ordinal C++ re-checks at the click edge, and a
-- page-positioned hit carries the model's own rectangle, which does not move
-- between frames. A line located by OCR is the third kind and has neither -- it
-- reaches the host as a bare coordinate with no handle to check, and its
-- position is true of the frame it was read on and of nothing else. Recording
-- the ticket makes "is this hit about the frame in front of you" answerable for
-- every kind alike, which is stricter than the two older kinds needed and costs
-- them nothing.
--
-- IT IS NOT PUBLISHED, and that is the whole of its confinement -- the same
-- decision `mint` gets and for a stronger reason. `mint` is the toolkit the
-- model constructors share, so a project holding it could shape a model the
-- constructors never saw; a project holding THIS could mint a hit claiming
-- interact on an element no page ever authorised, and `observe.click` would
-- accept it. The project environment is a whitelist with no metatable, so a
-- name that is not published is not reachable by any route
-- (docs/plans/2026-07-29-three-layer-task-system.md 7). Do not add it to
-- `frameworkProjectGlobals`.
--
-- IT IS EVIDENCE, NEVER A CACHE. `docs/plans/2026-07-31-script-owned-page-model.md`
-- 5 forbids caching a fingerprint, an observation lease or a ticket identity in
-- a Luau table to make anything faster, and this is not that: nothing here is
-- read to SKIP a host check. C++ still validates the ticket, the lease, the
-- fingerprint and the single consumption on every click, unchanged. A ticket is
-- held only so two Luau values can be compared for having come from one cycle,
-- which is a question C++ is never asked.
--
-- It loads before every module that mints, names nothing, and holds no state a
-- run can reset: the two ledgers live as long as the VM, exactly as the hits
-- and receipts in them do.

local evidence = {}

-- Every hit this framework minted, held against the ticket it was minted on.
local mintedHits: { [any]: any } = {}

-- Every receipt it minted. `true` rather than a ticket because a receipt
-- carries its own ticket in a field the holder is meant to read; a hit
-- deliberately does not, so that a caller cannot be tempted to pass the ticket
-- it finds there instead of the one it is actually holding.
local mintedReceipts: { [any]: boolean } = {}

-- Freezes `hit`, records it against `ticket`, and hands it back.
--
-- Minting and freezing are one act on purpose. A hit that was recorded and left
-- writable would be a value `observe.click` trusts and a script can still edit,
-- so the two must not be separable calls that a future verb could get half
-- right.
function evidence.mint_hit(hit: any, ticket: any): any
    local frozen = table.freeze(hit)
    mintedHits[frozen] = ticket
    return frozen
end

-- The ticket `hit` was minted on, or nil when this framework did not mint it.
--
-- One accessor for both questions, because they are one question: a hit nobody
-- minted and a hit from an earlier frame are both "this is not evidence about
-- the frame in front of you", and a caller given two predicates would sooner or
-- later check one and forget the other.
function evidence.hit_ticket(hit: any): any
    return mintedHits[hit]
end

-- The receipt saying `page` resolved on `ticket`.
--
-- WHY THE SUCCESSFUL RESOLUTION IS AN OBJECT RATHER THAN `true`. A click is
-- only authorised by the page the SAME frame resolved. While the page model was
-- C++ that held by construction; once `resolve_page` returned a boolean, nothing
-- tied the answer to the frame it was answered about, and a script could resolve
-- on one frame and click on the next. Making the success an object that names
-- its own ticket restores the property with no hidden state: the evidence travels
-- in the value the caller already had to obtain, and `observe.click` refuses to
-- act without it.
function evidence.mint_receipt(page: any, ticket: any): any
    local receipt = table.freeze({ page = page, ticket = ticket })
    mintedReceipts[receipt] = true
    return receipt
end

-- Whether `value` is a receipt this framework minted for `ticket`.
--
-- Both halves are checked here rather than at each call site, for the reason
-- `hit_ticket` answers both of its questions at once: a receipt from another
-- cycle and a table nobody minted are the same failure.
function evidence.is_receipt_for(value: any, ticket: any): boolean
    return mintedReceipts[value] == true and rawequal(value.ticket, ticket)
end

-- Whether `value` is a receipt this framework minted at all, whatever cycle it
-- came from. It is the weaker question `observe.click` asks first, so that a
-- forged table and a stale receipt get the sentence each of them deserves.
function evidence.is_receipt(value: any): boolean
    return mintedReceipts[value] == true
end

return evidence
