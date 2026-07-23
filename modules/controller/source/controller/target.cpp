#include "target.hpp"

#include "detail/target-logic.hpp"
#include "platform/windows-controller.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto formatDebugString(std::string const& value) -> std::string
        {
            auto output = std::string{"\""};
            for (auto const character : value)
            {
                switch (character)
                {
                case '\0':
                    output += "\\0";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '"':
                    output += "\\\"";
                    break;
                case '\\':
                    output += "\\\\";
                    break;
                default:
                {
                    auto const byte = static_cast<unsigned char>(character);
                    if (byte < 0x20U || byte == 0x7FU)
                    {
                        output += std::format("\\u{{{:x}}}", byte);
                    }
                    else
                    {
                        output += character;
                    }
                    break;
                }
                }
            }
            output += '"';
            return output;
        }

        [[nodiscard]]
        auto formatWindowHandle(WindowHandle handle) -> std::string
        {
            return std::format(
                "{:#x}",
                static_cast<uintptr>(handle.value())
            );
        }

        [[nodiscard]]
        auto join(std::vector<std::string> const& parts) -> std::string
        {
            auto output = std::string{};
            for (auto const& part : parts)
            {
                if (!output.empty())
                {
                    output += ' ';
                }
                output += part;
            }
            return output;
        }

        [[nodiscard]]
        auto describeSelector(TargetSelector const& selector) -> std::string
        {
            auto parts = std::vector<std::string>{};
            if (auto const process = selector.process())
            {
                parts.emplace_back(std::format("pid={}", process->value()));
            }
            if (auto const handle = selector.windowHandle())
            {
                parts.emplace_back("hwnd=" + formatWindowHandle(*handle));
            }
            if (selector.windowClass())
            {
                parts.emplace_back("class=" + formatDebugString(*selector.windowClass()));
            }
            if (selector.title())
            {
                parts.emplace_back("title=" + formatDebugString(*selector.title()));
            }

            return parts.empty() ? "(none)" : join(parts);
        }

        [[nodiscard]]
        auto matchingCandidateIndices(
            std::span<TargetCandidate const> candidates,
            TargetSelector const& selector
        ) -> std::vector<std::size_t>
        {
            auto indices = std::vector<std::size_t>{};
            for (auto index = std::size_t{0}; index < candidates.size(); ++index)
            {
                if (selector.matches(candidates[index]))
                {
                    indices.emplace_back(index);
                }
            }
            return indices;
        }

        [[nodiscard]]
        auto describeCandidates(
            std::span<TargetCandidate const> candidates,
            std::vector<std::size_t> const& indices
        ) -> std::string
        {
            auto parts = std::vector<std::string>{};
            parts.reserve(indices.size());
            for (auto const index : indices)
            {
                auto const& candidate = candidates[index];
                parts.emplace_back(
                    std::format(
                        "[pid={} hwnd={} class={} title={}]",
                        candidate.process().value(),
                        formatWindowHandle(candidate.handle()),
                        formatDebugString(candidate.windowClass()),
                        formatDebugString(candidate.title())
                    )
                );
            }
            return join(parts);
        }

        [[nodiscard]]
        auto resolveCandidate(
            std::span<TargetCandidate const> candidates,
            TargetSelector const& selector
        ) -> Result<TargetCandidate>
        {
            auto const process = selector.process();
            auto const requestedHandle = selector.windowHandle();
            if (process && requestedHandle)
            {
                auto const named = std::ranges::find_if(
                    candidates,
                    [requestedHandle](TargetCandidate const& candidate)
                    {
                        return candidate.handle() == *requestedHandle;
                    }
                );
                if (named != candidates.end() && named->process() != *process)
                {
                    return fail(
                        AutomationErrorKind::TargetUnavailable,
                        std::format(
                            "requested hwnd {} belongs to pid {} not requested pid {}",
                            formatWindowHandle(*requestedHandle),
                            named->process().value(),
                            process->value()
                        )
                    );
                }
            }

            auto const indices = matchingCandidateIndices(candidates, selector);
            if (indices.empty())
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    "no window matched selector " + describeSelector(selector)
                );
            }
            if (indices.size() != 1U)
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    std::format(
                        "{} windows matched selector {}; disambiguate with --pid or --hwnd: {}",
                        indices.size(),
                        describeSelector(selector),
                        describeCandidates(candidates, indices)
                    )
                );
            }

            return candidates[indices.front()];
        }

        [[nodiscard]]
        auto identityFromCandidate(TargetCandidate const& candidate) -> TargetIdentity
        {
            return TargetIdentity{
                candidate.handle(),
                candidate.process(),
                candidate.processStartTime(),
                candidate.clientSize()
            };
        }
    }
}

