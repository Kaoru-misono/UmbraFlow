# Script handles are in-process userdata; the serializable-DTO rule binds only a future worker boundary

Roadmap §5 constraint 8 ("the `IScriptRuntime` boundary passes only
serializable DTOs, scripts never hold C++ pointers") reads like a P0 rule, but
the engine's frozen handle API is built on fat, host-minted evidence objects:
`Observation` carries an ~8 MiB `Frame`, and `ResolvedPage`/`ActionFound` are
privately constructed and must flow back into `act()` unmodified.

**Decision (2026-07-27):** In P0, where Luau runs in-process, scripts hold
host-owned opaque userdata that wraps these engine objects one-to-one. The
DTO-only rule is reinterpreted as a requirement on the future out-of-process
worker seam (it activates with the documented C# worker re-evaluation
triggers), not on the P0 binding.

**Why:** evidence objects are only trustworthy while they stay in host custody
— private construction is the proof of origin, and serialized copies could be
tampered with or replayed; act-time invalidation must hit every script
reference at once, which only a single host-owned object allows; scripts never
inspect a handle, they only pass it back, so copying 8 MiB buys nothing; and
the script-visible API shape is representation-agnostic, so a later switch to
ID-plus-IPC stays inside the binding layer with no script changes.

**Consequences:** the binding layer wraps engine handles as userdata behind
protected metatables; roadmap §5 constraint 8 gets a clarifying note pointing
here; no engine API changes.
