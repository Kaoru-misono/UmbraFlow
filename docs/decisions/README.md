# Decisions

One frozen ruling per file, named `<YYYY-MM-DD>-<topic>.md` after the date the
ruling was made. Each states the decision, the context that made it a real
decision, and its consequences.

**These files are immutable.** A changed mind writes a new file that names the
one it supersedes; the superseded file keeps its bytes. Nothing here is edited
to stay current, because nothing here claims to be current: a ruling is a fact
about a date, and a fact about a date cannot drift.

This is not a return of `docs/adr/`, retired in `82f8027`. ADRs were retired in
favour of decisions living beside the plan that executed them. What this
directory separates is the *frozen ruling* from the *live plan*: a plan that
also holds rulings is rewritten whenever its status changes, and the rulings are
dragged along by every rewrite. Plans keep the work; decisions keep the
reasoning.

A decision file holds no status, no version belonging to another repository, and
no digest that something else generates.
