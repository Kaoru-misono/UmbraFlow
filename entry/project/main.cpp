#include <project/command.hpp>

#include "release-bootstrap.hpp"

#include <core/error/contracts.hpp>
#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::project_entry
{
    namespace
    {
        // The one subcommand this executable wires anything onto. `build`
        // and `check` used to record and verify the files a deployment named
        // by path; every declarative member is now inline in the root
        // document, so the kit's own comparisons cover the whole declaration
        // and there is nothing left for this layer to add.
        enum class ProjectWiring : uint8
        {
            Init,
        };

        struct ProjectWiringDefinition final
        {
            std::string_view name{};
            ProjectWiring    wiring{};
        };

        constexpr auto k_projectWiring = std::array{
            ProjectWiringDefinition{"init", ProjectWiring::Init},
        };

        [[nodiscard]]
        auto runWired(std::span<std::string const> raw) -> uf::project::ProjectExitCode
        {
            if (raw.empty())
            {
                return uf::project::runProjectCommand(raw);
            }

            auto const definition = std::ranges::find(
                k_projectWiring,
                raw.front(),
                &ProjectWiringDefinition::name
            );
            if (definition == k_projectWiring.end())
            {
                return uf::project::runProjectCommand(raw);
            }

            switch (definition->wiring)
            {
            case ProjectWiring::Init:
            {
                auto const directories = uf::project::parseProjectDirectories(
                    raw.subspan(1),
                    "init"
                );
                if (!directories)
                {
                    std::cerr << directories.error().message() << '\n';
                    return uf::project::ProjectExitCode::Failure;
                }
                auto const prepared = prepareReleaseBundle(
                    directories->sourceDirectory
                );
                if (!prepared)
                {
                    std::cerr << prepared.error().message() << '\n';
                    return uf::project::ProjectExitCode::Failure;
                }
                return uf::project::runProjectCommand(raw);
            }
            }
            UF_UNREACHABLE_MSG("Unknown ProjectWiring value");
        }
    }
}

namespace
{
    // Reporting a fatal exception must not itself become the reason the process
    // dies: std::cerr's inserters are not noexcept, and a throw out of a catch
    // handler in main leaves nowhere to report it. A stream that fails while
    // printing why an earlier failure happened has nothing further to say, so
    // the exit code carries the outcome on its own.
    [[nodiscard]]
    auto reportFatalException(std::string_view what) noexcept -> int
    {
        try
        {
            std::cerr << "project exception: " << what << '\n';
        }
        catch (...)
        {
        }
        return std::to_underlying(uf::project::ProjectExitCode::Failure);
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
            std::cerr << "project error: invalid process argument vector\n";
            return std::to_underlying(uf::project::ProjectExitCode::Failure);
        }
        // SAFETY: a hosted entry point receives argumentCount argument pointers
        // followed by a null one ([basic.start.main]/2). That count arrives
        // beside the pointer rather than within it, so this is the only place
        // the C contract becomes a span.
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
        return std::to_underlying(uf::project_entry::runWired(raw));
    }
    catch (std::exception const& error)
    {
        return reportFatalException(error.what());
    }
    catch (...)
    {
        return reportFatalException("unknown failure");
    }
}
