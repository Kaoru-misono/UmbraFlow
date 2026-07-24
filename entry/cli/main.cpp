#include "args.hpp"
#include "run.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/project.hpp>

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
                std::cerr << formatRunError(report.error()) << '\n';
                return exitCodeForError(report.error(), runCancellationRequested());
            }

            if (!report->m_actionDelivered)
            {
                std::cerr << std::format(
                    "run: action absent on resolved page "
                    "(page=\"{}\" action=\"{}\") trace=\"{}\"\n",
                    report->m_pageName,
                    report->m_actionName,
                    report->m_tracePath
                );
                return ExitCode::ActionAbsent;
            }

            std::cout << std::format(
                "run: page=\"{}\" action=\"{}\" click=({:.1f}, {:.1f}) trace=\"{}\"\n",
                report->m_pageName,
                report->m_actionName,
                report->m_clickClientX,
                report->m_clickClientY,
                report->m_tracePath
            );
            return ExitCode::Success;
        }

        [[nodiscard]]
        auto dispatch(std::span<std::string const> raw) -> ExitCode
        {
            if (raw.empty())
            {
                std::cout << k_projectName << '\n';
                std::cout << runUsageText();
                return ExitCode::Success;
            }

            if (raw.front() == "run")
            {
                return dispatchRun(raw.subspan(1));
            }

            std::cerr << std::format("unknown subcommand \"{}\"\n", raw.front());
            std::cerr << runUsageText();
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
