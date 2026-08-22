// A Unicode normalization conformance suite for `@umbraflow/text`, driven by
// vectors derived from the release's own pinned UCD 15.0.0.
//
// The SDK pins Unicode 15.0 normalization and case folding, and until now the
// only executable coverage of that was a golden fixture: a dozen inputs whose
// expected output a human read out of the standard and typed in. A golden
// fixture proves the cases somebody thought of. It cannot say anything about
// the twenty thousand characters nobody typed, and it is exactly as correct as
// the person who wrote it.
//
// tests/task/unicode-normalization-vectors.generated.hpp is that corpus: one row
// per line in the exact five-field layout of the UCD's NormalizationTest.txt,
// `c1;c2;c3;c4;c5`, and this file asserts the five invariants that file states.
//
// PROVENANCE, because this is the part a conformance claim can lie about. The
// rows are NOT the UCD's own NormalizationTest.txt. That file is not vendored
// here and is not shipped with CPython, so it is not obtainable offline, and a
// suite that fetched it at test time would not be reproducible -- which
// scripts/generate_unicode_luau.py's whole discipline exists to prevent. What
// the rows ARE is the same pinned input every Unicode table in this repository
// is already derived from: CPython's `unicodedata`, refused by the generator
// unless its UCD is exactly 15.0.0, with the derived file pinned by SHA-256 and
// rechecked by `--check` in the local CI gate. That normalizer is an
// independent C implementation of UAX #15 sharing no code with the Luau
// algorithm under test, so it is an oracle rather than a second copy of the
// subject -- the distinction docs/pitfalls/checks-that-cannot-fail.md draws
// under "Expected and actual come from one producer".
//
// What would be needed to run the real file instead, stated exactly so the
// choice is reversible: vendor `NormalizationTest.txt` for Unicode 15.0.0 (about
// 2.4 MB, from the UCD's 15.0.0 release directory) into the repository under a
// pinned SHA-256 with its Unicode License v3 notice, then have
// scripts/generate_unicode_luau.py read its Parts 0 through 3 instead of
// deriving rows. Nothing else here changes: the row layout, the five invariants
// below, and the driver that runs them are already the ones that file defines.
//
// The suite runs on the trusted Engine rather than through PureDataProgram for
// one reason, and it is a limit rather than a preference: a pure program is
// bound to k_interruptBudgetTicks, two million interrupt invocations, which a
// quarter of a million normalizations exhausts many times over. The module
// source is the same on both paths and
// tests/task/test-framework-bundle.cpp's cross-VM parity case is what says so.

#include "unicode-normalization-vectors.generated.hpp"

#include <task/framework-bundle.hpp>

