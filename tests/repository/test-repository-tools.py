#!/usr/bin/env python3
"""Retained compatibility tests for repository initialization and normalization."""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def load_format_module():
    module_path = ROOT / "scripts" / "fix_format.py"
    specification = importlib.util.spec_from_file_location("fix_format", module_path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {module_path}")

    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class RepositoryToolTests(unittest.TestCase):
    def test_text_normalization_is_deterministic(self) -> None:
        module = load_format_module()

        self.assertEqual(
            module.normalize("alpha \r\n\tbeta\tgamma\t\r\n\r\n", replace_tabs=True),
            "alpha\n    beta    gamma\n",
        )
        self.assertEqual(
            module.normalize("alpha\r\n\tbeta\t\r\n", replace_tabs=False),
            "alpha\n\tbeta\n",
        )

    def test_initializer_converts_template_in_a_disposable_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            project_root = Path(temporary_directory) / "project"
            shutil.copytree(
                ROOT,
                project_root,
                ignore=shutil.ignore_patterns(
                    ".git",
                    "__pycache__",
                    "build",
                    "external",
                    "install",
                ),
            )

            command = [
                sys.executable,
                str(project_root / "scripts" / "initialize_project.py"),
                "SampleProject",
                "--description",
                "Sample project",
            ]
            subprocess.run(command, cwd=project_root, check=True, capture_output=True, text=True)

            readme = (project_root / "README.md").read_text(encoding="utf-8")
            instructions = (project_root / "CLAUDE.md").read_text(encoding="utf-8")
            coding_standard = (
                project_root
                / ".claude"
                / "skills"
                / "cpp-coding"
                / "references"
                / "coding-standard.md"
            ).read_text(encoding="utf-8")
            architecture = (project_root / "docs" / "ARCHITECTURE.md").read_text(
                encoding="utf-8"
            )
            capability_kernel = (
                project_root / "docs" / "rules" / "capability-kernel.md"
            ).read_text(encoding="utf-8")
            result_header = (
                project_root / "modules" / "core" / "source" / "core" / "error" / "result.hpp"
            ).read_text(encoding="utf-8")

            self.assertIn("# SampleProject\n", readme)
            self.assertIn("Sample project", readme)
            self.assertNotIn("## Create a project", readme)
            self.assertNotIn("UMBRA_FLOW_ONLY", readme)
            self.assertNotIn("initialize_project.py <ProjectName>", instructions)
            self.assertIn("The project root namespace is `sample_project`.", coding_standard)
            self.assertIn("The project uses April2's", architecture)
            self.assertNotIn("## Core additions", capability_kernel)
            self.assertIn("namespace sample_project", result_header)
            self.assertIn("SAMPLE_PROJECT_TRY", result_header)


if __name__ == "__main__":
    unittest.main()
