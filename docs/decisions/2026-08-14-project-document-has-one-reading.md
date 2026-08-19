# 2026-08-14 — `umbraflow-project.json` is a published document with one reading

## Decision

`umbraflow-project.json`'s shape is one of the published documents, at
`schema/umbraflow-project-v1.schema.json`, and both of its readers reach it
through the generated schema catalog.

## Context

The document has two readers in modules that cannot link one another:
Deployment's runtime loader and Project's offline kit. Project cannot link
Deployment — Deployment reaches Task, and `entry/project` links Project and Core
alone — so before this the offline kit carried a second, weaker reading of the
same document.

That is exactly the shape of drift the catalog
([2026-08-13](2026-08-13-one-generated-schema-catalog.md)) exists to prevent: two
readings of one document, one of them authoritative and one of them merely
approximately right, with nothing that reds when they disagree. Publishing the
shape and routing both readers through the catalog closes it for this document
too, without giving Project a link edge it must not have.

## Consequences

- Neither reader parses the document by hand; the catalog is how both reach the
  same bytes.
- The link restriction stands: Project still cannot link Deployment, and the
  catalog is what makes that restriction free of cost.
