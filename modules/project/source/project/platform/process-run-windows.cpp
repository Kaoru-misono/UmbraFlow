#include "process-run.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <cerrno>
#include <cstddef>
#include <format>
#include <process.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <windows.h>

namespace uf::project
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
                    "a process argument exceeds the Windows argument limit"
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
                    "cannot convert a process argument from UTF-8"
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
                    "cannot convert a process argument from UTF-8"
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

    auto runProcess(std::span<std::string const> commandLine) -> Result<int32>
    {
        if (commandLine.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "a process command line must name a program"
            );
        }

        auto arguments = std::vector<std::wstring>{};
        arguments.reserve(commandLine.size());
        for (auto const& argument : commandLine)
        {
            UF_TRY_VALUE(wide, utf8ToWide(argument));
            arguments.emplace_back(std::move(wide));
        }
        // EVERY element is quoted, argv[0] included. The spawn call locates the
        // program from its own cmdname parameter, which takes the path as
        // written, while the vector below is joined into the command line the
        // child's CRT parses back apart -- so an unquoted argv[0] holding a
        // space makes the child read its own program path as its first two
        // arguments. That is invisible until a project lives somewhere with a
        // space in the path, and then every argument is off by one.
        auto const program = arguments.front();
        for (auto& argument : arguments)
        {
            argument = quotedProcessArgument(argument);
        }

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
            program.c_str(),
            pointers.data()
        );
        if (result == -1)
        {
            return fail(
                std::error_code{errno, std::generic_category()},
                std::format("cannot start \"{}\"", commandLine.front())
            );
        }
        auto const status = checkedCast<int32>(result);
        if (!status)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "\"{}\" exited with a status this platform cannot report",
                    commandLine.front()
                )
            );
        }
        return *status;
    }
}
