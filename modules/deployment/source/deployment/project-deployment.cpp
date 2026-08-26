#include "project-deployment.hpp"

#include <json/error.hpp>
#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::deployment
{
    namespace
    {
        // umbraflow-project.json's shape, by the path it is published under.
        // The offline project kit compiles the same bytes out of the same
        // catalog, which is the whole reason a Tool's shape is not written
        // here: a second, narrower reading in this module would accept
        // documents the kit refuses, or refuse ones it accepts.
        constexpr auto k_projectSchemaPath = std::string_view{
            "schema/umbraflow-project-v3.schema.json"
        };

        // The `$defs` of the published project schema that states one Tool
        // entry's shape. Nothing here restates that shape: this module compiles
        // the published bytes and asks them to judge each entry, so there is
        // one statement of a Tool and both readers of the document compile it.
        constexpr auto k_toolDefinition = std::string_view{"Tool"};

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto adopt(Status outcome, std::string_view what) -> Status
        {
            if (outcome.has_value())
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("{}: {}", what, outcome.error().message())
            );
        }

        [[nodiscard]]
        auto compile(std::string_view label, std::string_view exactBytes)
            -> Result<json::Schema>
        {
            auto compiled = json::Schema::compile(
                json::Schema::Document{.label = label, .exactBytes = exactBytes}
            );
            if (!compiled.has_value())
            {
                return refuse(std::format(
                    "{} is not a schema this deployment can apply: {}",
                    label,
                    compiled.error().message()
                ));
            }
            return *std::move(compiled);
        }

        [[nodiscard]]
        auto publishedProjectSchema() -> Result<json::Schema::Document>
        {
            auto const published = framework_schema::findFrameworkSchema(
                k_projectSchemaPath
            );
            if (!published.has_value())
            {
                return refuse(
                    "generated framework schema catalog is missing "
                    + std::string{k_projectSchemaPath}
                );
            }
            return json::Schema::Document{
                .label      = published->relativePath,
                .exactBytes = published->exactBytes,
            };
        }

        [[nodiscard]]
        auto parseDocument(std::string_view exactJcs) -> Result<json::Value>
        {
            auto parsed = json::parse(exactJcs);
            if (!parsed.has_value())
            {
                return refuse(std::format(
                    "document is not JSON: {}",
                    parsed.error().message()
                ));
            }
            return *std::move(parsed);
        }

        [[nodiscard]]
        auto hashOf(std::string_view bytes) -> Result<ContentHash>
        {
            return sha256(std::as_bytes(std::span{bytes}));
        }

        // Every observer below reads a document json::Schema has already
        // accepted, so the member is present and of the stated kind. The
        // contract check is what makes that assumption falsifiable rather than
        // undefined if a schema is ever loosened.
        [[nodiscard]]
        auto member(json::Value const& object UF_LIFETIME_BOUND, std::string_view name)
            -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        constexpr auto k_mutabilities = std::array{
            operator_runtime::ToolMutability::ReadOnly,
            operator_runtime::ToolMutability::Mutating,
        };

        constexpr auto k_surfaces = std::array{
            operator_runtime::ToolSurface::Semantic,
            operator_runtime::ToolSurface::Privileged,
        };

        // OP:`Risk`, spelled once. The wire names are riskWireName's and are
        // not restated here: the enumerators are the domain and the projection
        // is the mapping, so a name that drifted would drift in one place.
        constexpr auto k_risks = std::array{
            operator_runtime::Risk::ReadOnly,
            operator_runtime::Risk::Low,
            operator_runtime::Risk::Medium,
            operator_runtime::Risk::High,
            operator_runtime::Risk::Critical,
        };

        constexpr auto k_idempotencies = std::array{
            operator_runtime::ToolIdempotency::ReadSafe,
            operator_runtime::ToolIdempotency::DeliverySafe,
            operator_runtime::ToolIdempotency::KeyedExternal,
            operator_runtime::ToolIdempotency::NonIdempotent,
        };

        constexpr auto k_timeoutActions = std::array{
            operator_runtime::TimeoutAction::Reobserve,
            operator_runtime::TimeoutAction::Stop,
        };

        // One Tool as this deployment holds it: the name it is addressed by,
        // the compiled guard its own declaration asked for, and the descriptor
        // every bound is read from.
        //
        struct ToolEntry final
        {
            std::string name{};
            std::string description{};
            json::Value inputSchema{};

            // Null when this project did not write its result down. That is
            // a statement rather than a gap the framework fills in: a Project
            // Tool's result shape is optional because a dozen mandatory
            // documents before a project can run is a cost this repository
            // refuses to charge, while a Framework Tool always states one.
            json::Value                      outputSchema{};

            json::Schema                     argumentSchema;
            operator_runtime::ToolDescriptor descriptor{};
        };

        [[nodiscard]]
        auto names(json::Value const& array) -> std::vector<std::string>
        {
            auto values = std::vector<std::string>{};
            for (auto const& item : array.items())
            {
                values.emplace_back(item.string());
            }
            return values;
        }

        // OP:`TimeoutPolicy`, which both step intents and every Tool descriptor
        // carry. The schema has already bounded both members, so this reads
        // rather than judges.
        [[nodiscard]]
        auto readTimeoutPolicy(
            json::Value const& policy
        ) -> operator_runtime::TimeoutPolicy
        {
            auto const action = std::ranges::find(
                k_timeoutActions,
                member(policy, "on_timeout").string(),
                operator_runtime::timeoutActionWireName
            );
            UF_CHECK(action != k_timeoutActions.end());
            return operator_runtime::TimeoutPolicy{
                .maximumElapsedMillis = static_cast<uint64>(
                    member(policy, "maximum_elapsed_ms").number()
                ),
                .onTimeout = *action,
            };
        }

        [[nodiscard]]
        auto readEffectBounds(json::Value const& tool)
            -> Result<std::vector<operator_runtime::EffectBound>>
        {
            auto bounds = std::vector<operator_runtime::EffectBound>{};
            for (auto const& bound : member(tool, "effect_bounds").items())
            {
                UF_TRY_VALUE(
                    payloadSchemaHash,
                    ContentHash::parse(
                        std::string{"sha256:"}
                        + std::string{member(bound, "payload_schema_hash").string()}
                    )
                );
                auto const risk = std::ranges::find(
                    k_risks,
                    member(bound, "maximum_risk").string(),
                    operator_runtime::riskWireName
                );
                UF_CHECK(risk != k_risks.end());
                bounds.emplace_back(operator_runtime::EffectBound{
                    .namespacedType = std::string{
                        member(bound, "namespaced_type").string()
                    },
                    .scopeKind         = std::string{
                        member(bound, "scope_kind").string()
                    },
                    .payloadSchemaHash = payloadSchemaHash,
                    .maximumRisk       = *risk,
                });
            }
            return bounds;
        }

        // The guard one Tool's declaration asked for, compiled from the exact
        // bytes it stated. It compiles on its own: reuse inside one schema is
        // JSON Schema's own $defs, and a document set around it would let a
        // schema whose bytes the registration never pinned decide what a call's
        // arguments are judged against.
        [[nodiscard]]
        auto readArgumentSchema(
            std::string_view toolName,
            json::Value const& declared
        ) -> Result<json::Schema>
        {
            return compile(
                std::format("argument schema of {}", toolName),
                json::canonicalBytes(declared)
            );
        }
    } // namespace

    // Everything create() read and compiled, and the whole of what judging a
    // call consults. It carries the operations rather than leaving them as free
    // functions, because each of them would otherwise take it as a first
    // parameter.
    class ProjectDeployment::State final
    {
    public:
        std::vector<ToolEntry> tools{};

        // One observed identity schema this deployment compiled, in the order
        // the deployment declared them. The registration pins the same bytes by
        // sha256, and ObservedInstanceIdentitySchemas::create refuses any
        // validator set that is not exactly that pinned set.
        struct IdentitySchema final
        {
            std::string  name{};
            ContentHash  schemaHash;
            json::Schema schema;
        };
        std::vector<IdentitySchema> identitySchemas{};

        [[nodiscard]] auto findTool(std::string_view name) const -> ToolEntry const*;

        [[nodiscard]]
        auto validateIdentityBasis(
            std::size_t index,
            json::Value const& basis
        ) const -> Status;

        [[nodiscard]]
        auto validateToolArguments(
            std::string_view toolName,
            json::Value const& arguments
        ) const -> Status;
    };

    auto ProjectDeployment::State::findTool(std::string_view name) const
        -> ToolEntry const*
    {
        auto const found = std::ranges::find(tools, name, &ToolEntry::name);
        return found == tools.end() ? nullptr : &*found;
    }

    auto ProjectDeployment::State::validateIdentityBasis(
        std::size_t index,
        json::Value const& basis
    ) const -> Status
    {
        auto const& schema = identitySchemas[index].schema;
        return adopt(
            schema.validate(basis),
            std::format(
                "observed identity basis under {}",
                identitySchemas[index].name
            )
        );
    }

    // The arguments of one call, against the schema this project's declaration
    // states for that Tool. A Tool this deployment does not declare is a
    // refusal here, which is what makes tool_name inside an OP:`PlanProposal` a
    // stronger statement than the operator protocol's own NamespacedIdentifier.
    //
    // Every Project Tool publishes one flat input schema. The trusted seam
    // validates the arguments against it before it builds the immutable Luau
    // input value; there is no unchecked spelling.
    auto ProjectDeployment::State::validateToolArguments(
        std::string_view toolName,
        json::Value const& arguments
    ) const -> Status
    {
        auto const* const p_tool = findTool(toolName);
        if (p_tool == nullptr)
        {
            return refuse(std::format(
                "this project declares no Tool named {}",
                toolName
            ));
        }
        return adopt(
            p_tool->argumentSchema.validate(arguments),
            std::format("arguments of {}", toolName)
        );
    }

    auto currentToolRuntimeProtocolMaterial() -> Result<std::string>
    {
        UF_TRY_VALUE(
            preimageBytes,
            operator_runtime::toolIdentityPreimageMaterial()
        );
        UF_TRY_VALUE(preimages, json::parse(preimageBytes));
        UF_TRY_VALUE(
            vocabulary,
            json::parse(operator_runtime::toolCallVocabularyMaterial())
        );
        UF_TRY_VALUE(
            durableRecord,
            json::parse(operator_runtime::toolRuntimeDurableRecordMaterial())
        );
        UF_TRY_VALUE(published, publishedProjectSchema());
        return json::canonicalBytes(json::Value::ofObject({
            {"call_vocabulary", std::move(vocabulary)},

            // A named token rather than a derivation, for the reason the
            // plugin environment's freeze and budget tokens are: no constant in
            // this tree says "RFC 8785 exactness with no second form admitted",
            // and the only way an identity can move when that rule changes is
            // for the author changing it to bump this word. It is the one
            // member here nothing else can falsify.
            {"canonical_form_contract",
             json::Value::ofString("rfc8785-exact-bytes-no-second-form-v1")},

            {"durable_record", std::move(durableRecord)},
            {"identity_preimages", std::move(preimages)},

            // The exact bytes that decide what a Tool declaration IS. A Tool
            // call's whole provider surface is read out of a document judged by
            // this schema, so a protocol identity that did not carry it would
            // be named for a property it cannot observe -- the defect that made
            // this material necessary in the first place.
            {"tool_declaration_schema",
             json::Value::ofString(std::string{published.exactBytes})},
            {"tool_declaration_wire_tag",
             json::Value::ofString("umbraflow-project/v3")},
        }));
    }

    auto currentToolRuntimeProtocolIdentity() -> Result<ContentHash>
    {
        UF_TRY_VALUE(material, currentToolRuntimeProtocolMaterial());
        return sha256(std::as_bytes(std::span{material}));
    }

    ProjectDeployment::ProjectDeployment(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectDeployment::create(ProjectDeploymentSources const& sources)
        -> Result<ProjectDeployment>
    {
        UF_TRY_VALUE(published, publishedProjectSchema());
        UF_TRY_VALUE(
            directorySchema,
            compile(published.label, published.exactBytes)
        );
        UF_TRY_VALUE(declarations, parseDocument(sources.tools));
        if (declarations.kind() != json::ValueKind::Array)
        {
            return refuse("a deployment's Tool declarations must be an array");
        }
        auto state = std::make_shared<State>(State{
            .tools           = {},
            .identitySchemas = {},
        });

        for (auto const& tool : declarations.items())
        {
            // The published shape first, so every member read below is one the
            // schema has already accepted and of the kind it stated.
            UF_TRY(adopt(
                directorySchema.validateDefinition(k_toolDefinition, tool),
                "a Tool this deployment declares"
            ));

            auto const name = std::string{member(tool, "name").string()};

            // Ownership is the one rule no JSON Schema can state, because it
            // compares a Tool's name with the namespace the deployment
            // registered. It is refused where the Tool is written rather than
            // when a call of it arrives.
            UF_TRY(operator_runtime::validateToolNameOwnership(
                name,
                sources.pluginId
            ));

            auto const mutability = std::ranges::find(
                k_mutabilities,
                member(tool, "mutability").string(),
                operator_runtime::toolMutabilityWireName
            );
            auto const surface = std::ranges::find(
                k_surfaces,
                member(tool, "surface").string(),
                operator_runtime::toolSurfaceWireName
            );
            auto const idempotency = std::ranges::find(
                k_idempotencies,
                member(tool, "idempotency").string(),
                operator_runtime::toolIdempotencyWireName
            );
            UF_CHECK(mutability != k_mutabilities.end());
            UF_CHECK(surface != k_surfaces.end());
            UF_CHECK(idempotency != k_idempotencies.end());

            UF_TRY_VALUE(bounds, readEffectBounds(tool));
            UF_TRY_VALUE(
                argumentSchema,
                readArgumentSchema(name, member(tool, "argument_schema"))
            );

            // output_schema is the one Tool member a Project may leave out.
            // Where it is written it is compiled like any other, so a result
            // shape this evaluator could not apply is refused where the Tool
            // is declared rather than believed and never checked.
            auto outputSchema = json::Value{};
            if (auto const* const p_output = tool.find("output_schema"))
            {
                UF_TRY_VALUE(
                    compiled,
                    compile(
                        std::format("result schema of {}", name),
                        json::canonicalBytes(*p_output)
                    )
                );
                // Compiled and then dropped: nothing judges a Project's result
                // against its declaration, because a Project Tool's handler is
                // the Project's own code and the framework does not audit what
                // a project returns to itself. What the compile buys is that a
                // published result shape is a schema a reader can actually
                // apply, refused where it is written rather than believed.
                static_cast<void>(compiled);
                outputSchema = *p_output;
            }

            state->tools.emplace_back(ToolEntry{
                .name           = name,
                .description    = std::string{member(tool, "description").string()},
                .inputSchema    = member(tool, "argument_schema"),
                .outputSchema   = std::move(outputSchema),
                .argumentSchema = std::move(argumentSchema),
                .descriptor     = operator_runtime::ToolDescriptor{
                        .toolVersion = std::string{member(tool, "version").string()},
                        .requiredCapabilities = names(
                        member(tool, "required_capabilities")
                    ),
                        .effectBounds   = std::move(bounds),
                        .uiActionBounds = names(member(tool, "ui_action_bounds")),
                        .timeout        = readTimeoutPolicy(member(tool, "timeout_policy")),
                        .mutability     = *mutability,
                        .surface        = *surface,
                        .idempotency    = *idempotency,
                },
            });
        }

        // The identity schemas, each compiled on its own. One document may not
        // reference another: reuse inside a schema is JSON Schema's own $defs,
        // and a set around a document would let bytes the registration never
        // pinned decide what an identity basis is judged against.
        for (auto const& declared : sources.observedInstanceIdentitySchemas)
        {
            if (
                std::ranges::contains(
                    state->identitySchemas,
                    declared.name,
                    &State::IdentitySchema::name
                )
            )
            {
                return refuse(std::format(
                    "this deployment declares the observed instance identity "
                    "schema {} twice",
                    declared.name
                ));
            }
            UF_TRY_VALUE(schemaHash, hashOf(declared.schema));
            UF_TRY_VALUE(
                schema,
                compile(
                    std::format("observed identity {}", declared.name),
                    declared.schema
                )
            );
            state->identitySchemas.emplace_back(State::IdentitySchema{
                .name       = std::string{declared.name},
                .schemaHash = schemaHash,
                .schema     = std::move(schema),
            });
        }

        return ProjectDeployment{std::shared_ptr<State const>{std::move(state)}};
    }

    auto ProjectDeployment::carriedTool(std::string_view name) const
        -> std::optional<operator_runtime::ToolDescriptor>
    {
        auto const* const p_tool = m_state->findTool(name);
        if (p_tool == nullptr)
        {
            return std::nullopt;
        }
        return p_tool->descriptor;
    }

    auto ProjectDeployment::toolCatalogReader() const
        -> operator_runtime::ToolCatalogReader
    {
        return [p_state = m_state]()
                   -> Result<std::vector<operator_runtime::ToolCatalogEntry>>
        {
            auto entries = std::vector<operator_runtime::ToolCatalogEntry>{};
            entries.reserve(p_state->tools.size());
            for (auto const& tool : p_state->tools)
            {
                entries.emplace_back(operator_runtime::ToolCatalogEntry{
                    .name         = tool.name,
                    .description  = tool.description,
                    .inputSchema  = tool.inputSchema,
                    .outputSchema = tool.outputSchema,
                    .descriptor   = tool.descriptor,
                });
            }
            return entries;
        };
    }

    auto ProjectDeployment::toolArgumentValidator() const
        -> operator_runtime::ToolArgumentValidator
    {
        return [p_state = m_state](
                   std::string_view toolName,
                   std::string_view exactArgsJcs
               ) -> Status
        {
            UF_TRY_VALUE(arguments, parseDocument(exactArgsJcs));
            return p_state->validateToolArguments(toolName, arguments);
        };
    }

    auto ProjectDeployment::observedIdentitySchemas() const
        -> std::vector<operator_runtime::ObservedInstanceIdentitySchema>
    {
        auto bindings = std::vector<operator_runtime::ObservedInstanceIdentitySchema>{};
        bindings.reserve(m_state->identitySchemas.size());
        for (auto index = std::size_t{0}; index < m_state->identitySchemas.size(); ++index)
        {
            bindings.emplace_back(operator_runtime::ObservedInstanceIdentitySchema{
                .schemaId   = m_state->identitySchemas[index].name,
                .schemaHash = m_state->identitySchemas[index].schemaHash,
                .validate   = [p_state = m_state, index](json::Value const& basis) -> Status
                {
                    return p_state->validateIdentityBasis(index, basis);
                },
            });
        }
        return bindings;
    }
}
