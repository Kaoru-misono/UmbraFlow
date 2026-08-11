#include "application-info.hpp"

#include <cli/args.hpp>
#include <cli/cli-result.hpp>
#include <cli/explore.hpp>
#include <cli/targets.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto dispatchExplore(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseExploreArguments(raw);
            if (!args)
            {
                std::cerr << formatError(args.error()) << '\n';
                std::cerr << exploreUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const report = exploreProduct(*args);
            if (!report)
            {
                std::cerr << formatError(report.error()) << '\n';
                return exitCodeForError(
                    report.error(),
                    exploreCancellationRequested()
                );
            }

            if (report->failure)
            {
                std::cerr << formatError(*report->failure) << '\n';
            }
            std::cout << std::format(
                "explore: queue=\"{}\" results=\"{}\" trace=\"{}\"\n",
                args->queue.string(),
                args->results.string(),
                report->tracePath.string()
            );
            return exitCodeForTaskReport(
                *report,
                exploreCancellationRequested()
            );
        }

        [[nodiscard]]
        auto dispatchTargets(std::span<std::string const> raw) -> ExitCode
        {
            if (!raw.empty())
            {
                std::cerr << std::format(
                    "unknown argument \"{}\"\n",
                    raw.front()
                );
                std::cerr << targetsUsageText();
                return ExitCode::Failure;
            }

            auto const listings = targetsProduct();
            if (!listings)
            {
                std::cerr << formatError(listings.error()) << '\n';
                return exitCodeForError(listings.error(), false);
            }

            std::cout << formatTargetListings(*listings);
            return ExitCode::Success;
        }

        using CommandHandler = ExitCode (*)(std::span<std::string const>);

        struct Command final
        {
            std::string_view name{};
            CommandHandler   handler{};
        };

        constexpr auto k_commands = std::array{
            Command{"explore", &dispatchExplore},
            Command{"targets", &dispatchTargets},
        };

        [[nodiscard]]
        auto dispatch(std::span<std::string const> raw) -> ExitCode
        {
            if (raw.empty())
            {
                std::cout << application::k_name << '\n';
                std::cout << usageText();
                return ExitCode::Success;
            }

            auto const command = std::ranges::find(
                k_commands,
                raw.front(),
                &Command::name
            );
            if (command == k_commands.end())
            {
                std::cerr << std::format(
                    "unknown subcommand \"{}\"\n",
                    raw.front()
                );
                std::cerr << usageText();
                return ExitCode::Failure;
            }
            return command->handler(raw.subspan(1));
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
            std::cerr << "umbra-flow error: invalid process argument vector\n";
            return std::to_underlying(uf::cli::ExitCode::Failure);
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

        return std::to_underlying(uf::cli::dispatch(raw));
    }
    catch (std::exception const& error)
    {
        std::cerr << "umbra-flow exception: " << error.what() << '\n';
        return std::to_underlying(uf::cli::ExitCode::Failure);
    }
    catch (...)
    {
        std::cerr << "umbra-flow exception: unknown failure\n";
        return std::to_underlying(uf::cli::ExitCode::Failure);
    }
}
