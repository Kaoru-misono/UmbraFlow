#include "process-run.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <cerrno>
#include <format>
#include <spawn.h>
#include <span>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace uf::project
{
    auto runProcess(std::span<std::string const> commandLine) -> Result<int32>
    {
        if (commandLine.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "a process command line must name a program"
            );
        }

        // posix_spawnp takes a mutable argv it does not modify. The copy is
        // this function's own, so nothing the caller owns is exposed to it.
        auto arguments = std::vector<std::string>{
            commandLine.begin(),
            commandLine.end()
        };
        auto pointers = std::vector<char*>{};
        pointers.reserve(arguments.size() + 1U);
        for (auto& argument : arguments)
        {
            pointers.emplace_back(argument.data());
        }
        pointers.emplace_back(nullptr);

        auto process       = pid_t{};
        auto const spawned = posix_spawnp(
            &process,
            pointers.front(),
            nullptr,
            nullptr,
            pointers.data(),
            environ
        );
        if (spawned != 0)
        {
            return fail(
                std::error_code{spawned, std::generic_category()},
                std::format("cannot start \"{}\"", commandLine.front())
            );
        }

        auto status = int{};
        auto waited = pid_t{};
        do
        {
            waited = waitpid(process, &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited == -1)
        {
            return fail(
                std::error_code{errno, std::generic_category()},
                std::format("cannot wait for \"{}\"", commandLine.front())
            );
        }
        if (!WIFEXITED(status))
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "\"{}\" did not exit normally",
                    commandLine.front()
                )
            );
        }
        auto const code = checkedCast<int32>(WEXITSTATUS(status));
        if (!code)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "\"{}\" exited with a status this platform cannot report",
                    commandLine.front()
                )
            );
        }
        return *code;
    }
}
