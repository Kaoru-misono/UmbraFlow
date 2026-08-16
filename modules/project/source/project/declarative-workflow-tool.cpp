#include "declarative-workflow-tool.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <json/error.hpp>
#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <algorithm>
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

        // A member the published schema has already declared present, so its
        // absence would be a defect in this reader rather than in the document.
        [[nodiscard]]
        auto member(
            json::Value const& object,
            std::string_view name
        ) -> json::Value const&
        {
            auto const* const p_value = object.find(name);
            UF_CHECK(p_value != nullptr);
            return *p_value;
        }

        constexpr auto k_workflowSchemaPath = std::string_view{
            "schema/umbraflow-declarative-workflow-tool-v1.schema.json"
        };

        // The published schema is the single authority over the declaration's
        // shape: closure, membership, types, patterns and value bounds all come
        // from it. Its refusal kinds map onto the lock's codes -- a closed
        // object carrying an undeclared member is ClosedSchema, every other
        // schema rejection is MalformedWorkflowTool. A schema problem (the
        // compiled catalog missing the document, a keyword this evaluator does
        // not implement) is a deployment defect no declaration can fix and is
        // reported without a code.
        [[nodiscard]]
        auto adoptSchema(Status outcome) -> Status
        {
            if (outcome.has_value())
            {
                return ok();
            }
            auto const kind = json::errorKind(outcome.error());
            if (kind == json::ErrorKind::DocumentClosureRejected)
            {
                return refuse("ClosedSchema", outcome.error().message());
            }
            if (kind == json::ErrorKind::DocumentRejected)
            {
                return refuse("MalformedWorkflowTool", outcome.error().message());
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::string{outcome.error().message()}
            );
        }

        [[nodiscard]]
        auto validateDeclaration(json::Value const& root) -> Status
        {
            auto const published = framework_schema::findFrameworkSchema(
                k_workflowSchemaPath
            );
            if (!published.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated framework schema catalog is missing "
                        + std::string{k_workflowSchemaPath}
                );
            }
            auto compiled = json::Schema::compile(json::Schema::Document{
                .label      = published->relativePath,
                .exactBytes = published->exactBytes,
            });
            if (!compiled.has_value())
            {
                // A schema the catalog holds that does not compile is the
                // same class of defect as one this evaluator refuses: a
                // deployment problem no declaration can fix, mapped like the
                // validate outcome below and reported without a lock code.
                return adoptSchema(std::unexpected<Error>{
                    std::move(compiled.error())
                });
            }
            return adoptSchema(compiled->validate(root));
        }

        // Readers, not validators: the published schema has already accepted
        // the whole document, so every member is present with the type and
        // range the schema declares. The kind dispatch below re-chooses the
        // oneOf branch the schema chose, and the check documents that the
        // dispatch is total rather than silently treating an unknown kind as a
        // ui_action state.
        [[nodiscard]]
        auto parseBounds(json::Value const& value) -> WorkflowBounds
        {
            return WorkflowBounds{
                .maximumStates = static_cast<uint32>(
                    member(value, "maximum_states").number()
                ),
                .maximumSteps = static_cast<uint32>(
                    member(value, "maximum_steps").number()
                ),
                .maximumDispatches = static_cast<uint32>(
                    member(value, "maximum_dispatches").number()
                ),
                .maximumObservations = static_cast<uint32>(
                    member(value, "maximum_observations").number()
                ),
                .maximumWaits = static_cast<uint32>(
                    member(value, "maximum_waits").number()
                ),
                .maximumElapsedMillis = static_cast<uint64>(
                    member(value, "maximum_elapsed_ms").number()
                ),
            };
        }

        [[nodiscard]]
        auto parseState(json::Value const& value) -> WorkflowState
        {
            auto const kind = member(value, "kind").string();
            if (kind == "wait")
            {
                return WorkflowState{
                    .stateKey = std::string{member(value, "state_key").string()},
                    .kind     = StateKind::Wait,
                    .observationBudget = static_cast<uint32>(
                        member(value, "observation_budget").number()
                    ),
                    .timeoutMillis = static_cast<uint64>(
                        member(value, "timeout_ms").number()
                    ),
                };
            }
            UF_CHECK(kind == "ui_action");
            return WorkflowState{
                .stateKey = std::string{member(value, "state_key").string()},
                .kind     = StateKind::UiAction,
                .uiAction = std::string{member(value, "ui_action").string()},
                .timeoutMillis = static_cast<uint64>(
                    member(value, "timeout_ms").number()
                ),
            };
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
            UF_TRY(validateDeclaration(root));

            // The published schema just accepted the document, so every member
            // read below is present with the type and range the schema
            // declares. Only relationships no JSON Schema can state remain
            // checked by hand: state_key uniqueness across the states array,
            // and the schedule naming declared states.
            auto const& fresh   = member(root, "fresh_observation");
            auto const& finding = member(root, "ui_finding");
            auto const& kinds   = member(root, "allowed_instance_kinds");
            auto const& states  = member(root, "states");
            auto const& steps   = member(root, "steps");
            auto const& bounds  = member(root, "bounds");

            auto const toolName       = member(root, "tool_name").string();
            auto const targetArgument = member(root, "target_argument").string();
            auto const requiredSurface = member(fresh, "required_surface").string();
            auto const findingKind     = member(finding, "kind").string();

            auto allowedKinds = std::vector<std::string>{};
            for (auto const& kind : kinds.items())
            {
                allowedKinds.emplace_back(kind.string());
            }
            // The schema's uniqueItems makes the set unique; sorting makes the
            // rendered adapter deterministic.
            std::ranges::sort(allowedKinds);

            auto stateByKey = std::map<std::string, WorkflowState>{};
            for (auto const& stateValue : states.items())
            {
                auto state = parseState(stateValue);
                if (!stateByKey.emplace(state.stateKey, std::move(state)).second)
                {
                    return refuse("MalformedWorkflowTool", "state_key values must be unique");
                }
            }

            auto scheduled = std::vector<WorkflowState>{};
            for (auto const& step : steps.items())
            {
                if (!stateByKey.contains(std::string{step.string()}))
                {
                    return refuse("MalformedWorkflowTool", "step names no declared state");
                }
                scheduled.emplace_back(stateByKey.at(std::string{step.string()}));
            }
            auto const parsedBounds = parseBounds(bounds);

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
