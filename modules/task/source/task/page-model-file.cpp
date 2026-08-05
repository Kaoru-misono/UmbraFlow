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
        // The two top-level keys layer two states for this reader. They are layer
        // two's own under the schema's rule that every non-`extra` key belongs to
        // it, and both are REQUIRED: a page model that states no geometry is
        // refused below rather than given one.
        constexpr auto k_baseResolutionKey = std::string_view{"base_resolution"};
        constexpr auto k_baseDpiKey        = std::string_view{"base_dpi"};

        constexpr auto k_elementSection = std::string_view{"element"};
        constexpr auto k_pageSection    = std::string_view{"page"};
        constexpr auto k_nameKey        = std::string_view{"name"};

        // Whose table a line's keys belong to. A `[element.extra]` subtable is
        // the project's own namespace, so a key it spells -- `name`, and equally
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
        auto recordName(
            std::vector<std::string>& names,
            std::string_view kind,
            std::string_view name
        ) -> Status
        {
            if (std::ranges::contains(names, name))
            {
                return invalidModel(
                    std::format(
                        "this page model declares {} '{}' twice, so a script "
                        "naming it would resolve against two different rows",
                        kind,
                        name
                    )
                );
            }
            names.emplace_back(name);
            return ok();
        }
    }

    auto parsePageModelFacts(std::string_view text) -> Result<PageModelFacts>
    {
        auto resolution = std::optional<std::array<uint32, 2>>{};
        auto dpi        = std::optional<std::array<uint32, 2>>{};

        auto elementNames = std::vector<std::string>{};
        auto pageNames    = std::vector<std::string>{};

        // Which table the scan is inside, and the `[[kind]]` it opened. `section`
        // carries a name only in Scan::ArraySection; which keys a line may set is
        // decided by `scan` alone, so a foreign subtable cannot be mistaken for
        // the top level.
        auto scan    = Scan::TopLevel;
        auto section = std::string_view{};

        // Whether the current `[[element]]` or `[[page]]` has already named
        // itself. The first `name` wins, exactly as layer two's own parser takes
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
                // A single-bracket subtable: `[element.extra]` and anything else
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
                if (assignment->key == k_baseResolutionKey)
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

            if (named || assignment->key != k_nameKey)
            {
                continue;
            }
            if (section != k_elementSection && section != k_pageSection)
            {
                continue;
            }
            auto const name = quotedOf(assignment->value);
            if (!name)
            {
                return invalidModel(
                    std::format(
                        "a [[{}]] names itself with {}, which is not a quoted "
                        "name",
                        section,
                        assignment->value
                    )
                );
            }
            named = true;
            UF_TRY(
                recordName(
                    section == k_elementSection ? elementNames : pageNames,
                    section,
                    *name
                )
            );
        }

        if (!resolution || !dpi)
        {
            return invalidModel(
                std::format(
                    "this page model states no {}, so nothing says what geometry "
                    "its rectangles were measured at; add "
                    "'{} = [width, height]' and '{} = [dpiX, dpiY]' at the top of "
                    "the file",
                    !resolution ? k_baseResolutionKey : k_baseDpiKey,
                    k_baseResolutionKey,
                    k_baseDpiKey
                )
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

        return PageModelFacts{
            .fingerprint  = fingerprint,
            .contentHash  = contentHash,
            .elementNames = std::move(elementNames),
            .pageNames    = std::move(pageNames),
        };
    }

    auto readPageModelFacts(
        std::filesystem::path const& projectRoot
    ) -> Result<PageModelFacts>
    {
        auto const path = projectRoot / k_pageModelFileName;
        UF_TRY_VALUE(
            text,
            readCappedFile(path, k_maximumPageModelBytes, "page model")
        );

        auto facts = parsePageModelFacts(text);
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