namespace uf
{
    auto TargetSelector::withProcess(ProcessId process) const -> TargetSelector
    {
        auto selector      = *this;
        selector.m_process = process;
        return selector;
    }

    auto TargetSelector::withWindowHandle(WindowHandle windowHandle) const -> TargetSelector
    {
        auto selector           = *this;
        selector.m_windowHandle = windowHandle;
        return selector;
    }

    auto TargetSelector::withWindowClass(std::string windowClass) const -> TargetSelector
    {
        auto selector          = *this;
        selector.m_windowClass = std::move(windowClass);
        return selector;
    }

    auto TargetSelector::withTitle(std::string title) const -> TargetSelector
    {
        auto selector    = *this;
        selector.m_title = std::move(title);
        return selector;
    }

    auto TargetSelector::process() const noexcept -> std::optional<ProcessId>
    {
        return m_process;
    }
    auto TargetSelector::windowHandle() const noexcept -> std::optional<WindowHandle>
    {
        return m_windowHandle;
    }
    auto TargetSelector::windowClass() const noexcept -> std::optional<std::string> const&
    {
        return m_windowClass;
    }
    auto TargetSelector::title() const noexcept -> std::optional<std::string> const&
    {
        return m_title;
    }

    auto TargetSelector::matches(TargetCandidate const& candidate) const noexcept -> bool
    {
        if (m_process && candidate.process() != *m_process)
        {
            return false;
        }
        if (m_windowHandle && candidate.handle() != *m_windowHandle)
        {
            return false;
        }
        if (m_windowClass && candidate.windowClass() != *m_windowClass)
        {
            return false;
        }
        if (m_title && candidate.title() != *m_title)
        {
            return false;
        }
        return true;
    }

    auto matchingCandidates(
        std::span<TargetCandidate const> candidates,
        TargetSelector const& selector
    ) -> std::vector<TargetCandidate>
    {
        auto matches = std::vector<TargetCandidate>{};
        for (auto const& candidate : candidates)
        {
            if (selector.matches(candidate))
            {
                matches.emplace_back(candidate);
            }
        }
        return matches;
    }

    auto errorOnLost(RevalidateOutcome outcome) -> Result<RevalidateOutcome>
    {
        if (outcome == RevalidateOutcome::Lost)
        {
            return fail(
                AutomationErrorKind::ControllerDisconnected,
                "target window or process is gone"
            );
        }
        return outcome;
    }

    ResolvedTarget::ResolvedTarget(TargetIdentity identity) noexcept
        : m_identity{identity}
        , m_generation{}
        , m_continuity{Continuity::Confirmed}
    {
    }

    auto ResolvedTarget::windowHandle() const noexcept -> WindowHandle
    {
        return m_identity.handle();
    }
    auto ResolvedTarget::clientSize() const noexcept -> ClientSize
    {
        return m_identity.clientSize();
    }
    auto ResolvedTarget::currentGeneration() const noexcept -> TargetGeneration
    {
        return m_generation;
    }
    auto ResolvedTarget::identity() const noexcept -> TargetIdentity const&
    {
        return m_identity;
    }
    auto ResolvedTarget::requiresReresolution() const noexcept -> bool
    {
        return m_continuity != Continuity::Confirmed;
    }

