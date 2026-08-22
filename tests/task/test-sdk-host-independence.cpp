// The negative half of E6: "locale and host Unicode differences must not move
// results" (docs/plans/2026-08-21-project-plugin-cycle-spi.md 10).
//
// The positive half -- one golden answer for one host -- is
// tests/task/test-framework-bundle.cpp's cross-VM parity fixture. A golden
// answer proves the SDK computes what it should on the machine that ran it. It
// cannot say whether that answer came from the release's own pinned tables or
// from the operating system underneath, because on one host the two agree.
// These cases separate them, in the only two directions a test on this platform
// can actually push:
//
//   1. Move the HOST and hold the release still. std::setlocale is the whole of
//      what a test may perturb here, and it is not a token gesture: Luau's
//      string.format delegates to the C runtime's snprintf, tonumber and the
//      Luau parser itself delegate to strtod, and all three read LC_NUMERIC's
//      decimal separator. Under de-DE or tr-TR that separator is a comma, so
//      this sweep exercises a real fault line rather than a hypothetical one.
//      LC_CTYPE moves the CRT's codepage with it.
//
//   2. Move the RELEASE and hold the host still. The second case edits one row
//      out of the pinned Unicode 15.0 composition table and requires the answer
//      to move. Without it the first case would be green for a bundle that
//      consulted no table at all.
//
// What is NOT reachable, stated plainly rather than faked: the host's own
// Unicode tables cannot be replaced from a test on Windows. Windows' NLS and
// ICU data are the operating system's, there is no supported way to make
// GetStringTypeW, LCMapStringW or NormalizeString answer for a different
// Unicode version, and nothing here pretends otherwise. What case 2 establishes
// instead is the property that would make such a swap irrelevant: these answers
// are a function of the release's own table, and they move when it moves.
//
// One further host dependence was found while writing this and is deliberately
// NOT asserted here, because it belongs to the vendored compiler rather than to
// the SDK: Luau's own lexer parses a decimal number literal with strtod
// (Ast/src/Parser.cpp, parseDouble), so `1.5` in ANY Luau source is a malformed
// number under a comma locale. It fails loudly at compile time rather than
// evaluating to 1, and no embedded framework module spells a decimal literal --
// which is why the whole pure closure still compiles in every locale below.
// That compile is itself the detector: a framework module that gained a decimal
// literal would stop compiling here under de-DE.

