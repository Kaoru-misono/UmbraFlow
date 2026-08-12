#include <project/command.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
        std::cerr << "project exception: " << error.what() << '\n';
        return std::to_underlying(uf::project::ProjectExitCode::Failure);
    }
    catch (...)
    {
        std::cerr << "project exception: unknown failure\n";
        return std::to_underlying(uf::project::ProjectExitCode::Failure);
    }
}