    auto ResolvedTarget::reResolve(
        std::span<TargetCandidate const> candidates,
        TargetSelector const& selector
    ) -> Status
    {
        UF_TRY_VALUE(candidate, resolveCandidate(candidates, selector));
        if (m_continuity == Continuity::Confirmed)
        {
            UF_TRY_VALUE(nextGeneration, m_generation.next());
            m_generation = nextGeneration;
        }

        m_identity   = identityFromCandidate(candidate);
        m_continuity = Continuity::Confirmed;
        return ok();
    }

    auto ResolvedTarget::applyRevalidation(
        std::optional<TargetIdentity> observed
    ) -> Result<RevalidateOutcome>
    {
        if (m_continuity == Continuity::Lost)
        {
            return RevalidateOutcome::Lost;
        }
        if (m_continuity == Continuity::InstanceUnconfirmed)
        {
            return RevalidateOutcome::InstanceUnconfirmed;
        }
        if (!observed)
        {
            UF_TRY(invalidate(Continuity::Lost));
            return RevalidateOutcome::Lost;
        }

        auto const instanceMatch = controller_detail::compareProcessInstance(
            m_identity.process(),
            m_identity.processStartTime(),
            observed->process(),
            observed->processStartTime()
        );
        switch (instanceMatch)
        {
        case controller_detail::ProcessInstanceMatch::Unconfirmed:
            UF_TRY(invalidate(Continuity::InstanceUnconfirmed));
            return RevalidateOutcome::InstanceUnconfirmed;
        case controller_detail::ProcessInstanceMatch::Different:
        {
            UF_TRY_VALUE(nextGeneration, m_generation.next());
            m_generation = nextGeneration;
            m_identity   = *observed;
            return RevalidateOutcome::GenerationBumped;
        }
        case controller_detail::ProcessInstanceMatch::Same:
            if (
                m_identity.handle() == observed->handle()
                && m_identity.clientSize() == observed->clientSize()
            )
            {
                return RevalidateOutcome::Unchanged;
            }

            UF_TRY_VALUE(nextGeneration, m_generation.next());
            m_generation = nextGeneration;
            m_identity   = *observed;
            return RevalidateOutcome::GenerationBumped;
        }

        UF_UNREACHABLE_MSG("Unknown ProcessInstanceMatch value");
    }

    auto ResolvedTarget::invalidate(Continuity continuity) -> Status
    {
        if (m_continuity == Continuity::Confirmed)
        {
            UF_TRY_VALUE(nextGeneration, m_generation.next());
            m_generation = nextGeneration;
            m_continuity = continuity;
        }
        return ok();
    }

    auto ResolvedTarget::readLiveIdentity() const -> Result<std::optional<TargetIdentity>>
    {
        auto const handle = m_identity.handle();
        if (!controller_platform::windowIsAlive(handle))
        {
            return std::optional<TargetIdentity>{};
        }

        auto process = controller_platform::windowProcess(handle);
        if (!process)
        {
            if (!controller_platform::windowIsAlive(handle))
            {
                return std::optional<TargetIdentity>{};
            }
            return std::unexpected{std::move(process).error()};
        }

        auto startTime = controller_platform::processStartTime(*process);
        if (!startTime)
        {
            if (!controller_platform::windowIsAlive(handle))
            {
                return std::optional<TargetIdentity>{};
            }
            return std::unexpected{std::move(startTime).error()};
        }

        auto clientSize = controller_platform::windowClientSize(handle);
        if (!clientSize)
        {
            if (!controller_platform::windowIsAlive(handle))
            {
                return std::optional<TargetIdentity>{};
            }
            return std::unexpected{std::move(clientSize).error()};
        }

        return std::optional<TargetIdentity>{
            std::in_place,
            handle,
            *process,
            *startTime,
            *clientSize
        };
    }

    auto ResolvedTarget::revalidate() -> Result<RevalidateOutcome>
    {
        UF_TRY_VALUE(observed, readLiveIdentity());
        return applyRevalidation(observed);
    }

    auto resolveTarget(
        std::span<TargetCandidate const> candidates,
        TargetSelector const& selector
    ) -> Result<ResolvedTarget>
    {
        UF_TRY_VALUE(candidate, resolveCandidate(candidates, selector));
        return ResolvedTarget{identityFromCandidate(candidate)};
    }
}
