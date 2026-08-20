#include "curl-download.hpp"

#include <domain/error.hpp>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <spawn.h>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace uf::project_entry
{
    auto downloadFile(
        std::string_view url,
        std::filesystem::path const& target,
        std::uintmax_t maximumBytes
    ) -> Status
    {
        auto arguments = std::vector<std::string>{
            "curl",
            "--fail",
            "--location",
            "--silent",
            "--show-error",
            "--connect-timeout",
            "15",
            "--max-time",
            "600",
            "--proto",
            "=https,file",
            "--proto-redir",
            "=https",
            "--header",
            "Accept:application/octet-stream",
            "--max-filesize",
            std::to_string(maximumBytes),
            "--output",
            target.string(),
            std::string{url},
        };
        auto pointers = std::vector<char*>{};
        pointers.reserve(arguments.size() + 1U);
        for (auto& argument : arguments)
        {
            pointers.emplace_back(argument.data());
        }
        pointers.emplace_back(nullptr);

        auto process = pid_t{};
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
                "cannot start curl while acquiring the UmbraFlow release"
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
                "cannot wait for curl while acquiring the UmbraFlow release"
            );
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            auto const exitCode = WIFEXITED(status)
                ? WEXITSTATUS(status)
                : -1;
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "curl refused release URL \"{}\" with exit code {}",
                    url,
                    exitCode
                )
            );
        }
        return ok();
    }
}
