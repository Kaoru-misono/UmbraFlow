#include "command-runner.hpp"
#include "command.hpp"

// The product CLI's error and exit-code boundary. An authoring failure and a
// run failure are the same Error carrying the same AutomationErrorKind, so they
// are rendered and mapped to a process code by the same two functions rather
// than by a second convention that can drift from the documented one.
#include <run.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::authoring
{
    namespace
    {
        [[nodiscard]]
        auto reportFailure(Error const& error) -> cli::ExitCode
        {
            // The rendered line is for a human reading a terminal. stdout stays
            // the machine's channel and carries the same failure as JSON, so a
            // caller parses one stream whatever happened and reads the exit code
            // to know which it got.
            std::cerr << cli::formatRunError(error) << '\n';
            std::cout << authoringErrorJson(error) << '\n';
            // Nothing here installs a console handler, so no stop can have been
            // requested by the time a failure is reported.
            return cli::exitCodeForError(error, false);
        }

        [[nodiscard]]
        auto dispatch(std::span<std::string const> raw) -> cli::ExitCode
        {
            if (raw.empty())
            {
                std::cerr << authoringUsageText();
                return cli::ExitCode::Success;
            }

            auto const command = parseAuthoringCommand(raw);
            if (!command)
            {
                auto const code = reportFailure(command.error());
                std::cerr << authoringUsageText();
                return code;
            }

            auto const output = runAuthoringCommand(*command);
            if (!output)
            {
                return reportFailure(output.error());
            }

            std::cout << *output << '\n';
            return cli::ExitCode::Success;
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
            std::cerr << "umbra-authoring error: invalid process argument vector\n";
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

        return std::to_underlying(uf::authoring::dispatch(raw));
    }
    catch (std::exception const& error)
    {
        std::cerr << "umbra-authoring exception: " << error.what() << '\n';
        return std::to_underlying(uf::cli::ExitCode::Failure);
    }
    catch (...)
    {
        std::cerr << "umbra-authoring exception: unknown failure\n";
        return std::to_underlying(uf::cli::ExitCode::Failure);
    }
}
