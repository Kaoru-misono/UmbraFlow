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
import member_init


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

    def test_annotated_pure_virtual_must_use_function_is_not_reported(self) -> None:
        # A correctly attributed virtual method must not be flagged. Without
        # "virtual" in the specifiers alternation the [[nodiscard]] cannot attach
        # to the "auto" that follows "virtual", producing a false positive here.
        content = "[[nodiscard]] virtual auto f() -> Result<int> = 0;\n"

        actual = check_safety.missing_must_use_nodiscard_lines(content)

        self.assertEqual(actual, [])

    def test_unannotated_pure_virtual_must_use_function_is_reported(self) -> None:
        content = "virtual auto f() -> Status = 0;\n"

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


class MemberInitializationTests(unittest.TestCase):
    @staticmethod
    def check(**sources: str) -> list[str]:
        return [
            violation.split(": ", 1)[1]
            for violation in member_init.violations(
                [(f"{name}.hpp", text) for name, text in sources.items()]
            )
        ]

    def test_member_initialized_by_every_constructor_must_not_repeat_it(self) -> None:
        source = """class Source final
{
    std::string m_path{};

    explicit Source(std::string path) : m_path{std::move(path)}
    {
    }
};
"""

        actual = self.check(source=source)

        self.assertEqual(len(actual), 1)
        self.assertIn("Source::m_path", actual[0])
        self.assertIn("dead", actual[0])

    def test_constructor_leaving_a_member_alone_keeps_its_initializer_live(self) -> None:
        source = """class Reader final
{
    std::string m_text{};
    std::size_t m_line{1};

    explicit Reader(std::string text) : m_text{std::move(text)}
    {
    }
};
"""

        actual = self.check(source=source)

        self.assertEqual(len(actual), 1)
        self.assertIn("Reader::m_text", actual[0])
        self.assertNotIn("m_line", actual[0])

    def test_defaulted_default_constructor_keeps_initializers_live(self) -> None:
        source = """class Holder final
{
    Value m_value{};

    Holder() = default;

    explicit Holder(Value value) : m_value{std::move(value)}
    {
    }
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_copy_and_move_constructors_are_not_construction_paths(self) -> None:
        source = """class Buffer final
{
    std::vector<std::byte> m_data;

    explicit Buffer(std::vector<std::byte> data) : m_data{std::move(data)}
    {
    }

    Buffer(Buffer const&) = default;
    Buffer(Buffer&&) noexcept = default;
    ~Buffer() = default;
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_delegating_constructor_is_not_a_construction_path(self) -> None:
        source = """class Error final
{
    std::string m_message;

    Error(Code code, std::string message)
        : Error{code, std::error_code{}, std::move(message)}
    {
    }

    Error(Code code, std::error_code detail, std::string message)
        : m_detail{detail}
        , m_message{std::move(message)}
    {
    }
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_parenthesized_initializer_does_not_hide_the_constructor_body(self) -> None:
        source = """class Wrapper final
{
    Value m_value{};

    explicit Wrapper(Value value) : m_value(std::move(value))
    {
    }
};
"""

        actual = self.check(source=source)

        self.assertEqual(len(actual), 1)
        self.assertIn("Wrapper::m_value", actual[0])

    def test_out_of_line_constructor_is_matched_across_files(self) -> None:
        header = """class Frame final
{
    std::string m_name{};

    explicit Frame(std::string name) noexcept;
};
"""
        definition = """Frame::Frame(std::string name) noexcept
    : m_name{std::move(name)}
{
}
"""

        actual = member_init.violations(
            [("frame.hpp", header), ("frame.cpp", definition)]
        )

        self.assertEqual(len(actual), 1)
        self.assertIn("Frame::m_name", actual[0])
        self.assertTrue(actual[0].startswith("frame.hpp:3:"))

    def test_defaultable_member_without_an_initializer_is_reported(self) -> None:
        source = """class Config final
{
    std::string m_name;
    uint32      m_count;

    explicit Config(uint32 count) : m_count{count}
    {
    }
};
"""

        actual = self.check(source=source)

        self.assertEqual(len(actual), 1)
        self.assertIn("Config::m_name", actual[0])
        self.assertIn("no in-class initializer", actual[0])

    def test_member_of_an_unknown_type_is_not_reported(self) -> None:
        source = """class Holder final
{
    ContentHash m_hash;
    SourceId    m_id;
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_nested_class_members_belong_to_the_nested_class(self) -> None:
        source = """class Runtime final
{
    struct GrayTemplate final
    {
        uint32 m_width{};
    };

    std::vector<GrayTemplate> m_templates{};

    explicit Runtime(std::vector<GrayTemplate> templates)
        : m_templates{std::move(templates)}
    {
    }
};
"""

        actual = self.check(source=source)

        self.assertEqual(len(actual), 1)
        self.assertIn("Runtime::m_templates", actual[0])

    def test_out_of_line_constructor_spellings_are_not_mistaken_for_absence(
        self,
    ) -> None:
        """A nested, template, or qualified definition is not a missing one.

        Each of these spellings escapes a bare `Name::Name(` search. Treating
        the resulting empty constructor list as "nothing initializes this"
        reported every member of the class.
        """
        nested = (
            "class Outer final\n{\n    class Inner final\n    {\n"
            "        std::string m_name;\n"
            "        explicit Inner(std::string name);\n    };\n};\n",
            "Outer::Inner::Inner(std::string name) : m_name{std::move(name)}\n{\n}\n",
        )
        templated = (
            "template <typename T>\nclass Holder final\n{\n"
            "    std::string m_label;\n"
            "    explicit Holder(std::string label);\n};\n",
            "template <typename T>\n"
            "Holder<T>::Holder(std::string label) : m_label{std::move(label)} {}\n",
        )
        qualified = (
            "class Frame final\n{\n    std::string m_name;\n"
            "    explicit Frame(std::string name);\n};\n",
            "uf::vision::Frame::Frame(std::string name) : m_name{std::move(name)} {}\n",
        )

        for header, definition in (nested, templated, qualified):
            actual = member_init.violations(
                [("a.hpp", header), ("a.cpp", definition)]
            )

            self.assertEqual(actual, [])

    def test_template_parameter_is_not_read_as_a_class(self) -> None:
        source = """template <class Element>
class Pool final
{
    std::string m_name;
    explicit Pool(std::string name) : m_name{std::move(name)} {}
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_export_macro_does_not_become_the_class_name(self) -> None:
        source = """class UF_CORE_API Frame final
{
    std::string m_name;
    explicit Frame(std::string name) : m_name{std::move(name)} {}
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_same_name_in_one_file_is_not_merged(self) -> None:
        source = """namespace a { class Frame final {
    std::string m_name;
    explicit Frame(std::string name) : m_name{std::move(name)} {}
}; }
namespace b { class Frame final { Frame() = default; }; }
"""

        self.assertEqual(self.check(source=source), [])

    def test_directive_inside_an_initializer_list_silences_the_class(self) -> None:
        source = """class Tracer final
{
    explicit Tracer(std::size_t depth)
        : m_depth{depth}
#if defined(UF_TRACE)
        , m_label{"on"}
#endif
    {}
    std::size_t m_depth;
#if defined(UF_TRACE)
    std::string m_label;
#endif
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_braces_inside_literals_do_not_unbalance_the_body(self) -> None:
        plain = """class Router final
{
    std::string m_prefix;
    std::string m_pattern{"a)b"};
    explicit Router(std::string prefix) : m_prefix{std::move(prefix)} {}
};
"""
        raw = """class Writer final
{
    std::string m_name;
    std::string m_close{R"(})"};
    explicit Writer(std::string name) : m_name{std::move(name)} {}
};
"""

        self.assertEqual(self.check(source=plain), [])
        self.assertEqual(self.check(source=raw), [])

    def test_construction_inside_a_member_function_is_not_a_constructor(self) -> None:
        source = """class Frame final
{
    std::string m_name;
    explicit Frame(std::string name) : m_name{std::move(name)} {}

    auto check() const -> bool
    {
        if (Frame(std::string{"probe"}).valid())
        {
            return true;
        }
        return false;
    }
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_inherited_constructors_keep_initializers_live(self) -> None:
        source = """class Derived final : public Base
{
    using Base::Base;
    Derived(int value, std::string name) : Base{value}, m_name{std::move(name)} {}
    std::string m_name{};
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_west_const_copy_constructor_is_recognized(self) -> None:
        source = """class Config final
{
    std::string m_path;
    explicit Config(std::string path) : m_path{std::move(path)} {}
    Config(const Config&) = default;
};
"""

        self.assertEqual(self.check(source=source), [])

    def test_line_numbers_survive_a_block_comment(self) -> None:
        source = """/**
 * A documented class.
 * More prose.
 */
class Source final
{
    std::string m_path{};
    explicit Source(std::string path) : m_path{std::move(path)} {}
};
"""

        actual = member_init.violations([("doc.hpp", source)])

        self.assertEqual(len(actual), 1)
        self.assertTrue(actual[0].startswith("doc.hpp:7:"), actual[0])

    def test_repository_sources_satisfy_the_member_initialization_rule(self) -> None:
        paths = check_safety.source_files(REPOSITORY_ROOT)

        actual = member_init.violations_for_paths(paths, REPOSITORY_ROOT)

        self.assertEqual(actual, [])


if __name__ == "__main__":
    unittest.main()
