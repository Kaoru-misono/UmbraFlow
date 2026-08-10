#include "controller.hpp"

#include <core/error/contracts.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace uf::operator_runtime
{
    auto controllerProfile(ControllerKind kind) -> ControllerProfile
    {
        auto const index = static_cast<std::size_t>(kind);
        UF_CHECK(index < k_controllerProfiles.size());
        auto const profile = k_controllerProfiles[index];
        // The table is indexed by the enumerator's own value, so a reordered or
        // extended enum must fail here rather than answer with another kind's
        // row. A profile is a ceiling; reading the wrong one silently widens it.
        UF_CHECK(profile.kind == kind);
        return profile;
    }

    auto controllerKindWireName(ControllerKind kind) noexcept -> std::string_view
    {
        switch (kind)
        {
        case ControllerKind::Script: return "script";
        case ControllerKind::Agent: return "agent";
        case ControllerKind::Human: return "human";
        }

        UF_UNREACHABLE_MSG("Unknown ControllerKind value");
    }

    auto parseControllerKind(std::string_view value) -> Result<ControllerKind>
    {
        constexpr auto kinds = std::array{
            ControllerKind::Script,
            ControllerKind::Agent,
            ControllerKind::Human,
        };
        auto const match = std::ranges::find_if(
            kinds,
            [value](ControllerKind candidate)
            {
                return controllerKindWireName(candidate) == value;
            }
        );
        if (match == kinds.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("Unknown controller kind: {}", value)
            );
        }
        return *match;
    }

    ControllerBinding::ControllerBinding(
        std::string sessionId,
        std::string controllerId,
        std::string controlledTargetKey,
        ContentHash capabilityProfileHash,
        uint64 sessionEpoch,
        ControllerKind kind
    )
        : m_sessionId{std::move(sessionId)}
        , m_controllerId{std::move(controllerId)}
        , m_controlledTargetKey{std::move(controlledTargetKey)}
        , m_capabilityProfileHash{capabilityProfileHash}
        , m_sessionEpoch{sessionEpoch}
        , m_kind{kind}
    {
    }

    auto ControllerBinding::sessionId() const noexcept -> std::string const&
    {
        return m_sessionId;
    }

    auto ControllerBinding::controllerId() const noexcept -> std::string const&
    {
        return m_controllerId;
    }

    auto ControllerBinding::controlledTargetKey() const noexcept
        -> std::string const&
    {
        return m_controlledTargetKey;
    }

    auto ControllerBinding::capabilityProfileHash() const -> ContentHash
    {
        return m_capabilityProfileHash;
    }

    auto ControllerBinding::sessionEpoch() const noexcept -> uint64
    {
        return m_sessionEpoch;
    }

    auto ControllerBinding::kind() const noexcept -> ControllerKind
    {
        return m_kind;
    }

    auto ControllerBinding::profile() const -> ControllerProfile
    {
        return controllerProfile(m_kind);
    }
}
