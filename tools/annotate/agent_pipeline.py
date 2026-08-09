"""Traceable offline Agent stages that produce reviewable semantic patches."""

from __future__ import annotations

import datetime as _datetime
import uuid
from collections import defaultdict
from typing import Any

from .evidence_store import EvidenceStore


STAGES = ("capture", "perception", "structure", "contract", "verification", "repair")


def _now() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def _provenance(run_id: str, agent: str) -> dict[str, str]:
    return {"run_id": run_id, "agent": agent, "agent_version": "p5-stdlib-v1", "created_at": _now()}


class AgentPipeline:
    def __init__(self, store: EvidenceStore, agent: str = "annotation-agent") -> None:
        self.store = store
        self.agent = agent

    def run(self, candidate_id: str | None = None, run_id: str | None = None) -> dict[str, Any]:
        run_id = run_id or f"run-{uuid.uuid4().hex[:20]}"
        workspace = self.store.workspace()
        frame_ids = [frame["id"] for frame in workspace["frames"]]
        observation_ids = [observation["id"] for observation in workspace["observations"]]
        stages: list[dict[str, Any]] = []

        def stage(name: str, inputs: list[str], outputs: list[str], findings: list[str] | None = None) -> None:
            stages.append(
                {
                    "run_id": run_id,
                    "agent": self.agent,
                    "stage": name,
                    "status": "completed",
                    "started_at": _now(),
                    "completed_at": _now(),
                    "input_ids": inputs,
                    "output_ids": outputs,
                    "findings": findings or [],
                    "provenance": _provenance(run_id, self.agent),
                }
            )

        stage("capture", frame_ids, frame_ids, [f"read {len(frame_ids)} offline frame(s)"])
        stage("perception", frame_ids, observation_ids, [f"read {len(observation_ids)} offline observation(s)"])

        candidate = self.store.get_candidate(candidate_id) if candidate_id else None
        if candidate is None:
            candidate = self._new_candidate(workspace, run_id)
        surface_claim_ids = [row["id"] for row in workspace["assertions"] if row["claim"]["kind"] in {"surface_identity", "surface_stack"}]
        stage("structure", observation_ids, surface_claim_ids, ["kept surface claims as review evidence"])

        if not candidate.get("patches") and not candidate.get("targets") and not candidate.get("entities"):
            self._propose_from_observations(candidate, workspace, run_id)
        patch_ids = [patch["id"] for patch in candidate["patches"]]
        stage("contract", observation_ids, patch_ids, ["emitted semantic patches; no patch was auto-accepted"])

        unknown_ids = [observation["id"] for observation in workspace["observations"] if observation["classification"] == "unknown"]
        conflict_ids = self._add_unknown_conflicts(candidate, unknown_ids, run_id)
        stage("verification", frame_ids + observation_ids, conflict_ids, ["unknown evidence blocks automatic acceptance"] if unknown_ids else ["no unknown observations found"])
        repair_ids = [patch["id"] for patch in candidate["patches"] if patch["status"] == "proposed" and patch.get("conflict_ids")]
        stage("repair", conflict_ids, repair_ids, ["repair remains a proposal until a human reviews it"] if conflict_ids else [])

        candidate["revision"] += 1
        self.store.save_candidate(candidate)
        self.store.append_pipeline_records(stages)
        return {"run_id": run_id, "candidate": candidate, "stages": stages}

    def _new_candidate(self, workspace: dict[str, Any], run_id: str) -> dict[str, Any]:
        if not workspace["frames"]:
            raise ValueError("capture stage requires at least one offline frame")
        return {
            "id": f"candidate-{run_id.removeprefix('run-')}",
            "project_id": workspace["project_id"],
            "revision": 1,
            "status": "candidate",
            "source_frame_ids": [frame["id"] for frame in workspace["frames"]],
            "targets": [],
            "entities": [],
            "patches": [],
            "conflicts": [],
        }

    def _propose_from_observations(self, candidate: dict[str, Any], workspace: dict[str, Any], run_id: str) -> None:
        by_target: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for observation in workspace["observations"]:
            if observation["subject"]["kind"] == "target" and observation["classification"] == "present":
                by_target[observation["subject"]["target_id"]].append(observation)
        for target_id, observations in sorted(by_target.items()):
            observation = max(observations, key=lambda item: item["confidence"])
            measurement = observation["measurement"]
            if measurement["kind"] == "geometry":
                rect = measurement["rect"]
            elif measurement["kind"] == "template_score":
                rect = measurement["rect"]
            elif measurement["kind"] == "ocr":
                rect = measurement["rect"]
            else:
                continue
            locator_id = f"{target_id}-geometry"
            provenance = _provenance(run_id, self.agent)
            target_value = {
                "id": target_id,
                "kind": "control",
                "geometry": {"kind": "fixed", "rect": rect},
                "locators": [locator_id],
                "readers": [],
            }
            locator_value = {"id": locator_id, "target": target_id, "kind": "geometry", "rect": rect}
            evidence_ids = [item["id"] for item in observations]
            candidate["targets"].append(
                {
                    "candidate_id": f"{target_id}-candidate",
                    "state": "proposed",
                    "value": target_value,
                    "confidence": observation["confidence"],
                    "evidence_ids": evidence_ids,
                    "provenance": provenance,
                }
            )
            candidate["entities"].append(
                {
                    "candidate_id": f"{locator_id}-candidate",
                    "entity_kind": "locator",
                    "state": "proposed",
                    "value": locator_value,
                    "confidence": observation["confidence"],
                    "evidence_ids": evidence_ids,
                    "provenance": provenance,
                }
            )
            candidate["patches"].extend(
                [
                    {
                        "id": f"create-{target_id}",
                        "change": {"kind": "create_entity", "entity_kind": "target", "value": target_value},
                        "summary": f"Create target {target_id}",
                        "confidence": observation["confidence"],
                        "risk": "low",
                        "evidence_ids": evidence_ids,
                        "provenance": provenance,
                        "status": "proposed",
                    },
                    {
                        "id": f"create-{locator_id}",
                        "change": {"kind": "create_entity", "entity_kind": "locator", "value": locator_value},
                        "summary": f"Create geometry locator for {target_id}",
                        "confidence": observation["confidence"],
                        "risk": "low",
                        "evidence_ids": evidence_ids,
                        "provenance": provenance,
                        "status": "proposed",
                    },
                ]
            )

    def _add_unknown_conflicts(self, candidate: dict[str, Any], observation_ids: list[str], run_id: str) -> list[str]:
        existing = {conflict["id"] for conflict in candidate["conflicts"]}
        created: list[str] = []
        for observation_id in observation_ids:
            conflict_id = f"unknown-{observation_id}"
            if conflict_id in existing:
                continue
            candidate["conflicts"].append(
                {
                    "id": conflict_id,
                    "kind": "unknown_evidence",
                    "severity": "error",
                    "subject_ids": [observation_id],
                    "message": "Unknown evidence is not negative evidence and cannot be auto-accepted.",
                    "evidence_ids": [observation_id],
                    "status": "open",
                }
            )
            created.append(conflict_id)
        return created
