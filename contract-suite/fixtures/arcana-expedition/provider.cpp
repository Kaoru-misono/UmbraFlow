// A second game, standing in for a consuming repository.
//
// Nothing here is shared with this repository's own fixture: different
// registration bytes and byte layout, different schema identities, a project
// artifact root and its blob, different tool names, versions and argument
// shape, different event types and payloads, a different provenance document,
// and a plugin whose reconcile is not the identity -- so the disposition the
// authority reads is nowhere in the request that produced it.
//
// It is written the way a consumer writes one: include the suite's public
// header, build the five authorities out of the deployment's own validators,
// and define projectUnderTest.

#include <operator-contract/project-under-test.hpp>

#include <operator/journal-entry.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::contract
{
    namespace
    {
        // What separates this project's two registrations. A consumer needs a
        // second one only so the suite can prove that authority does not cross
        // between them.
        struct ProjectIdentity final
        {
            std::string_view pluginId{};
            std::string_view baselineEventType{};
            std::string_view schemaSalt{};
            std::string_view pluginSource{};
            std::string_view eventNamespace{};
        };

        constexpr auto k_expeditionPlugin = std::string_view{R"LUAU(
return {
    plugin_id = "arcana.expedition",
    derive = function(_input) return '{"visible":true}' end,
    plan = function(_input) return '{"visible":true}' end,
    next_step = function(_input) return '{"visible":true}' end,
    reconcile = function(input)
        if input == '{"observed":"advanced"}' then return '{"verdict":"underway"}' end
        if input == '{"observed":"arrived"}' then return '{"verdict":"settled"}' end
        if input == '{"observed":"blocked"}' then return '{"verdict":"refused"}' end
        return '{"verdict":"unclear"}'
    end,
    reduce = function(_input) return '{"turn":0}' end,
}
)LUAU"};

        constexpr auto k_rivalPlugin = std::string_view{R"LUAU(
return {
    plugin_id = "arcana.rival",
    derive = function(_input) return '{"visible":true}' end,
    plan = function(_input) return '{"visible":true}' end,
    next_step = function(_input) return '{"visible":true}' end,
    reconcile = function(input)
        if input == '{"observed":"advanced"}' then return '{"verdict":"underway"}' end
        if input == '{"observed":"arrived"}' then return '{"verdict":"settled"}' end
        if input == '{"observed":"blocked"}' then return '{"verdict":"refused"}' end
        return '{"verdict":"unclear"}'
    end,
    reduce = function(_input) return '{"turn":0}' end,
}
)LUAU"};

        constexpr auto k_artifactRootName = std::string_view{"map"};
        constexpr auto k_artifactBytes = std::string_view{"expedition-map-bytes"};
        constexpr auto k_provenance = std::string_view{"{\"witness\":\"suite\"}"};
        constexpr auto k_projectState = std::string_view{"{\"turn\":0}"};
        constexpr auto k_visible = std::string_view{"{\"visible\":true}"};

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto refuse(std::string message) -> Status
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // The reduce envelope is the Operator's shape, not this project's, so
        // the deployment recognizes it structurally: however many events a
        // commit appends, they arrive here and nowhere else.
        [[nodiscard]]
        auto looksLikeReduceEnvelope(std::string_view exactJcs) -> bool
        {
            constexpr auto prefix = std::string_view{"{\"journal_events\":["};
            constexpr auto middle = std::string_view{"],\"prior_project_state\":"};
            if (!exactJcs.starts_with(prefix) || !exactJcs.ends_with('}'))
            {
                return false;
            }
            auto const middleAt = exactJcs.find(middle);
            if (middleAt == std::string_view::npos)
            {
                return false;
            }

            auto const priorState = exactJcs.substr(
                middleAt + middle.size(),
                exactJcs.size() - middleAt - middle.size() - 1U
            );
            if (priorState != "null" && priorState != k_projectState)
            {
                return false;
            }

            constexpr auto eventPrefix =
                std::string_view{"{\"namespaced_event_type\":\""};
            constexpr auto eventSuffix =
                std::string_view{",\"provenance\":{\"witness\":\"suite\"}}"};

            auto events = exactJcs.substr(prefix.size(), middleAt - prefix.size());
            while (!events.empty())
            {
                if (!events.starts_with(eventPrefix))
                {
                    return false;
                }
                auto const suffixAt = events.find(eventSuffix);
                if (
                    suffixAt == std::string_view::npos
                    || events.substr(0U, suffixAt)
                            .find("\",\"opaque_project_payload\":")
                        == std::string_view::npos
                )
                {
                    return false;
                }
                events.remove_prefix(suffixAt + eventSuffix.size());
                if (events.starts_with(','))
                {
                    events.remove_prefix(1U);
                    continue;
                }
                if (!events.empty())
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]]
        auto vocabularyOf(ProjectIdentity const& identity) -> ProjectVocabulary
        {
            return ProjectVocabulary{
                .mutatingTool         = "expedition.move",
                .otherMutatingTool    = "expedition.trade",
                .readOnlyTool         = "expedition.survey",
                .toolArguments        = "{\"steps\":2}",
                .refusedToolArguments = "{\"steps\":9}",
                .absentTool           = "expedition.smuggle",
                .baselineEntry        = JournalDocument{
                    .eventType = std::string{identity.baselineEventType},
                    .payload   = "{\"camp\":\"north\"}",
                },
                .progressEntry = JournalDocument{
                    .eventType = std::format("{}.advanced", identity.eventNamespace),
                    .payload   = "{\"leagues\":4}",
                },
                .confirmedEntry = JournalDocument{
                    .eventType = std::format("{}.arrived", identity.eventNamespace),
                    .payload   = "{\"leagues\":7}",
                },
                .supersededEntry = JournalDocument{
                    .eventType = std::format("{}.blocked", identity.eventNamespace),
                    .payload   = "{\"leagues\":9}",
                },
                .provenance     = std::string{k_provenance},
                .continueInput  = "{\"observed\":\"advanced\"}",
                .confirmedInput = "{\"observed\":\"arrived\"}",
                .rejectedInput  = "{\"observed\":\"blocked\"}",
                .ambiguousInput = "{\"observed\":\"nothing\"}",
            };
        }

        // Everything this deployment holds for one registration. The suite sees
        // only the authorities built from it.
        struct DeploymentSchemas final
        {
            std::string registration{};
            std::string projectState{};
            std::string observation{};
            std::string precondition{};
            std::string toolCatalog{};
            std::string reconcileManifest{};
            std::string journalManifest{};
        };

        [[nodiscard]]
        auto schemasOf(ProjectIdentity const& identity) -> DeploymentSchemas
        {
            auto const salt = identity.schemaSalt;
            return DeploymentSchemas{
                .registration      = std::format("{}/registration.schema", salt),
                .projectState      = std::format("{}/project-state.schema", salt),
                .observation       = std::format("{}/observation.schema", salt),
                .precondition      = std::format("{}/precondition.schema", salt),
                .toolCatalog       = std::format("{}/tool-catalog.json", salt),
                .reconcileManifest = std::format("{}/reconcile.manifest", salt),
                .journalManifest   = std::format("{}/journal.manifest", salt),
            };
        }

        // This deployment's registration is a nested document rather than the
        // flat one this repository's own fixture writes. Only its hash binds,
        // which is the point: the Operator never reads a project's layout.
        [[nodiscard]]
        auto registrationBytes(
            ProjectIdentity const& identity,
            DeploymentSchemas const& schemas
        ) -> std::string
        {
            return std::format(
                "{{\"artifacts\":[{{\"name\":\"{}\",\"sha256\":\"{}\"}}],"
                "\"binding\":{{\"plugin\":{{\"id\":\"{}\",\"sha256\":\"{}\"}},"
                "\"tool_catalog_sha256\":\"{}\"}},"
                "\"documents\":{{\"observation_sha256\":\"{}\","
                "\"precondition_sha256\":\"{}\",\"state_sha256\":\"{}\"}},"
                "\"journal\":{{\"baseline\":\"{}\",\"manifest_sha256\":\"{}\"}},"
                "\"reconcile_manifest_sha256\":\"{}\",\"schema_sha256\":\"{}\"}}",
                k_artifactRootName,
                hashOf(k_artifactBytes).hex(),
                identity.pluginId,
                hashOf(identity.pluginSource).hex(),
                hashOf(schemas.toolCatalog).hex(),
                hashOf(schemas.observation).hex(),
                hashOf(schemas.precondition).hex(),
                hashOf(schemas.projectState).hex(),
                identity.baselineEventType,
                hashOf(schemas.journalManifest).hex(),
                hashOf(schemas.reconcileManifest).hex(),
                hashOf(schemas.registration).hex()
            );
        }

        [[nodiscard]]
        auto verifiedRegistration(
            ProjectIdentity const& identity,
            DeploymentSchemas const& schemas
        ) -> VerifiedProjectRegistration
        {
            auto const exactJcs = registrationBytes(identity, schemas);
            auto const claims   = ProjectRegistrationClaims{
                .manifestSchemaHash                 = hashOf(schemas.registration),
                .pluginId                           = std::string{identity.pluginId},
                .pluginHash                         = hashOf(identity.pluginSource),
                .toolCatalogHash                    = hashOf(schemas.toolCatalog),
                .projectStateSchemaHash             = hashOf(schemas.projectState),
                .projectObservationSchemaHash       = hashOf(schemas.observation),
                .projectToolPreconditionSchemaHash  = hashOf(schemas.precondition),
                .reconcilePayloadSchemaManifestHash = hashOf(schemas.reconcileManifest),
                .journalEventSchemaManifestHash     = hashOf(schemas.journalManifest),
                .baselineEventType                  = std::string{identity.baselineEventType},
                .projectArtifactRoots               = {
                    NamedArtifactRoot{
                        .name     = std::string{k_artifactRootName},
                        .rootHash = hashOf(k_artifactBytes),
                    },
                },
            };
            auto const owner = ProjectRegistrationSchemaOwner::create(
                hashOf(schemas.registration),
                [exactJcs, claims](
                    std::string_view candidate
                ) -> Result<ProjectRegistrationClaims>
                {
                    if (candidate != exactJcs)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "expedition registration is not the exact pinned document"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(owner.has_value());
            auto registration = ProjectRegistration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                *owner
            );
            REQUIRE(registration.has_value());
            return *std::move(registration);
        }

        [[nodiscard]]
        auto canonicalValidator() -> CanonicalJsonValidator
        {
            return [](std::string_view candidateJcs) -> Status
            {
                constexpr auto accepted = std::array{
                    std::string_view{"{\"camp\":\"north\"}"},
                    std::string_view{"{\"leagues\":4}"},
                    std::string_view{"{\"leagues\":7}"},
                    std::string_view{"{\"leagues\":9}"},
                    std::string_view{"{\"observed\":\"advanced\"}"},
                    std::string_view{"{\"observed\":\"arrived\"}"},
                    std::string_view{"{\"observed\":\"blocked\"}"},
                    std::string_view{"{\"observed\":\"nothing\"}"},
                    std::string_view{"{\"steps\":2}"},
                    std::string_view{"{\"steps\":9}"},
                    std::string_view{"{\"verdict\":\"refused\"}"},
                    std::string_view{"{\"verdict\":\"settled\"}"},
                    std::string_view{"{\"verdict\":\"unclear\"}"},
                    std::string_view{"{\"verdict\":\"underway\"}"},
                    k_projectState,
                    k_provenance,
                    k_visible,
                };
                if (
                    std::ranges::find(accepted, candidateJcs) == accepted.end()
                    && !looksLikeReduceEnvelope(candidateJcs)
                )
                {
                    return refuse("expedition canonical validator rejected bytes");
                }
                return ok();
            };
        }

        [[nodiscard]]
        auto documentValidator(
            std::shared_ptr<std::string> observedReduceInput
        ) -> ProjectDocumentValidator
        {
            return [observedReduceInput = std::move(observedReduceInput)](
                       ProjectPluginFunction function,
                       ProjectDocumentDirection direction,
                       std::string_view candidateJcs
                   ) -> Status
            {
                auto valid = false;
                if (direction == ProjectDocumentDirection::Input)
                {
                    switch (function)
                    {
                    case ProjectPluginFunction::Reduce:
                        *observedReduceInput = std::string{candidateJcs};
                        valid = looksLikeReduceEnvelope(candidateJcs);
                        break;
                    case ProjectPluginFunction::Reconcile:
                        valid = candidateJcs.starts_with("{\"observed\":\"")
                            && candidateJcs.ends_with("\"}");
                        break;
                    case ProjectPluginFunction::Derive:
                    case ProjectPluginFunction::Plan:
                    case ProjectPluginFunction::NextStep:
                        valid = candidateJcs == k_visible;
                        break;
                    }
                }
                else
                {
                    switch (function)
                    {
                    case ProjectPluginFunction::Reduce:
                        valid = candidateJcs == k_projectState;
                        break;
                    case ProjectPluginFunction::Reconcile:
                        valid = candidateJcs.starts_with("{\"verdict\":\"")
                            && candidateJcs.ends_with("\"}");
                        break;
                    case ProjectPluginFunction::Derive:
                    case ProjectPluginFunction::Plan:
                    case ProjectPluginFunction::NextStep:
                        valid = candidateJcs == k_visible;
                        break;
                    }
                }
                if (!valid)
                {
                    return refuse("expedition document schema rejected a document");
                }
                return ok();
            };
        }

        [[nodiscard]]
        auto journalPayloadValidator(
            std::string_view eventNamespace
        ) -> JournalPayloadSchemaValidator
        {
            struct PayloadCase final
            {
                std::string eventType{};
                std::string payload{};
                std::string schemaIdentity{};
            };
            auto cases = std::vector<PayloadCase>{};
            cases.emplace_back(PayloadCase{
                .eventType      = std::format("{}.founded", eventNamespace),
                .payload        = "{\"camp\":\"north\"}",
                .schemaIdentity = "expedition/founded.schema",
            });
            cases.emplace_back(PayloadCase{
                .eventType      = std::format("{}.advanced", eventNamespace),
                .payload        = "{\"leagues\":4}",
                .schemaIdentity = "expedition/advanced.schema",
            });
            cases.emplace_back(PayloadCase{
                .eventType      = std::format("{}.arrived", eventNamespace),
                .payload        = "{\"leagues\":7}",
                .schemaIdentity = "expedition/arrived.schema",
            });
            cases.emplace_back(PayloadCase{
                .eventType      = std::format("{}.blocked", eventNamespace),
                .payload        = "{\"leagues\":9}",
                .schemaIdentity = "expedition/blocked.schema",
            });
            return [cases = std::move(cases)](
                       std::string_view eventType,
                       std::string_view payload
                   ) -> Result<ContentHash>
            {
                auto const found = std::ranges::find_if(
                    cases,
                    [eventType, payload](PayloadCase const& candidate)
                    {
                        return candidate.eventType == eventType
                            && candidate.payload == payload;
                    }
                );
                if (found == cases.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "expedition event payload schema rejected a document"
                    );
                }
                return hashOf(found->schemaIdentity);
            };
        }

        [[nodiscard]]
        auto toolCatalogValidator() -> ToolCatalogValidator
        {
            return [](
                       std::string_view toolName,
                       std::string_view exactArgsJcs
                   ) -> Result<ToolDescriptor>
            {
                struct ToolCase final
                {
                    std::string_view name{};
                    std::string_view version{};
                    ToolMutability   mutability{ToolMutability::Mutating};
                };
                constexpr auto catalog = std::array{
                    ToolCase{
                        .name       = "expedition.move",
                        .version    = "3",
                        .mutability = ToolMutability::Mutating,
                    },
                    ToolCase{
                        .name       = "expedition.trade",
                        .version    = "3",
                        .mutability = ToolMutability::Mutating,
                    },
                    ToolCase{
                        .name       = "expedition.survey",
                        .version    = "2",
                        .mutability = ToolMutability::ReadOnly,
                    },
                };
                auto const found = std::ranges::find_if(
                    catalog,
                    [toolName](ToolCase const& candidate)
                    {
                        return candidate.name == toolName;
                    }
                );
                if (found == catalog.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "expedition Tool Catalog has no such tool"
                    );
                }
                if (exactArgsJcs != "{\"steps\":2}")
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "expedition tool argument schema rejected a document"
                    );
                }
                return ToolDescriptor{
                    .toolVersion = std::string{found->version},
                    .mutability  = found->mutability,
                };
            };
        }

        // The disposition is nowhere in the reconcile input: it is read out of
        // the verdict this project's plugin reached.
        [[nodiscard]]
        auto dispositionReader() -> ReconcileDispositionReader
        {
            return [](std::string_view candidateJcs) -> Result<ReconcileDisposition>
            {
                struct VerdictCase final
                {
                    std::string_view     document{};
                    ReconcileDisposition disposition{ReconcileDisposition::Ambiguous};
                };
                constexpr auto cases = std::array{
                    VerdictCase{
                        .document    = "{\"verdict\":\"underway\"}",
                        .disposition = ReconcileDisposition::Continue,
                    },
                    VerdictCase{
                        .document    = "{\"verdict\":\"settled\"}",
                        .disposition = ReconcileDisposition::Confirmed,
                    },
                    VerdictCase{
                        .document    = "{\"verdict\":\"refused\"}",
                        .disposition = ReconcileDisposition::Rejected,
                    },
                    VerdictCase{
                        .document    = "{\"verdict\":\"unclear\"}",
                        .disposition = ReconcileDisposition::Ambiguous,
                    },
                };
                auto const found = std::ranges::find_if(
                    cases,
                    [candidateJcs](VerdictCase const& candidate)
                    {
                        return candidate.document == candidateJcs;
                    }
                );
                if (found == cases.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "expedition reconcile output carries no known verdict"
                    );
                }
                return found->disposition;
            };
        }

        [[nodiscard]]
        auto makeProject(ProjectIdentity const& identity) -> ProjectUnderTest
        {
            auto const schemas  = schemasOf(identity);
            auto registration   = verifiedRegistration(identity, schemas);
            auto observedReduce = std::make_shared<std::string>();

            auto schemaOwner = ProjectSchemaOwner::create(
                registration,
                ProjectDocumentSchemaBytes{
                    .projectState       = schemas.projectState,
                    .projectObservation = schemas.observation,
                    .toolPrecondition   = schemas.precondition,
                },
                canonicalValidator(),
                documentValidator(observedReduce)
            );
            REQUIRE(schemaOwner.has_value());

            auto journalSchemaOwner = ProjectJournalSchemaOwner::create(
                registration,
                schemas.journalManifest,
                journalPayloadValidator(identity.eventNamespace),
                [](std::string_view provenance) -> Status
                {
                    if (provenance != k_provenance)
                    {
                        return refuse("expedition provenance schema rejected a document");
                    }
                    return ok();
                }
            );
            REQUIRE(journalSchemaOwner.has_value());

            auto toolCatalogSchemaOwner = ProjectToolCatalogSchemaOwner::create(
                registration,
                schemas.toolCatalog,
                toolCatalogValidator()
            );
            REQUIRE(toolCatalogSchemaOwner.has_value());

            auto reconcileSchemaOwner = ProjectReconcileSchemaOwner::create(
                registration,
                schemas.reconcileManifest,
                dispositionReader()
            );
            REQUIRE(reconcileSchemaOwner.has_value());

            auto artifactBlobs = std::vector<ProjectPluginRegistrar::ArtifactBlob>{};
            artifactBlobs.emplace_back(ProjectPluginRegistrar::ArtifactBlob{
                .name  = std::string{k_artifactRootName},
                .bytes = std::string{k_artifactBytes},
            });

            return ProjectUnderTest{
                .registration           = std::move(registration),
                .schemaOwner            = *std::move(schemaOwner),
                .journalSchemaOwner     = *std::move(journalSchemaOwner),
                .toolCatalogSchemaOwner = *std::move(toolCatalogSchemaOwner),
                .reconcileSchemaOwner   = *std::move(reconcileSchemaOwner),
                .pluginBytes            = std::string{identity.pluginSource},
                .artifactBlobs          = std::move(artifactBlobs),
                .observedReduceInput    = std::move(observedReduce),
                .vocabulary             = vocabularyOf(identity),
            };
        }
    }

    auto projectUnderTest(ProjectRole role) -> ProjectUnderTest
    {
        constexpr auto expedition = ProjectIdentity{
            .pluginId          = "arcana.expedition",
            .baselineEventType = "expedition.founded",
            .schemaSalt        = "arcana/expedition",
            .pluginSource      = k_expeditionPlugin,
            .eventNamespace    = "expedition",
        };
        constexpr auto rival = ProjectIdentity{
            .pluginId          = "arcana.rival",
            .baselineEventType = "expedition.founded",
            .schemaSalt        = "arcana/rival",
            .pluginSource      = k_rivalPlugin,
            .eventNamespace    = "expedition",
        };
        return makeProject(role == ProjectRole::UnderTest ? expedition : rival);
    }
}
