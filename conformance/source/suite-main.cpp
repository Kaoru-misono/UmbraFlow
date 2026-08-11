// umbra-flow-conformance: the Operator conformance suite as a shipped test
// runner, pointed at a project directory.
//
// It is a second binary rather than a subcommand of umbra-flow because the two
// do structurally different things. Every frame this suite observes and every
// action it posts declares TargetWorld::Recorded, and EngineSession refuses a
// session whose source and sink disagree -- so a run here can never reach a
// live window, while reaching one is the whole job of umbra-flow. Two binaries
// make that separation a link-time fact rather than a subcommand string. See
// docs/plans/2026-08-11-project-as-data.md 3.
//
// DOCTEST_CONFIG_IMPLEMENT rather than ..._WITH_MAIN: the project directory has
// to be set before any case runs, and doctest gives a TEST_CASE no parameters.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "suite-support.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::conformance
{
    namespace
    {
        constexpr auto k_projectOption = std::string_view{"--project"};

        constexpr auto k_usageText = std::string_view{
            "usage: umbra-flow-conformance --project <directory> [doctest arguments]\n"
            "\n"
            "  --project <directory>  the project directory this run is judged\n"
            "                         against: its two root documents, its plugins,\n"
            "                         its schemas, its RuntimeArtifact and its probe\n"
            "                         frame.\n"
            "\n"
            "Every remaining argument is doctest's, so one case is selected with\n"
            "--test-case=<name> and nothing here invents a second spelling of it.\n"
        };

        // What main took out of the argument vector, and what it hands on.
        struct SuiteArguments final
        {
            std::filesystem::path    project{};
            std::vector<std::string> forwarded{};
        };

        [[nodiscard]]
        auto parseArguments(
            std::span<std::string const> raw
        ) -> std::optional<SuiteArguments>
        {
            auto parsed = SuiteArguments{};
            for (auto at = std::size_t{0}; at < raw.size(); ++at)
            {
                if (raw[at] != k_projectOption)
                {
                    parsed.forwarded.emplace_back(raw[at]);
                    continue;
                }
                if (at + 1U >= raw.size())
                {
                    std::cerr << "--project needs a directory\n";
                    return std::nullopt;
                }
                ++at;
                parsed.project = std::filesystem::path{raw[at]};
            }

            if (parsed.project.empty())
            {
                std::cerr << "--project is required\n";
                return std::nullopt;
            }
            return parsed;
        }

        [[nodiscard]]
        auto runSuite(std::span<std::string const> raw) -> int
        {
            auto const parsed = parseArguments(raw);
            if (!parsed)
            {
                std::cerr << k_usageText;
                return 2;
            }
            setProjectDirectory(parsed->project);

            // doctest reads its own options out of an argument vector, so the
            // forwarded arguments are handed back in that shape. The pointers
            // are into `parsed->forwarded`, which outlives the call.
            auto arguments = std::vector<char const*>{};
            arguments.reserve(parsed->forwarded.size() + 1U);
            arguments.emplace_back("umbra-flow-conformance");
            for (auto const& one : parsed->forwarded)
            {
                arguments.emplace_back(one.c_str());
            }
            auto const argumentCount = checkedCast<int>(arguments.size());
            if (!argumentCount)
            {
                std::cerr << "too many arguments\n";
                return 2;
            }

            auto context = doctest::Context{};
            context.applyCommandLine(*argumentCount, arguments.data());
            return context.run();
        }
    }
}

auto main(int argumentCount, char const* const* p_arguments) -> int
{
    try
    {
        auto const convertedArgumentCount = uf::checkedCast<std::size_t>(
            argumentCount
        );
        if (!convertedArgumentCount || *convertedArgumentCount == 0U)
        {
            std::cerr
                << "umbra-flow-conformance error: invalid process argument vector\n";
            return 2;
        }
        // SAFETY: a hosted entry point receives argumentCount argument pointers
        // followed by a null one ([basic.start.main]/2). That count arrives
        // beside the pointer rather than within it, so no expression can restate
        // the bound; this is the single place the C contract becomes a span.
        UF_UNSAFE_BUFFER_BEGIN
        auto const arguments = std::span<char const* const>{
            p_arguments,
            *convertedArgumentCount
        };
        UF_UNSAFE_BUFFER_END
        auto raw = std::vector<std::string>{};
        for (auto const* argument : arguments.subspan(1U))
        {
            raw.emplace_back(argument);
        }

        return uf::operator_runtime::conformance::runSuite(raw);
    }
    catch (std::exception const& error)
    {
        std::cerr << "umbra-flow-conformance exception: " << error.what() << '\n';
        return 2;
    }
    catch (...)
    {
        std::cerr << "umbra-flow-conformance exception: unknown failure\n";
        return 2;
    }
}
