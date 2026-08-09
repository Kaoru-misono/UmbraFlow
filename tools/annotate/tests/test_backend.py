from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.annotate.agent_pipeline import AgentPipeline, STAGES
from tools.annotate.evidence_store import EvidenceStore
from tools.annotate.model_file import build_runtime_model, load_runtime_toml, validate_runtime_model
from tools.annotate.serve import AnnotationBackend, BackendError


PROVENANCE = {
    "run_id": "run-test",
    "agent": "test",
    "agent_version": "test-v1",
    "created_at": "2026-08-09T00:00:00Z",
}


def frame() -> dict:
    return {
        "id": "frame-1",
        "asset": {"path": "frames/frame-1.png", "sha256": "0" * 64, "width": 1920, "height": 1080},
        "captured_at": "2026-08-09T00:00:00Z",
        "source": {"kind": "manual_capture", "source": "test"},
    }


def observation(classification: str = "present") -> dict:
    return {
        "id": "observation-1",
        "frame_id": "frame-1",
        "subject": {"kind": "target", "target_id": "confirm_button", "rect": [10, 20, 100, 40]},
        "measurement": {"kind": "geometry", "rect": [10, 20, 100, 40]},
        "classification": classification,
        "confidence": 0.99,
        "provenance": PROVENANCE,
    }


def candidate() -> dict:
    target = {
        "id": "confirm_button",
        "kind": "control",
        "geometry": {"kind": "fixed", "rect": [10, 20, 100, 40]},
        "locators": ["confirm_button-geometry"],
        "readers": [],
    }
    return {
        "id": "candidate-1",
        "project_id": "demo",
        "revision": 1,
        "status": "accepted",
        "source_frame_ids": ["frame-1"],
        "targets": [{"candidate_id": "confirm-button-candidate", "state": "accepted", "value": target, "confidence": 0.99, "evidence_ids": ["observation-1"], "provenance": PROVENANCE}],
        "entities": [
            {"candidate_id": "context-camp-candidate", "entity_kind": "context", "state": "accepted", "value": {"id": "camp"}, "confidence": 1, "evidence_ids": ["observation-1"], "provenance": PROVENANCE},
            {"candidate_id": "locator-candidate", "entity_kind": "locator", "state": "accepted", "value": {"id": "confirm_button-geometry", "target": "confirm_button", "kind": "geometry", "rect": [10, 20, 100, 40]}, "confidence": 0.99, "evidence_ids": ["observation-1"], "provenance": PROVENANCE},
            {"candidate_id": "surface-candidate", "entity_kind": "surface", "state": "accepted", "value": {"id": "camp-scene", "kind": "scene", "contexts": ["camp"]}, "confidence": 0.99, "evidence_ids": ["observation-1"], "provenance": PROVENANCE},
            {"candidate_id": "binding-candidate", "entity_kind": "binding", "state": "accepted", "value": {"id": "camp-confirm", "surface": "camp-scene", "target": "confirm_button", "placement": {"kind": "target_geometry"}, "identity": {"all": [{"kind": "locator_present", "locator": {"locator": "confirm_button-geometry"}}], "any": [], "none": []}, "readers": [], "actions": [{"id": "click", "kind": "click", "locator": {"locator": "confirm_button-geometry"}, "preconditions": []}]}, "confidence": 0.99, "evidence_ids": ["observation-1"], "provenance": PROVENANCE},
        ],
        "patches": [],
        "conflicts": [],
    }


class BackendTests(unittest.TestCase):
    def make_store(self, root: Path) -> EvidenceStore:
        store = EvidenceStore(root, "demo")
        store.save_collection("frames", [frame()])
        store.save_collection("observations", [observation()])
        return store

    def test_runtime_model_is_canonical_and_compiles_without_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = self.make_store(Path(directory))
            store.save_candidate(candidate())
            backend = AnnotationBackend(store, Path(directory) / "page-model.toml")
            validation = backend.run_validation("candidate-1", 1)
            self.assertTrue(validation["valid"], validation)
            compiled = backend.compile_candidate("candidate-1", {"candidate_revision": 1})
            self.assertTrue(compiled["valid"], compiled)
            runtime_path = Path(directory) / "page-model.toml"
            self.assertFalse(runtime_path.exists())
            compiled = backend.compile_candidate("candidate-1", {"candidate_revision": compiled["revision"], "write": True})
            self.assertTrue(compiled["valid"], compiled)
            model = load_runtime_toml(runtime_path)
            self.assertEqual(validate_runtime_model(model), [])
            text = runtime_path.read_text(encoding="utf-8")
            self.assertNotIn("frames/frame-1.png", text)
            self.assertNotIn("observation-1", text)

    def test_unknown_evidence_blocks_compile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = self.make_store(Path(directory))
            store.save_collection("observations", [observation("unknown")])
            item = candidate()
            item["conflicts"] = [{"id": "unknown-observation-1", "kind": "unknown_evidence", "severity": "error", "subject_ids": ["observation-1"], "message": "unknown", "evidence_ids": ["observation-1"], "status": "open"}]
            store.save_candidate(item)
            result = AnnotationBackend(store).run_validation("candidate-1", 1)
            self.assertFalse(result["valid"])
            self.assertEqual(result["conflict_ids"], ["unknown-observation-1"])

    def test_pipeline_has_all_stages_and_keeps_proposals_open(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = self.make_store(Path(directory))
            result = AgentPipeline(store).run(run_id="run-pipeline")
            self.assertEqual([stage["stage"] for stage in result["stages"]], list(STAGES))
            self.assertTrue(result["candidate"]["patches"])
            self.assertTrue(all(patch["status"] == "proposed" for patch in result["candidate"]["patches"]))

    def test_revision_conflict_is_optimistic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = self.make_store(Path(directory))
            item = candidate()
            item["patches"] = [{"id": "noop-patch", "change": {"kind": "set_field", "entity_kind": "target", "entity_id": "confirm_button", "field": "kind", "value": "control"}, "summary": "Keep control", "confidence": 1, "risk": "low", "evidence_ids": ["observation-1"], "provenance": PROVENANCE, "status": "proposed"}]
            store.save_candidate(item)
            backend = AnnotationBackend(store)
            accepted = backend.accept_patch("candidate-1", "noop-patch", {"candidate_revision": 1, "actor": "reviewer"})
            self.assertEqual(accepted["revision"], 2)
            with self.assertRaises(BackendError) as error:
                backend.reject_patch("candidate-1", "noop-patch", {"candidate_revision": 1, "actor": "reviewer"})
            self.assertEqual(error.exception.code, "revision_conflict")


if __name__ == "__main__":
    unittest.main()
