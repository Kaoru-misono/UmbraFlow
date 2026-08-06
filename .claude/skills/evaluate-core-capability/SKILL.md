---
name: evaluate-core-capability
description: Decide whether a reusable C++ facility belongs in core, in the C++23 standard library, product-local, or deferred. Use when proposing, designing, reviewing, or promoting a generic core abstraction, ownership or runtime facility, container, concurrency primitive, reflection helper, or serialization primitive. Not for ordinary work inside an approved facility.
---

# Evaluate Core Capability

Read `references/capability-kernel.md` completely before making a recommendation.

## Procedure

1. Identify the concrete call sites and the invalid state, lifetime hazard, or
   repeated control-flow problem the proposal would remove.
2. Inspect `modules/core/source/core/` and nearby product code for an existing
   facility that already provides the contract.
3. Compare the proposal with the C++23 standard library and adopted
   dependencies. Prefer those facilities when their semantics are sufficient.
4. Test the proposal against the kernel admission criteria: enforceability,
   portability, audit size, and concise retained behavior tests.
5. Classify it as one of:
   - reuse an existing facility;
   - use the standard library directly;
   - admit the smallest enforceable contract to `core`;
   - keep it product-local;
   - defer it until evidence exists;
   - reject it as a misleading or parallel abstraction.
6. If admission is justified, define the narrow public contract, deliberate
   limits, ownership semantics, failure model, and regression evidence before
   implementation.

## Output

Report the demonstrated need, alternatives considered, classification,
smallest acceptable contract, deliberate exclusions, and required tests. Do not
recommend a general core facility from a single speculative call site.
