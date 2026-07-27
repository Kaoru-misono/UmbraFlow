#include <task/task-loader.hpp>

#include <core/error/result.hpp>

#include <annotation/content-hash.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto ioFailure(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::IoFailure, std::move(message));
        }

        // The task-name allowlist. A single path segment drawn only from these
        // characters cannot be empty, "..", a separator, a drive, or an absolute
        // path, so name resolution provably stays inside the tasks directory. It
        // is deliberately narrower than any filesystem would accept: a task name
        // is an addressing key, and a conservative allowlist fails closed on
        // anything surprising rather than reasoning about traversal case by case.
        [[nodiscard]]
        auto isSafeTaskNameChar(char value) noexcept -> bool
        {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9') || value == '_' || value == '-';
        }

        [[nodiscard]]
        auto validateTaskName(std::string_view name) -> Status
        {
            if (name.empty())
            {
                return invalid(
                    "task name is empty; pass a single name segment matching "
                    "[A-Za-z0-9_-]"
                );
            }
            for (auto const character : name)
            {
                if (!isSafeTaskNameChar(character))
                {
                    return invalid(
                        std::format(
                            "task name \"{}\" contains an unsupported character; "
                            "only [A-Za-z0-9_-] is allowed, with no path "
                            "separators, dots, or '..'",
                            name
                        )
                    );
                }
            }
            return ok();
        }

        // The .luau task names present under `tasksDir`, sorted, for a
        // did-you-mean list. A missing or unreadable directory yields an empty
        // list rather than an error: the caller is already reporting a missing
        // task, and the enumeration only enriches that message.
        [[nodiscard]]
        auto existingTaskNames(
            std::filesystem::path const& tasksDir
        ) -> std::vector<std::string>
        {
            auto names     = std::vector<std::string>{};
            auto stepError = std::error_code{};
            auto const last = std::filesystem::directory_iterator{};

            // The error_code-taking construction and increment keep this
            // best-effort listing from ever throwing across loadTask's Result
            // boundary: a missing directory yields the end iterator, and any
            // mid-iteration failure stops the walk with whatever was gathered.
            for (
                auto iterator = std::filesystem::directory_iterator{tasksDir, stepError};
                !stepError && iterator != last;
                iterator.increment(stepError)
            )
            {
                auto entryError = std::error_code{};
                auto const& entry = *iterator;
                if (!entry.is_regular_file(entryError) || entryError)
                {
                    continue;
                }
                if (entry.path().extension() == ".luau")
                {
                    names.emplace_back(entry.path().stem().string());
                }
            }
            std::ranges::sort(names);
            return names;
        }

        [[nodiscard]]
        auto missingTaskError(
            std::string_view name,
            std::filesystem::path const& tasksDir
        ) -> std::unexpected<Error>
        {
            auto const available = existingTaskNames(tasksDir);
            if (available.empty())
            {
                return invalid(
                    std::format(
                        "no task named \"{}\": no tasks exist in '{}'",
                        name,
                        tasksDir.string()
                    )
                );
            }

            auto joined = std::string{};
            for (auto const& existing : available)
            {
                if (!joined.empty())
                {
                    joined += ", ";
                }
                joined += existing;
            }
            return invalid(
                std::format(
                    "no task named \"{}\" in '{}'; available tasks: {}",
                    name,
                    tasksDir.string(),
                    joined
                )
            );
        }

        // Reads a whole file, refusing anything past `maximumBytes` on the bytes
        // the stream actually yields rather than a pre-read stat, so a file that
        // grows after a stat cannot slip past the cap. Mirrors the engine runtime
        // loader's readCappedFile; the two modules share no file-reading facility,
        // so the discipline is duplicated deliberately.
        [[nodiscard]]
        auto readCappedFile(
            std::filesystem::path const& path,
            std::size_t maximumBytes
        ) -> Result<std::string>
        {
            auto sizeError       = std::error_code{};
            auto const fileBytes = std::filesystem::file_size(path, sizeError);
            if (!sizeError && fileBytes > maximumBytes)
            {
                return invalid(
                    std::format(
                        "'{}' is {} bytes, exceeding the {}-byte task cap",
                        path.string(),
                        fileBytes,
                        maximumBytes
                    )
                );
            }

            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return ioFailure(std::format("cannot open '{}'", path.string()));
            }

            constexpr auto chunkBytes = std::size_t{64} * 1024U;
            auto contents             = std::string{};
            for (;;)
            {
                auto const oldSize = contents.size();
                contents.resize(oldSize + chunkBytes);
                stream.read(
                    contents.data() + oldSize,
                    static_cast<std::streamsize>(chunkBytes)
                );
                contents.resize(oldSize + static_cast<std::size_t>(stream.gcount()));

                if (stream.bad())
                {
                    return ioFailure(std::format("cannot read '{}'", path.string()));
                }
                if (contents.size() > maximumBytes)
                {
                    return invalid(
                        std::format(
                            "'{}' exceeds the {}-byte task cap",
                            path.string(),
                            maximumBytes
                        )
                    );
                }
                if (stream.eof())
                {
                    break;
                }
            }
            return contents;
        }
    }

    auto loadTask(
        std::filesystem::path const& projectRoot,
        std::string_view taskName
    ) -> Result<LoadedTask>
    {
        UF_TRY(validateTaskName(taskName));

        auto const tasksDir = projectRoot / "tasks";
        auto const path     = tasksDir / (std::string{taskName} + ".luau");

        auto existsError = std::error_code{};
        if (!std::filesystem::is_regular_file(path, existsError))
        {
            return missingTaskError(taskName, tasksDir);
        }

        UF_TRY_VALUE(source, readCappedFile(path, k_maximumTaskSourceBytes));
        UF_TRY_VALUE(hash, annotation::sha256(std::as_bytes(std::span{source})));

        return LoadedTask{
            .name   = std::string{taskName},
            .source = std::move(source),
            .hash   = hash,
        };
    }
}
