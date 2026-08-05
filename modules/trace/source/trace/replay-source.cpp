#include "replay-source.hpp"

#include "json-scan.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::trace
{
    namespace
    {
        // The front-end a wire name spells, or nullopt. It reads
        // `frontEndWireName` for every spelling rather than listing them again,
        // so the two directions cannot come to disagree.
        [[nodiscard]]
        auto frontEndFromWire(std::string_view name) noexcept
            -> std::optional<FrontEnd>
        {
            auto constexpr known = std::array{
                FrontEnd::Task,
                FrontEnd::Annotation,
                FrontEnd::Check,
            };
            for (auto const candidate : known)
            {
                if (frontEndWireName(candidate) == name)
                {
                    return candidate;
                }
            }
            return std::nullopt;
        }

        // Which step one event kind projects to, and which member carries its
        // label. A table, so a kind added to the stream later joins this
        // projection only when someone adds a row -- which is the right default,
        // because a step this cannot label is a step a checker cannot judge.
        struct ProjectedKind final
        {
            std::string_view wireName{};
            ReplayStepKind   step{};

            // Empty when the kind carries no label at all; see
            // `ReplayStep::label` for why a delivered click is one.
            std::string_view labelMember{};
        };

        inline constexpr auto k_projectedKinds = std::array{
            ProjectedKind{
                .wireName    = "framework.page_resolved",
                .step        = ReplayStepKind::PageResolved,
                .labelMember = "label",
            },
            ProjectedKind{
                .wireName    = "framework.element_clicked",
                .step        = ReplayStepKind::ElementClicked,
                .labelMember = "label",
            },
            ProjectedKind{
                .wireName    = "engine.action_delivered",
                .step        = ReplayStepKind::ActionDelivered,
                .labelMember = {},
            },
            ProjectedKind{
                .wireName    = "engine.key_delivered",
                .step        = ReplayStepKind::KeyDelivered,
                .labelMember = "key",
            },
        };

        [[nodiscard]]
        auto projectedKind(std::string_view kind) noexcept
            -> ProjectedKind const*
        {
            for (auto const& candidate : k_projectedKinds)
            {
                if (candidate.wireName == kind)
                {
                    return &candidate;
                }
            }
            return nullptr;
        }

        // One line of `text` starting at `cursor`, and where the next one starts.
        // A trailing carriage return is dropped: the sink writes \n, and a file
        // that has been through a text-mode copy carries \r\n, whose stray byte
        // would otherwise land inside the last member's value.
        struct SplitLine final
        {
            std::string_view line{};
            std::size_t      next{};
        };

        [[nodiscard]]
        auto nextLine(std::string_view text, std::size_t cursor) noexcept
            -> SplitLine
        {
            auto const breakAt = text.find('\n', cursor);
            auto const end     = breakAt == std::string_view::npos
                ? text.size()
                : breakAt;

            auto line = text.substr(cursor, end - cursor);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1U);
            }
            return SplitLine{
                .line = line,
                .next = breakAt == std::string_view::npos ? text.size() + 1U
                                                          : breakAt + 1U,
            };
        }
    }

    auto projectReplayedRun(std::string_view text) -> Result<ReplayedRun>
    {
        auto run   = ReplayedRun{};
        auto begun = false;

        auto lineNumber = std::size_t{0};
        auto cursor     = std::size_t{0};
        while (cursor <= text.size())
        {
            auto const split = nextLine(text, cursor);
            cursor           = split.next;
            ++lineNumber;
            if (split.line.empty())
            {
                continue;
            }

            auto const kind = memberString(split.line, "kind");
            if (!kind)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "line {} is not a trace event: every line of a run "
                        "carries a top-level string 'kind', and skipping a line "
                        "that does not would report a run shorter than the one "
                        "recorded",
                        lineNumber
                    )
                );
            }

            if (!begun)
            {
                if (*kind != "run.started")
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "this stream opens with '{}' rather than "
                            "run.started; a projection beginning midway would "
                            "report a run that started on whatever page the file "
                            "happens to open with",
                            *kind
                        )
                    );
                }

                auto const frontEnd = memberString(split.line, "frontEnd");
                if (!frontEnd)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "run.started states no frontEnd, so nothing says whether "
                        "this stream is a run at all"
                    );
                }

                auto const known = frontEndFromWire(*frontEnd);
                if (!known)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "run.started names front end '{}', which this build "
                            "does not know; a stream from a newer one may have "
                            "recorded verbs this projection would silently drop",
                            *frontEnd
                        )
                    );
                }

                // Every member run.started carries is required here. A default
                // would accept a malformed opening line as a run, and the run it
                // reported would be attributed to no project and no task.
                for (auto const member : {"projectId", "taskName", "modelHash"})
                {
                    if (!memberString(split.line, member))
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "run.started states no {}, so nothing says which "
                                "run this stream records or which page model it "
                                "stood on",
                                member
                            )
                        );
                    }
                }

                run.frontEnd  = *known;
                run.projectId = *memberString(split.line, "projectId");
                run.taskName  = *memberString(split.line, "taskName");
                run.modelHash = *memberString(split.line, "modelHash");
                begun         = true;
                continue;
            }

            auto const* const p_kind = projectedKind(*kind);
            if (p_kind == nullptr)
            {
                continue;
            }

            auto const seq = memberUnsigned(split.line, "seq");
            if (!seq)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "line {} carries kind '{}' and no seq; a step this "
                        "projection cannot place is a step a report cannot point "
                        "at",
                        lineNumber,
                        *kind
                    )
                );
            }

            auto label = std::string{};
            if (!p_kind->labelMember.empty())
            {
                auto read = memberString(split.line, p_kind->labelMember);
                if (!read)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "line {} carries kind '{}' and no '{}'",
                            lineNumber,
                            *kind,
                            p_kind->labelMember
                        )
                    );
                }
                label = *std::move(read);
            }

            run.steps.push_back(
                ReplayStep{
                    .kind  = p_kind->step,
                    .seq   = *seq,
                    .label = std::move(label),
                }
            );
        }

        if (!begun)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this stream carries no run.started"
            );
        }
        return run;
    }

    auto readReplayedRun(
        std::filesystem::path const& path
    ) -> Result<ReplayedRun>
    {
        auto       code = std::error_code{};
        auto const size = std::filesystem::file_size(path, code);
        if (code)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("'{}': {}", path.string(), code.message())
            );
        }
        if (size > k_maximumTraceBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "'{}' is {} bytes, past the {} a trace is read within",
                    path.string(),
                    size,
                    k_maximumTraceBytes
                )
            );
        }

        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.is_open())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("'{}' cannot be opened", path.string())
            );
        }
        auto const text = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}
        };

        auto projected = projectReplayedRun(text);
        if (!projected)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "'{}': {}",
                    path.string(),
                    projected.error().message()
                )
            );
        }
        return projected;
    }
}
