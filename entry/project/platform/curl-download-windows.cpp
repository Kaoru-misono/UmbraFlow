#include "curl-download.hpp"

#include <core/numeric/checked-cast.hpp>

#include <domain/error.hpp>

#include <cerrno>
#include <filesystem>
#include <format>
#include <process.h>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>

namespace uf::project_entry
{
    namespace
    {
        [[nodiscard]]
        auto utf8ToWide(std::string_view text) -> Result<std::wstring>
        {
            auto const size = checkedCast<int>(text.size());
            if (!size)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release URL exceeds the Windows process argument limit"
                );
            }
            // SAFETY: text.data() is readable for exactly size bytes, the
            // sizing call receives no output buffer, and Windows retains no
            // pointer after either call.
            auto const required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                *size,
                nullptr,
                0
            );
            if (required <= 0)
            {
                return fail(
                    std::error_code{
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                    },
                    "cannot convert a release URL from UTF-8"
                );
            }
            auto wide = std::wstring(
                static_cast<std::size_t>(required),
                L'\0'
            );
            // SAFETY: wide owns required writable wchar_t elements, text is
            // unchanged since the sizing call, and Windows retains neither
            // pointer.
            auto const written = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                *size,
                wide.data(),
                required
            );
            if (written != required)
            {
                return fail(
                    std::error_code{
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                    },
                    "cannot convert a release URL from UTF-8"
                );
            }
            return wide;
        }

        // The Windows spawn family joins argv into one command line without
        // preserving argument boundaries. Quote with the inverse of the CRT
        // argv parser so spaces and quotes in a project path remain data.
        [[nodiscard]]
        auto quotedProcessArgument(std::wstring_view argument) -> std::wstring
        {
            if (argument.find_first_of(L" \t\n\v\f\r\"") == std::wstring_view::npos)
                return std::wstring{argument};

            auto quoted      = std::wstring{L'"'};
            auto backslashes = std::size_t{};
            for (auto const character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'"')
                {
                    quoted.append((backslashes * 2U) + 1U, L'\\');
                    quoted.push_back(character);
                    backslashes = 0U;
                    continue;
                }
                quoted.append(backslashes, L'\\');
                quoted.push_back(character);
                backslashes = 0U;
            }
            quoted.append(backslashes * 2U, L'\\');
            quoted.push_back(L'"');
            return quoted;
        }
    }

    auto downloadFile(
        std::string_view url,
        std::filesystem::path const& target,
        std::uintmax_t maximumBytes
    ) -> Status
    {
        UF_TRY_VALUE(wideUrl, utf8ToWide(url));
        auto arguments = std::vector<std::wstring>{
            L"curl.exe",
            L"--fail",
            L"--location",
            L"--silent",
            L"--show-error",
            L"--connect-timeout",
            L"15",
            L"--max-time",
            L"600",
            L"--proto",
            L"=https,file",
            L"--proto-redir",
            L"=https",
            L"--header",
            L"Accept:application/octet-stream",
            L"--max-filesize",
            std::to_wstring(maximumBytes),
            L"--output",
            target.wstring(),
            std::move(wideUrl),
        };
        for (auto iterator = arguments.begin() + 1; iterator != arguments.end(); ++iterator)
            *iterator = quotedProcessArgument(*iterator);
        auto pointers = std::vector<wchar_t const*>{};
        pointers.reserve(arguments.size() + 1U);
        for (auto const& argument : arguments)
        {
            pointers.emplace_back(argument.c_str());
        }
        pointers.emplace_back(nullptr);

        // SAFETY: pointers is null-terminated and every element observes one
        // stable string owned by arguments for the synchronous _P_WAIT call.
        // The CRT copies the argument vector into the child and retains none.
        auto const result = _wspawnvp(
            _P_WAIT,
            arguments.front().c_str(),
            pointers.data()
        );
        if (result == -1)
        {
            return fail(
                std::error_code{errno, std::generic_category()},
                "cannot start curl while acquiring the UmbraFlow release"
            );
        }
        if (result != 0)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "curl refused release URL \"{}\" with exit code {}",
                    url,
                    result
                )
            );
        }
        return ok();
    }
}