#include <task/framework-bundle.hpp>

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        // Every answer is rendered as ASCII hex of its UTF-8 bytes, so two hosts
        // that disagree report their disagreement as bytes instead of as a
        // string a console might re-encode on the way out.
        //
        // Numbers are deliberately built by arithmetic rather than written as
        // decimal literals: a literal would be the vendored lexer's answer, and
        // what is under test here is the SDK's. `1e21` and friends carry no
        // decimal point and so are lexed identically everywhere.
        constexpr auto k_probe = std::string_view{R"LUAU(
local jcs = require("@umbraflow/jcs")
local json = require("@umbraflow/json")
local text = require("@umbraflow/text")
local unicode = require("@umbraflow/utf8")

return {
    plugin_id = "fixture.host-independence",
    probe = function(_)
        local parts = {}
        local function record(label: string, value: any)
            local rendered
            if type(value) == "string" then
                local bytes = {}
                for index = 1, #value do
                    bytes[index] = string.format("%02X", string.byte(value, index))
                end
                rendered = table.concat(bytes)
            elseif type(value) == "boolean" then
                rendered = if value then "true" else "false"
            elseif type(value) == "number" then
                rendered = jcs.encode(value)
            else
                rendered = "kind:" .. type(value)
            end
            table.insert(parts, label .. "=" .. rendered)
        end

        -- Normalization, all four forms.
        record("nfc_combining", text.normalize("e\u{0301}", "NFC"))
        record("nfd_precomposed", text.normalize("\u{01FA}", "NFD"))
        record("nfc_hangul", text.normalize("\u{1100}\u{1161}", "NFC"))
        record("nfkc_ligature", text.normalize("\u{FB03}", "NFKC"))
        record("nfkd_circled", text.normalize("\u{2460}", "NFKD"))
        record("nfd_reorder", text.normalize("a\u{0315}\u{0300}", "NFD"))

        -- Case folding, including every trap a locale-driven implementation
        -- falls into: Turkish dotted and dotless i, final sigma, sharp s.
        record("fold_latin_i", text.case_fold("I"))
        record("fold_latin_i_small", text.case_fold("i"))
        record("fold_dotted_i", text.case_fold("\u{0130}"))
        record("fold_dotless_i", text.case_fold("\u{0131}"))
        record("fold_sharp_s", text.case_fold("Stra\u{00DF}e"))
        record("fold_sigma", text.case_fold("\u{03A3}\u{03C2}"))
        record("fold_capital_sharp_s", text.case_fold("\u{1E9E}"))

        -- Deterministic matching over folded text.
        record("equals_ascii_i", text.equals("I", "i", { case_fold = true }))
        record("equals_dotted_i", text.equals("\u{0130}", "I", { case_fold = true }))
        record("equals_sharp_s", text.equals("Stra\u{00DF}e", "STRASSE", {
            case_fold = true,
        }))

        -- Classification and the White_Space property.
        record("class_letter", unicode.classify("A"))
        record("class_mark", unicode.classify(0x0301))
        record("class_sharp_s", unicode.classify("\u{00DF}"))
        record("class_unassigned", unicode.classify(0x0378))
        record("class_ideographic_space", unicode.classify("\u{3000}"))
        record("space_nbsp", unicode.is_whitespace(0x00A0))
        record("space_file_separator", unicode.is_whitespace(0x001C))
        record("trim", text.trim("\u{00A0} Menu \u{3000}"))
        record("collapse", text.collapse_whitespace("\u{00A0}  Menu\t Start\u{3000}"))
        record("tokens", table.concat(text.tokens("Go, \u{4E2D}\u{1F642} 42"), "/"))
        record("text_version", text.unicode_version)
        record("utf8_version", unicode.unicode_version)

        -- Canonical number text. `string.format` and `tonumber` both read
        -- LC_NUMERIC underneath, so this is the sharpest locale probe here.
        record("number_three_halves", 3 / 2)
        record("number_tenth", 1 / 10)
        record("number_third", 1 / 3)
        record("number_negative_zero", 0 * -1)
        record("number_large_exponent", 1e21)
        record("number_small_exponent", 1e-7)
        record("number_max_double", (2 - 2 ^ -52) * 2 ^ 1023)
        record("number_min_subnormal", 2 ^ -1074)

        -- The decimal text side. Luau's tonumber requires strtod to consume
        -- the whole string, so under a comma locale `tonumber("1.5")` is nil
        -- and a well-formed JSON document stops parsing on that host -- unless
        -- the parser refuses to depend on the separator at all.
        record("parse_half", json.encode(json.parse("1.5")))
        record("parse_pi", json.encode(json.parse("3.141592653589793")))
        record("parse_negative", json.encode(json.parse("-0.25")))
        record("parse_exponent", json.encode(json.parse("1e-3")))
        record("parse_max_double", json.encode(json.parse("1.7976931348623157e308")))
        record("parse_min_subnormal", json.encode(json.parse("5e-324")))
        record("parse_object", json.encode(json.parse([[{"b":[1,2],"a":"é"}]])))
        record("canonical_object", jcs.encode({ b = 3 / 2, a = "\u{00E9}" }))

        return table.concat(parts, " ")
    end,
}
)LUAU"};

        // The one row of the pinned Unicode 15.0 composition table that turns
        // `e` plus U+0301 into U+00E9. Removing it is a change to the release's
        // own data with no effect on any host table, which is exactly the
        // perturbation the host does not allow.
        constexpr auto k_compositionRow = std::string_view{"\n101,769:233\n"};

        // Every label the probe records, split back out of its answer. Splitting
        // rather than comparing one long string is what lets a failure name the
        // field that moved.
        [[nodiscard]] auto fields(std::string_view answer)
            -> std::vector<std::string>
        {
            auto parts  = std::vector<std::string>{};
            auto offset = std::size_t{0U};
            while (offset <= answer.size())
            {
                auto const next = answer.find(' ', offset);
                auto const end  = next == std::string_view::npos
                    ? answer.size()
                    : next;
                parts.emplace_back(answer.substr(offset, end - offset));
                if (next == std::string_view::npos)
                {
                    break;
                }
                offset = next + 1U;
            }
            return parts;
        }

        [[nodiscard]] auto probeAnswer(
            std::span<script::FrameworkModule const> closure
        ) -> std::string
        {
            constexpr auto entryPoints = std::array{std::string_view{"probe"}};
            auto program = script::PureDataProgram::compile(
                "fixture.host-independence",
                "main",
                {
                    script::PureDataProgram::Module{
                        .name   = "main",
                        .source = std::string{k_probe},
                    },
                },
                entryPoints,
                {},
                closure
            );
            REQUIRE(program.has_value());

            auto input = json::parse("{}");
            REQUIRE(input.has_value());
            auto answer = program->invoke("probe", *input);
            INFO(
                "probe refusal: ",
                answer.has_value() ? std::string{} : answer.error().message()
            );
            REQUIRE(answer.has_value());
            REQUIRE(answer->kind() == json::ValueKind::String);
            return std::string{answer->string()};
        }

        // Saves the whole C locale on construction and puts it back on
        // destruction. The locale is process-global state, so a case that left
        // it moved would silently retune every later case in this binary.
        class ScopedHostLocale final
        {
            std::string m_previous{};

        public:
            ScopedHostLocale()
            {
                auto const* const current = std::setlocale(LC_ALL, nullptr);
                m_previous = current == nullptr ? std::string{"C"} : current;
            }

            ScopedHostLocale(ScopedHostLocale const&)                    = delete;
            ScopedHostLocale(ScopedHostLocale&&)                         = delete;
            auto operator=(ScopedHostLocale const&) -> ScopedHostLocale& = delete;
            auto operator=(ScopedHostLocale&&) -> ScopedHostLocale&      = delete;

            ~ScopedHostLocale() { std::setlocale(LC_ALL, m_previous.c_str()); }

            // Whether the host actually has this locale. A name it refuses
            // leaves the previous one in place, so a skip is a skip and never a
            // silent second run of the same configuration.
            [[nodiscard]] static auto apply(char const* name) -> bool
            {
                return std::setlocale(LC_ALL, name) != nullptr;
            }

            // The witness that applying a locale changed something the SDK's
            // dependencies can see. Without it a sweep over names the host
            // silently normalizes to "C" would be a sweep over one
            // configuration.
            [[nodiscard]] static auto decimalPoint() -> std::string
            {
                auto const* const conventions = std::localeconv();
                if (conventions == nullptr || conventions->decimal_point == nullptr)
                {
                    return {};
                }
                return std::string{conventions->decimal_point};
            }
        };
    }

    TEST_CASE("SDK text and number answers do not move with the host locale")
    {
        auto const restore = ScopedHostLocale{};

        auto closure = pureFrameworkScriptModules();
        REQUIRE(closure.has_value());

        REQUIRE(ScopedHostLocale::apply("C"));
        auto const baseline = fields(probeAnswer(*closure));

        // The control on the fixture itself: an answer that came back empty, or
        // one field short, would make every comparison below hold for the wrong
        // reason.
        REQUIRE(baseline.size() == 44U);
        for (auto const& field : baseline)
        {
            INFO("baseline field: ", field);
            CHECK(field.find('=') != std::string::npos);
            CHECK(field.back() != '=');
        }

        // Names rather than a portable enumeration, because there is no
        // portable enumeration: the host offers what it offers. Both the BCP-47
        // and the legacy language_country.codepage spellings are tried, and a
        // name the host refuses is skipped rather than failed.
        constexpr auto candidates = std::array{
            std::string_view{"de-DE"},
            std::string_view{"German_Germany.1252"},
            std::string_view{"tr-TR"},
            std::string_view{"Turkish_Turkey.1254"},
            std::string_view{"fr-FR.UTF-8"},
            std::string_view{"ja-JP"},
            std::string_view{".UTF-8"},
        };

        auto applied       = std::vector<std::string>{};
        auto decimalPoints = std::vector<std::string>{};
        for (auto const& candidate : candidates)
        {
            if (!ScopedHostLocale::apply(std::string{candidate}.c_str()))
            {
                continue;
            }
            applied.emplace_back(candidate);
            auto point = ScopedHostLocale::decimalPoint();
            if (!std::ranges::contains(decimalPoints, point))
            {
                decimalPoints.emplace_back(std::move(point));
            }

            auto const answer = fields(probeAnswer(*closure));
            INFO("locale: ", candidate);
            REQUIRE(answer.size() == baseline.size());
            for (auto index = std::size_t{0U}; index < answer.size(); ++index)
            {
                INFO("field: ", baseline.at(index));
                CHECK(answer.at(index) == baseline.at(index));
            }
        }

        // Two witnesses that the sweep was not vacuous. A host with no extra
        // locale installed would otherwise run the "C" configuration once and
        // report success; and a host whose extra locales all share "C"'s
        // decimal separator would never touch the LC_NUMERIC path that Luau's
        // snprintf, strtod and lexer all sit on.
        INFO("locales applied: ", applied.size());
        REQUIRE(applied.size() >= 2U);
        INFO("distinct decimal separators: ", decimalPoints.size());
        REQUIRE(decimalPoints.size() >= 2U);
    }

    TEST_CASE("SDK text answers move when the pinned Unicode table moves")
    {
        auto const restore = ScopedHostLocale{};
        REQUIRE(ScopedHostLocale::apply("C"));

        auto closure = pureFrameworkScriptModules();
        REQUIRE(closure.has_value());
        auto const baseline = fields(probeAnswer(*closure));

        auto const target = std::ranges::find(
            *closure,
            std::string_view{"@umbraflow/internal/unicode-text-data"},
            &script::FrameworkModule::name
        );
        REQUIRE(target != closure->end());

        auto const original = std::string{target->source};
        REQUIRE(original.find(k_compositionRow) != std::string::npos);
        auto const edited = std::string{original}.replace(
            original.find(k_compositionRow),
            k_compositionRow.size(),
            "\n"
        );
        REQUIRE(edited.size() < original.size());
        target->source = edited;

        auto const perturbed = fields(probeAnswer(*closure));
        REQUIRE(perturbed.size() == baseline.size());

        // The composition row that was removed is the one that makes NFC of
        // `e` plus U+0301 a single U+00E9, so exactly the fields that compose
        // that pair must move and the rest must not. A perturbation that moved
        // everything, or nothing, would say the answers are not really a
        // function of this table.
        auto moved = std::string{};
        for (auto index = std::size_t{0U}; index < perturbed.size(); ++index)
        {
            if (perturbed.at(index) != baseline.at(index))
            {
                if (!moved.empty())
                {
                    moved += ',';
                }
                moved += baseline.at(index).substr(
                    0U,
                    baseline.at(index).find('=')
                );
            }
        }
        CHECK(moved == "nfc_combining");
    }
}
