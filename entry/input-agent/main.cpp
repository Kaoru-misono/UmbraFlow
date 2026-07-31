#include "agent.hpp"
#include "args.hpp"
#include "error-text.hpp"

#include <core/numeric/checked-cast.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace uf::input_agent
{
    namespace
    {
        auto writeUnhandledException(std::exception const& error) noexcept -> void
        {
            static_cast<void>(std::fputs("umbra-input-agent exception: ", stderr));
            static_cast<void>(std::fputs(error.what(), stderr));
            static_cast<void>(std::fputc('\n', stderr));
        }

        auto writeUnknownException() noexcept -> void
        {
            static_cast<void>(
                std::fputs("umbra-input-agent exception: unknown failure\n", stderr)
            );
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
            std::cerr << "umbra-input-agent error: invalid process argument vector\n";
            return EXIT_FAILURE;
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

        // An agent launched with nothing to do says what it needs rather than
        // failing on the first missing flag. Every other argument error still
        // comes from the parser, which names the flag it read.
        if (raw.empty() || raw.front() == "--help")
        {
            std::cerr << uf::input_agent::inputAgentUsageText();
            return raw.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
        }

        auto const outcome = uf::input_agent::runInputAgent(raw);
        if (!outcome)
        {
            std::cerr
                << "umbra-input-agent error: "
                << uf::input_agent::formatAutomationError(outcome.error())
                << '\n';
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    catch (std::exception const& error)
    {
        uf::input_agent::writeUnhandledException(error);
        return EXIT_FAILURE;
    }
    catch (...)
    {
        uf::input_agent::writeUnknownException();
        return EXIT_FAILURE;
    }
}
