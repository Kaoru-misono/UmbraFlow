#include <task/framework-bundle.hpp>

#include <annotation/content-hash.hpp>
#include <core/error/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto hexDigestOf(std::string_view text) -> std::string
        {
            auto const digest = annotation::sha256(std::as_bytes(std::span{text}));
            REQUIRE(digest.has_value());
            return digest->hex();
        }
    }

    TEST_CASE("the embedded bundle carries the modules the project environment names")
    {
        auto const entries = frameworkBundleEntries();
        REQUIRE(!entries.empty());

        // Each module name and its published project global are the same string,
        // and nothing derives one from the other: the generator takes the name
        // from a file stem while frameworkProjectGlobals() is a C++ constant. A
        // rename that touched only one side fails here rather than at VM boot.
        auto const published = frameworkProjectGlobals();
        CHECK(
            published
            == std::vector<std::string>{
                std::string{"ctx"},
                std::string{"task"},
                std::string{"model"},
                std::string{"observe"},
                std::string{"project"},
            }
        );

        for (auto const& name : published)
        {
            auto const module = std::ranges::find(
                entries,
                std::string_view{name},
                &FrameworkBundleEntry::name
            );
            REQUIRE(module != entries.end());
            CHECK(module->sourceHash.size() == 64U);
        }

        // The declaration module reaches the context module's registry channel,
        // which only holds because modules load in bundle order and `ctx` sorts
        // first. Asserting the order here keeps a rename that reverses it from
        // failing later as a nil index inside task.define.
        CHECK(std::string_view{entries.front().name} == "ctx");

        CHECK(!frameworkVersion().empty());
        CHECK(frameworkBundleHash().size() == 64U);
    }

    TEST_CASE("bundle entries are sorted by name and each name appears once")
    {
        auto names = std::vector<std::string_view>{};
        for (auto const& entry : frameworkBundleEntries())
        {
            names.emplace_back(entry.name);
        }

        CHECK(std::ranges::is_sorted(names));
        CHECK(std::ranges::adjacent_find(names) == names.end());
    }

    // The load-bearing case: the digest scripts/embed_luau.py recorded at build
    // time must equal the one annotation::sha256 computes at run time. A failure
    // here means the Python and C++ hash definitions have drifted, or that
    // embedding did not reproduce the source bytes exactly.
    TEST_CASE("each recorded hash equals annotation::sha256 of the embedded source")
    {
        for (auto const& entry : frameworkBundleEntries())
        {
            CHECK(entry.sourceHash == hexDigestOf(entry.source));
        }
    }

    TEST_CASE("the bundle hash matches the recipe documented on the accessor")
    {
        auto preimage = std::string{};
        for (auto const& entry : frameworkBundleEntries())
        {
            preimage += entry.name;
            preimage.push_back('\0');
            preimage += entry.source;
        }

        CHECK(frameworkBundleHash() == hexDigestOf(preimage));
    }

    // The repository's syntax gate for .luau sources. It runs the vendored Luau
    // parser the host itself uses, so a malformed framework module fails under
    // `ctest -L CI` instead of at VM load time. The vendored tree is configured
    // with LUAU_BUILD_CLI OFF, so no luau-ast or luau-compile executable exists
    // for a script gate to shell out to; driving the parser from here behind the
    // module's ffi boundary adds no build surface at all.
    TEST_CASE("every embedded framework module parses as valid Luau")
    {
        for (auto const& entry : frameworkBundleEntries())
        {
            auto const parsed = checkFrameworkModuleSyntax(entry.source, entry.name);
            // Named rather than inlined: doctest's message macro binds its
            // stream operator tighter than a conditional expression would.
            auto const diagnostic = parsed
                ? std::string{}
                : toString(parsed.error());
            CHECK_MESSAGE(parsed.has_value(), diagnostic);
        }
    }
}
