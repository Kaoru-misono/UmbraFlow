#!/usr/bin/env python3
"""Tests for repository safety-gate configuration."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

import check_safety


class SafetyRuleTests(unittest.TestCase):
    def test_adr_011_forbidden_identifier_list(self) -> None:
        actual = tuple(
            rule.pattern.pattern
            for rule in check_safety.RULES
            if rule.name.startswith("ADR-011 forbidden ")
        )
        expected = (
            r"\bSetForegroundWindow\b",
            r"\bSetFocus\b",
            r"\bSendInput\b",
            r"\bmouse_event\b",
            r"\bkeybd_event\b",
            r"\bSetCursorPos\b",
            r"\bBringWindowToTop\b",
            r"\bSwitchToThisWindow\b",
            r"\bAttachThreadInput\b",
            r"\bSetActiveWindow\b",
        )

        self.assertEqual(actual, expected)

    def test_source_files_include_first_party_h_and_exclude_vendored_headers(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            included = root / "entry" / "ffi" / "wrapper.h"
            external = root / "entry" / "ffi" / "external" / "vendor.h"
            third_party = root / "modules" / "third_party" / "vendor.hpp"
            for path in (included, external, third_party):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// test\n", encoding="utf-8")

            actual = {
                path.relative_to(root).as_posix()
                for path in check_safety.source_files(root)
            }

        self.assertEqual(actual, {"entry/ffi/wrapper.h"})


if __name__ == "__main__":
    unittest.main()
