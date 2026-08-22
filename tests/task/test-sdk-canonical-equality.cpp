// `@umbraflow/jcs` canonical equality, the second half of what section 4.1 of
// docs/plans/2026-08-21-project-plugin-cycle-spi.md asks the module for.
//
// Canonical equality means one thing and it is not negotiable: two values are
// canonically equal when their RFC 8785 canonical forms are the same byte
// string. `jcs.equals` is therefore a comparison of the two encodings, and the
// heart of this file is the case that says so exhaustively -- over a corpus
// chosen so that shape equality, reference equality and Lua's own `==` each
// disagree with canonical equality somewhere in it, `jcs.equals(a, b)` is
// `jcs.encode(a) == jcs.encode(b)` for every ordered pair.
//
// That pairwise sweep is what a structural comparator would have to survive,
// and it is why one was not written. The corpus contains -0 against 0, 1e2
// against 100, two tables holding the same members inserted in opposite orders,
// a precomposed character against its decomposition, and the empty table
// against the empty string -- every place where "these look the same" and
// "these canonicalize the same" part company.
//
// The refusal cases are the other half. A value with no canonical form has no
// canonical equality either, so `jcs.equals` raises rather than answering
// `false`, and the message names which side could not be spoken. The cycle case
// is the sharpest: one table compared against itself is reference-equal, and a
// `rawequal` fast path would answer `true` for it without ever discovering that
// the value cannot be encoded at all.

#include <task/framework-bundle.hpp>

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>

namespace uf::task
{
    namespace
    {
        // Numbers are built by arithmetic rather than written as decimal
        // literals for the reason tests/task/test-sdk-host-independence.cpp
        // records: Luau's own lexer reads a decimal literal through strtod.
        constexpr auto k_probe = std::string_view{R"LUAU(
local jcs = require("@umbraflow/jcs")

return {
    plugin_id = "fixture.canonical-equality",
    equality = function(_)
        -- Deliberately heterogeneous, and deliberately full of pairs that some
        -- cheaper comparison would get wrong. Positions 5 and 6 are -0 and 0;
        -- 9 and 10 are 1e2 and 100; 17 and 18 hold the same two members
        -- inserted in opposite orders; 21 and 22 are a precomposed character
        -- and its canonical decomposition, which RFC 8785 does NOT normalize
        -- and which must therefore stay unequal.
        local reversedOrder = {}
        reversedOrder.b = 2
        reversedOrder.a = 1

        local corpus = {
            jcs.null,
            true,
            false,
            0,
            0 * -1,
            1,
            3 / 2,
            2 / 4,
            1e2,
            100,
            "",
            "a",
            "A",
            "0",
            {},
            { 1, 2 },
            { a = 1, b = 2 },
            reversedOrder,
            { a = 1 },
            { a = 2 },
            "\u{00E9}",
            "e\u{0301}",
            { { 1 }, { 2 } },
            { { 1 }, { 3 } },
        }

        local comparisons = 0
        local disagreements = {}
        for leftIndex, left in ipairs(corpus) do
            for rightIndex, right in ipairs(corpus) do
                comparisons += 1
                local byBytes = jcs.encode(left) == jcs.encode(right)
                if jcs.equals(left, right) ~= byBytes then
                    table.insert(
                        disagreements,
                        tostring(leftIndex) .. ":" .. tostring(rightIndex)
                    )
                end
            end
        end

        local function refusal(left: any, right: any): string
            local ok, message = pcall(function()
                return jcs.equals(left, right)
            end)
            if ok then return "did not refuse" end
            return tostring(message)
        end

        local function names(message: string, fragment: string): boolean
            return string.find(message, fragment, 1, true) ~= nil
        end

        local cycle = {}
        cycle.self = cycle
        local cycleMessage = refusal(cycle, cycle)
        local nan = 0 / 0
        local leftNanMessage = refusal(nan, 1)
        local rightNanMessage = refusal(1, nan)
        local nilMessage = refusal(nil, 1)
        local functionMessage = refusal(1, function() end)
        local mixedMessage = refusal(1, { [1] = true, named = true })

        return {
            comparisons = comparisons,
            corpus_size = #corpus,
            disagreements = table.concat(disagreements, " "),

            -- Named witnesses for the pairs the sweep above would let pass if
            -- the corpus itself were wrong. Each states the direction it needs.
            negative_zero_equals_zero = jcs.equals(0 * -1, 0),
            exponent_equals_integer = jcs.equals(1e2, 100),
            member_order_is_irrelevant = jcs.equals({ a = 1, b = 2 }, reversedOrder),
            distinct_members_differ = not jcs.equals({ a = 1 }, { a = 2 }),
            nesting_is_compared = not jcs.equals({ { 1 } }, { { 2 } }),
            precomposed_differs_from_decomposed = not jcs.equals(
                "\u{00E9}",
                "e\u{0301}"
            ),
            empty_table_differs_from_empty_string = not jcs.equals({}, ""),
            string_zero_differs_from_zero = not jcs.equals("0", 0),
            null_differs_from_empty_table = not jcs.equals(jcs.null, {}),
            null_equals_null = jcs.equals(jcs.null, jcs.null),

            -- A cycle compared with itself is reference-equal and canonically
            -- unspeakable. Refusing it is what says there is no fast path.
            cycle_refused = names(cycleMessage, "is a cycle"),
            cycle_names_a_side = names(cycleMessage, "the left value"),
            left_nan_named = names(leftNanMessage, "the left value")
                and names(leftNanMessage, "is NaN"),
            right_nan_named = names(rightNanMessage, "the right value")
                and names(rightNanMessage, "is NaN"),
            nil_refused = names(nilMessage, "the left value")
                and names(nilMessage, "is nil"),
            function_refused = names(functionMessage, "the right value")
                and names(functionMessage, "is a function"),
            mixed_keys_refused = names(mixedMessage, "the right value")
                and names(mixedMessage, "mixes array positions with object names"),

            -- The export itself: a frozen module member, not a name a Project
            -- module could replace on its way past.
            equals_is_frozen = not pcall(function()
                jcs.equals = function() return true end
            end),
        }
    end,
}
)LUAU"};
    }

