#pragma once

#include <operator-contract/host-delivery-fixture.hpp>

#include <operator/effective-plan.hpp>

#include <core/error/result.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::contract
{
    // Readers for the operator protocol documents a ProjectPlugin returns:
    // OP:`PlanProposal`, OP:`UIActionIntent` and OP:`WaitIntent`.
    //
    // They belong to the suite rather than to a project provider because the
    // operator protocol is the Operator's own schema and is the same for every
    // project. What a provider supplies is the documents; what the Operator
    // supplies is the reading of them. A consumer therefore writes a
    // ProjectVocabulary and never a JSON reader.
    //
    // This is trusted deployment code: it never runs inside a business VM and
    // is never handed to plugin code. It refuses anything that is not the exact
    // definition -- unknown member, missing member, wrong shape -- and anything
    // that is not canonical: JCS carries no whitespace and orders members by
    // UTF-16 code unit, so both are checked rather than assumed.

    // One member of a JSON object, as exact slices of the document.
    struct ProtocolMember final
    {
        std::string_view name{};
        std::string_view value{};
    };

    // The index just past the complete JSON value starting at `at`, or npos if
    // the text there is not one. No whitespace is skipped anywhere: JCS has
    // none, so a document carrying any is not canonical and is refused.
    [[nodiscard]]
    inline auto protocolValueEnd(
        std::string_view text,
        std::size_t at
    ) -> std::size_t
    {
        if (at >= text.size())
        {
            return std::string_view::npos;
        }
        auto const opening = text[at];
        if (opening == '"')
        {
            auto index   = at + 1U;
            auto escaped = false;
            while (index < text.size())
            {
                auto const character = text[index];
                ++index;
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (character == '"')
                {
                    return index;
                }
            }
            return std::string_view::npos;
        }
        if (opening == '{' || opening == '[')
        {
            auto const closing = opening == '{' ? '}' : ']';
            auto index         = at + 1U;
            if (index < text.size() && text[index] == closing)
            {
                return index + 1U;
            }
            while (true)
            {
                if (opening == '{')
                {
                    if (index >= text.size() || text[index] != '"')
                    {
                        return std::string_view::npos;
                    }
                    auto const nameEnd = protocolValueEnd(text, index);
                    if (
                        nameEnd == std::string_view::npos
                        || nameEnd >= text.size()
                        || text[nameEnd] != ':'
                    )
                    {
                        return std::string_view::npos;
                    }
                    index = nameEnd + 1U;
                }
                auto const valueEnd = protocolValueEnd(text, index);
                if (valueEnd == std::string_view::npos || valueEnd >= text.size())
                {
                    return std::string_view::npos;
                }
                index = valueEnd;
                if (text[index] == ',')
                {
                    ++index;
                    continue;
                }
                if (text[index] == closing)
                {
                    return index + 1U;
                }
                return std::string_view::npos;
            }
        }

        constexpr auto literals = std::array{
            std::string_view{"true"},
            std::string_view{"false"},
            std::string_view{"null"},
        };
        for (auto const literal : literals)
        {
            if (text.substr(at).starts_with(literal))
            {
                return at + literal.size();
            }
        }

        auto index = at;
        if (index < text.size() && text[index] == '-')
        {
            ++index;
        }
        auto const digitsAt = index;
        while (
            index < text.size()
            && text[index] >= '0'
            && text[index] <= '9'
        )
        {
            ++index;
        }
        if (index == digitsAt)
        {
            return std::string_view::npos;
        }
        // A leading zero is not canonical, and neither is a fraction or an
        // exponent on a value the protocol types as an integer. Every number
        // this reader accepts is an integer, so both are refused here rather
        // than after conversion.
        if (
            index - digitsAt > 1U
            && text[digitsAt] == '0'
        )
        {
            return std::string_view::npos;
        }
        if (index < text.size() && (text[index] == '.' || text[index] == 'e'))
        {
            return std::string_view::npos;
        }
        return index;
    }

    // The members of a canonical JSON object, or nothing when the text is not
    // one, carries a trailing byte, or is not sorted the way JCS requires.
    [[nodiscard]]
    inline auto protocolObjectMembers(
        std::string_view text
    ) -> std::optional<std::vector<ProtocolMember>>
    {
        if (text.size() < 2U || text.front() != '{' || text.back() != '}')
        {
            return std::nullopt;
        }
        if (protocolValueEnd(text, 0U) != text.size())
        {
            return std::nullopt;
        }
        auto members = std::vector<ProtocolMember>{};
        auto index   = std::size_t{1};
        while (index + 1U < text.size())
        {
            auto const nameEnd = protocolValueEnd(text, index);
            auto const name    = text.substr(index + 1U, nameEnd - index - 2U);
            auto const valueAt = nameEnd + 1U;
            auto const end     = protocolValueEnd(text, valueAt);
            members.push_back(ProtocolMember{
                .name  = name,
                .value = text.substr(valueAt, end - valueAt),
            });
            index = end;
            if (text[index] == ',')
            {
                ++index;
                continue;
            }
            break;
        }
        for (auto at = std::size_t{1}; at < members.size(); ++at)
        {
            if (!jsonMemberNameLess(members[at - 1U].name, members[at].name))
            {
                return std::nullopt;
            }
        }
        return members;
    }

    [[nodiscard]]
    inline auto protocolArrayElements(
        std::string_view text
    ) -> std::optional<std::vector<std::string_view>>
    {
        if (text.size() < 2U || text.front() != '[' || text.back() != ']')
        {
            return std::nullopt;
        }
        if (protocolValueEnd(text, 0U) != text.size())
        {
            return std::nullopt;
        }
        auto elements = std::vector<std::string_view>{};
        auto index    = std::size_t{1};
        while (index + 1U < text.size())
        {
            auto const end = protocolValueEnd(text, index);
            elements.push_back(text.substr(index, end - index));
            index = end;
            if (text[index] == ',')
            {
                ++index;
                continue;
            }
            break;
        }
        return elements;
    }

    // The value of a JSON string token. Only the escapes RFC 8785 emits are
    // accepted: a document carrying any other escape is not canonical, whatever
    // it would decode to.
    [[nodiscard]]
    inline auto protocolString(
        std::string_view token
    ) -> std::optional<std::string>
    {
        if (token.size() < 2U || token.front() != '"' || token.back() != '"')
        {
            return std::nullopt;
        }
        auto const body = token.substr(1U, token.size() - 2U);
        auto value      = std::string{};
        for (auto at = std::size_t{0}; at < body.size(); ++at)
        {
            auto const character = body[at];
            if (character != '\\')
            {
                if (static_cast<unsigned char>(character) < 0x20U)
                {
                    return std::nullopt;
                }
                value.push_back(character);
                continue;
            }
            ++at;
            if (at >= body.size())
            {
                return std::nullopt;
            }
            struct ShortEscape final
            {
                char spelling{};
                char decoded{};
            };
            constexpr auto shortEscapes = std::array{
                ShortEscape{.spelling = '"', .decoded = '"'},
                ShortEscape{.spelling = '\\', .decoded = '\\'},
                ShortEscape{.spelling = 'b', .decoded = '\b'},
                ShortEscape{.spelling = 'f', .decoded = '\f'},
                ShortEscape{.spelling = 'n', .decoded = '\n'},
                ShortEscape{.spelling = 'r', .decoded = '\r'},
                ShortEscape{.spelling = 't', .decoded = '\t'},
            };
            auto decoded = std::optional<char>{};
            for (auto const escape : shortEscapes)
            {
                if (escape.spelling == body[at])
                {
                    decoded = escape.decoded;
                }
            }
            if (!decoded.has_value())
            {
                return std::nullopt;
            }
            value.push_back(*decoded);
        }
        if (!isValidUtf8(value))
        {
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]]
    inline auto protocolUnsigned(std::string_view token) -> std::optional<uint64>
    {
        if (token.empty())
        {
            return std::nullopt;
        }
        auto value = uint64{0};
        for (auto const character : token)
        {
            if (character < '0' || character > '9')
            {
                return std::nullopt;
            }
            constexpr auto ceiling = uint64{1} << 40U;
            if (value > ceiling)
            {
                return std::nullopt;
            }
            value = value * 10U + static_cast<uint64>(character - '0');
        }
        return value;
    }

    // Whether the object carries exactly these member names. Both directions
    // matter: a missing member is an incomplete document and an extra one is
    // additionalProperties, which every definition in the operator protocol
    // sets to false.
    [[nodiscard]]
    inline auto protocolMembersAre(
        std::span<ProtocolMember const> members,
        std::span<std::string_view const> names
    ) -> bool
    {
        if (members.size() != names.size())
        {
            return false;
        }
        for (auto const name : names)
        {
            auto found = false;
            for (auto const& member : members)
            {
                if (member.name == name)
                {
                    found = true;
                }
            }
            if (!found)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    inline auto protocolMember(
        std::span<ProtocolMember const> members,
        std::string_view name
    ) -> std::string_view
    {
        for (auto const& member : members)
        {
            if (member.name == name)
            {
                return member.value;
            }
        }
        return {};
    }

    [[nodiscard]]
    inline auto protocolRisk(std::string_view token) -> std::optional<Risk>
    {
        constexpr auto risks = std::array{
            Risk::ReadOnly,
            Risk::Low,
            Risk::Medium,
            Risk::High,
            Risk::Critical,
        };
        auto const decoded = protocolString(token);
        if (!decoded.has_value())
        {
            return std::nullopt;
        }
        for (auto const risk : risks)
        {
            if (riskWireName(risk) == *decoded)
            {
                return risk;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]]
    inline auto protocolRefusal(
        std::string_view detail
    ) -> std::unexpected<Error>
    {
        return fail(
            AutomationErrorKind::InvalidResource,
            std::string{"operator protocol document is not exact "}
                + std::string{detail}
        );
    }

    // OP:`PlanProposal`, read for the six members the Operator acts on.
    [[nodiscard]]
    inline auto readPlanProposal(
        std::string_view exactProposalJcs
    ) -> Result<PlanProposalClaims>
    {
        constexpr auto proposalNames = std::array{
            std::string_view{"allowed_ui_actions"},
            std::string_view{"canonical_args"},
            std::string_view{"effects"},
            std::string_view{"tool_name"},
            std::string_view{"tool_version"},
            std::string_view{"workflow_limits"},
        };
        constexpr auto effectNames = std::array{
            std::string_view{"namespaced_type"},
            std::string_view{"opaque_project_payload"},
            std::string_view{"payload_schema_hash"},
            std::string_view{"risk"},
            std::string_view{"scope_key"},
            std::string_view{"scope_kind"},
        };
        constexpr auto limitNames = std::array{
            std::string_view{"maximum_dispatches"},
            std::string_view{"maximum_elapsed_ms"},
            std::string_view{"maximum_observations"},
            std::string_view{"maximum_steps"},
            std::string_view{"maximum_waits"},
        };

        auto const members = protocolObjectMembers(exactProposalJcs);
        if (!members.has_value() || !protocolMembersAre(*members, proposalNames))
        {
            return protocolRefusal("PlanProposal");
        }
        auto claims = PlanProposalClaims{};
        auto const toolName = protocolString(protocolMember(*members, "tool_name"));
        auto const toolVersion =
            protocolString(protocolMember(*members, "tool_version"));
        if (!toolName.has_value() || !toolVersion.has_value())
        {
            return protocolRefusal("PlanProposal tool identity");
        }
        claims.toolName      = *toolName;
        claims.toolVersion   = *toolVersion;
        claims.canonicalArgs = protocolMember(*members, "canonical_args");

        auto const actions =
            protocolArrayElements(protocolMember(*members, "allowed_ui_actions"));
        if (!actions.has_value())
        {
            return protocolRefusal("PlanProposal allowed_ui_actions");
        }
        for (auto const action : *actions)
        {
            auto const decoded = protocolString(action);
            if (!decoded.has_value())
            {
                return protocolRefusal("PlanProposal allowed_ui_actions entry");
            }
            claims.allowedUiActions.push_back(*decoded);
        }

        auto const effects = protocolArrayElements(protocolMember(*members, "effects"));
        if (!effects.has_value())
        {
            return protocolRefusal("PlanProposal effects");
        }
        for (auto const effect : *effects)
        {
            auto const effectMembers = protocolObjectMembers(effect);
            if (
                !effectMembers.has_value()
                || !protocolMembersAre(*effectMembers, effectNames)
            )
            {
                return protocolRefusal("ExpectedEffect");
            }
            auto const namespacedType =
                protocolString(protocolMember(*effectMembers, "namespaced_type"));
            auto const scopeKind =
                protocolString(protocolMember(*effectMembers, "scope_kind"));
            auto const scopeKey =
                protocolString(protocolMember(*effectMembers, "scope_key"));
            auto const payloadSchemaHex =
                protocolString(protocolMember(*effectMembers, "payload_schema_hash"));
            auto const risk = protocolRisk(protocolMember(*effectMembers, "risk"));
            if (
                !namespacedType.has_value()
                || !scopeKind.has_value()
                || !scopeKey.has_value()
                || !payloadSchemaHex.has_value()
                || !risk.has_value()
            )
            {
                return protocolRefusal("ExpectedEffect member");
            }
            // OP:`Hash` is bare lowercase hex; ContentHash spells its own
            // canonical form with the algorithm in front.
            UF_TRY_VALUE(
                payloadSchemaHash,
                ContentHash::parse("sha256:" + *payloadSchemaHex)
            );
            claims.effects.push_back(ProposedEffect{
                .namespacedType    = *namespacedType,
                .risk              = *risk,
                .scopeKind         = *scopeKind,
                .scopeKey          = *scopeKey,
                .payloadSchemaHash = payloadSchemaHash,
                .opaqueProjectPayload = std::string{
                    protocolMember(*effectMembers, "opaque_project_payload"),
                },
            });
        }

        auto const limitMembers =
            protocolObjectMembers(protocolMember(*members, "workflow_limits"));
        if (
            !limitMembers.has_value()
            || !protocolMembersAre(*limitMembers, limitNames)
        )
        {
            return protocolRefusal("WorkflowLimits");
        }
        auto const steps = protocolUnsigned(protocolMember(*limitMembers, "maximum_steps"));
        auto const dispatches =
            protocolUnsigned(protocolMember(*limitMembers, "maximum_dispatches"));
        auto const observations =
            protocolUnsigned(protocolMember(*limitMembers, "maximum_observations"));
        auto const waits = protocolUnsigned(protocolMember(*limitMembers, "maximum_waits"));
        auto const elapsed =
            protocolUnsigned(protocolMember(*limitMembers, "maximum_elapsed_ms"));
        if (
            !steps.has_value()
            || !dispatches.has_value()
            || !observations.has_value()
            || !waits.has_value()
            || !elapsed.has_value()
        )
        {
            return protocolRefusal("WorkflowLimits member");
        }
        claims.limits = WorkflowLimits{
            .maximumSteps         = static_cast<uint32>(*steps),
            .maximumDispatches    = static_cast<uint32>(*dispatches),
            .maximumObservations  = static_cast<uint32>(*observations),
            .maximumWaits         = static_cast<uint32>(*waits),
            .maximumElapsedMillis = *elapsed,
        };
        return claims;
    }

    // OP:`UIActionIntent` or OP:`WaitIntent`. The two are told apart by their
    // complete member sets rather than by a discriminator field, because the
    // schema gives them none: an intent that satisfies neither set is refused
    // rather than read as the more permissive of the two.
    [[nodiscard]]
    inline auto readStepIntent(
        std::string_view exactStepJcs
    ) -> Result<StepIntentClaims>
    {
        constexpr auto uiActionNames = std::array{
            std::string_view{"action"},
            std::string_view{"binding_variant_constraints"},
            std::string_view{"delivery_class"},
            std::string_view{"expected_ui_postconditions"},
            std::string_view{"required_ui_preconditions"},
            std::string_view{"step_key"},
            std::string_view{"timeout_policy"},
        };
        constexpr auto waitNames = std::array{
            std::string_view{"condition"},
            std::string_view{"observation_budget"},
            std::string_view{"step_key"},
            std::string_view{"timeout_policy"},
        };

        auto const members = protocolObjectMembers(exactStepJcs);
        if (!members.has_value())
        {
            return protocolRefusal("step intent");
        }
        auto const kind = protocolMembersAre(*members, uiActionNames)
            ? std::optional{StepKind::UiAction}
            : (
                protocolMembersAre(*members, waitNames)
                    ? std::optional{StepKind::Wait}
                    : std::nullopt
            );
        if (!kind.has_value())
        {
            return protocolRefusal("UIActionIntent or WaitIntent");
        }
        auto const stepKey = protocolString(protocolMember(*members, "step_key"));
        if (!stepKey.has_value())
        {
            return protocolRefusal("step intent step_key");
        }
        return StepIntentClaims{.stepKey = *stepKey, .kind = *kind};
    }

    // The UI one OP:`UIActionIntent` acts on. StepIntentClaims does not carry
    // it and neither does anything downstream: the ledger stores the intent
    // bytes and hashes them, and the task::DispatchAuthority the Host is handed
    // carries no UI identifier at all. A plan naming a surface, target or action
    // that exists in no RuntimeModel therefore reaches delivery unremarked --
    // and did, in both in-tree fixtures, until this reader was written.
    [[nodiscard]]
    inline auto readStepIntentUi(
        std::string_view exactStepJcs
    ) -> std::optional<task::UiActionUnderTest>
    {
        auto const members = protocolObjectMembers(exactStepJcs);
        if (!members.has_value())
        {
            return std::nullopt;
        }
        auto const action = protocolObjectMembers(
            protocolMember(*members, "action")
        );
        if (!action.has_value())
        {
            return std::nullopt;
        }
        auto surface  = protocolString(protocolMember(*action, "surface_id"));
        auto uiTarget = protocolString(protocolMember(*action, "ui_target_id"));
        auto actionId = protocolString(protocolMember(*action, "action_id"));
        if (!surface.has_value() || !uiTarget.has_value() || !actionId.has_value())
        {
            return std::nullopt;
        }
        return task::UiActionUnderTest{
            .surface  = *std::move(surface),
            .uiTarget = *std::move(uiTarget),
            .action   = *std::move(actionId),
        };
    }

    // The plan authority a deployment builds. The exact operator protocol bytes
    // are the ones the session manifest is pinned to, so an authority that
    // answers for another schema cannot be created at all.
    //
    // Its step-intent reader also refuses a UI-action step naming anything but
    // `uiAction`. A contract run drives exactly one UI action -- the one the
    // project's ProjectVocabulary names -- so a plan that named another would be
    // telling the suite two different things about what this Operation does.
    // The refusal is the suite's, not the Operator's: nothing in the Operator
    // reads these three members, so production still accepts a plan against UI
    // no model defines.
    [[nodiscard]]
    inline auto planAuthority(
        VerifiedProjectRegistration const& registration,
        SessionManifest const& manifest,
        std::string_view exactOperatorProtocolSchemaBytes,
        task::UiActionUnderTest const& uiAction
    ) -> Result<OperatorPlanAuthority>
    {
        return OperatorPlanAuthority::create(
            registration,
            manifest,
            exactOperatorProtocolSchemaBytes,
            readPlanProposal,
            [uiAction](std::string_view exactStepJcs) -> Result<StepIntentClaims>
            {
                UF_TRY_VALUE(claims, readStepIntent(exactStepJcs));
                if (claims.kind != StepKind::UiAction)
                {
                    return claims;
                }
                auto const named = readStepIntentUi(exactStepJcs);
                if (
                    !named.has_value()
                    || named->surface != uiAction.surface
                    || named->uiTarget != uiAction.uiTarget
                    || named->action != uiAction.action
                )
                {
                    return protocolRefusal(
                        "UIActionIntent naming the project's own UI action"
                    );
                }
                return claims;
            }
        );
    }
}
