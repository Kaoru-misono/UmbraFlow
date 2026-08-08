#include "args.hpp"
#include "application-info.hpp"
#include "check.hpp"
#include "explore.hpp"
#include "replay.hpp"
#include "run.hpp"
#include "targets.hpp"

#include <core/numeric/checked-cast.hpp>

#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto dispatchRun(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseRunArguments(raw);
            if (!args)
            {
                std::cerr << formatRunError(args.error()) << '\n';
                std::cerr << runUsageText();
                // Argument parsing precedes handler installation, so no stop can
                // have been requested yet.
                return exitCodeForError(args.error(), false);
            }

            auto const report = runProduct(*args);
            if (!report)
            {
                // The run never started, so there is no report to print: only
                // the reason it could not begin.
                std::cerr << formatRunError(report.error()) << '\n';
                return exitCodeForError(report.error(), runCancellationRequested());
            }

            // A started run always names what ran and where its evidence went,
            // whether it completed, was cancelled, or failed; a failure adds the one
            // rendered line explaining it.
            if (report->failure)
            {
                std::cerr << formatRunError(*report->failure) << '\n';
            }
            std::cout << std::format(
                "run: task=\"{}\" hash={} seed={} trace=\"{}\"\n",
                report->taskName,
                report->sourceHash,
                report->seed,
                report->tracePath.string()
            );

            // The task's own account of the run, printed under the host's four
            // facts because it is the only one that can say where the run got to.
            // A task that returned nothing prints nothing.
            if (!report->returned.empty())
            {
                std::cout << std::format("said: {}\n", report->returned);
            }
            return exitCodeForReport(*report, runCancellationRequested());
        }

        [[nodiscard]]
        auto dispatchExplore(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseExploreArguments(raw);
            if (!args)
            {
                std::cerr << formatRunError(args.error()) << '\n';
                std::cerr << exploreUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const report = exploreProduct(*args);
            if (!report)
            {
                std::cerr << formatRunError(report.error()) << '\n';
                return exitCodeForError(report.error(), runCancellationRequested());
            }

            if (report->failure)
            {
                std::cerr << formatRunError(*report->failure) << '\n';
            }
            std::cout << std::format(
                "explore: queue=\"{}\" results=\"{}\" seed={} trace=\"{}\"\n",
                args->queue.string(),
                args->results.string(),
                report->seed,
                report->tracePath.string()
            );
            return exitCodeForReport(*report, runCancellationRequested());
        }

        // The one verb that binds nothing and reads no project: it answers the
        // --hwnd every other target-bound verb requires, so it must work before
        // anything else does.
        [[nodiscard]]
        auto dispatchTargets(std::span<std::string const> raw) -> ExitCode
        {
            if (!raw.empty())
            {
                std::cerr << std::format("unknown argument \"{}\"\n", raw.front());
                std::cerr << targetsUsageText();
                return ExitCode::Failure;
            }

            auto const listings = targetsProduct();
            if (!listings)
            {
                std::cerr << formatRunError(listings.error()) << '\n';
                return exitCodeForError(listings.error(), false);
            }

            std::cout << formatTargetListings(*listings);
            return ExitCode::Success;
        }

        // The replay verb, on dispatchCheck's shape for its reason: the verdict
        // is JSON on standard output and the human summary is on standard error,
        // so the two never interleave in a pipe.
        [[nodiscard]]
        auto dispatchReplay(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseReplayArguments(raw);
            if (!args)
            {
                std::cerr << formatRunError(args.error()) << '\n';
                std::cerr << replayUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const report = replayProduct(*args);
            if (!report)
            {
                std::cerr << formatRunError(report.error()) << '\n';
                return exitCodeForError(report.error(), false);
            }

            if (report->run.failure)
            {
                std::cerr << formatRunError(*report->run.failure) << '\n';
            }
            std::cerr << std::format(
                "replay: project=\"{}\" findings={} trace=\"{}\"\n",
                args->project.string(),
                report->findings,
                args->trace.string()
            );
            return exitCodeForReplay(*report);
        }

        [[nodiscard]]
        auto dispatchCheck(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseCheckArguments(raw);
            if (!args)
            {
                std::cerr << formatRunError(args.error()) << '\n';
                std::cerr << checkUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const report = checkProduct(*args);
            if (!report)
            {
                std::cerr << formatRunError(report.error()) << '\n';
                return exitCodeForError(report.error(), false);
            }

            // Every line this subcommand writes to standard output is JSON. The
            // verdict is already there, written by the routine itself, so the human
            // summary goes to standard error -- a check is the one product verb
            // whose output is meant to be piped into something that parses it.
            if (report->run.failure)
            {
                std::cerr << formatRunError(*report->run.failure) << '\n';
            }
            std::cerr << std::format(
                "check: project=\"{}\" findings={} trace=\"{}\"\n",
                args->project.string(),
                report->findings,
                report->run.tracePath.string()
            );
            return exitCodeForCheck(*report);
        }

        // The mode is chosen HERE and nowhere else: one subcommand token selects one
        // handler, and no handler can reach another. That is the argument-level half
        // of the exclusion; the half that actually holds it is TaskHost's
        // per-generation front-end claim, which refuses the second front-end however
        // it was reached.
        [[nodiscard]]
        auto dispatch(std::span<std::string const> raw) -> ExitCode
        {
            if (raw.empty())
            {
                std::cout << application::k_name << '\n';
                std::cout << usageText();
                return ExitCode::Success;
            }

            if (raw.front() == "run")
            {
                return dispatchRun(raw.subspan(1));
            }
            if (raw.front() == "explore")
            {
                return dispatchExplore(raw.subspan(1));
            }
            if (raw.front() == "targets")
            {
                return dispatchTargets(raw.subspan(1));
            }
            if (raw.front() == "check")
            {
                return dispatchCheck(raw.subspan(1));
            }
            if (raw.front() == "replay")
            {
                return dispatchReplay(raw.subspan(1));
            }

            std::cerr << std::format("unknown subcommand \"{}\"\n", raw.front());
            std::cerr << usageText();
            return ExitCode::Failure;
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
        auto const arguments = std::span<char const* const>{
            p_arguments,
            *convertedArgumentCount
        };
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