    TEST_CASE("@umbraflow/jcs answers canonical equality")
    {
        auto closure = pureFrameworkScriptModules();
        REQUIRE(closure.has_value());

        constexpr auto entryPoints = std::array{std::string_view{"equality"}};
        auto program = script::PureDataProgram::compile(
            "fixture.canonical-equality",
            "main",
            {
                script::PureDataProgram::Module{
                    .name   = "main",
                    .source = std::string{k_probe},
                },
            },
            entryPoints,
            {},
            *closure
        );
        REQUIRE(program.has_value());

        auto input = json::parse("{}");
        REQUIRE(input.has_value());
        auto answer = program->invoke("equality", *input);
        INFO(
            "probe refusal: ",
            answer.has_value() ? std::string{} : answer.error().message()
        );
        REQUIRE(answer.has_value());

        // One golden line rather than member-by-member checks, for the same
        // reason the SDK parity fixture uses one: every member has to hold, and
        // a member that silently stopped being produced would slip past a
        // per-member read. `disagreements` is empty and `comparisons` is the
        // full 24x24 sweep -- an empty corpus would make the sweep vacuous and
        // shows up here as a changed count.
        CHECK(
            json::canonicalBytes(*answer)
            == R"({"comparisons":576,"corpus_size":24,"cycle_names_a_side":true,"cycle_refused":true,"disagreements":"","distinct_members_differ":true,"empty_table_differs_from_empty_string":true,"equals_is_frozen":true,"exponent_equals_integer":true,"function_refused":true,"left_nan_named":true,"member_order_is_irrelevant":true,"mixed_keys_refused":true,"negative_zero_equals_zero":true,"nesting_is_compared":true,"nil_refused":true,"null_differs_from_empty_table":true,"null_equals_null":true,"precomposed_differs_from_decomposed":true,"right_nan_named":true,"string_zero_differs_from_zero":true})"
        );
    }
}
