#include "args.hpp"
#include "application-info.hpp"
#include "check.hpp"
#include "drive.hpp"
#include "run.hpp"

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
            // whether it completed, was cancelled, or failed; a failure adds the
            // one rendered line explaining it. This is the CLI's whole remaining
            // job after argument parsing and target binding.
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
            return exitCodeForReport(*report, runCancellationRequested());
        }

        [[nodiscard]]
        auto dispatchDrive(std::span<std::string const> raw) -> ExitCode
        {
            auto const args = parseDriveArguments(raw);
            if (!args)
            {
                std::cerr << formatRunError(args.error()) << '\n';
                std::cerr << driveUsageText();
                return exitCodeForError(args.error(), false);
            }

            auto const report = driveProduct(*args);
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
                "drive: queue=\"{}\" results=\"{}\" trace=\"{}\"\n",
                args->queue.string(),
                args->results.string(),
                report->tracePath.string()
            );
            return exitCodeForReport(*report, runCancellationRequested());
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

            // EVERY LINE THIS SUBCOMMAND WRITES TO STANDARD OUTPUT IS JSON. The
            // verdict is already there, written by the routine itself, so the
            // human summary goes to standard error instead of after it -- a
            // check is the one product verb whose output is meant to be piped
            // into something that parses it.
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

        // The mode is chosen HERE and nowhere else, and there is exactly one choice
        // per process: one subcommand token selects one handler, and no handler
        // can reach another. That is the argument-level half of the exclusion; the
        // half that actually holds it is TaskHost's per-generation front-end claim,
        // which refuses the second front-end however it was reached.
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
            if (raw.front() == "drive")
            {
                return dispatchDrive(raw.subspan(1));
            }
            if (raw.front() == "check")
            {
                return dispatchCheck(raw.subspan(1));
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
