#include <project/command.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        return std::to_underlying(uf::project::runProjectCommand(raw));
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
