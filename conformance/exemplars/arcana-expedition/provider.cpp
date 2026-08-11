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
// and define provideProject.

#include "project-schemas.hpp"

#include <conformance/operator-protocol.hpp>
#include <conformance/provider.hpp>

#include <deployment/project-deployment.hpp>

#include <operator/journal-entry.hpp>
#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>
#include <operator/reconcile-outcome.hpp>
#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::conformance
{
    namespace
    {
        // What separates this project's two registrations. A consumer needs a
        // second one only so the suite can prove that authority does not cross
        // between them. The two carry the same schema documents and differ in
        // the plugin id every manifest declares, so their catalog, journal and
        // reconcile manifest hashes differ and their registrations do too.
        struct ProjectIdentity final
        {
            std::string_view pluginId{};
            std::string_view baselineEventType{};
            std::string_view eventNamespace{};
        };

        // The operator protocol documents this project answers with. plan reads
        // the tool name out of the envelope the Operator assembled, so one
        // registration reaches the approval edge without a second plugin_hash.
        //
        // It is assembled rather than written out because the effect payload
        // schema hash is the sha256 of this deployment's own schema bytes: a
        // literal there would be an identity naming nothing.
        constexpr auto k_pluginBody = std::string_view{R"LUAU(
local function effect(risk, camp)
    return '{"namespaced_type":"expedition.march","opaque_project_payload":{"turn":0}'
        .. ',"payload_schema_hash":"' .. schema .. '","risk":"' .. risk
        .. '","scope_key":"' .. camp .. '","scope_kind":"camp"}'
end
local ordinary = '[' .. effect("low", "north") .. ',' .. effect("medium", "south") .. ']'
local risky = '[' .. effect("high", "north") .. ']'
local function proposal(tool, effects, steps, dispatches)
    return '{"allowed_ui_actions":["expedition.step"],"canonical_args":{"steps":2}'
        .. ',"effects":' .. effects
        .. ',"tool_name":"' .. tool .. '","tool_version":"3"'
        .. ',"workflow_limits":{"maximum_dispatches":' .. dispatches
        .. ',"maximum_elapsed_ms":60000,"maximum_observations":16,"maximum_steps":'
        .. steps .. ',"maximum_waits":4}}'
end
local plans = {
    ["expedition.move"] = proposal("expedition.move", ordinary, "8", "8"),
    ["expedition.trade"] = proposal("expedition.trade", ordinary, "8", "8"),
    ["expedition.approval"] = proposal("expedition.approval", risky, "8", "8"),
}
local step_intent = '{"action":{"action_id":"expedition.press"'
    .. ',"canonical_parameters":{"steps":2},"surface_id":"expedition.surface"'
    .. ',"ui_target_id":"expedition.target"},"binding_variant_constraints":[]'
    .. ',"delivery_class":"delivery_safe","expected_ui_postconditions":[]'
    .. ',"required_ui_preconditions":[],"step_key":"expedition.step"'
    .. ',"timeout_policy":{"maximum_elapsed_ms":5000,"on_timeout":"reobserve"}}'
return {
    plugin_id = ")LUAU"};

        constexpr auto k_pluginTail = std::string_view{R"LUAU(",
    derive = function(_input) return '{"visible":true}' end,
    plan = function(input)
        local tool = string.match(input, '"tool_name":"([^"]*)"')
        local answer = plans[tool]
        if answer == nil then
            error("expedition has no plan for " .. tostring(tool))
        end
        return answer
    end,
    next_step = function(_input) return step_intent end,
    reconcile = function(input)
        if input == '{"observed":"advanced"}' then return '{"verdict":"underway"}' end
        if input == '{"observed":"arrived"}' then return '{"verdict":"settled"}' end
        if input == '{"observed":"blocked"}' then return '{"verdict":"refused"}' end
        return '{"verdict":"unclear"}'
    end,
    reduce = function(_input) return '{"turn":0}' end,
}
)LUAU"};

        [[nodiscard]]
        auto pluginSource(std::string_view pluginId) -> std::string
        {
            auto source = std::string{"local schema = \""};
            source += expedition::schemaHashHex(expedition::k_effectPayloadSchema);
            source += '"';
            source += k_pluginBody;
            source += pluginId;
            source += k_pluginTail;
            return source;
        }

        // The geometry this project's model was authored at, and the one its
        // capture is taken at. It is deliberately nothing like the umbraflow
        // fixture's three-by-one at 96 DPI: two projects whose worlds happen to
        // agree cannot show that the geometry travels from the project, because
        // every value would be right by coincidence. These are the numbers a
        // real consumer reports -- a 16:9 client area at a scaled DPI.
        constexpr auto k_frameWidth  = uint32{1600};
        constexpr auto k_frameHeight = uint32{900};
        constexpr auto k_frameDpi    = uint32{144};

        // Where each template sits in that frame. The model states the search
        // rectangles that contain them, so these numbers appear twice; the
        // suite's requireResolvedSurface is what keeps the two from drifting,
        // because a patch that moved out of its rectangle resolves nothing.
        //
        // The two are far apart so that a frame carrying one and not the other
        // resolves differently, and each rectangle is larger than its patch so
        // the matcher has somewhere to look.
        constexpr auto k_patch   = uint32{8};
        constexpr auto k_bannerX = uint32{112};
        constexpr auto k_bannerY = uint32{96};
        constexpr auto k_marchX  = uint32{1288};
        constexpr auto k_marchY  = uint32{742};

        // The two grays those patches carry. Both are exact under the matcher's
        // luma weights, which sum to 256, so an equal-valued crop scores zero
        // and clears a threshold of 1.
        constexpr auto k_bannerGray = uint8{37};
        constexpr auto k_marchGray  = uint8{211};

        // This project's RuntimeModel. Nothing in it is shared with the
        // umbraflow fixture: different surface, target, locator, binding and
        // action ids, different asset file names, and a resolution and DPI of
        // its own. The step intent above names expedition.surface,
        // expedition.target and expedition.press, and so does this, because a
        // plan that named UI this model does not define would be planning
        // against a world that does not exist.
        //
        // base_resolution and base_dpi here and the ProjectProbeFrame's
        // fingerprint below are one fact stated twice, and must not drift: C++
        // parses no RuntimeModel, so nothing but this comment holds them
        // together. EngineSession refuses the capture when they disagree.
        constexpr auto k_runtimeModel = std::string_view{R"toml(schema_version = 2
base_resolution = [1600, 900]
base_dpi = [144, 144]

[[ui_target]]
id = "expedition.camp"
kind = "region"

[[ui_target]]
id = "expedition.target"
kind = "control"

[[locator]]
id = "expedition.banner"
kind = "template"
asset_path = "assets/expedition-banner.png"
threshold = 1

[[locator]]
id = "expedition.march-mark"
kind = "template"
asset_path = "assets/expedition-march.png"
threshold = 1

[[binding]]
id = "expedition.camp.banner"
surface = "expedition.surface"
ui_target = "expedition.camp"
variant = "primary"
placement = { kind = "fixed", rect = [108, 92, 16, 16] }
detector = { all = [{ kind = "locator_present", locator = "expedition.banner" }], any = [], none = [] }
actions = []

[[binding]]
id = "expedition.target.primary"
surface = "expedition.surface"
ui_target = "expedition.target"
variant = "primary"
placement = { kind = "fixed", rect = [1284, 738, 16, 16], action_point = [1292, 746] }
detector = { all = [{ kind = "locator_present", locator = "expedition.march-mark" }], any = [], none = [] }
actions = [{ id = "expedition.press", kind = "click", proof_locator = "expedition.march-mark" }]

[[surface]]
id = "expedition.surface"
kind = "scene"
covers = []
identity = { all = ["expedition.camp.banner"], any = [], none = [] }
)toml"};

        // The ProjectRegistration schema this deployment pins. It stays a
        // name rather than a document: ProjectRegistrationSchemaOwner is the
        // one authority whose validator reads a layout this project chose,
        // and giving it a real schema is not part of this change.
        constexpr auto k_registrationSchemaIdentity =
            std::string_view{"arcana/expedition/registration.schema"};

        constexpr auto k_artifactRootName = std::string_view{"map"};
        constexpr auto k_artifactBytes = std::string_view{"expedition-map-bytes"};
        // JR:`JournalProvenance`, whose schema is the framework's and fixed.
        // This project exercises the branches the umbraflow fixture does not:
        // a named principal and a non-empty source_hashes.
        constexpr auto k_provenance = std::string_view{
            "{\"kind\":\"human_correction\",\"observation_ids\":[],"
            "\"principal_id\":\"expedition.witness\",\"source_hashes\":"
            "[\"7d4f3b2a19c8e6d5f4a3b2c1d0e9f8a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e1\"]}"
        };

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
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

                // Five more mutating tools, told apart by the plan this
                // project's plugin answers each of them with.
                .approvalRequiredPlanTool = "expedition.approval",

                .uiAction = task::UiActionUnderTest{
                    .surface  = "expedition.surface",
                    .uiTarget = "expedition.target",
                    .action   = "expedition.press",
                },
            };
        }

        // One solid patch of one gray, PNG-encoded: this project's crop of its
        // own screen, the way an annotation front end would have cut it.
        [[nodiscard]]
        auto patchPng(uint8 gray) -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>{};
            pixels.reserve(std::size_t{k_patch} * k_patch * 4U);
            for (auto index = uint32{0}; index < k_patch * k_patch; ++index)
            {
                pixels.emplace_back(static_cast<std::byte>(gray));
                pixels.emplace_back(static_cast<std::byte>(gray));
                pixels.emplace_back(static_cast<std::byte>(gray));
                pixels.emplace_back(std::byte{255});
            }
            auto encoded = image::encodeRgbaPng(
                "expedition-patch.png",
                k_patch,
                k_patch,
                pixels
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        // The buffer with one patch written into it. The pixels are taken by
        // value and handed back rather than mutated through a parameter: the
        // caller composes a frame by stamping one patch after another.
        [[nodiscard]]
        auto withPatch(
            std::vector<std::byte> pixels,
            uint32 left,
            uint32 top,
            uint8 gray
        ) -> std::vector<std::byte>
        {
            REQUIRE(left + k_patch <= k_frameWidth);
            REQUIRE(top + k_patch <= k_frameHeight);
            for (auto row = uint32{0}; row < k_patch; ++row)
            {
                for (auto column = uint32{0}; column < k_patch; ++column)
                {
                    auto const at = (
                        (std::size_t{top + row} * k_frameWidth)
                        + (left + column)
                    ) * 4U;
                    pixels[at]      = static_cast<std::byte>(gray);
                    pixels[at + 1U] = static_cast<std::byte>(gray);
                    pixels[at + 2U] = static_cast<std::byte>(gray);
                }
            }
            return pixels;
        }

        // This project's capture: an opaque client area of its own size,
        // carrying the two patches its locators name at the coordinates its
        // model searches.
        [[nodiscard]]
        auto probePng() -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>(
                std::size_t{k_frameWidth} * k_frameHeight * 4U,
                std::byte{0}
            );
            for (auto at = std::size_t{3}; at < pixels.size(); at += 4U)
            {
                pixels[at] = std::byte{255};
            }
            pixels = withPatch(std::move(pixels), k_bannerX, k_bannerY, k_bannerGray);
            pixels = withPatch(std::move(pixels), k_marchX, k_marchY, k_marchGray);

            auto encoded = image::encodeRgbaPng(
                "expedition-probe.png",
                k_frameWidth,
                k_frameHeight,
                pixels
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto probeFrame() -> ProjectProbeFrame
        {
            auto fingerprint = ProjectFingerprint::create(
                k_frameWidth,
                k_frameHeight,
                k_frameDpi,
                k_frameDpi
            );
            REQUIRE(fingerprint.has_value());
            return ProjectProbeFrame{
                .fingerprint = *fingerprint,
                .png         = probePng(),
            };
        }

        // The closure this model's two template locators name, cropped from the
        // capture above.
        [[nodiscard]]
        auto runtimeArtifact() -> ProjectRuntimeArtifact
        {
            auto assets = std::vector<ArtifactFile>{};
            assets.emplace_back(ArtifactFile{
                .path  = "assets/expedition-banner.png",
                .bytes = patchPng(k_bannerGray),
            });
            assets.emplace_back(ArtifactFile{
                .path  = "assets/expedition-march.png",
                .bytes = patchPng(k_marchGray),
            });
            return ProjectRuntimeArtifact{
                .model  = std::string{k_runtimeModel},
                .assets = std::move(assets),
            };
        }

        // This deployment's registration is a nested document rather than the
        // flat one this repository's own fixture writes. Only its hash binds,
        // which is the point: the Operator never reads a project's layout.
        [[nodiscard]]
        auto registrationBytes(
            ProjectIdentity const& identity,
            expedition::DeploymentBundle const& bundle,
            std::string_view pluginBytes
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
                hashOf(pluginBytes).hex(),
                hashOf(bundle.toolCatalog()).hex(),
                hashOf(expedition::k_projectObservationSchema).hex(),
                hashOf(expedition::k_toolPreconditionSchema).hex(),
                hashOf(expedition::k_projectStateSchema).hex(),
                identity.baselineEventType,
                hashOf(bundle.journalEventManifest()).hex(),
                hashOf(bundle.reconcileManifest()).hex(),
                hashOf(k_registrationSchemaIdentity).hex()
            );
        }

        [[nodiscard]]
        auto verifiedRegistration(
            ProjectIdentity const& identity,
            expedition::DeploymentBundle const& bundle,
            std::string_view pluginBytes
        ) -> VerifiedProjectRegistration
        {
            auto const exactJcs = registrationBytes(identity, bundle, pluginBytes);
            auto const claims   = ProjectRegistrationClaims{
                .manifestSchemaHash = hashOf(k_registrationSchemaIdentity),
                .pluginId           = std::string{identity.pluginId},
                .pluginHash         = hashOf(pluginBytes),
                .toolCatalogHash    = hashOf(bundle.toolCatalog()),
                .projectStateSchemaHash =
                    hashOf(expedition::k_projectStateSchema),
                .projectObservationSchemaHash =
                    hashOf(expedition::k_projectObservationSchema),
                .projectToolPreconditionSchemaHash =
                    hashOf(expedition::k_toolPreconditionSchema),
                .reconcilePayloadSchemaManifestHash =
                    hashOf(bundle.reconcileManifest()),
                .journalEventSchemaManifestHash =
                    hashOf(bundle.journalEventManifest()),
                .baselineEventType    = std::string{identity.baselineEventType},
                .projectArtifactRoots = {
                    NamedArtifactRoot{
                        .name     = std::string{k_artifactRootName},
                        .rootHash = hashOf(k_artifactBytes),
                    },
                },
            };
            auto const owner = ProjectRegistrationSchemaOwner::create(
                hashOf(k_registrationSchemaIdentity),
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
        auto makeProject(ProjectIdentity const& identity) -> ProvidedProject
        {
            auto const bundle      = expedition::DeploymentBundle{identity.pluginId};
            auto const pluginBytes = pluginSource(identity.pluginId);
            auto registration      = verifiedRegistration(
                identity,
                bundle,
                pluginBytes
            );

            auto const deployed =
                deployment::ProjectDeployment::create(bundle.sources());
            {
                auto const why = deployed.has_value()
                    ? std::string{}
                    : std::string{deployed.error().message()};
                INFO(why);
                REQUIRE(deployed.has_value());
            }

            auto observedReduce = std::make_shared<std::string>();
            auto observedDerive = std::make_shared<std::string>();

            auto schemaOwner = ProjectSchemaOwner::create(
                registration,
                ProjectDocumentSchemaBytes{
                    .projectState       = expedition::k_projectStateSchema,
                    .projectObservation = expedition::k_projectObservationSchema,
                    .toolPrecondition   = expedition::k_toolPreconditionSchema,
                },
                deployment::canonicalJsonValidator(),
                // The deployment's own document validator, with the two
                // envelopes the suite asserts against recorded on the way in.
                // Recording is this provider's; deciding is the deployment's.
                [validate = deployed->documentValidator(),
                 observedReduce,
                 observedDerive](
                    ProjectPluginFunction function,
                    ProjectDocumentDirection direction,
                    std::string_view candidateJcs
                ) -> Status
                {
                    if (direction == ProjectDocumentDirection::Input)
                    {
                        if (function == ProjectPluginFunction::Reduce)
                        {
                            *observedReduce = std::string{candidateJcs};
                        }
                        if (function == ProjectPluginFunction::Derive)
                        {
                            *observedDerive = std::string{candidateJcs};
                        }
                    }
                    return validate(function, direction, candidateJcs);
                }
            );
            REQUIRE(schemaOwner.has_value());

            auto journalSchemaOwner = ProjectJournalSchemaOwner::create(
                registration,
                bundle.journalEventManifest(),
                deployed->journalPayloadValidator()
            );
            REQUIRE(journalSchemaOwner.has_value());

            auto toolCatalogSchemaOwner = ProjectToolCatalogSchemaOwner::create(
                registration,
                bundle.toolCatalog(),
                deployed->toolCatalogValidator()
            );
            REQUIRE(toolCatalogSchemaOwner.has_value());

            auto reconcileSchemaOwner = ProjectReconcileSchemaOwner::create(
                registration,
                bundle.reconcileManifest(),
                deployed->reconcileDispositionReader()
            );
            REQUIRE(reconcileSchemaOwner.has_value());

            auto artifactBlobs = std::vector<ProjectPluginRegistrar::ArtifactBlob>{};
            artifactBlobs.emplace_back(ProjectPluginRegistrar::ArtifactBlob{
                .name  = std::string{k_artifactRootName},
                .bytes = std::string{k_artifactBytes},
            });

            return ProvidedProject{
                .registration           = std::move(registration),
                .schemaOwner            = *std::move(schemaOwner),
                .journalSchemaOwner     = *std::move(journalSchemaOwner),
                .toolCatalogSchemaOwner = *std::move(toolCatalogSchemaOwner),
                .reconcileSchemaOwner   = *std::move(reconcileSchemaOwner),
                .pluginBytes            = pluginBytes,
                .artifactBlobs          = std::move(artifactBlobs),
                .runtimeArtifact        = runtimeArtifact(),
                .probeFrame             = probeFrame(),
                .lastReduceInput        = std::move(observedReduce),
                .lastDeriveInput        = std::move(observedDerive),
                .vocabulary             = vocabularyOf(identity),
            };
        }
    }

    auto provideProject(ProjectRole role) -> ProvidedProject
    {
        constexpr auto expeditionIdentity = ProjectIdentity{
            .pluginId          = "arcana.expedition",
            .baselineEventType = "expedition.founded",
            .eventNamespace    = "expedition",
        };
        constexpr auto rivalIdentity = ProjectIdentity{
            .pluginId          = "arcana.rival",
            .baselineEventType = "expedition.founded",
            .eventNamespace    = "expedition",
        };
        return makeProject(
            role == ProjectRole::UnderTest ? expeditionIdentity : rivalIdentity
        );
    }
}
