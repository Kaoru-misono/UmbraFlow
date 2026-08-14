#include "declarative-workflow-tool.hpp"

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        constexpr auto k_schemaId = std::string_view{
            "umbraflow-declarative-workflow-tool/v1"
        };
        constexpr auto k_rootMembers = std::array{
            std::string_view{"schema"},
            std::string_view{"tool_name"},
            std::string_view{"target_argument"},
            std::string_view{"allowed_instance_kinds"},
            std::string_view{"fresh_observation"},
            std::string_view{"ui_finding"},
            std::string_view{"states"},
            std::string_view{"steps"},
            std::string_view{"bounds"},
        };
        constexpr auto k_freshObservationMembers = std::array{
            std::string_view{"required_surface"},
            std::string_view{"require_unambiguous"},
        };
        constexpr auto k_uiFindingMembers = std::array{
            std::string_view{"kind"},
        };
        constexpr auto k_boundsMembers = std::array{
            std::string_view{"maximum_states"},
            std::string_view{"maximum_steps"},
            std::string_view{"maximum_dispatches"},
            std::string_view{"maximum_observations"},
            std::string_view{"maximum_waits"},
            std::string_view{"maximum_elapsed_ms"},
        };
        constexpr auto k_waitStateMembers = std::array{
            std::string_view{"state_key"},
            std::string_view{"kind"},
            std::string_view{"observation_budget"},
            std::string_view{"timeout_ms"},
        };
        constexpr auto k_actionStateMembers = std::array{
            std::string_view{"state_key"},
            std::string_view{"kind"},
            std::string_view{"ui_action"},
            std::string_view{"timeout_ms"},
        };

        enum class StateKind : uint8
        {
            Wait,
            UiAction,
        };

        struct WorkflowState final
        {
            std::string stateKey{};
            StateKind   kind{StateKind::Wait};
            std::string uiAction{};
            uint32      observationBudget{};
            uint64      timeoutMillis{};
        };

        struct WorkflowBounds final
        {
            uint32 maximumStates{};
            uint32 maximumSteps{};
            uint32 maximumDispatches{};
            uint32 maximumObservations{};
            uint32 maximumWaits{};
            uint64 maximumElapsedMillis{};
        };

        struct WorkflowTool final
        {
            std::string                toolName{};
            std::string                targetArgument{};
            std::vector<std::string>   allowedInstanceKinds{};
            std::string                requiredSurface{};
            std::string                findingKind{};
            std::vector<WorkflowState> steps{};
            WorkflowBounds             bounds{};
        };

        [[nodiscard]]
        auto refuse(
            std::string_view code,
            std::string_view detail
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("{}: {}", code, detail)
            );
        }

        [[nodiscard]]
        auto isAsciiLetter(char value) noexcept -> bool
        {
            return (
                (value >= 'A' && value <= 'Z')
                || (value >= 'a' && value <= 'z')
            );
        }

        [[nodiscard]]
        auto isAsciiDigit(char value) noexcept -> bool
        {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]]
        auto isNamespacedIdentifier(std::string_view value) noexcept -> bool
        {
            if (value.empty() || !isAsciiLetter(value.front()))
            {
                return false;
            }
            auto sawSeparator   = false;
            auto atSegmentStart = false;
            for (auto const character : value)
            {
                if (character == '.')
                {
                    if (atSegmentStart)
                    {
                        return false;
                    }
                    sawSeparator   = true;
                    atSegmentStart = true;
                    continue;
                }
                if (
                    !isAsciiLetter(character)
                    && !isAsciiDigit(character)
                    && character != '_'
                    && character != '-'
                )
                {
                    return false;
                }
                if (
                    atSegmentStart
                    && !isAsciiLetter(character)
                    && !isAsciiDigit(character)
                )
                {
                    return false;
                }
                atSegmentStart = false;
            }
            return sawSeparator && !atSegmentStart;
        }

        [[nodiscard]]
        auto isIdentifier(std::string_view value) noexcept -> bool
        {
            return (
                !value.empty()
                && value.size() <= 128U
                && (isAsciiLetter(value.front()) || isAsciiDigit(value.front()))
                && std::ranges::all_of(
                    value.substr(1U),
                    [](char character)
                    {
                        return (
                            isAsciiLetter(character)
                            || isAsciiDigit(character)
                            || character == '.'
                            || character == '_'
                            || character == ':'
                            || character == '-'
                        );
                    }
                )
            );
        }

        [[nodiscard]]
        auto isArgumentName(std::string_view value) noexcept -> bool
        {
            return (
                !value.empty()
                && value.size() <= 64U
                && (isAsciiLetter(value.front()) || value.front() == '_')
                && std::ranges::all_of(
                    value.substr(1U),
                    [](char character)
                    {
                        return (
                            isAsciiLetter(character)
                            || isAsciiDigit(character)
                            || character == '_'
                        );
                    }
                )
            );
        }

        template <std::size_t Size>
        [[nodiscard]]
        auto hasExactMembers(
            json::Value const& value,
            std::array<std::string_view, Size> const& expected
        ) -> bool
        {
            if (value.kind() != json::ValueKind::Object || value.members().size() != Size)
            {
                return false;
            }
            return std::ranges::all_of(
                expected,
                [&value](std::string_view name)
                {
                    return value.find(name) != nullptr;
                }
            );
        }

        [[nodiscard]]
        auto stringMember(
            json::Value const& object,
            std::string_view name
        ) -> Result<std::string_view>
        {
            auto const* value = object.find(name);
            if (value == nullptr || value->kind() != json::ValueKind::String)
            {
                return refuse("MalformedWorkflowTool", "required string member is malformed");
            }
            return value->string();
        }

        [[nodiscard]]
        auto integerMember(
            json::Value const& object,
            std::string_view name,
            uint64 maximum
        ) -> Result<uint64>
        {
            auto const* value = object.find(name);
            if (
                value == nullptr
                || value->kind() != json::ValueKind::Number
                || !value->isInteger()
                || value->number() < 0.0
                || value->number() > static_cast<double>(maximum)
            )
            {
                return refuse("MalformedWorkflowTool", "required integer member is malformed");
            }
            return static_cast<uint64>(value->number());
        }

        [[nodiscard]]
        auto parseBounds(json::Value const& value) -> Result<WorkflowBounds>
        {
            if (!hasExactMembers(value, k_boundsMembers))
            {
                return refuse("ClosedSchema", "workflow bounds do not have their exact members");
            }
            UF_TRY_VALUE(states, integerMember(value, "maximum_states", 64U));
            UF_TRY_VALUE(steps, integerMember(value, "maximum_steps", 256U));
            UF_TRY_VALUE(dispatches, integerMember(value, "maximum_dispatches", 256U));
            UF_TRY_VALUE(observations, integerMember(value, "maximum_observations", 256U));
            UF_TRY_VALUE(waits, integerMember(value, "maximum_waits", 256U));
            UF_TRY_VALUE(elapsed, integerMember(value, "maximum_elapsed_ms", 15'360'000U));
            if (states == 0U || steps == 0U || observations == 0U || elapsed == 0U)
            {
                return refuse("MalformedWorkflowTool", "positive workflow bounds cannot be zero");
            }
            return WorkflowBounds{
                .maximumStates        = static_cast<uint32>(states),
                .maximumSteps         = static_cast<uint32>(steps),
                .maximumDispatches    = static_cast<uint32>(dispatches),
                .maximumObservations  = static_cast<uint32>(observations),
                .maximumWaits         = static_cast<uint32>(waits),
                .maximumElapsedMillis = elapsed,
            };
        }

        [[nodiscard]]
        auto parseState(json::Value const& value) -> Result<WorkflowState>
        {
            if (value.kind() != json::ValueKind::Object)
            {
                return refuse("MalformedWorkflowTool", "workflow state must be an object");
            }
            UF_TRY_VALUE(kind, stringMember(value, "kind"));
            UF_TRY_VALUE(stateKey, stringMember(value, "state_key"));
            if (!isIdentifier(stateKey))
            {
                return refuse("MalformedWorkflowTool", "state_key is not an Identifier");
            }
            if (kind == "wait")
            {
                if (!hasExactMembers(value, k_waitStateMembers))
                {
                    return refuse("ClosedSchema", "wait state does not have its exact members");
                }
                UF_TRY_VALUE(budget, integerMember(value, "observation_budget", 256U));
                UF_TRY_VALUE(timeout, integerMember(value, "timeout_ms", 60'000U));
                if (budget == 0U || timeout == 0U)
                {
                    return refuse("MalformedWorkflowTool", "wait state bounds cannot be zero");
                }
                return WorkflowState{
                    .stateKey          = std::string{stateKey},
                    .kind              = StateKind::Wait,
                    .observationBudget = static_cast<uint32>(budget),
                    .timeoutMillis     = timeout,
                };
            }
            if (kind == "ui_action")
            {
                if (!hasExactMembers(value, k_actionStateMembers))
                {
                    return refuse("ClosedSchema", "UI-action state does not have its exact members");
                }
                UF_TRY_VALUE(uiAction, stringMember(value, "ui_action"));
                UF_TRY_VALUE(timeout, integerMember(value, "timeout_ms", 60'000U));
                if (!isNamespacedIdentifier(uiAction) || timeout == 0U)
                {
                    return refuse("MalformedWorkflowTool", "UI-action state is malformed");
                }
                return WorkflowState{
                    .stateKey      = std::string{stateKey},
                    .kind          = StateKind::UiAction,
                    .uiAction      = std::string{uiAction},
                    .timeoutMillis = timeout,
                };
            }
            return refuse("MalformedWorkflowTool", "workflow state kind is unknown");
        }

        [[nodiscard]]
        auto parseWorkflowTool(std::string_view declarationBytes) -> Result<WorkflowTool>
        {
            auto parsed = json::parse(declarationBytes);
            if (!parsed.has_value())
            {
                return refuse("MalformedWorkflowTool", "declaration is not JSON");
            }
            auto const& root = *parsed;
            if (!hasExactMembers(root, k_rootMembers))
            {
                return refuse("ClosedSchema", "workflow declaration does not have its exact members");
            }
            UF_TRY_VALUE(schema, stringMember(root, "schema"));
            UF_TRY_VALUE(toolName, stringMember(root, "tool_name"));
            UF_TRY_VALUE(targetArgument, stringMember(root, "target_argument"));
            if (
                schema != k_schemaId
                || !isNamespacedIdentifier(toolName)
                || !isArgumentName(targetArgument)
            )
            {
                return refuse("MalformedWorkflowTool", "declaration identity is malformed");
            }

            auto const* fresh = root.find("fresh_observation");
            auto const* finding = root.find("ui_finding");
            auto const* kinds = root.find("allowed_instance_kinds");
            auto const* states = root.find("states");
            auto const* steps = root.find("steps");
            auto const* bounds = root.find("bounds");
            if (
                fresh == nullptr
                || finding == nullptr
                || kinds == nullptr
                || states == nullptr
                || steps == nullptr
                || bounds == nullptr
                || !hasExactMembers(*fresh, k_freshObservationMembers)
                || !hasExactMembers(*finding, k_uiFindingMembers)
                || kinds->kind() != json::ValueKind::Array
                || states->kind() != json::ValueKind::Array
                || steps->kind() != json::ValueKind::Array
            )
            {
                return refuse("MalformedWorkflowTool", "declaration shape is malformed");
            }
            UF_TRY_VALUE(requiredSurface, stringMember(*fresh, "required_surface"));
            UF_TRY_VALUE(findingKind, stringMember(*finding, "kind"));
            auto const* unambiguous = fresh->find("require_unambiguous");
            if (
                !isNamespacedIdentifier(requiredSurface)
                || unambiguous == nullptr
                || unambiguous->kind() != json::ValueKind::Boolean
                || !unambiguous->boolean()
                || (
                    findingKind != "observed_instance_absent"
                    && findingKind != "observed_instance_present"
                )
            )
            {
                return refuse("MalformedWorkflowTool", "observation policy is malformed");
            }

            auto allowedKinds = std::vector<std::string>{};
            for (auto const& kind : kinds->items())
            {
                if (
                    kind.kind() != json::ValueKind::String
                    || !isNamespacedIdentifier(kind.string())
                )
                {
                    return refuse("MalformedWorkflowTool", "instance kind is malformed");
                }
                allowedKinds.emplace_back(kind.string());
            }
            std::ranges::sort(allowedKinds);
            if (
                allowedKinds.empty()
                || std::ranges::adjacent_find(allowedKinds) != allowedKinds.end()
            )
            {
                return refuse("MalformedWorkflowTool", "instance kinds must be non-empty and unique");
            }

            auto stateByKey = std::map<std::string, WorkflowState>{};
            for (auto const& stateValue : states->items())
            {
                UF_TRY_VALUE(state, parseState(stateValue));
                if (!stateByKey.emplace(state.stateKey, std::move(state)).second)
                {
                    return refuse("MalformedWorkflowTool", "state_key values must be unique");
                }
            }
            if (stateByKey.empty() || steps->items().empty())
            {
                return refuse("MalformedWorkflowTool", "states and steps must be non-empty");
            }

            auto scheduled = std::vector<WorkflowState>{};
            for (auto const& step : steps->items())
            {
                if (step.kind() != json::ValueKind::String || !stateByKey.contains(std::string{step.string()}))
                {
                    return refuse("MalformedWorkflowTool", "step names no declared state");
                }
                scheduled.emplace_back(stateByKey.at(std::string{step.string()}));
            }
            UF_TRY_VALUE(parsedBounds, parseBounds(*bounds));

            auto const dispatches = std::ranges::count(
                scheduled,
                StateKind::UiAction,
                &WorkflowState::kind
            );
            auto const waits = std::ranges::count(
                scheduled,
                StateKind::Wait,
                &WorkflowState::kind
            );
            auto elapsed = uint64{};
            for (auto const& state : scheduled)
            {
                elapsed += state.timeoutMillis;
            }
            if (stateByKey.size() > parsedBounds.maximumStates)
            {
                return refuse("WorkflowStateBound", "workflow exceeds maximum_states");
            }
            if (scheduled.size() > parsedBounds.maximumSteps)
            {
                return refuse("WorkflowStepBound", "workflow exceeds maximum_steps");
            }
            if (dispatches > parsedBounds.maximumDispatches)
            {
                return refuse("WorkflowDispatchBound", "workflow exceeds maximum_dispatches");
            }
            if (scheduled.size() > parsedBounds.maximumObservations)
            {
                return refuse("WorkflowObservationBound", "workflow exceeds maximum_observations");
            }
            if (waits > parsedBounds.maximumWaits)
            {
                return refuse("WorkflowWaitBound", "workflow exceeds maximum_waits");
            }
            if (elapsed > parsedBounds.maximumElapsedMillis)
            {
                return refuse("WorkflowElapsedBound", "workflow exceeds maximum_elapsed_ms");
            }
            return WorkflowTool{
                .toolName             = std::string{toolName},
                .targetArgument       = std::string{targetArgument},
                .allowedInstanceKinds = std::move(allowedKinds),
                .requiredSurface      = std::string{requiredSurface},
                .findingKind          = std::string{findingKind},
                .steps                = std::move(scheduled),
                .bounds               = parsedBounds,
            };
        }

        auto appendQuoted(std::string& output, std::string_view value) -> void
        {
            output.push_back('"');
            output += value;
            output.push_back('"');
        }

        [[nodiscard]]
        auto renderAdapter(
            std::string_view pluginId,
            WorkflowTool const& tool
        ) -> std::string
        {
            auto source = std::string{"local plugin_id = "};
            appendQuoted(source, pluginId);
            source += "\nlocal tool_name = ";
            appendQuoted(source, tool.toolName);
            source += "\nlocal target_argument = ";
            appendQuoted(source, tool.targetArgument);
            source += "\nlocal required_surface = ";
            appendQuoted(source, tool.requiredSurface);
            source += "\nlocal finding_kind = ";
            appendQuoted(source, tool.findingKind);
            source += "\nlocal allowed_instance_kinds = {\n";
            for (auto const& kind : tool.allowedInstanceKinds)
            {
                source += "    [";
                appendQuoted(source, kind);
                source += "] = true,\n";
            }
            source += "}\nlocal steps = {\n";
            auto allowedActions = std::set<std::string>{};
            for (auto const& state : tool.steps)
            {
                source += "    { kind = ";
                appendQuoted(
                    source,
                    state.kind == StateKind::Wait ? "wait" : "ui_action"
                );
                source += ", step_key = ";
                appendQuoted(source, state.stateKey);
                source += ", timeout_ms = ";
                source += std::to_string(state.timeoutMillis);
                if (state.kind == StateKind::Wait)
                {
                    source += ", observation_budget = ";
                    source += std::to_string(state.observationBudget);
                }
                else
                {
                    source += ", ui_action = ";
                    appendQuoted(source, state.uiAction);
                    allowedActions.emplace(state.uiAction);
                }
                source += " },\n";
            }
            source += "}\nlocal allowed_ui_actions = {";
            for (auto const& action : allowedActions)
            {
                source += " ";
                appendQuoted(source, action);
                source += ",";
            }
            source += " }\nlocal maximum_steps = ";
            source += std::to_string(tool.bounds.maximumSteps);
            source += "\nlocal maximum_dispatches = ";
            source += std::to_string(tool.bounds.maximumDispatches);
            source += "\nlocal maximum_observations = ";
            source += std::to_string(tool.bounds.maximumObservations);
            source += "\nlocal maximum_waits = ";
            source += std::to_string(tool.bounds.maximumWaits);
            source += "\nlocal maximum_elapsed_ms = ";
            source += std::to_string(tool.bounds.maximumElapsedMillis);
            source += R"luau(

local function target_of(input)
    if type(input.canonical_args) ~= "table" then
        error("WorkflowCanonicalArgsMissing: canonical_args must be an object")
    end
    local target = input.canonical_args[target_argument]
    if type(target) ~= "string" or target == "" then
        error("WorkflowTargetMissing: canonical args do not name the observed-instance target")
    end
    return target
end

local function require_observation(input)
    local observation = input.project_observation
    if type(observation) ~= "table" then
        error("MissingStepObservation: every workflow step requires a fresh observation")
    end
    if type(observation.canonical_opaque_payload) ~= "table"
        or type(observation.canonical_opaque_payload.surface_observations) ~= "table" then
        error("MissingStepObservation: fresh observation carries no Surface evidence")
    end
    for _, surface in ipairs(observation.canonical_opaque_payload.surface_observations) do
        if surface.surface_id == required_surface then
            if surface.fresh ~= true then
                error("StaleObservation: required Surface evidence is stale")
            end
            if surface.resolution ~= "resolved" then
                error("FreshSurfaceUnresolved: required Surface is not resolved")
            end
            if surface.unambiguous ~= true then
                error("FreshSurfaceAmbiguous: required Surface is ambiguous")
            end
            return observation
        end
    end
    error("MissingStepObservation: required Surface is absent")
end

local function find_target(observation, target)
    if type(observation.observed_instances) ~= "table" then
        error("MissingStepObservation: observed_instances must be an array")
    end
    for _, instance in ipairs(observation.observed_instances) do
        if instance.observed_instance_id == target then
            if allowed_instance_kinds[instance.kind] ~= true then
                error("WorkflowTargetKindRejected: target kind is outside allowed_instance_kinds")
            end
            return instance
        end
    end
    return nil
end

-- The step envelope the Operator assembles carries frozen_plan_hash, the
-- observation, the state and step_index, and its schema closes the object --
-- so a step has no canonical_args to read the target out of, and the id the
-- plan named is not carried forward. A UI action must aim at something the
-- world shows now, so the target is resolved from the fresh observation: the
-- one instance of an allowed kind it holds. Two is a refusal rather than a
-- choice, which is what require_unambiguous already asserts for the Surface.
local function resolve_target(observation)
    if type(observation.observed_instances) ~= "table" then
        error("MissingStepObservation: observed_instances must be an array")
    end
    local resolved = nil
    for _, instance in ipairs(observation.observed_instances) do
        if allowed_instance_kinds[instance.kind] == true then
            if resolved ~= nil then
                error("WorkflowTargetAmbiguous: the fresh observation holds more than one instance of an allowed kind")
            end
            resolved = instance
        end
    end
    if resolved == nil then
        error("ObservedInstanceStale: no instance of an allowed kind is present in the fresh observation")
    end
    if type(resolved.observed_instance_id) ~= "string" or resolved.observed_instance_id == "" then
        error("MissingStepObservation: an observed instance carries no identifier")
    end
    return resolved.observed_instance_id
end

local function finding_for(observation, target)
    local present = find_target(observation, target) ~= nil
    local matches = (finding_kind == "observed_instance_present" and present)
        or (finding_kind == "observed_instance_absent" and not present)
    if not matches then return {} end
    return {{ kind = finding_kind, observed_instance_id = target }}
end

return {
    plugin_id = plugin_id,
    derive = function(_input)
        return {
            schema = "umbraflow-project-observation-proposal/v1",
            canonical_opaque_payload = { surface_observations = {} },
            project_tool_preconditions = {},
            observed_instance_proposals = {},
        }
    end,
    plan = function(input)
        if input.tool_name ~= tool_name then
            error("WorkflowToolMismatch: plan input names another tool")
        end
        local target = target_of(input)
        local observation = require_observation(input)
        if find_target(observation, target) == nil then
            error("ObservedInstanceStale: target is absent from the fresh observation")
        end
        return {
            allowed_ui_actions = allowed_ui_actions,
            canonical_args = input.canonical_args,
            effects = {},
            tool_name = tool_name,
            tool_version = input.tool_version,
            workflow_limits = {
                maximum_dispatches = maximum_dispatches,
                maximum_elapsed_ms = maximum_elapsed_ms,
                maximum_observations = maximum_observations,
                maximum_steps = maximum_steps,
                maximum_waits = maximum_waits,
            },
        }
    end,
    next_step = function(input)
        local step = steps[input.step_index]
        if step == nil then
            error("WorkflowStepBound: step_index exceeds the finite schedule")
        end
        local observation = require_observation(input)
        if step.kind == "wait" then
            return {
                step_key = step.step_key,
                condition = {
                    kind = "fresh_unambiguous_surface",
                    required_surface = required_surface,
                },
                timeout_policy = {
                    maximum_elapsed_ms = step.timeout_ms,
                    on_timeout = "reconcile",
                },
                observation_budget = step.observation_budget,
            }
        end
        local target = resolve_target(observation)
        return {
            action = {
                action_id = step.ui_action,
                canonical_parameters = { [target_argument] = target },
                surface_id = required_surface,
                ui_target_id = target,
            },
            binding_variant_constraints = {},
            delivery_class = "delivery_safe",
            expected_ui_postconditions = {},
            required_ui_preconditions = {{
                kind = "fresh_unambiguous_surface",
                required_surface = required_surface,
            }},
            step_key = step.step_key,
            timeout_policy = {
                maximum_elapsed_ms = step.timeout_ms,
                on_timeout = "reconcile",
            },
        }
    end,
    reconcile = function(input)
        local target = target_of(input)
        local observation = require_observation(input)
        return {
            disposition = "continue",
            findings = finding_for(observation, target),
            journal_events = {},
            observed_outcomes = {},
        }
    end,
    reduce = function(_input)
        return canon.emptyObject
    end,
}
)luau";
            return source;
        }
    }

    auto generateDeclarativeWorkflowAdapter(
        std::string_view pluginId,
        std::string_view declarationBytes
    ) -> Result<std::string>
    {
        if (!isNamespacedIdentifier(pluginId))
        {
            return refuse(
                "MalformedWorkflowTool",
                "generated ProjectPlugin id must be namespaced"
            );
        }
        UF_TRY_VALUE(tool, parseWorkflowTool(declarationBytes));
        return renderAdapter(pluginId, tool);
    }
}
