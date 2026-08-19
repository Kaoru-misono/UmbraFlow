# 2026-08-09 — Policy is Operator-owned

## Decision

Projects may supply a content-addressed policy artifact as deployment input.
Only Operator parses and evaluates it. ProjectPlugin receives no policy
capability and no hidden policy input.

## Context

One of the four executable specification resolutions, following the consumer main
design §5.3 pure plugin capability list and policy order.

A plugin that can read policy is a plugin whose output depends on something its
arguments do not name, and determinism then cannot be stated as a property of the
call. Keeping evaluation entirely inside Operator means plugin determinism is
bounded by its explicit arguments and its pinned ProjectRegistration, and nothing
else.

## Consequences

- The policy artifact is content-addressed, so which policy an operation was
  evaluated under is a recorded fact rather than an ambient one.
- ProjectPlugin remains a five-function data boundary; core has no game-name
  branch and no policy hook.
