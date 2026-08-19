# Plans

Plans hold live framework work and product direction. They do not hold frozen
rulings, consumer status or facts copied from code. Rulings live under
[`docs/decisions/`](../decisions/README.md); current system shape lives under
[`docs/design/`](../design/).

## Live plans

- [Product form and roadmap](2026-07-21-product-form-and-roadmap.md)
- [Runtime and game-operator breaking rewrite](2026-08-09-runtime-hardening-rewrite.md)
- [Runtime migration report](2026-08-09-runtime-migration-report.md)
- [Framework verification gaps](2026-08-19-framework-verification-gaps.md)
- [Project Kit release manifest](2026-08-19-project-kit-release-manifest.md)

The two JSON manifests beside the migration report are its machine-readable
baseline and disposition data.

Completed and superseded plans are frozen under [`docs/archive/plans/`](../archive/plans/);
closed reviews are frozen under [`docs/archive/reviews/`](../archive/reviews/).
Nothing in either archive is current or edited.

Before moving a live file into an archive, move every ruling it contains to
`docs/decisions/` and give every surviving obligation a live owner. Move the
file normally, never with `git mv`, and do not stage or commit as part of
archiving.
