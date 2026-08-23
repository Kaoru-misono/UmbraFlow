# Operator schema identity is the STORED DDL text, whitespace included

## Symptom

A migration lands, `verifyExactDatabaseSchema` refuses, and the two hashes in
the message differ by one. Every column, type, `CHECK` and index is right;
diffing the two schemas by column set finds nothing. The same DDL constant
produced the two databases.

The mirror of it: a migration test winds a fresh database back to a historical
generation, re-creates every table that generation had, and still computes a
source identity the registered pair does not name — again with no visible
difference in the tables.

## Root cause

`k_operatorDatabaseSchemaIdentity` is the sha256 of a canonicalization over
every `sqlite_schema` row, and one of the four columns it hashes is `sql` — the
**stored text of the statement**, byte for byte. SQLite stores what it was
handed, so two things nobody looks at are inside the identity:

1. **Where a statement was executed from.** A `CREATE TABLE` that sat inside a
   multi-statement block, terminated by `;` immediately after `) STRICT`, stores
   no trailing bytes. Move the same table into its own `constexpr` raw string
   whose closing `)sql"` sits on the next line, execute it on its own, and
   SQLite stores the newline and the indentation after `) STRICT` as part of
   `sql`. The schema is identical; the identity is not.
2. **Comment and indentation edits.** A `--` comment inside the DDL, or
   reindenting a column, moves the identity even though nothing about the
   schema changed. `rewriteSnapshotIdentityComment` existed only to repair one
   of those.

The two hashes in the refusal are the whole diagnosis and yet say nothing about
which of these it is, because both sides are digests.

## Fix

When a table moves between an inline block and a named constant — which is what
extracting it for a migration to rebuild from requires — make **both** paths
execute the identical text. A wind-back that re-creates a historical table must
reproduce that generation's stored bytes, which means stripping the trailing
whitespace a standalone `constexpr` block contributes:

```cpp
// The bytes that generation stored, not the bytes this constant holds.
database.execute(withoutTrailingSpace(k_projectObservationsDdl));
```

Then recompute the identity by creating a fresh database and reading the actual
value out of the refusal, rather than by hand.

## Regression check

1. Create a fresh database and verify it against the pinned identity. This is
   what `initialize()` already does, and it is why a forgotten recomputation
   cannot ship green.
2. For every registered migration pair, a fixture must wind a fresh database
   back to the pair's source identity, assert that identity **before** the
   upgrade, and then reopen. An assertion only on the target identity passes for
   a wind-back that reproduced the wrong generation.
3. When a table is extracted into a named constant, assert that a fresh database
   and a migrated one land on the same identity. They will not if only one path
   carries the constant's trailing bytes.
