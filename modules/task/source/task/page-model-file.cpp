#include <task/page-model-file.hpp>

#include "capped-file.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
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
        constexpr auto k_schemaVersionKey = std::string_view{"schema_version"};
        constexpr auto k_supportedVersion = uint32{1};

        // The two top-level geometry keys are required envelope facts. A runtime
        // model that states no geometry is refused below rather than given one.
        constexpr auto k_baseResolutionKey = std::string_view{"base_resolution"};
        constexpr auto k_baseDpiKey        = std::string_view{"base_dpi"};

        constexpr auto k_targetSection  = std::string_view{"target"};
        constexpr auto k_surfaceSection = std::string_view{"surface"};
        constexpr auto k_idKey          = std::string_view{"id"};

        // Whose table a line's keys belong to. A `[target.extra]` subtable is
        // the project's own namespace, so a key it spells -- `id`, and equally
        // `base_resolution` -- is not this reader's to read; that state is
        // distinct from the top level rather than an absence of section.
        enum class Scan : uint8
        {
            TopLevel,
            ArraySection,
            ForeignSubtable,
        };

        [[nodiscard]]
        auto invalidModel(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto trim(std::string_view text) noexcept -> std::string_view
        {
            auto const isSpace = [](char character) noexcept -> bool
            {
                return character == ' ' || character == '\t' || character == '\r';
            };
            while (!text.empty() && isSpace(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && isSpace(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        // The `[[kind]]` a line opens, or nothing when the line is not one.
        [[nodiscard]]
        auto arraySectionOf(std::string_view line) noexcept
            -> std::optional<std::string_view>
        {
            if (!line.starts_with("[[") || !line.ends_with("]]"))
            {
                return std::nullopt;
            }
            return line.substr(2, line.size() - 4U);
        }

        // The `key` and the raw text after the `=`, or nothing when the line is
        // not an assignment.
        struct Assignment final
        {
            std::string_view key{};
            std::string_view value{};
        };

        [[nodiscard]]
        auto assignmentOf(std::string_view line) noexcept
            -> std::optional<Assignment>
        {
            auto const equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                return std::nullopt;
            }
            return Assignment{
                .key   = trim(line.substr(0, equals)),
                .value = trim(line.substr(equals + 1U)),
            };
        }

        // The contents of the first double-quoted run in `value`. Names in this
        // format are Luau member keys, so none of them carries an escape; a value
        // that is not quoted at all has no name in it.
        [[nodiscard]]
        auto quotedOf(std::string_view value) noexcept
            -> std::optional<std::string_view>
        {
            auto const open = value.find('"');
            if (open == std::string_view::npos)
            {
                return std::nullopt;
            }
            auto const close = value.find('"', open + 1U);
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }
            return value.substr(open + 1U, close - open - 1U);
        }

        // A `[a, b]` pair of whole numbers, both of which have to fit a uint32.
        [[nodiscard]]
        auto uint32PairOf(
            std::string_view key,
            std::string_view value
        ) -> Result<std::array<uint32, 2>>
        {
            if (!value.starts_with('[') || !value.ends_with(']'))
            {
                return invalidModel(
                    std::format(
                        "'{}' must be written as [first, second]",
                        key
                    )
                );
            }
            auto const body  = value.substr(1, value.size() - 2U);
            auto const comma = body.find(',');
            if (comma == std::string_view::npos)
            {
                return invalidModel(
                    std::format("'{}' must carry two numbers", key)
                );
            }

            auto pair  = std::array<uint32, 2>{};
            auto parts = std::array<std::string_view, 2>{
                trim(body.substr(0, comma)),
                trim(body.substr(comma + 1U)),
            };
            for (auto index = std::size_t{0}; index < parts.size(); ++index)
            {
                auto const part   = parts.at(index);
                auto       parsed = uint32{};
                auto const result = std::from_chars(
                    part.data(),
                    part.data() + part.size(),
                    parsed
                );
                if (
                    result.ec != std::errc{}
                    || result.ptr != part.data() + part.size()
                )
                {
                    return invalidModel(
                        std::format(
                            "'{}' holds '{}', which is not a whole number",
                            key,
                            part
                        )
                    );
                }
                pair.at(index) = parsed;
            }
            return pair;
        }

        [[nodiscard]]
        auto uint32Of(std::string_view key, std::string_view value) -> Result<uint32>
        {
            auto parsed = uint32{};
            auto const result = std::from_chars(
                value.data(),
                value.data() + value.size(),
                parsed
            );
            if (
                result.ec != std::errc{}
                || result.ptr != value.data() + value.size()
            )
            {
                return invalidModel(
                    std::format(
                        "'{}' holds '{}', which is not a whole number",
                        key,
                        value
                    )
                );
            }
            return parsed;
        }

        [[nodiscard]]
        auto isIdentifier(std::string_view value) noexcept -> bool
        {
            if (value.empty() || value.size() > 128U)
            {
                return false;
            }

            auto const isAlpha = [](char character) noexcept -> bool
            {
                return character >= 'a' && character <= 'z';
            };
            auto const isDigit = [](char character) noexcept -> bool
            {
                return character >= '0' && character <= '9';
            };
            if (!isAlpha(value.front()))
            {
                return false;
            }

            auto needsSegmentStart = false;
            for (auto const character : value.substr(1))
            {
                if (isAlpha(character) || isDigit(character))
                {
                    needsSegmentStart = false;
                    continue;
                }
                if (
                    character == '.' || character == '_' || character == '-'
                )
                {
                    if (needsSegmentStart)
                    {
                        return false;
                    }
                    needsSegmentStart = true;
                    continue;
                }
                return false;
            }
            return !needsSegmentStart;
        }

        auto recordId(
            std::vector<std::string>& names,
            std::string_view kind,
            std::string_view id
        ) -> Status
        {
            if (!isIdentifier(id))
            {
                return invalidModel(
                    std::format(
                        "this runtime model declares {} id '{}' with an invalid "
                        "identifier; ids must start with a lowercase letter and "
                        "contain only lowercase letters, digits, '.', '_' or '-' "
                        "between non-empty segments, up to 128 characters",
                        kind,
                        id
                    )
                );
            }
            if (std::ranges::contains(names, id))
            {
                return invalidModel(
                    std::format(
                        "this runtime model declares {} '{}' twice, so a resource "
                        "naming it would resolve against two different rows",
                        kind,
                        id
                    )
                );
            }
            names.emplace_back(id);
            return ok();
        }
    }

    auto parseRuntimeModelEnvelope(std::string_view text)
        -> Result<RuntimeModelEnvelope>
    {
        auto schemaVersion = std::optional<uint32>{};
        auto resolution = std::optional<std::array<uint32, 2>>{};
        auto dpi        = std::optional<std::array<uint32, 2>>{};

        auto targetIds  = std::vector<std::string>{};
        auto surfaceIds = std::vector<std::string>{};

        // Which table the scan is inside, and the `[[kind]]` it opened. `section`
        // carries a name only in Scan::ArraySection; which keys a line may set is
        // decided by `scan` alone, so a foreign subtable cannot be mistaken for
        // the top level.
        auto scan    = Scan::TopLevel;
        auto section = std::string_view{};

        // Whether the current `[[target]]` or `[[surface]]` has already named
        // itself. The first `id` wins, exactly as layer two's own parser takes
        // the first assignment of a field.
        auto named = false;

        auto rest = text;
        while (!rest.empty())
        {
            auto const breakAt = rest.find('\n');
            auto const raw     = rest.substr(0, breakAt);
            rest = breakAt == std::string_view::npos
                ? std::string_view{}
                : rest.substr(breakAt + 1U);

            auto const line = trim(raw);
            if (line.empty() || line.starts_with('#'))
            {
                continue;
            }

            if (auto const opened = arraySectionOf(line))
            {
                scan    = Scan::ArraySection;
                section = *opened;
                named   = false;
                continue;
            }
            if (line.starts_with('[') && line.ends_with(']'))
            {
                // A single-bracket subtable: `[target.extra]` and anything else
                // this reader has no business in.
                scan = Scan::ForeignSubtable;
                continue;
            }

            if (scan == Scan::ForeignSubtable)
            {
                continue;
            }

            auto const assignment = assignmentOf(line);
            if (!assignment)
            {
                continue;
            }

            if (scan == Scan::TopLevel)
            {
                if (assignment->key == k_schemaVersionKey)
                {
                    UF_TRY_VALUE(
                        version,
                        uint32Of(k_schemaVersionKey, assignment->value)
                    );
                    schemaVersion = version;
                }
                else if (assignment->key == k_baseResolutionKey)
                {
                    UF_TRY_VALUE(
                        pair,
                        uint32PairOf(k_baseResolutionKey, assignment->value)
                    );
                    resolution = pair;
                }
                else if (assignment->key == k_baseDpiKey)
                {
                    UF_TRY_VALUE(pair, uint32PairOf(k_baseDpiKey, assignment->value));
                    dpi = pair;
                }
                continue;
            }

            if (named || assignment->key != k_idKey)
            {
                continue;
            }
            if (section != k_targetSection && section != k_surfaceSection)
            {
                continue;
            }
            auto const name = quotedOf(assignment->value);
            if (!name)
            {
                return invalidModel(
                    std::format(
                        "a [[{}]] names itself with {}, which is not a quoted id",
                        section,
                        assignment->value
                    )
                );
            }
            named = true;
            UF_TRY(
                recordId(
                    section == k_targetSection ? targetIds : surfaceIds,
                    section,
                    *name
                )
            );
        }

        if (!schemaVersion)
        {
            return invalidModel(
                "this runtime model states no schema_version; add "
                "'schema_version = 1' at the top of the file"
            );
        }
        if (*schemaVersion != k_supportedVersion)
        {
            return invalidModel(
                std::format(
                    "this runtime model uses unsupported schema_version {}, "
                    "but this host understands {}",
                    *schemaVersion,
                    k_supportedVersion
                )
            );
        }

        if (!resolution || !dpi)
        {
            return invalidModel(
                std::format(
                    "this runtime model states no {}, so nothing says what geometry "
                    "its rectangles were measured at; add "
                    "'{} = [width, height]' and '{} = [dpiX, dpiY]' at the top of "
                    "the file",
                    !resolution ? k_baseResolutionKey : k_baseDpiKey,
                    k_baseResolutionKey,
                    k_baseDpiKey
                )
            );
        }

        if (targetIds.empty() || surfaceIds.empty())
        {
            return invalidModel(
                "this runtime model must declare at least one target and one "
                "surface"
            );
        }

        UF_TRY_VALUE(
            fingerprint,
            ProjectFingerprint::create(
                (*resolution).at(0),
                (*resolution).at(1),
                (*dpi).at(0),
                (*dpi).at(1)
            )
        );
        // Over the bytes handed in, not over what was recognised above: a model
        // differing only in a section this scan skips is a different model, and a
        // replay checked against it would be checked against the wrong file.
        UF_TRY_VALUE(
            contentHash,
            sha256(std::as_bytes(std::span{text.data(), text.size()}))
        );

        return RuntimeModelEnvelope{
            .schemaVersion = *schemaVersion,
            .fingerprint   = fingerprint,
            .contentHash   = contentHash,
            .targetIds     = std::move(targetIds),
            .surfaceIds    = std::move(surfaceIds),
        };
    }

    auto readRuntimeModelEnvelope(
        std::filesystem::path const& projectRoot
    ) -> Result<RuntimeModelEnvelope>
    {
        auto const path = projectRoot / k_runtimeModelFileName;
        UF_TRY_VALUE(
            text,
            readCappedFile(path, k_maximumRuntimeModelBytes, "runtime model")
        );

        auto facts = parseRuntimeModelEnvelope(text);
        if (!facts)
        {
            return invalidModel(
                std::format(
                    "'{}': {}",
                    path.string(),
                    facts.error().message()
                )
            );
        }
        return facts;
    }
}
