# Offline annotation backend

This is the P5 stdlib backend for the frozen v1 contracts. The evidence store
is a directory of canonical JSONL collections:

```text
frames.jsonl
observations.jsonl
assertions.jsonl
candidates.jsonl
pipeline.jsonl       # traceable Agent stages
decisions.jsonl      # review audit records
```

The backend loads the repository's offline and runtime schemas, rejects unknown
fields, then applies cross-object checks for ownership, references, surface
identity, actions, and transitions. It does not translate non-canonical data.

Run the HTTP server:

```powershell
python -m tools.annotate.serve --store annotation --project-id demo --port 8765
```

The API exposes `GET /api/schema`, candidate list/get, patch accept/reject,
compile, validate, conflicts, and provenance. Mutations require the reviewed
candidate revision. Compile defaults to `write=false`; `write=true` writes
only runtime collections to `page-model.toml`. Offline frames, OCR,
assertions, review records, and Agent traces never enter that file.

The pipeline can be run from Python:

```python
from tools.annotate.agent_pipeline import AgentPipeline
from tools.annotate.evidence_store import EvidenceStore

result = AgentPipeline(EvidenceStore("annotation", "demo")).run()
```

It records `capture`, `perception`, `structure`, `contract`, `verification`,
and `repair`. It only proposes patches; unknown evidence and open conflicts
block compilation until a human review decision resolves them.
