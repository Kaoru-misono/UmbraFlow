#!/usr/bin/env python3
"""Tests for repository safety-gate configuration."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

import check_cpp_format
import check_safety


class SafetyRuleTests(unittest.TestCase):
    def test_repository_gates_share_the_first_party_cpp_scope(self) -> None:
        self.assertEqual(check_safety.SOURCE_ROOTS, check_cpp_format.SOURCE_ROOTS)
        self.assertEqual(
            check_safety.SOURCE_EXTENSIONS,
            check_cpp_format.SOURCE_EXTENSIONS,
        )
        self.assertEqual(
            check_safety.VENDORED_DIRECTORY_NAMES,
            check_cpp_format.VENDORED_DIRECTORY_NAMES,
        )

    def test_nodiscard_declaration_covers_friend_redeclaration(self) -> None:
        content = """class Store final
{
    friend auto loadValue(int key) -> Result<std::optional<int>>;
};

[[nodiscard]]
auto loadValue(int key) -> Result<std::optional<int>>;
"""

        actual = check_safety.missing_must_use_nodiscard_lines(content)

        self.assertEqual(actual, [])

    def test_unannotated_must_use_function_is_reported(self) -> None:
        content = (
            "auto loadValue() const noexcept UF_LIFETIME_BOUND "
            "-> std::optional<std::span<int>>;\n"
        )

        actual = check_safety.missing_must_use_nodiscard_lines(content)

        self.assertEqual(actual, [1])

    def test_nodiscard_overload_does_not_cover_friend_redeclaration(self) -> None:
        content = """[[nodiscard]]
auto loadValue(int key) -> Result<int>;

class Store final
{
    friend auto loadValue(std::string key) -> Result<int>;
};
"""

        actual = check_safety.missing_must_use_nodiscard_lines(content)

        self.assertEqual(actual, [6])

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
            test_source = root / "tests" / "unit" / "test-widget.cpp"
            uppercase_source = root / "tests" / "unit" / "test-legacy.CPP"
            c_source = root / "modules" / "ffi" / "wrapper.c"
            external = root / "entry" / "ffi" / "external" / "vendor.h"
            test_external = root / "tests" / "external" / "vendor.cpp"
            third_party = root / "modules" / "third_party" / "vendor.hpp"
            for path in (
                included,
                test_source,
                uppercase_source,
                c_source,
                external,
                test_external,
                third_party,
            ):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("// test\n", encoding="utf-8")

            actual = {
                path.relative_to(root).as_posix()
                for path in check_safety.source_files(root)
            }

        self.assertEqual(
            actual,
            {
                "entry/ffi/wrapper.h",
                "modules/ffi/wrapper.c",
                "tests/unit/test-legacy.CPP",
                "tests/unit/test-widget.cpp",
            },
        )

    def test_uppercase_hpp_receives_nodiscard_header_checks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            header = root / "modules" / "sample" / "result.HPP"
            header.parent.mkdir(parents=True)
            header.write_text(
                "auto loadValue() -> Result<int>;\n",
                encoding="utf-8",
            )
            errors = io.StringIO()
            with (
                mock.patch.object(
                    sys,
                    "argv",
                    ["check_safety.py", "--root", str(root)],
                ),
                redirect_stderr(errors),
            ):
                actual = check_safety.main()

        self.assertEqual(actual, 1)
        self.assertIn(
            "modules/sample/result.HPP:1: Result, Status, and optional "
            "functions must be [[nodiscard]]",
            errors.getvalue(),
        )


class CppFormatGateTests(unittest.TestCase):
    def test_unpadded_member_block_reports_alignment(self) -> None:
        lines = """class Widget final
{
    int m_short;
    LongType m_long;
};
""".splitlines()

        _, violations = check_cpp_format.analyze_lines(lines)

        self.assertEqual(
            violations,
            [
                check_cpp_format.Violation(
                    line_index=2,
                    target_name="member identifier",
                    expected_column=13,
                )
            ],
        )

    def test_unpadded_assignment_block_reports_alignment(self) -> None:
        lines = """void update()
{
    value = 1;
    longer_value = 2;
}
""".splitlines()

        _, violations = check_cpp_format.analyze_lines(lines)

        self.assertEqual(
            violations,
            [
                check_cpp_format.Violation(
                    line_index=2,
                    target_name="assignment operator",
                    expected_column=17,
                )
            ],
        )

    def test_uniformly_overpadded_block_reports_exact_target_column(self) -> None:
        lines = """class Widget final
{
    int         m_short;
    LongType    m_long;
};
""".splitlines()

        _, violations = check_cpp_format.analyze_lines(lines)

        self.assertEqual(
            violations,
            [
                check_cpp_format.Violation(
                    line_index=2,
                    target_name="member identifier",
                    expected_column=13,
                ),
                check_cpp_format.Violation(
                    line_index=3,
                    target_name="member identifier",
                    expected_column=13,
                ),
            ],
        )

    def test_fix_changes_only_alignment_and_clears_violations(self) -> None:
        original = """class Widget final
{
    int m_short;
    LongType m_long;
};

void update()
{
    value = 1;
    longer_value = 2;
}
"""
        expected = """class Widget final
{
    int      m_short;
    LongType m_long;
};

void update()
{
    value        = 1;
    longer_value = 2;
}
"""
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "widget.cpp"
            path.write_text(original, encoding="utf-8")

            violations = check_cpp_format.check_file(path, fix=True)
            fixed = path.read_text(encoding="utf-8")
            remaining = check_cpp_format.check_file(path, fix=False)

        self.assertEqual(len(violations), 2)
        self.assertEqual(fixed, expected)
        self.assertEqual(remaining, [])

    def test_pointer_and_reference_members_are_skipped(self) -> None:
        lines = """class Widget final
{
    int* m_pointer;
    LongType& m_reference;
};
""".splitlines()

        candidates, violations = check_cpp_format.analyze_lines(lines)

        self.assertEqual(candidates, [])
        self.assertEqual(violations, [])

    def test_assignments_with_another_equals_are_skipped(self) -> None:
        lines = """void update()
{
    first = second = 1;
    longer_value = first == second;
}
""".splitlines()

        candidates, violations = check_cpp_format.analyze_lines(lines)

        self.assertEqual(candidates, [])
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
