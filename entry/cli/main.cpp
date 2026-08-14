#include "application-info.hpp"

#include <cli/args.hpp>
#include <cli/cli-result.hpp>
#include <cli/explore.hpp>
#include <cli/observe.hpp>
#include <cli/ocr.hpp>
#include <cli/open-project.hpp>
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
        auto dispatchOcr(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseOcrArguments(raw);
            if (!args)
            {
                std::cerr << formatError(args.error()) << '\n';
                std::cerr << ocrUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const text = ocrProduct(*args);
            if (!text)
            {
                std::cerr << formatError(text.error()) << '\n';
                return exitCodeForError(text.error(), false);
            }

            // The document and nothing else on stdout. Every diagnostic this
            // verb produces went to stderr above, so a caller may pipe this
            // stream straight into a parser or a hash.
            std::cout << formatImageText(*text) << '\n';
            return ExitCode::Success;
        }

        [[nodiscard]]
        auto dispatchObserve(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseObserveArguments(raw);
            if (!args)
            {
                std::cerr << formatError(args.error()) << '\n';
                std::cerr << observeUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const observed = observeProduct(*args);
            if (!observed)
            {
                std::cerr << formatError(observed.error()) << '\n';
                return exitCodeForError(observed.error(), false);
            }

            std::cout << formatObservedState(*observed);
            return ExitCode::Success;
        }

        [[nodiscard]]
        auto dispatchOpen(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseOpenArguments(raw);
            if (!args)
            {
                std::cerr << formatError(args.error()) << '\n';
                std::cerr << openUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const opened = openProjectProduct(*args);
            if (!opened)
            {
                std::cerr << formatError(opened.error()) << '\n';
                return exitCodeForError(opened.error(), false);
            }

            std::cout << formatOpenedProject(*opened);
            if (!everyPluginRegistered(*opened))
            {
                return ExitCode::Failure;
            }
            return ExitCode::Success;
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
            Command{"observe", &dispatchObserve},
            Command{"ocr", &dispatchOcr},
            Command{"open", &dispatchOpen},
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
            std::cerr << "umbra-flow exception: " << what << '\n';
        }
        catch (...)
        {
        }
        return std::to_underlying(uf::cli::ExitCode::Failure);
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
        return reportFatalException(error.what());
    }
    catch (...)
    {
        return reportFatalException("unknown failure");
    }
}
