--!strict
-- evidence: what counts as proof about the frame a run is looking at, who is
-- allowed to mint it, and which frame each piece of it is about. It enforces
-- "an action is authorised by the frame it is delivered against"
-- (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
--
-- A Luau table a script wrote is not evidence: layer three can build a table
-- with any fields it likes, so a check reading `hit.interact` off a
-- caller-supplied table would be asking the caller for permission. Both ledgers
-- are therefore keyed by IDENTITY -- the value has to be the very object this
-- module minted -- and neither is reachable from an environment a script runs
-- in.
--
-- The hit ledger's value is a ticket rather than `true` because one of the three
-- hit kinds cannot be checked any other way: a line located by OCR reaches the
-- host as a bare coordinate with no handle, and its position is true of the
-- frame it was read on and of no other. (A template hit carries a match handle
-- whose cycle ordinal C++ re-checks, and a page-positioned hit carries a
-- rectangle that does not move between frames.)
--
-- IT IS NOT PUBLISHED. A project holding this could mint a hit claiming interact
-- on an element no page ever authorised, and `observe.click` would accept it.
-- The project environment is a whitelist with no metatable, so an unpublished
-- name is unreachable (docs/plans/2026-07-29-three-layer-task-system.md 7). Do
-- not add it to `frameworkProjectGlobals`.
--
-- It is evidence, never a cache: nothing here is read to SKIP a host check, so
-- it does not violate docs/plans/2026-07-31-script-owned-page-model.md 5. C++
-- still validates the ticket, the lease, the fingerprint and the single
-- consumption on every click. A ticket is held only so two Luau values can be
-- compared for having come from one cycle, which C++ is never asked.
--
-- It loads before every module that mints, names nothing, and holds no state a
-- run can reset: the two ledgers live as long as the VM.

local evidence = {}

-- Every hit this framework minted, held against the ticket it was minted on.
--
-- Weak keys, for the reason spelled out over `model`'s registries, and here the
-- rate is what makes it matter: one menu-to-menu run opened 3992 cycles, and a
-- strong key would have kept every hit any of them produced. The value is the
-- ticket rather than `true`, which is safe to hold strongly because a hit
-- deliberately does not carry its ticket -- so the value cannot reach back to
-- the key and keep it alive.
local mintedHits: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any

-- Every receipt it minted. `true` rather than a ticket because a receipt carries
-- its own ticket in a field the holder reads; a hit deliberately does not, so a
-- caller cannot pass the ticket it finds there instead of the one it holds.
local mintedReceipts: { [any]: boolean } = setmetatable({}, { __mode = "k" }) :: any

-- Freezes `hit`, records it against `ticket`, and hands it back. One act, not
-- two: a hit that was recorded and left writable would be a value
-- `observe.click` trusts and a script can still edit.
function evidence.mint_hit(hit: any, ticket: any): any
    local frozen = table.freeze(hit)
    mintedHits[frozen] = ticket
    return frozen
end

-- The ticket `hit` was minted on, or nil when this framework did not mint it.
-- One accessor rather than two predicates: a hit nobody minted and a hit from an
-- earlier frame are both "not evidence about the frame in front of you".
function evidence.hit_ticket(hit: any): any
    return mintedHits[hit]
end

-- The receipt saying `page` resolved on `ticket`. A click is only authorised by
-- the page the SAME frame resolved, so the success is an object naming its own
-- ticket rather than a boolean: a boolean lets a script resolve on one frame and
-- click on the next, and `observe.click` refuses to act without the receipt.
function evidence.mint_receipt(page: any, ticket: any): any
    local receipt = table.freeze({ page = page, ticket = ticket })
    mintedReceipts[receipt] = true
    return receipt
end

-- Whether `value` is a receipt this framework minted for `ticket`. Both halves
-- are checked here rather than at each call site: a receipt from another cycle
-- and a table nobody minted are the same failure.
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
