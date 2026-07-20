#!/usr/bin/env python3
"""Tests for repository safety-gate configuration."""

from __future__ import annotations

import sys
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


if __name__ == "__main__":
    unittest.main()
