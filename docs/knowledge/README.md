# Knowledge base / 架构知识库

The same eleven-document architecture walkthrough is maintained in two languages, mirrored
file for file:

- English: [`en/README.md`](en/README.md)
- 中文: [`cn/README.md`](cn/README.md)

When a code change invalidates a statement, update both language trees in the same commit.

`atlas.html` and `tour.html` beside this file are **generated**, not hand-edited;
they are rendered from `scripts/generate_code_atlas/data/*.json` by the
`generate-code-atlas` skill.

> **Stale since 2026-07-31.** The generated atlas, the tour, and the three module
> JSONs behind them (`annotation.json`, `engine.json`, `entry-workbench.json`)
> still describe `AnnotationType`, `RecognizerId`, `allowedPageIds`, and the
> `umbra-workbench` GUI — all removed by
> [the capability plan](../plans/2026-07-31-annotation-model-capabilities.md) and
> `b57b67b`. They were deliberately not hand-patched, because a half-patched
> generated file is worse than a wholly stale one. Re-run the
> `generate-code-atlas` skill to refresh them.