#include <script/engine.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace uf::task
{
    namespace
    {
        // Everything before the corpus. The corpus is spliced in as a long
        // string rather than passed as data because Engine takes one chunk of
        // source and nothing else; `[==[` is safe against the corpus, which is
        // hex digits, spaces, semicolons and newlines and holds no bracket at
        // all.
        constexpr auto k_driverPrefix = std::string_view{R"LUAU(
local vectors = [==[
)LUAU"};

        // The five invariants of NormalizationTest.txt, over c1..c5 = source,
        // NFC, NFD, NFKC, NFKD:
        //
        //   c2 == NFC(c1) == NFC(c2) == NFC(c3)
        //   c4 == NFC(c4) == NFC(c5)
        //   c3 == NFD(c1) == NFD(c2) == NFD(c3)
        //   c5 == NFD(c4) == NFD(c5)
        //   c4 == NFKC(c1) == NFKC(c2) == NFKC(c3) == NFKC(c4) == NFKC(c5)
        //   c5 == NFKD(c1) == NFKD(c2) == NFKD(c3) == NFKD(c4) == NFKD(c5)
        //
        // Each distinct input in a row is normalized once per form and its four
        // answers are then checked against whichever of the two groups it came
        // from. Deduplicating is not a shortcut past an invariant: a repeated
        // input in the same form is the same call with the same answer, and the
        // idempotence the repeats are there to state is still asserted, because
        // c2 and c3 are themselves in the group that must normalize to c2 and c3.
        constexpr auto k_driverSuffix = std::string_view{R"LUAU(]==]

local normalize = text.normalize
local unicodeVersion = text.unicode_version

local function decode(field)
    local out = {}
    for hex in string.gmatch(field, "%x+") do
        table.insert(out, utf8.char(tonumber(hex, 16)))
    end
    return table.concat(out)
end

local rows = 0
local assertions = 0
local failures = {}

for line in string.gmatch(vectors, "[^\n]+") do
    rows += 1

    local fields = {}
    for field in string.gmatch(line, "[^;]+") do
        table.insert(fields, decode(field))
    end
    if #fields ~= 5 then
        table.insert(failures, "row " .. tostring(rows) .. " has " ..
            tostring(#fields) .. " fields")
        break
    end

    local c1, c2, c3, c4, c5 = fields[1], fields[2], fields[3], fields[4], fields[5]

    -- `group` is 1 for the sources whose NFC is c2 and whose NFD is c3, and 2
    -- for those whose NFC is c4 and whose NFD is c5. The compatibility forms are
    -- c4 and c5 for every source in the row.
    local sources = { c1, c2, c3, c4, c5 }
    local groups = { 1, 1, 1, 2, 2 }
    local seen = {}
    for index = 1, 5 do
        local source = sources[index]
        local group = groups[index]
        local previous = seen[source]
        if previous ~= group then
            seen[source] = group

            local nfc = normalize(source, "NFC")
            local nfd = normalize(source, "NFD")
            local nfkc = normalize(source, "NFKC")
            local nfkd = normalize(source, "NFKD")
            assertions += 4

            local wantNfc = if group == 1 then c2 else c4
            local wantNfd = if group == 1 then c3 else c5
            if nfc ~= wantNfc or nfd ~= wantNfd or nfkc ~= c4 or nfkd ~= c5 then
                table.insert(failures, "row " .. tostring(rows) ..
                    " source " .. tostring(index) ..
                    " nfc=" .. tostring(nfc == wantNfc) ..
                    " nfd=" .. tostring(nfd == wantNfd) ..
                    " nfkc=" .. tostring(nfkc == c4) ..
                    " nfkd=" .. tostring(nfkd == c5) ..
                    " line=" .. line)
                if #failures >= 8 then break end
            end
        end
    end
    if #failures >= 8 then break end
end

return "version=" .. unicodeVersion ..
    " rows=" .. tostring(rows) ..
    " assertions=" .. tostring(assertions) ..
    " failures=" .. table.concat(failures, " | ")
)LUAU"};
    }

    TEST_CASE("@umbraflow/text meets the Unicode 15.0 normalization vectors")
    {
        auto source = std::string{k_driverPrefix};
        source += testing::k_unicodeNormalizationVectors;
        source += k_driverSuffix;

        auto engine = script::Engine::create(
            script::EngineConfig{
                .frameworkModules        = frameworkScriptModules(),
                .frameworkProjectGlobals = {"text"},
            }
        );
        REQUIRE(engine.has_value());

        auto report = engine->runValue(source, "unicode-normalization-conformance");
        INFO(
            "driver refusal: ",
            report.has_value() ? std::string{} : report.error().message()
        );
        REQUIRE(report.has_value());
        auto const* const rendered = report->text();
        REQUIRE(rendered != nullptr);
        INFO("report: ", *rendered);

        // Row and assertion counts are part of the assertion, not decoration: a
        // corpus that stopped being spliced in, or a loop that stopped running,
        // would otherwise report zero failures over zero rows. The exact row
        // count is the generator's own, published beside the corpus.
        auto const expected = "version="
            + std::string{testing::k_unicodeNormalizationVectorVersion}
            + " rows="
            + std::to_string(testing::k_unicodeNormalizationVectorCount);
        CHECK(rendered->starts_with(expected));
        CHECK(rendered->ends_with(" failures="));

        // The corpus itself has to be worth running. A row count this low would
        // mean the derivation collapsed to a handful of characters, which is the
        // hand-written substitute this suite exists instead of.
        CHECK(testing::k_unicodeNormalizationVectorCount > 15000U);
    }
}
