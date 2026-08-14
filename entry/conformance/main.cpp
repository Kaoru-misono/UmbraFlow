#include <conformance/suite-run.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/safety/annotations.hpp>

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
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
            std::cerr << "umbra-flow-conformance exception: " << what << '\n';
        }
        catch (...)
        {
        }
        return 2;
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
            std::cerr
                << "umbra-flow-conformance error: invalid process argument vector\n";
            return 2;
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

        return uf::operator_runtime::conformance::runSuite(raw);
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
