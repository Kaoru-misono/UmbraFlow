#include "tool-invocation.hpp"

#include "evidence-store.hpp"
#include "snapshot-reference.hpp"

#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_auditTool = std::string_view{
            "framework.audit.record"
        };
        constexpr auto k_inputClickTool = std::string_view{
            "framework.input.click"
        };
        constexpr auto k_inputDragTool = std::string_view{
            "framework.input.drag"
        };
        constexpr auto k_inputHoldTool = std::string_view{
            "framework.input.hold"
        };
        constexpr auto k_inputKeyTool = std::string_view{
            "framework.input.key"
        };
        constexpr auto k_inputMoveTool = std::string_view{
            "framework.input.move"
        };
        constexpr auto k_inputScrollTool = std::string_view{
            "framework.input.scroll"
        };
        constexpr auto k_uiClickTool = std::string_view{"framework.ui.click"};
        constexpr auto k_uiDragTool = std::string_view{"framework.ui.drag"};
        constexpr auto k_uiHoldTool = std::string_view{"framework.ui.hold"};
        constexpr auto k_uiKeyTool = std::string_view{"framework.ui.key"};
        constexpr auto k_uiMoveTool = std::string_view{"framework.ui.move"};
        constexpr auto k_uiScrollTool = std::string_view{"framework.ui.scroll"};
        constexpr auto k_observeTool = std::string_view{
            "framework.screen.observe"
        };
        constexpr auto k_captureTool = std::string_view{
            "framework.screen.capture"
        };
        constexpr auto k_censusGridTool = std::string_view{
            "framework.screen.census_grid"
        };
        constexpr auto k_cropTool = std::string_view{
            "framework.screen.crop"
        };
        constexpr auto k_matchShapesTool = std::string_view{
            "framework.screen.match_shapes"
        };
        constexpr auto k_probeTool = std::string_view{
            "framework.screen.probe"
        };
        constexpr auto k_readLinesTool = std::string_view{
            "framework.screen.read_lines"
        };
        constexpr auto k_readSingleLineTool = std::string_view{
            "framework.screen.read_single_line"
        };
        constexpr auto k_projectReadTextTool = std::string_view{
            "framework.project.read_text"
        };
        constexpr auto k_projectWriteFileTool = std::string_view{
            "framework.project.write_file"
        };
        constexpr auto k_projectWriteTextTool = std::string_view{
            "framework.project.write_text"
        };
        constexpr auto k_sessionGetTool = std::string_view{
            "framework.session.get"
        };
        constexpr auto k_sessionListTool = std::string_view{
            "framework.session.list"
        };
        constexpr auto k_sessionSetTool = std::string_view{
            "framework.session.set"
        };
        constexpr auto k_nowTool = std::string_view{
            "framework.workflow.now"
        };
        constexpr auto k_statusTool = std::string_view{
            "framework.workflow.status"
        };
        constexpr auto k_waitTool = std::string_view{
            "framework.workflow.wait"
        };

        // The one effect a Framework input Tool proposes. Framework owns this
        // vocabulary the way a Project owns its own: the type and scope are
        // spelled here, and their bytes reach tool_catalog_hash through the
        // descriptor material, so widening either moves the catalog identity.
        //
        constexpr auto k_inputEffectScope = std::string_view{
            "controlled_target"
        };

        // THE ONE AUTHORING-WRITE EFFECT LINE. Its effect is a change to the
        // PROJECT'S OWN STATE, its scope is that project's unsealed in-progress
        // generation's authoring store, and its containment is the seal itself:
        // the install door and the bind door both refuse an unsealed hash by
        // name, so an authoring write cannot leak into production admission and
        // no extra containment layer is invented here
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md
        // V4).
        //
        constexpr auto k_authoringEffectScope = std::string_view{
            "project_authoring_store"
        };

        constexpr auto k_frameworkToolVersion = std::string_view{"1"};

        constexpr auto k_maximumObserveMillis = uint64{60'000U};
        constexpr auto k_maximumCaptureMillis = uint64{10'000U};
        constexpr auto k_maximumMeasureMillis = uint64{10'000U};
        constexpr auto k_maximumProjectMillis = uint64{10'000U};
        constexpr auto k_maximumSessionMillis = uint64{1'000U};
        constexpr auto k_maximumWaitMillis = uint64{60'000U};
        constexpr auto k_maximumAuditMillis = uint64{1'000U};
        constexpr auto k_maximumNowMillis = uint64{1'000U};
        constexpr auto k_maximumStatusMillis = uint64{1'000U};
        constexpr auto k_maximumInputMillis = uint64{15'000U};
        constexpr auto k_maximumHoldMillis = uint64{75'000U};

        // The wall clock framework.workflow.wait is judged against, and the one
        // Framework Tool whose timeout ceiling cannot be its workflow ceiling.
        // A wait that sleeps for exactly k_maximumWaitMillis returns a little
        // after it, so a timeout ceiling equal to the longest admitted wait
        // would refuse the longest legal call every time. The headroom is the
        // Framework's to choose because framework.* Tools are the Framework's
        // own declaration; a Project's ceilings stay the Project's.
        constexpr auto k_waitTimeoutMillis = uint64{66'000U};

        // The two identity domain tags. They are constants rather than literals
        // inside their builders because the protocol material renders the exact
        // preimage bytes: one spelling reaches both, so a tag bumped for a
        // preimage change cannot move the identity without moving the material.
        constexpr auto k_rootPreimageTag = std::string_view{
            "umbraflow-internal-tool-root-v0"
        };
        constexpr auto k_callPreimageTag = std::string_view{
            "umbraflow-internal-tool-call-v1"
        };
        constexpr auto k_maximumCallerIdentityBytes = std::size_t{256U};

        auto appendIdentityPart(
            std::string& material,
            std::string_view value
        ) -> void
        {
            material += std::to_string(value.size());
            material.push_back(':');
            material += value;
        }

        auto appendIdentityHash(
            std::string& material,
            ContentHash const& hash
        ) -> void
        {
            appendIdentityPart(material, hash.toString());
        }

        struct AppendProviderIdentity final
        {
            std::string& material;

            auto operator()(FrameworkToolProvider const& provider) const -> void
            {
                appendIdentityPart(material, "framework");
                appendIdentityHash(material, provider.toolCatalogHash);
            }

            auto operator()(ProjectToolProvider const& provider) const -> void
            {
                appendIdentityPart(material, "project");
                appendIdentityHash(material, provider.projectRegistrationHash);
                appendIdentityHash(material, provider.toolCatalogHash);
            }
        };

        [[nodiscard]]
        auto rootIdentityMaterial(
            CallerIdempotencyNamespace const& callerNamespace,
            RootRequestKey const& requestKey,
            CanonicalJson const& requestPreimage
        ) -> std::string
        {
            auto material = std::string{k_rootPreimageTag};
            appendIdentityPart(material, callerNamespace.value());
            appendIdentityPart(material, requestKey.value());
            appendIdentityHash(material, requestPreimage.contentHash());
            return material;
        }

        // The preimage moved to v1 when the root run became a real positioned
        // call: every position now names a durable parent coordinate, so the
        // child/top-level discriminator and the conditional parent hash are
        // gone and the parent is appended unconditionally.
        [[nodiscard]]
        auto callIdentityMaterial(
            ContentHash const& rootIdentity,
            ContentHash const& parentIdentity,
            uint32 sequence,
            ToolExecutionIdentity const& executionIdentity,
            ValidatedToolInvocation const& invocation,
            std::optional<ContentHash> const& observationReference
        ) -> std::string
        {
            auto material = std::string{k_callPreimageTag};
            appendIdentityHash(material, rootIdentity);
            appendIdentityHash(material, parentIdentity);
            appendIdentityPart(material, std::to_string(sequence));
            appendIdentityHash(material, executionIdentity.runIdentity);
            appendIdentityHash(
                material,
                executionIdentity.frameworkReleaseIdentity
            );
            appendIdentityHash(
                material,
                executionIdentity.toolRuntimeProtocolIdentity
            );
            appendIdentityHash(material, executionIdentity.environmentIdentity);
            std::visit(AppendProviderIdentity{material}, invocation.provider());
            appendIdentityPart(material, invocation.toolName());
            appendIdentityPart(material, invocation.descriptor().toolVersion);
            appendIdentityHash(material, invocation.canonicalArgs().contentHash());
            appendIdentityPart(
                material,
                observationReference.has_value() ? "observed" : "unobserved"
            );
            if (observationReference)
            {
                appendIdentityHash(material, *observationReference);
            }
            return material;
        }

        using FrameworkSchemaMaterial = json::Value (*)();

        // Result rather than a plain descriptor: a mutating Framework Tool
        // declares an effect bound, and an effect bound names the sha256 of a
        // payload schema. Deriving that digest can fail, and a factory that
        // could not say so would have to invent a hash.
        using FrameworkDescriptorFactory = Result<ToolDescriptor> (*)();

        // One Framework Tool's whole published declaration.
        //
        // There is no hand-written argument validator beside the schema. The
        // schema IS the argument contract and json::Schema is what judges a
        // call against it -- the same evaluator a Project's inline
        // argument_schema is compiled through -- so one dialect answers for
        // both catalogs and there is no second spelling of the same rule that
        // could drift from the published one.
        struct FrameworkToolDefinition final
        {
            std::string_view           name{};

            // What a model that has never seen this repository needs in order
            // to call the Tool and to CHAIN it: what the call does, what a
            // confirmed result carries, which member came from which other
            // Tool, and any reference semantics it has to respect. It is
            // written per Tool and never generated from the name -- a
            // description derived from the name carries no information the name
            // did not already carry.
            std::string_view           description{};

            FrameworkDescriptorFactory descriptor{};
            FrameworkSchemaMaterial    argumentMaterial{};
            FrameworkSchemaMaterial    outputMaterial{};
        };

        // A schema refusal, in the vocabulary every other Tool refusal speaks.
        // json::ErrorKind separates "this evaluator cannot apply the schema"
        // from "this document fails it", which is a real distinction for a
        // Project's own declaration; here it is not, because the schema is the
        // Framework's own and compiled at startup, so the only refusal a caller
        // can provoke is about its arguments.
        [[nodiscard]]
        auto adoptSchemaRefusal(Status outcome, std::string_view toolName)
            -> Status
        {
            if (outcome.has_value())
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::string{toolName} + " arguments: "
                    + std::string{outcome.error().message()}
            );
        }

        // What one member of a Framework Tool's argument contract is allowed to
        // be. The kinds are named for what the value MEANS rather than for its
        // JSON type, because two integers can mean different things: a surface
        // pixel is an index into a frame and cannot be negative, while a notch
        // count is a direction as much as a magnitude.
        //
        // Where a bound belongs to a delivery layer it is NOT restated here.
        // The notch range is controller::WheelDelta's and the travel ceiling is
        // task::k_maxDragTravel, each refused once by the layer that owns it;
        // a second copy inside this contract would be a second answer to one
        // question, and only one of the two would be inside tool_catalog_hash.
        enum class ArgumentMemberKind : uint8
        {
            KeyName,
            SurfacePixel,
            Milliseconds,
            SignedCount,
            ColourChannel,
            Count,
            Flag,
            Name,
            ScreenReturn,
            Text,
            Sha256,
            JsonObject,
            JsonValue,
        };

        struct ArgumentMember final
        {
            std::string_view   name{};
            ArgumentMemberKind kind{ArgumentMemberKind::Name};

            // The member's own description, published inside its `properties`
            // entry. It is written once per member name rather than once per
            // Tool, because a member name means the same thing in every
            // contract that carries it; where one Tool needs a bound of its own
            // -- the wait ceiling is the only one -- that Tool states its
            // property itself.
            std::string_view   description{};
        };

        // Every member any Framework Tool's contract may carry, spelled once,
        // in UTF-8 order of the names.
        constexpr auto k_argumentMembers = std::array{
            ArgumentMember{
                "action",
                ArgumentMemberKind::Name,
                "Action identifier the named observation resolved for this "
                "ui_target and binding.",
            },
            ArgumentMember{
                "binding",
                ArgumentMemberKind::Name,
                "Binding identifier the named observation resolved on this "
                "frame.",
            },
            ArgumentMember{
                "cell_height",
                ArgumentMemberKind::SurfacePixel,
                "Height of one census cell, in screenshot pixels.",
            },
            ArgumentMember{
                "cell_width",
                ArgumentMemberKind::SurfacePixel,
                "Width of one census cell, in screenshot pixels.",
            },
            ArgumentMember{
                "colour_blue",
                ArgumentMemberKind::ColourChannel,
                "Blue channel of the colour to match, 0 to 255.",
            },
            ArgumentMember{
                "colour_green",
                ArgumentMemberKind::ColourChannel,
                "Green channel of the colour to match, 0 to 255.",
            },
            ArgumentMember{
                "colour_red",
                ArgumentMemberKind::ColourChannel,
                "Red channel of the colour to match, 0 to 255.",
            },
            ArgumentMember{
                "content",
                ArgumentMemberKind::Text,
                "UTF-8 text to write into the project authoring store.",
            },
            ArgumentMember{
                "duration_ms",
                ArgumentMemberKind::Milliseconds,
                "How long the input stays engaged, in whole milliseconds, "
                "before it is released.",
            },
            ArgumentMember{
                k_fileSha256Member,
                ArgumentMemberKind::Sha256,
                "Lowercase sha256 digest of a retained evidence artifact, as "
                "returned in the screenshot_sha256 member of "
                "framework.screen.capture or framework.screen.crop.",
            },
            ArgumentMember{
                "height",
                ArgumentMemberKind::SurfacePixel,
                "Height of the rectangle, in screenshot pixels.",
            },
            ArgumentMember{
                "key",
                ArgumentMemberKind::KeyName,
                "Canonical name of the key to press, for example escape, "
                "enter, f1 or a.",
            },
            ArgumentMember{
                "name",
                ArgumentMemberKind::Name,
                "Name of one entry in this session's own state. It is the "
                "caller's vocabulary and nothing the framework reads into: any "
                "non-empty string is a name, and the same one always addresses "
                "the same entry.",
            },
            ArgumentMember{
                "notches",
                ArgumentMemberKind::SignedCount,
                "Signed wheel-detent count. The sign is the direction: "
                "positive scrolls one way and negative the other.",
            },
            ArgumentMember{
                k_observationReferenceArgument,
                ArgumentMemberKind::JsonObject,
                "The observation handle framework.screen.observe returned in "
                "its observation_reference member, passed back unchanged. It "
                "is SINGLE USE and it expires: this call spends it, and a "
                "further action needs a further observe.",
            },
            ArgumentMember{
                "path",
                ArgumentMemberKind::Name,
                "Project-relative path inside this project's own authoring "
                "store.",
            },
            ArgumentMember{
                "record",
                ArgumentMemberKind::JsonObject,
                "The object to record. Its exact canonical bytes are the whole "
                "of what this call recorded.",
            },
            ArgumentMember{
                "removes",
                ArgumentMemberKind::Flag,
                "Select the pixels that do NOT lie within tolerance of the "
                "colour, rather than those that do.",
            },
            ArgumentMember{
                "return_screen",
                ArgumentMemberKind::ScreenReturn,
                "What to answer with about the screen while the input is still "
                "engaged: none for nothing, capture for a screenshot receipt, "
                "observe for a fully resolved observation.",
            },
            ArgumentMember{
                k_screenshotSha256Member,
                ArgumentMemberKind::Sha256,
                "Lowercase sha256 digest of a retained screenshot artifact, as "
                "returned in the screenshot_sha256 member of "
                "framework.screen.capture or framework.screen.crop. A digest "
                "whose artifact has expired or been reclaimed is refused by "
                "name.",
            },
            ArgumentMember{
                "to_x",
                ArgumentMemberKind::SurfacePixel,
                "X coordinate the drag travels to, in screenshot pixels from "
                "the screenshot's left edge.",
            },
            ArgumentMember{
                "to_y",
                ArgumentMemberKind::SurfacePixel,
                "Y coordinate the drag travels to, in screenshot pixels from "
                "the screenshot's top edge.",
            },
            ArgumentMember{
                "tolerance",
                ArgumentMemberKind::Count,
                "Inclusive per-channel tolerance for the colour match.",
            },
            ArgumentMember{
                "travel_ms",
                ArgumentMemberKind::Milliseconds,
                "How long the drag takes to travel from its start to its "
                "destination, in whole milliseconds.",
            },
            ArgumentMember{
                "ui_target",
                ArgumentMemberKind::Name,
                "UiTarget identifier the named observation resolved on this "
                "frame.",
            },
            ArgumentMember{
                "value",
                ArgumentMemberKind::JsonValue,
                "The value to store, as any JSON the caller passes -- an "
                "object, an array, a string, a number or a boolean. Its exact "
                "canonical bytes are stored and returned unchanged.",
            },
            ArgumentMember{
                "width",
                ArgumentMemberKind::SurfacePixel,
                "Width of the rectangle, in screenshot pixels.",
            },
            ArgumentMember{
                "x",
                ArgumentMemberKind::SurfacePixel,
                "X coordinate in screenshot pixels, from the screenshot's left "
                "edge.",
            },
            ArgumentMember{
                "y",
                ArgumentMemberKind::SurfacePixel,
                "Y coordinate in screenshot pixels, from the screenshot's top "
                "edge.",
            },
        };

        constexpr auto k_inputClickMembers = std::array{
            k_screenshotSha256Member,
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_inputDragMembers = std::array{
            k_screenshotSha256Member,
            std::string_view{"to_x"},
            std::string_view{"to_y"},
            std::string_view{"travel_ms"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_inputHoldMembers = std::array{
            std::string_view{"duration_ms"},
            k_screenshotSha256Member,
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_inputKeyMembers = std::array{
            std::string_view{"key"},
            k_screenshotSha256Member,
        };
        constexpr auto k_inputMoveMembers = std::array{
            k_screenshotSha256Member,
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_inputScrollMembers = std::array{
            std::string_view{"notches"},
            k_screenshotSha256Member,
        };
        constexpr auto k_uiActionMembers = std::array{
            std::string_view{"action"},
            std::string_view{"binding"},
            k_observationReferenceArgument,
            std::string_view{"ui_target"},
        };

        // The exact member list one measuring or authoring Tool requires. Every
        // one is REQUIRED: a Framework argument contract is a closed object, so
        // an absent member is a malformed call rather than a defaulted one.
        //
        // Declared in UTF-8 order of the member names, so the rendered contract
        // material is already sorted.
        constexpr auto k_screenshotMembers = std::array{
            k_screenshotSha256Member,
        };
        constexpr auto k_screenshotRectangleMembers = std::array{
            std::string_view{"height"},
            k_screenshotSha256Member,
            std::string_view{"width"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_probeMembers = std::array{
            std::string_view{"colour_blue"},
            std::string_view{"colour_green"},
            std::string_view{"colour_red"},
            std::string_view{"height"},
            std::string_view{"removes"},
            k_screenshotSha256Member,
            std::string_view{"tolerance"},
            std::string_view{"width"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_censusGridMembers = std::array{
            std::string_view{"cell_height"},
            std::string_view{"cell_width"},
            std::string_view{"colour_blue"},
            std::string_view{"colour_green"},
            std::string_view{"colour_red"},
            std::string_view{"height"},
            std::string_view{"removes"},
            k_screenshotSha256Member,
            std::string_view{"tolerance"},
            std::string_view{"width"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_projectReadTextMembers = std::array{
            std::string_view{"path"},
        };
        constexpr auto k_projectWriteFileMembers = std::array{
            k_fileSha256Member,
            std::string_view{"path"},
        };
        constexpr auto k_projectWriteTextMembers = std::array{
            std::string_view{"content"},
            std::string_view{"path"},
        };
        constexpr auto k_sessionEntryMembers = std::array{
            std::string_view{"name"},
        };
        constexpr auto k_sessionSetMembers = std::array{
            std::string_view{"name"},
            std::string_view{"value"},
        };

        [[nodiscard]]
        auto argumentMember(std::string_view member) -> ArgumentMember const&
        {
            auto const found = std::ranges::find(
                k_argumentMembers,
                member,
                &ArgumentMember::name
            );
            // Every name a contract lists is in the table above; a contract
            // naming anything else would not compile past the table it was
            // written beside.
            UF_CHECK(found != k_argumentMembers.end());
            return *found;
        }

        // One member's published shape, in JSON Schema Draft 2020-12 keywords
        // and no others. There is deliberately no private spelling here --
        // `additionalProperties`, not `additional_properties`; `minLength`, not
        // `min_length` -- because a caller reads this with an ordinary schema
        // reader or not at all, and a dialect only this repository understands
        // is a contract only this repository can check.
        [[nodiscard]]
        auto memberSchema(ArgumentMember const& member) -> json::Value
        {
            auto fields = std::vector<json::Member>{
                {"description",
                 json::Value::ofString(std::string{member.description})},
            };
            switch (member.kind)
            {
            case ArgumentMemberKind::KeyName:
            case ArgumentMemberKind::Name:
                fields.emplace_back("minLength", json::Value::ofNumber(1.0));
                fields.emplace_back("type", json::Value::ofString("string"));
                break;
            case ArgumentMemberKind::Text:
                fields.emplace_back("type", json::Value::ofString("string"));
                break;
            case ArgumentMemberKind::ScreenReturn:
                fields.emplace_back(
                    "default",
                    json::Value::ofString("none")
                );
                fields.emplace_back(
                    "enum",
                    json::Value::ofArray({
                        json::Value::ofString("none"),
                        json::Value::ofString("capture"),
                        json::Value::ofString("observe"),
                    })
                );
                fields.emplace_back("type", json::Value::ofString("string"));
                break;
            case ArgumentMemberKind::Sha256:
                fields.emplace_back(
                    "pattern",
                    json::Value::ofString("^[0-9a-f]{64}$")
                );
                fields.emplace_back("type", json::Value::ofString("string"));
                break;
            case ArgumentMemberKind::SurfacePixel:
                fields.emplace_back(
                    "maximum",
                    json::Value::ofNumber(
                        static_cast<double>(std::numeric_limits<uint32>::max())
                    )
                );
                fields.emplace_back("minimum", json::Value::ofNumber(0.0));
                fields.emplace_back("type", json::Value::ofString("integer"));
                break;
            case ArgumentMemberKind::Milliseconds:
            case ArgumentMemberKind::Count:
                fields.emplace_back("minimum", json::Value::ofNumber(0.0));
                fields.emplace_back("type", json::Value::ofString("integer"));
                break;
            case ArgumentMemberKind::SignedCount:
                fields.emplace_back("type", json::Value::ofString("integer"));
                break;
            case ArgumentMemberKind::ColourChannel:
                fields.emplace_back("maximum", json::Value::ofNumber(255.0));
                fields.emplace_back("minimum", json::Value::ofNumber(0.0));
                fields.emplace_back("type", json::Value::ofString("integer"));
                break;
            case ArgumentMemberKind::Flag:
                fields.emplace_back("type", json::Value::ofString("boolean"));
                break;
            case ArgumentMemberKind::JsonObject:
                fields.emplace_back("type", json::Value::ofString("object"));
                break;
            case ArgumentMemberKind::JsonValue:
                // Every JSON type BUT null, spelled out rather than left to an
                // absent `type` keyword: a caller filling this call in reads
                // the properties entry, and one that says only what the member
                // is for has told it nothing about what it may send.
                //
                // Null is left out because this contract already spells absence
                // once. A stored null and a name that was never stored would be
                // the same answer read two ways, and the reading Tool's
                // `present` is the one that survives.
                fields.emplace_back(
                    "type",
                    json::Value::ofArray({
                        json::Value::ofString("array"),
                        json::Value::ofString("boolean"),
                        json::Value::ofString("number"),
                        json::Value::ofString("object"),
                        json::Value::ofString("string"),
                    })
                );
                break;
            }
            return json::Value::ofObject(std::move(fields));
        }

        // A Framework Tool's whole argument contract: a closed object, the
        // members it requires, the members it admits, and one `properties`
        // entry per member carrying that member's type, its bounds and its own
        // description.
        //
        // The object is closed because a Framework argument contract is a
        // closed object: a member nobody declared is a malformed call rather
        // than one this Tool ignores.
        [[nodiscard]]
        auto argumentMaterial(
            std::span<std::string_view const> required,
            std::span<std::string_view const> optional = {}
        ) -> json::Value
        {
            auto properties  = std::vector<json::Member>{};
            auto requiredIds = std::vector<json::Value>{};
            properties.reserve(required.size() + optional.size());
            requiredIds.reserve(required.size());
            for (auto const member : required)
            {
                requiredIds.emplace_back(
                    json::Value::ofString(std::string{member})
                );
                properties.emplace_back(
                    std::string{member},
                    memberSchema(argumentMember(member))
                );
            }
            for (auto const member : optional)
            {
                properties.emplace_back(
                    std::string{member},
                    memberSchema(argumentMember(member))
                );
            }

            auto material = std::vector<json::Member>{
                {"additionalProperties", json::Value::ofBoolean(false)},
                {"type", json::Value::ofString("object")},
            };
            // A Tool that takes no arguments states neither keyword. An empty
            // `properties` and an empty `required` say nothing that
            // `additionalProperties: false` on an object has not already said,
            // and a reader would have to decide which of the three it was
            // meant to believe.
            if (!properties.empty())
            {
                material.emplace_back(
                    "properties",
                    json::Value::ofObject(std::move(properties))
                );
            }
            if (!requiredIds.empty())
            {
                material.emplace_back(
                    "required",
                    json::Value::ofArray(std::move(requiredIds))
                );
            }
            return json::Value::ofObject(std::move(material));
        }

        // The payload shape a Framework input effect carries. It is rendered
        // here rather than read from a file so that the Framework owns its own
        // effect schema exactly the way it owns its argument contracts, and so
        // that its digest is derived from material already inside
        // tool_catalog_hash.
        [[nodiscard]]
        auto inputEffectPayloadMaterial() -> json::Value
        {
            // These bytes are the PREIMAGE of an effect bound's
            // payload_schema_hash, not a schema anything compiles. They still
            // spell their keywords the standard way, because a reader meeting
            // two spellings of one keyword in one file cannot tell which is the
            // real one.
            return json::Value::ofObject({
                {"additionalProperties", json::Value::ofBoolean(false)},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("controlled_target_id"),
                     json::Value::ofString("frame_identity_hash"),
                     json::Value::ofString("ui_action"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto inputEffectPayloadSchemaHash() -> Result<ContentHash>
        {
            auto const bytes = json::canonicalBytes(
                inputEffectPayloadMaterial()
            );
            return sha256(std::as_bytes(std::span{bytes}));
        }

        // The bound each Framework input Tool declares. The effect type is the
        // Tool name itself, so a grant for move cannot admit click or key.
        //
        // ONE EFFECT LINE FOR INPUT INJECTION: the effect is a change to the
        // external world, its scope is the target surface the registration
        // declares and no wider, and its risk is the highest band there is
        // because an input that landed is external and irreversible
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
        //
        // Risk is no longer split by how the point was aimed. That split said a
        // resolved target is a smaller blast radius than a bare coordinate, and
        // it is not: a click delivered at a declared target and a click
        // delivered at a measured pixel are the same click to the world. What
        // separates the two Tools is the vocabulary they are stated in, which
        // ToolSurface already carries, and what an aim is worth is judged by
        // the observation authority rather than by a number in a bound.
        //
        // The scope is enforced rather than merely declared: an input aimed off
        // the target surface this registration declares is refused before
        // anything is captured or posted, naming the surface it was outside of.
        [[nodiscard]]
        auto inputEffectBounds(std::string_view toolName)
            -> Result<std::vector<EffectBound>>
        {
            UF_TRY_VALUE(payloadSchemaHash, inputEffectPayloadSchemaHash());
            auto bounds = std::vector<EffectBound>{};
            bounds.emplace_back(EffectBound{
                .namespacedType    = std::string{toolName},
                .scopeKind         = std::string{k_inputEffectScope},
                .payloadSchemaHash = payloadSchemaHash,
                .maximumRisk       = Risk::Critical,
            });
            return bounds;
        }

        [[nodiscard]]
        auto observeDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumObserveMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        [[nodiscard]]
        auto captureDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumCaptureMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // The measuring and crop Tools. Each is PRIVILEGED because its arguments
        // and its answer are the machine's vocabulary -- a rectangle of pixels,
        // a colour channel -- and never the project's; that label is what an
        // Operator's privileged_surface_tools list judges at admission. Every
        // one names its immutable screenshot explicitly, so no call position or
        // live frame scope participates in resolution.
        //
        // ReadOnly with no effect bound: a measurement changes nothing outside
        // the Operator, so it proposes no mutation and no policy is consulted.
        [[nodiscard]]
        auto measuringDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumMeasureMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Privileged,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Reading the project's own authoring store. Semantic, because a path
        // inside the project is the project's own vocabulary rather than the
        // machine's, and ReadOnly with no effect bound, so a session under
        // deny-all may read back what it wrote. What it may not do is write --
        // that is the Tool below, and it is where the grant is required.
        [[nodiscard]]
        auto projectReadTextDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumProjectMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        [[nodiscard]]
        auto authoringEffectPayloadMaterial() -> json::Value
        {
            // Preimage bytes, for the reason inputEffectPayloadMaterial states.
            return json::Value::ofObject({
                {"additionalProperties", json::Value::ofBoolean(false)},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("controlled_target_id"),
                     json::Value::ofString("project_path"),
                     json::Value::ofString("written_content_hash"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto authoringEffectBounds(std::string_view toolName)
            -> Result<std::vector<EffectBound>>
        {
            auto const bytes = json::canonicalBytes(
                authoringEffectPayloadMaterial()
            );
            UF_TRY_VALUE(
                payloadSchemaHash,
                sha256(std::as_bytes(std::span{bytes}))
            );
            auto bounds = std::vector<EffectBound>{};
            bounds.emplace_back(EffectBound{
                .namespacedType    = std::string{toolName},
                .scopeKind         = std::string{k_authoringEffectScope},
                .payloadSchemaHash = payloadSchemaHash,
                // Medium rather than the input line's Critical, and the
                // difference is what the seal buys: a write into an unsealed
                // authoring store reaches no production admission and no
                // external world, so it is reversible by the project that owns
                // it. It is not read_only either -- bytes on the operator's
                // disk changed.
                .maximumRisk       = Risk::Medium,
            });
            return bounds;
        }

        [[nodiscard]]
        auto projectWriteDescriptor(
            std::string_view toolName,
            ToolSurface surface
        ) -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, authoringEffectBounds(toolName));
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumProjectMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability = ToolMutability::Mutating,
                .surface    = surface,
                // A second write of the same bytes to the same path leaves the
                // store where the first left it, which is what delivery-safe
                // means: redelivering costs nothing beyond the write.
                .idempotency = ToolIdempotency::DeliverySafe,
            };
        }

        [[nodiscard]]
        auto projectWriteFileDescriptor() -> Result<ToolDescriptor>
        {
            return projectWriteDescriptor(
                k_projectWriteFileTool,
                ToolSurface::Privileged
            );
        }

        [[nodiscard]]
        auto projectWriteTextDescriptor() -> Result<ToolDescriptor>
        {
            return projectWriteDescriptor(
                k_projectWriteTextTool,
                ToolSurface::Semantic
            );
        }

        // THIS SESSION'S OWN STATE, and the whole of why it is three Tools
        // rather than a writable environment. A chunk runs in a VM that is
        // built for it and destroyed after it, so a global it assigns is gone
        // before the next chunk compiles; what survives a chunk is what the
        // HOST owns. Making the environment table writable would only move the
        // problem, because the table dies with the VM as well -- and a
        // per-session table that did survive would be state no name, no schema
        // and no ledger row describes, which is the ambient authority
        // docs/ARCHITECTURE.md refuses project code. A Tool is the shape that
        // is already named, described, schema'd, admitted and recorded.
        //
        // ReadOnly with no effect bound, on exactly the terms
        // framework.audit.record is: read-only here means no external-world
        // effect requiring plan authority and approval grants, and a store that
        // dies with the process reaches no world at all. So a session under the
        // deny-all artifact can still remember what it is doing, which is the
        // point -- an annotator forced to re-derive its state every chunk would
        // be no better off than one holding it in a single enormous chunk.
        //
        // Semantic, because a name and a value are the caller's own vocabulary
        // rather than the machine's; nothing here is a pixel, a receipt or a
        // key code.
        [[nodiscard]]
        auto sessionReadDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumSessionMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // DeliverySafe rather than ReadSafe, which is the one thing that
        // separates this descriptor from the reading one: redelivering a store
        // costs exactly the store, because the same name and the same bytes
        // leave the map where the first write left it -- the same reading
        // framework.project.write_text's idempotency has of writing one path
        // twice.
        [[nodiscard]]
        auto sessionSetDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumSessionMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::DeliverySafe,
            };
        }

        // Reading the Operator's wall clock. Semantic and ReadOnly with no
        // effect bound, for status's reasons: it observes no frame, spends no
        // observation and changes nothing outside the Operator, so no policy
        // is consulted.
        //
        // WHY THE CLOCK IS A TOOL AND NOT A VM GLOBAL. The sandbox nils
        // os.time, os.clock and os.date so that a pure-data VM carries no
        // nondeterministic source, and that floor stands. What a Tool call
        // adds is the durable record: the instant this call read enters the
        // call's terminal outcome, and a replayed position answers the
        // recorded instant rather than a fresh one, because only a Tool call
        // short-circuits on replay. A clock reached through a global would
        // tick again on every restart and nothing would record what it said.
        [[nodiscard]]
        auto nowDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumNowMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Run and call-tree status. It observes no frame and spends no
        // observation, so its limits admit neither.
        [[nodiscard]]
        auto statusDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumStatusMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Per ruling R5 this is a genuine catalog Tool and not an Operator-side
        // append: the scoped SDK exposes no native primitive except by making a
        // normal Tool call, and an append outside the Tool boundary would
        // re-append on every deterministic restart because only Tool calls
        // short-circuit on replay.
        //
        // ReadOnly is the correct classification even though the call persists
        // a durable record, for the same reason framework.screen.observe is
        // read-only while persisting a durable snapshot reference: read-only
        // here means no external-world effect requiring plan authority and
        // approval grants. It therefore declares no effect bound and needs no
        // OperatorPolicyAuthority, while still spending Tool-call budget.
        [[nodiscard]]
        auto auditDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumAuditMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // One verb per Tool. Raw input is privileged machine vocabulary;
        // semantic input is observation-bound project vocabulary. Hold alone
        // admits one internal screen return after its declared dwell.
        [[nodiscard]]
        auto inputDescriptor(
            std::string_view toolName,
            ToolSurface surface,
            bool hold
        ) -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, inputEffectBounds(toolName));
            auto const maximumElapsedMillis = hold
                ? k_maximumHoldMillis
                : k_maximumInputMillis;
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {std::string{toolName}},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = maximumElapsedMillis,
                    .onTimeout            = TimeoutAction::Reobserve,
                },
                .mutability  = ToolMutability::Mutating,
                .surface     = surface,
                .idempotency = ToolIdempotency::NonIdempotent,
            };
        }

        [[nodiscard]] auto inputClickDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_inputClickTool, ToolSurface::Privileged, false); }
        [[nodiscard]] auto inputDragDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_inputDragTool, ToolSurface::Privileged, false); }
        [[nodiscard]] auto inputHoldDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_inputHoldTool, ToolSurface::Privileged, true); }
        [[nodiscard]] auto inputKeyDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_inputKeyTool, ToolSurface::Privileged, false); }
        [[nodiscard]] auto inputMoveDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_inputMoveTool, ToolSurface::Privileged, false); }
        [[nodiscard]] auto inputScrollDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_inputScrollTool, ToolSurface::Privileged, false); }
        [[nodiscard]] auto uiClickDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_uiClickTool, ToolSurface::Semantic, false); }
        [[nodiscard]] auto uiDragDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_uiDragTool, ToolSurface::Semantic, false); }
        [[nodiscard]] auto uiHoldDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_uiHoldTool, ToolSurface::Semantic, true); }
        [[nodiscard]] auto uiKeyDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_uiKeyTool, ToolSurface::Semantic, false); }
        [[nodiscard]] auto uiMoveDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_uiMoveTool, ToolSurface::Semantic, false); }
        [[nodiscard]] auto uiScrollDescriptor() -> Result<ToolDescriptor>
        { return inputDescriptor(k_uiScrollTool, ToolSurface::Semantic, false); }

        [[nodiscard]]
        auto waitDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_waitTimeoutMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // The two Tools that take nothing, and the two member lists the shared
        // builder needs that no other contract states.
        constexpr auto k_noMembers = std::array<std::string_view, 0U>{};
        constexpr auto k_auditMembers = std::array{
            std::string_view{"record"},
        };
        constexpr auto k_returnScreenMembers = std::array{
            std::string_view{"return_screen"},
        };

        [[nodiscard]] auto auditArgumentMaterial() -> json::Value
        { return argumentMaterial(k_auditMembers); }
        [[nodiscard]] auto noArgumentsMaterial() -> json::Value
        { return argumentMaterial(k_noMembers); }
        [[nodiscard]] auto observeArgumentMaterial() -> json::Value
        { return argumentMaterial(k_screenshotMembers); }
        [[nodiscard]] auto cropArgumentMaterial() -> json::Value
        { return argumentMaterial(k_screenshotRectangleMembers); }
        [[nodiscard]] auto readLinesArgumentMaterial() -> json::Value
        { return argumentMaterial(k_screenshotRectangleMembers); }
        [[nodiscard]] auto readSingleLineArgumentMaterial() -> json::Value
        { return argumentMaterial(k_screenshotRectangleMembers); }
        [[nodiscard]] auto probeArgumentMaterial() -> json::Value
        { return argumentMaterial(k_probeMembers); }
        [[nodiscard]] auto censusGridArgumentMaterial() -> json::Value
        { return argumentMaterial(k_censusGridMembers); }
        [[nodiscard]] auto projectReadTextArgumentMaterial() -> json::Value
        { return argumentMaterial(k_projectReadTextMembers); }
        [[nodiscard]] auto projectWriteFileArgumentMaterial() -> json::Value
        { return argumentMaterial(k_projectWriteFileMembers); }
        [[nodiscard]] auto projectWriteTextArgumentMaterial() -> json::Value
        { return argumentMaterial(k_projectWriteTextMembers); }
        [[nodiscard]] auto sessionEntryArgumentMaterial() -> json::Value
        { return argumentMaterial(k_sessionEntryMembers); }
        [[nodiscard]] auto sessionSetArgumentMaterial() -> json::Value
        { return argumentMaterial(k_sessionSetMembers); }
        [[nodiscard]] auto inputClickArgumentMaterial() -> json::Value
        { return argumentMaterial(k_inputClickMembers); }
        [[nodiscard]] auto inputDragArgumentMaterial() -> json::Value
        { return argumentMaterial(k_inputDragMembers); }
        [[nodiscard]] auto inputHoldArgumentMaterial() -> json::Value
        { return argumentMaterial(k_inputHoldMembers, k_returnScreenMembers); }
        [[nodiscard]] auto inputKeyArgumentMaterial() -> json::Value
        { return argumentMaterial(k_inputKeyMembers); }
        [[nodiscard]] auto inputMoveArgumentMaterial() -> json::Value
        { return argumentMaterial(k_inputMoveMembers); }
        [[nodiscard]] auto inputScrollArgumentMaterial() -> json::Value
        { return argumentMaterial(k_inputScrollMembers); }
        [[nodiscard]] auto uiArgumentMaterial() -> json::Value
        { return argumentMaterial(k_uiActionMembers); }
        [[nodiscard]] auto uiHoldArgumentMaterial() -> json::Value
        { return argumentMaterial(k_uiActionMembers, k_returnScreenMembers); }

        // The one Framework Tool whose argument carries a bound of its own, so
        // the one whose property is written here rather than derived from the
        // member table.
        //
        // THE CEILING LIVES IN properties.duration_ms.maximum AND NOWHERE
        // ELSE. It used to sit under an invented top-level keyword, where no
        // standard validator and no model reading the catalog could find it and
        // only this repository knew where to look. A caller, an Operator and a
        // schema evaluator now read the same number out of the same place.
        [[nodiscard]]
        auto waitArgumentMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additionalProperties", json::Value::ofBoolean(false)},
                {"properties",
                 json::Value::ofObject({
                     {"duration_ms",
                      json::Value::ofObject({
                          {"description",
                           json::Value::ofString(
                               "How long to wait, in whole milliseconds. A wait "
                               "is bounded by construction: this is the "
                               "longest one this Tool admits, and a larger "
                               "value is refused rather than clamped."
                           )},
                          {"maximum",
                           json::Value::ofNumber(
                               static_cast<double>(k_maximumWaitMillis)
                           )},
                          {"minimum", json::Value::ofNumber(0.0)},
                          {"type", json::Value::ofString("integer")},
                      })},
                 })},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("duration_ms"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        // ------------------------------------------------------------------
        // Result shapes.
        //
        // Every Framework Tool declares what a CONFIRMED call answers with, so
        // that chaining -- capture, feed the digest to a measurement, feed a
        // reference to an action -- is something a caller reads rather than
        // something it has to be told. What surrounds the result is the answer
        // envelope, which is one shape for every Tool and is published once, at
        // the catalog, rather than repeated twenty-five times here.
        // ------------------------------------------------------------------

        [[nodiscard]]
        auto typedResult(std::string_view type, std::string_view description)
            -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(std::string{description})},
                {"type", json::Value::ofString(std::string{type})},
            });
        }

        [[nodiscard]]
        auto digestResult(std::string_view description) -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(std::string{description})},
                {"pattern", json::Value::ofString("^[0-9a-f]{64}$")},
                {"type", json::Value::ofString("string")},
            });
        }

        // A counter rendered as a decimal string rather than as a JSON number.
        // RFC 8785 numbers are IEEE-754 doubles, so a generation or an instant
        // above 2^53 would round inside a durable Tool result; the producers
        // render these as strings for exactly that reason, and the published
        // shape says so rather than leaving a caller to discover it.
        [[nodiscard]]
        auto counterResult(std::string_view description) -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(std::string{description})},
                {"pattern", json::Value::ofString("^(0|[1-9][0-9]*)$")},
                {"type", json::Value::ofString("string")},
            });
        }

        [[nodiscard]]
        auto countResult(std::string_view description) -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(std::string{description})},
                {"minimum", json::Value::ofNumber(0.0)},
                {"type", json::Value::ofString("integer")},
            });
        }

        // A score on the closed unit interval. Both reading Tools report an OCR
        // confidence, and one spelling of the bounds keeps the two contracts
        // from drifting into two ranges for one measurement.
        [[nodiscard]]
        auto unitResult(std::string_view description) -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(std::string{description})},
                {"maximum", json::Value::ofNumber(1.0)},
                {"minimum", json::Value::ofNumber(0.0)},
                {"type", json::Value::ofString("number")},
            });
        }

        [[nodiscard]]
        auto objectResult(
            std::vector<json::Member> properties,
            std::vector<std::string_view> required
        ) -> json::Value
        {
            auto requiredIds = std::vector<json::Value>{};
            requiredIds.reserve(required.size());
            for (auto const name : required)
            {
                requiredIds.emplace_back(
                    json::Value::ofString(std::string{name})
                );
            }
            auto material = std::vector<json::Member>{
                {"additionalProperties", json::Value::ofBoolean(false)},
                {"properties", json::Value::ofObject(std::move(properties))},
                {"type", json::Value::ofString("object")},
            };
            if (!requiredIds.empty())
            {
                material.emplace_back(
                    "required",
                    json::Value::ofArray(std::move(requiredIds))
                );
            }
            return json::Value::ofObject(std::move(material));
        }

        [[nodiscard]]
        auto shapeIntegerSchema(
            std::string_view description,
            double minimum,
            double maximum
        ) -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(std::string{description})},
                {"maximum", json::Value::ofNumber(maximum)},
                {"minimum", json::Value::ofNumber(minimum)},
                {"type", json::Value::ofString("integer")},
            });
        }

        [[nodiscard]]
        auto matchShapesArgumentMaterial() -> json::Value
        {
            auto templateSchema = objectResult(
                {
                    {"height",
                     shapeIntegerSchema("Template height in pixels.", 1.0, 64.0)},
                    {"id",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "Caller-owned opaque identifier, unique within "
                              "this templates array."
                          )},
                         {"maxLength", json::Value::ofNumber(128.0)},
                         {"minLength", json::Value::ofNumber(1.0)},
                         {"type", json::Value::ofString("string")},
                     })},
                    {"minimum_score",
                     unitResult(
                         "Inclusive minimum normalized grayscale correlation; "
                         "this is a similarity score, not a probability."
                     )},
                    {"pixels",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "Opaque grayscale pixels in row-major order, "
                              "exactly width times height entries. No alpha "
                              "mask; constant templates are refused."
                          )},
                         {"items",
                          shapeIntegerSchema("Grayscale intensity.", 0.0, 255.0)},
                         {"maxItems", json::Value::ofNumber(4096.0)},
                         {"minItems", json::Value::ofNumber(1.0)},
                         {"type", json::Value::ofString("array")},
                     })},
                    {"width",
                     shapeIntegerSchema("Template width in pixels.", 1.0, 64.0)},
                },
                {"height", "id", "minimum_score", "pixels", "width"}
            );
            return objectResult(
                {
                    {"height", memberSchema(argumentMember("height"))},
                    {"maximum_matches",
                     shapeIntegerSchema(
                         "Maximum number of matches after cross-template "
                         "suppression. Exceeding it refuses the whole search; "
                         "results are never silently truncated.",
                         1.0,
                         512.0
                     )},
                    {std::string{k_screenshotSha256Member},
                     memberSchema(argumentMember(k_screenshotSha256Member))},
                    {"suppression_radius",
                     shapeIntegerSchema(
                         "Suppress weaker candidates within this Chebyshev "
                         "distance (maximum axis difference) across all "
                         "templates. Centers are x + floor(width/2), "
                         "y + floor(height/2) in screenshot pixels. Zero "
                         "suppresses only coincident centers.",
                         0.0,
                         64.0
                     )},
                    {"templates",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "Grayscale templates prepared by the caller at "
                              "the desired sizes and angles. Duplicate ids "
                              "and templates larger than the search region "
                              "are refused."
                          )},
                         {"items", std::move(templateSchema)},
                         {"maxItems", json::Value::ofNumber(64.0)},
                         {"minItems", json::Value::ofNumber(1.0)},
                         {"type", json::Value::ofString("array")},
                     })},
                    {"width", memberSchema(argumentMember("width"))},
                    {"x", memberSchema(argumentMember("x"))},
                    {"y", memberSchema(argumentMember("y"))},
                },
                {
                    "height", "maximum_matches", k_screenshotSha256Member,
                    "suppression_radius", "templates", "width", "x", "y",
                }
            );
        }

        [[nodiscard]]
        auto matchShapesOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"completed_pixel_comparisons",
                     shapeIntegerSchema(
                         "Number of template-pixel comparisons completed by "
                         "this search, represented exactly as a JSON integer.",
                         0.0,
                         9'007'199'254'740'991.0
                     )},
                    {"image_height",
                     countResult("Height of the retained screenshot in pixels.")},
                    {"image_width",
                     countResult("Width of the retained screenshot in pixels.")},
                    {"matches",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "Matches after cross-template suppression, "
                              "strongest score first, then template id, y, x "
                              "to break ties. Empty means the complete search "
                              "found no match."
                          )},
                         {"items",
                          objectResult(
                              {
                                  {"height", countResult("Matched height.")},
                                  {"score",
                                   json::Value::ofObject({
                                       {"description",
                                        json::Value::ofString(
                                            "Normalized grayscale correlation, "
                                            "not a probability."
                                        )},
                                       {"maximum", json::Value::ofNumber(1.0)},
                                       {"minimum", json::Value::ofNumber(-1.0)},
                                       {"type", json::Value::ofString("number")},
                                   })},
                                  {"template_id",
                                   typedResult(
                                       "string", "The matched template's id."
                                   )},
                                  {"width", countResult("Matched width.")},
                                  {"x",
                                   countResult(
                                       "Left edge in screenshot pixels, not "
                                       "relative to the search rectangle."
                                   )},
                                  {"y",
                                   countResult(
                                       "Top edge in screenshot pixels, not "
                                       "relative to the search rectangle."
                                   )},
                              },
                              {"height", "score", "template_id", "width", "x", "y"}
                          )},
                         {"maxItems", json::Value::ofNumber(512.0)},
                         {"type", json::Value::ofString("array")},
                     })},
                },
                {"completed_pixel_comparisons", "image_height", "image_width", "matches"}
            );
        }

        [[nodiscard]]
        auto auditOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"record_hash",
                     digestResult(
                         "sha256 of the exact canonical bytes of the record "
                         "that was written."
                     )},
                    {"recorded",
                     typedResult(
                         "boolean",
                         "True on a confirmed call: the record is on this "
                         "run's durable Tool-call history."
                     )},
                },
                {"record_hash", "recorded"}
            );
        }

        // The receipt both screenshot-producing Tools answer with. One shape,
        // because a crop is a screenshot: whatever produced it, the digest it
        // returns is the digest every Tool that reads a screenshot takes.
        [[nodiscard]]
        auto evidenceReceiptOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"byte_count",
                     counterResult("Size of the stored artifact, in bytes.")},
                    {"created_at_unix_ms",
                     counterResult(
                         "When the artifact was stored, in Unix milliseconds."
                     )},
                    {"frame_identity",
                     objectResult(
                         {
                             {"capture_session_id",
                              counterResult(
                                  "The capture session the frame belongs to."
                              )},
                             {"frame_id",
                              counterResult(
                                  "The frame within that capture session."
                              )},
                             {"target_generation",
                              counterResult(
                                  "The controlled target's generation when the "
                                  "frame was taken."
                              )},
                         },
                         {
                             "capture_session_id",
                             "frame_id",
                             "target_generation",
                         }
                     )},
                    {"height",
                     countResult("Height of the stored image, in pixels.")},
                    {"media_type",
                     typedResult(
                         "string",
                         "IANA media type of the stored bytes, for example "
                         "image/png."
                     )},
                    {"rectangle",
                     objectResult(
                         {
                             {"height", counterResult("Height of the crop.")},
                             {"width", counterResult("Width of the crop.")},
                             {"x",
                              counterResult(
                                  "Left edge of the crop in the source "
                                  "screenshot."
                              )},
                             {"y",
                              counterResult(
                                  "Top edge of the crop in the source "
                                  "screenshot."
                              )},
                         },
                         {"height", "width", "x", "y"}
                     )},
                    {std::string{k_screenshotSha256Member},
                     digestResult(
                         "Content hash of the stored screenshot artifact. This "
                         "is the value to pass as screenshot_sha256 to "
                         "framework.screen.observe, to any framework.screen "
                         "measuring Tool, to any framework.input Tool, and as "
                         "file_sha256 to framework.project.write_file."
                     )},
                    {"width",
                     countResult("Width of the stored image, in pixels.")},
                },
                {
                    "byte_count",
                    "created_at_unix_ms",
                    "frame_identity",
                    "height",
                    "media_type",
                    std::string_view{k_screenshotSha256Member},
                    "width",
                }
            );
        }

        [[nodiscard]]
        auto observeOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"artifact_root_hash",
                     digestResult(
                         "Root hash of the RuntimeModel artifact this reading "
                         "was resolved against."
                     )},
                    {"controlled_target_id",
                     typedResult(
                         "string",
                         "The controlled target the frame was read from."
                     )},
                    {"decision_basis_hash",
                     digestResult(
                         "Hash of the basis this observation's decisions were "
                         "taken on."
                     )},
                    {"host_generation",
                     counterResult(
                         "The Host generation the reading was made under."
                     )},
                    {"observation_id",
                     typedResult(
                         "string",
                         "Identifier of this observation within the run."
                     )},
                    {std::string{k_observationReferenceArgument},
                     typedResult(
                         "object",
                         "The handle to pass back unchanged as "
                         "observation_reference to any framework.ui Tool. It "
                         "is SINGLE USE and it expires: the first ui call that "
                         "presents it spends it, and a second action needs a "
                         "second observe."
                     )},
                    {"project_registration_hash",
                     digestResult(
                         "The Project registration this reading was resolved "
                         "under."
                     )},
                    {std::string{k_screenshotSha256Member},
                     digestResult(
                         "The retained screenshot this reading was resolved "
                         "on, usable anywhere a screenshot reference is "
                         "taken. An observation names its own frame rather "
                         "than assuming the caller supplied it: a "
                         "framework.input.hold that observed while pressed "
                         "captured that frame itself, and this is the only "
                         "name its caller has for it."
                     )},
                    {"snapshot_identity_hash",
                     digestResult("Identity of the frame that was read.")},
                    {"snapshot_ref",
                     typedResult(
                         "string",
                         "Opaque reference to the snapshot this reading was "
                         "taken from."
                     )},
                    {"state_resolution",
                     typedResult(
                         "object",
                         "What the installed RuntimeModel resolved on this "
                         "frame, in the project's own vocabulary: its "
                         "surfaces, ui_targets, bindings and action "
                         "identities. Its shape is the project's declaration "
                         "and the framework does not restate it here."
                     )},
                    {"state_resolution_hash",
                     digestResult(
                         "Hash of the exact state_resolution bytes above."
                     )},
                    {"target_generation",
                     counterResult(
                         "The controlled target's generation when the frame "
                         "was taken."
                     )},
                },
                {
                    "artifact_root_hash",
                    "controlled_target_id",
                    "decision_basis_hash",
                    "host_generation",
                    "observation_id",
                    std::string_view{k_observationReferenceArgument},
                    "project_registration_hash",
                    std::string_view{k_screenshotSha256Member},
                    "snapshot_identity_hash",
                    "snapshot_ref",
                    "state_resolution",
                    "state_resolution_hash",
                    "target_generation",
                }
            );
        }

        [[nodiscard]]
        auto probeOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"distinct_colours",
                     countResult(
                         "How many distinct colours the rectangle contains."
                     )},
                    {"dominant_blue",
                     countResult("Blue channel of the dominant colour.")},
                    {"dominant_green",
                     countResult("Green channel of the dominant colour.")},
                    {"dominant_pixels",
                     countResult(
                         "How many pixels carry the dominant colour."
                     )},
                    {"dominant_red",
                     countResult("Red channel of the dominant colour.")},
                    {"fully_selected_pixels",
                     countResult(
                         "Pixels that matched the colour key outright. Present "
                         "with ramp_selected_pixels and selected_weight, or "
                         "not at all."
                     )},
                    {"ramp_selected_pixels",
                     countResult(
                         "Pixels that matched only partially, along the key's "
                         "tolerance ramp."
                     )},
                    {"rect_pixels",
                     countResult(
                         "How many pixels the measured rectangle holds."
                     )},
                    {"selected_weight",
                     countResult(
                         "Total weight of the selection, counting ramp matches "
                         "by how far they matched."
                     )},
                },
                {
                    "distinct_colours",
                    "dominant_blue",
                    "dominant_green",
                    "dominant_pixels",
                    "dominant_red",
                    "rect_pixels",
                }
            );
        }

        [[nodiscard]]
        auto censusGridOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"cell_height",
                     countResult("Cell height actually used, in pixels.")},
                    {"cell_width",
                     countResult("Cell width actually used, in pixels.")},
                    {"columns",
                     countResult("How many cells the grid is wide.")},
                    {"rows", countResult("How many cells the grid is tall.")},
                    {"selected_pixels",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "One matching-pixel count per cell, in row-major "
                              "order: the cell at (row, column) is at index "
                              "row * columns + column."
                          )},
                         {"items", countResult("Matching pixels in one cell.")},
                         {"type", json::Value::ofString("array")},
                     })},
                },
                {
                    "cell_height",
                    "cell_width",
                    "columns",
                    "rows",
                    "selected_pixels",
                }
            );
        }

        [[nodiscard]]
        auto textCharactersResult() -> json::Value
        {
            return json::Value::ofObject({
                {"description",
                 json::Value::ofString(
                     "Characters in emitted text order, each with its own "
                     "decoder confidence from 0 to 1, not the line average. "
                     "Their text concatenates to the line text; no character "
                     "boxes are inferred."
                 )},
                {"items",
                 objectResult(
                     {
                         {"confidence",
                          unitResult("Decoder confidence for this character.")},
                         {"text",
                          typedResult("string", "The emitted character's UTF-8 text.")},
                     },
                     {"confidence", "text"}
                 )},
                {"type", json::Value::ofString("array")},
            });
        }

        [[nodiscard]]
        auto recognizedTextResult() -> json::Value
        {
            return json::Value::ofObject({
                {"description", json::Value::ofString(
                    "Nonempty recognised text, preserved without trimming. "
                    "Empty decodes are omitted."
                )},
                {"minLength", json::Value::ofNumber(1.0)},
                {"type", json::Value::ofString("string")},
            });
        }

        [[nodiscard]]
        auto textFoundResult(bool found) -> json::Value
        {
            return json::Value::ofObject({
                {"const", json::Value::ofBoolean(found)},
                {"description",
                 json::Value::ofString(
                     "Whether the reader produced a reading. A false answer "
                     "carries no text, confidence or characters."
                 )},
                {"type", json::Value::ofString("boolean")},
            });
        }

        [[nodiscard]]
        auto readLinesOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"lines",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "One entry per recognised line of text, in the "
                              "order the reader produced them."
                          )},
                         {"items",
                          objectResult(
                              {
                                  {"characters", textCharactersResult()},
                                  {"confidence",
                                   unitResult(
                                       "How confident the reader is in this "
                                       "line, from 0 to 1."
                                   )},
                                  {"height",
                                   countResult(
                                       "Height of the line's box, in "
                                       "screenshot pixels."
                                   )},
                                  {"text", recognizedTextResult()},
                                  {"width",
                                   countResult(
                                       "Width of the line's box, in screenshot "
                                       "pixels."
                                   )},
                                  {"x",
                                   countResult(
                                       "Left edge of the line's box, in "
                                       "screenshot pixels."
                                   )},
                                  {"y",
                                   countResult(
                                       "Top edge of the line's box, in "
                                       "screenshot pixels."
                                   )},
                              },
                              {
                                  "characters",
                                  "confidence",
                                  "height",
                                  "text",
                                  "width",
                                  "x",
                                  "y",
                              }
                          )},
                         {"type", json::Value::ofString("array")},
                     })},
                },
                {"lines"}
            );
        }

        // ONE reading, and no rectangle beside it. Under single-line layout the
        // host locates nothing, so the only box it could report back is the one
        // the caller drew; echoing the question as an answer would read as a
        // measurement and be none. text_found follows framework.session.get's
        // `present`: a reader that looked and saw nothing is an answer, not a
        // failure, and the reading members it would otherwise have to invent for
        // that case are absent instead.
        [[nodiscard]]
        auto readSingleLineOutputMaterial() -> json::Value
        {
            auto found = objectResult(
                {
                    {"characters", textCharactersResult()},
                    {"confidence",
                     unitResult(
                         "How confident the reader is in the text it produced, "
                         "from 0 to 1. Present only when text_found is true. "
                         "It scores the CHARACTERS, not the caller's claim "
                         "that the rectangle held one line, so a rectangle "
                         "holding several can score well here."
                     )},
                    {"text", recognizedTextResult()},
                    {"text_found", textFoundResult(true)},
                },
                {"characters", "confidence", "text", "text_found"}
            );
            return json::Value::ofObject({
                {"oneOf",
                 json::Value::ofArray({
                     std::move(found),
                     objectResult(
                         {{"text_found", textFoundResult(false)}},
                         {"text_found"}
                     ),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto projectReadTextOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"content",
                     typedResult("string", "The file's UTF-8 text.")},
                    {"content_hash", digestResult("sha256 of that text.")},
                    {"path",
                     typedResult(
                         "string",
                         "The project-relative path that was read."
                     )},
                },
                {"content", "content_hash", "path"}
            );
        }

        [[nodiscard]]
        auto projectWriteOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"content_hash",
                     digestResult("sha256 of the bytes that were written.")},
                    {"path",
                     typedResult(
                         "string",
                         "The project-relative path that was written."
                     )},
                    {"written_bytes",
                     countResult("How many bytes were written.")},
                },
                {"content_hash", "path", "written_bytes"}
            );
        }

        [[nodiscard]]
        auto sessionSetOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"name",
                     typedResult(
                         "string",
                         "The name the value was stored under."
                     )},
                    {"stored_bytes",
                     countResult(
                         "What this session's whole state weighs after the "
                         "write, in bytes."
                     )},
                },
                {"name", "stored_bytes"}
            );
        }

        // ABSENCE IS SPELLED ONCE, and `present` is where. `value` is left
        // out of a miss rather than answered as null, which is the same
        // decision the argument contract takes when it admits every JSON type
        // but null: with null admitted on either side, a stored null and a name
        // this session never saw would be one answer read two ways.
        //
        // `value` therefore carries the same five types the stored argument
        // did, so a caller reads what it may get back where it reads what it
        // may send.
        [[nodiscard]]
        auto sessionGetOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"name",
                     typedResult("string", "The name that was read.")},
                    {"present",
                     typedResult(
                         "boolean",
                         "Whether this session has stored anything under that "
                         "name."
                     )},
                    {"value",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "The exact value that was stored, present only "
                              "when present is true."
                          )},
                         {"type",
                          json::Value::ofArray({
                              json::Value::ofString("array"),
                              json::Value::ofString("boolean"),
                              json::Value::ofString("number"),
                              json::Value::ofString("object"),
                              json::Value::ofString("string"),
                          })},
                     })},
                },
                {"name", "present"}
            );
        }

        [[nodiscard]]
        auto sessionListOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"maximum_bytes",
                     countResult(
                         "The most this session's state may weigh in total. A "
                         "framework.session.set past it is refused."
                     )},
                    {"maximum_names",
                     countResult(
                         "The most names this session's state may hold. A "
                         "framework.session.set of a further name past it is "
                         "refused."
                     )},
                    {"names",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "Every name this session has stored, in UTF-8 "
                              "order."
                          )},
                         {"items",
                          json::Value::ofObject({
                              {"type", json::Value::ofString("string")},
                          })},
                         {"type", json::Value::ofString("array")},
                     })},
                    {"stored_bytes",
                     countResult(
                         "What those names and their values weigh together, in "
                         "bytes."
                     )},
                },
                {"maximum_bytes", "maximum_names", "names", "stored_bytes"}
            );
        }

        // ONE UNIX-MILLISECOND INSTANT, SPELLED THE WAY THIS CATALOG ALREADY
        // SPELLS ONE. The unit, the epoch and the decimal-string rendering are
        // exactly a screenshot receipt's created_at_unix_ms; what varies is the
        // prefix, which names WHAT happened at that instant. The catalog
        // already carries two of them -- created_at_unix_ms on an evidence
        // receipt, expires_at_unix_ms on an observation reference -- so
        // <verb>_at_unix_ms is the convention rather than a second spelling of
        // one member. Nothing is created here, so created_at would name a
        // creation that did not happen.
        [[nodiscard]]
        auto nowOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"read_at_unix_ms",
                     counterResult(
                         "The instant the Operator's wall clock was read, in "
                         "Unix milliseconds since 1970-01-01T00:00:00Z. Same "
                         "clock, epoch and rendering as a screenshot "
                         "receipt's created_at_unix_ms."
                     )},
                },
                {"read_at_unix_ms"}
            );
        }

        [[nodiscard]]
        auto statusOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"access",
                     json::Value::ofObject({
                         {"description",
                          json::Value::ofString(
                              "Whether this run may still change anything "
                              "outside the Operator."
                          )},
                         {"enum",
                          json::Value::ofArray({
                              json::Value::ofString("writable"),
                              json::Value::ofString("read_only"),
                          })},
                         {"type", json::Value::ofString("string")},
                     })},
                    {"controlled_target_id",
                     typedResult(
                         "string",
                         "The target this run is driving."
                     )},
                    {"installed_generation",
                     counterResult(
                         "The installed generation this run is pinned to."
                     )},
                    {"session_id",
                     typedResult("string", "This run's session identifier.")},
                },
                {
                    "access",
                    "controlled_target_id",
                    "installed_generation",
                    "session_id",
                }
            );
        }

        [[nodiscard]]
        auto waitOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"completed",
                     typedResult(
                         "boolean",
                         "False when a cooperative stop ended the wait before "
                         "its full duration. That is a recorded fact about the "
                         "call, not a failure."
                     )},
                    {"duration_ms",
                     countResult(
                         "The duration this call was asked to wait, echoed "
                         "back."
                     )},
                },
                {"completed", "duration_ms"}
            );
        }

        // What a screen-returning hold answers with under `screen`. It is one
        // of two shapes and is stated as such rather than as a third: a hold
        // that captured answers with a framework.screen.capture receipt, and a
        // hold that observed answers with a framework.screen.observe result.
        [[nodiscard]]
        auto heldScreenResult() -> json::Value
        {
            return json::Value::ofObject({
                {"description",
                 json::Value::ofString(
                     "What was on screen while the input was still engaged. It "
                     "is a framework.screen.capture receipt when return_screen "
                     "was capture, and a framework.screen.observe result when "
                     "it was observe. Absent when return_screen was none. "
                     "Either way its screenshot_sha256 names a RETAINED "
                     "screenshot: pass it to read_lines, crop or probe exactly "
                     "as you would one framework.screen.capture answered."
                 )},
                {"oneOf",
                 json::Value::ofArray({
                     evidenceReceiptOutputMaterial(),
                     observeOutputMaterial(),
                 })},
            });
        }

        [[nodiscard]]
        auto deliveredResult() -> json::Value
        {
            return typedResult(
                "boolean",
                "True on a confirmed call: the input reached the target."
            );
        }

        [[nodiscard]]
        auto heldResult() -> json::Value
        {
            return typedResult(
                "boolean",
                "Always false. Every held input is released before its call "
                "returns, so no input outlives the call that delivered it."
            );
        }

        [[nodiscard]]
        auto actionResult() -> json::Value
        {
            return typedResult(
                "string",
                "The input verb this call delivered."
            );
        }

        [[nodiscard]]
        auto targetResult() -> json::Value
        {
            return typedResult(
                "string",
                "The controlled target the input reached."
            );
        }

        [[nodiscard]]
        auto rawInputOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"action", actionResult()},
                    {"controlled_target_id", targetResult()},
                    {"delivered", deliveredResult()},
                    {"held", heldResult()},
                },
                {"action", "controlled_target_id", "delivered", "held"}
            );
        }

        [[nodiscard]]
        auto rawHoldOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"action", actionResult()},
                    {"controlled_target_id", targetResult()},
                    {"delivered", deliveredResult()},
                    {"duration_ms",
                     countResult(
                         "The dwell this call was asked for, echoed back."
                     )},
                    {"held", heldResult()},
                    {"screen", heldScreenResult()},
                },
                {
                    "action",
                    "controlled_target_id",
                    "delivered",
                    "duration_ms",
                    "held",
                }
            );
        }

        [[nodiscard]]
        auto uiInputOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"delivered", deliveredResult()},
                    {"reason",
                     typedResult(
                         "string",
                         "What the Host recorded about the delivery."
                     )},
                    {"verdict",
                     typedResult(
                         "string",
                         "The Host's own delivery verdict, `delivered` on a "
                         "confirmed call."
                     )},
                },
                {"delivered", "reason", "verdict"}
            );
        }

        [[nodiscard]]
        auto uiHoldOutputMaterial() -> json::Value
        {
            return objectResult(
                {
                    {"action", actionResult()},
                    {"delivered", deliveredResult()},
                    {"held", heldResult()},
                    {"screen", heldScreenResult()},
                    {"verdict",
                     typedResult(
                         "string",
                         "The Host's own delivery verdict, `delivered` on a "
                         "confirmed call."
                     )},
                },
                {"action", "delivered", "held", "verdict"}
            );
        }

        // The one shape every answer has, published once here rather than
        // repeated inside twenty-five output schemas. Each Tool's own
        // output_schema describes the `result` member of this envelope and
        // nothing around it.
        [[nodiscard]]
        auto answerEnvelopeMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additionalProperties", json::Value::ofBoolean(false)},
                {"description",
                 json::Value::ofString(
                     "Every Tool call answers with this object. An ok answer "
                     "carries `result`, whose shape is the called Tool's own "
                     "output_schema; a failed one carries `error` instead and "
                     "no result. A failure IS the answer: it is never an "
                     "exception the caller has to interrogate, and `delivery` "
                     "says whether an input that failed may nonetheless have "
                     "landed."
                 )},
                {"properties",
                 json::Value::ofObject({
                     {"call_identity",
                      digestResult(
                          "The durable coordinate this call was recorded at."
                      )},
                     {"delivery",
                      json::Value::ofObject({
                          {"description",
                           json::Value::ofString(
                               "How the call ended. `confirmed` is the only "
                               "value an ok answer carries. `proven_absent` "
                               "means nothing reached the world; `possible` "
                               "means it may have; `terminal_failure` and "
                               "`terminally_unresolved` are failures that will "
                               "not resolve by waiting."
                           )},
                          {"enum",
                           json::Value::ofArray({
                               json::Value::ofString("confirmed"),
                               json::Value::ofString("proven_absent"),
                               json::Value::ofString("possible"),
                               json::Value::ofString("terminal_failure"),
                               json::Value::ofString("terminally_unresolved"),
                           })},
                          {"type", json::Value::ofString("string")},
                      })},
                     {"error",
                      objectResult(
                          {
                              {"code",
                               typedResult(
                                   "string",
                                   "Machine-readable classification of the "
                                   "failure."
                               )},
                              {"message",
                               typedResult(
                                   "string",
                                   "What failed, in the terms the caller has "
                                   "to act on."
                               )},
                              {"retryable",
                               typedResult(
                                   "boolean",
                                   "Whether repeating the same call could "
                                   "succeed."
                               )},
                          },
                          {"code", "message", "retryable"}
                      )},
                     {"ok",
                      typedResult(
                          "boolean",
                          "Whether the call did what it was asked to do."
                      )},
                     {"result",
                      typedResult(
                          "object",
                          "Present only when ok is true. Its shape is the "
                          "output_schema of the Tool that was called."
                      )},
                 })},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("call_identity"),
                     json::Value::ofString("delivery"),
                     json::Value::ofString("ok"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        // Declared in UTF-8 byte order of the tool names, so the rendered tools
        // array is already sorted and the catalog material has one spelling
        // rather than one per declaration order.
        constexpr auto k_frameworkTools = std::array{
            FrameworkToolDefinition{
                k_auditTool,
                "Write one JSON object into this run's durable Tool-call "
                "history, so that a later reader can see what the run decided "
                "and why. The record is the whole of the call's arguments; "
                "there is deliberately no attribution member, because the run, "
                "the caller and the call position are supplied by the runtime "
                "and a caller that could state them could attribute its record "
                "to another call's position. A confirmed result carries "
                "record_hash, the sha256 of the exact bytes recorded.",
                &auditDescriptor,
                &auditArgumentMaterial,
                &auditOutputMaterial,
            },
            FrameworkToolDefinition{
                k_inputClickTool,
                "Click the primary button at a raw coordinate of the "
                "controlled target. x and y are pixels of the retained "
                "screenshot named by screenshot_sha256, which is what ties the "
                "aim to a frame that was actually seen. Prefer "
                "framework.ui.click where the project's RuntimeModel names the "
                "thing being clicked; this Tool is for coordinates the model "
                "does not name. A confirmed result carries action, "
                "controlled_target_id, delivered and held.",
                &inputClickDescriptor,
                &inputClickArgumentMaterial,
                &rawInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_inputDragTool,
                "Press at (x, y), travel to (to_x, to_y) over travel_ms, and "
                "release. All four coordinates are pixels of the retained "
                "screenshot named by screenshot_sha256. A confirmed result "
                "carries action, controlled_target_id, delivered and held.",
                &inputDragDescriptor,
                &inputDragArgumentMaterial,
                &rawInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_inputHoldTool,
                "Press at (x, y), keep the input engaged for duration_ms, then "
                "release -- on every exit path, including a failure while "
                "held. Set return_screen to capture or observe to learn what "
                "was on screen WHILE STILL PRESSED; the result then carries "
                "screen, which is a framework.screen.capture receipt or a "
                "framework.screen.observe result, and whose screenshot_sha256 "
                "names a retained screenshot every framework.screen measuring "
                "Tool accepts. A confirmed result carries "
                "action, controlled_target_id, delivered, duration_ms and "
                "held, which is always false because the press and the release "
                "are one call's business.",
                &inputHoldDescriptor,
                &inputHoldArgumentMaterial,
                &rawHoldOutputMaterial,
            },
            FrameworkToolDefinition{
                k_inputKeyTool,
                "Press and release one key on the controlled target. key is "
                "the canonical key name; screenshot_sha256 names the retained "
                "screenshot this keystroke was decided on. A confirmed result "
                "carries action, controlled_target_id, delivered and held.",
                &inputKeyDescriptor,
                &inputKeyArgumentMaterial,
                &rawInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_inputMoveTool,
                "Move the pointer to a raw coordinate of the controlled "
                "target, without pressing anything. x and y are pixels of the "
                "retained screenshot named by screenshot_sha256. A confirmed "
                "result carries action, controlled_target_id, delivered and "
                "held.",
                &inputMoveDescriptor,
                &inputMoveArgumentMaterial,
                &rawInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_inputScrollTool,
                "Turn the wheel by notches detents on the controlled target. "
                "The sign of notches is the direction. screenshot_sha256 names "
                "the retained screenshot this scroll was decided on. A "
                "confirmed result carries action, controlled_target_id, "
                "delivered and held.",
                &inputScrollDescriptor,
                &inputScrollArgumentMaterial,
                &rawInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_projectReadTextTool,
                "Read one UTF-8 text file back out of this project's own "
                "authoring store. path is project-relative. Read-only, so a "
                "session that may not write can still read back what an "
                "earlier one wrote. A confirmed result carries content, its "
                "content_hash and the path that was read.",
                &projectReadTextDescriptor,
                &projectReadTextArgumentMaterial,
                &projectReadTextOutputMaterial,
            },
            FrameworkToolDefinition{
                k_projectWriteFileTool,
                "Copy one retained evidence artifact into this project's "
                "authoring store at path. file_sha256 is the digest "
                "framework.screen.capture or framework.screen.crop returned in "
                "its screenshot_sha256 member -- capturing and writing are two "
                "calls, so a failure between them is visible: the screenshot "
                "exists and the write did not happen. Writing the same digest "
                "to the same path twice leaves the store where the first write "
                "left it. A confirmed result carries content_hash, path and "
                "written_bytes.",
                &projectWriteFileDescriptor,
                &projectWriteFileArgumentMaterial,
                &projectWriteOutputMaterial,
            },
            FrameworkToolDefinition{
                k_projectWriteTextTool,
                "Write UTF-8 text into this project's authoring store at path, "
                "project-relative. Writing the same content to the same path "
                "twice leaves the store where the first write left it. A "
                "confirmed result carries content_hash, path and "
                "written_bytes.",
                &projectWriteTextDescriptor,
                &projectWriteTextArgumentMaterial,
                &projectWriteOutputMaterial,
            },
            FrameworkToolDefinition{
                k_captureTool,
                "Capture the controlled target's screen into an immutable, "
                "content-addressed screenshot artifact and answer with its "
                "receipt. Takes no arguments. THIS IS THE START OF EVERY "
                "SCREEN FLOW: the confirmed result's screenshot_sha256 is what "
                "framework.screen.observe, every framework.screen measuring "
                "Tool and every framework.input Tool takes as its explicit "
                "input, and what framework.project.write_file takes as "
                "file_sha256. The receipt also carries width, height, "
                "media_type, byte_count, created_at_unix_ms and the "
                "frame_identity the pixels came from. The artifact is retained "
                "by the Operator for a bounded lifetime; a call naming a "
                "digest that has expired or been reclaimed is refused, by "
                "name.",
                &captureDescriptor,
                &noArgumentsMaterial,
                &evidenceReceiptOutputMaterial,
            },
            FrameworkToolDefinition{
                k_censusGridTool,
                "Divide one rectangle of a retained screenshot into cells of "
                "cell_width by cell_height and count, per cell, the pixels "
                "within tolerance of the given colour. Set removes to count "
                "the pixels that do NOT match instead. Use it to find where on "
                "a screen a colour is concentrated. A confirmed result carries "
                "rows, columns, the cell_width and cell_height actually used, "
                "and selected_pixels: one count per cell in row-major order.",
                &measuringDescriptor,
                &censusGridArgumentMaterial,
                &censusGridOutputMaterial,
            },
            FrameworkToolDefinition{
                k_cropTool,
                "Cut a rectangle out of a retained screenshot and store it as "
                "a second immutable screenshot artifact. screenshot_sha256 is "
                "the digest framework.screen.capture returned. A confirmed "
                "result is the same receipt shape a capture answers with -- "
                "its own screenshot_sha256, usable anywhere a screenshot "
                "digest is -- plus a rectangle recording where in the source "
                "it came from. Writing the crop into the project is a separate "
                "framework.project.write_file call over that digest.",
                &measuringDescriptor,
                &cropArgumentMaterial,
                &evidenceReceiptOutputMaterial,
            },
            FrameworkToolDefinition{
                k_matchShapesTool,
                "Find caller-supplied grayscale shapes inside one rectangle "
                "of a retained screenshot using normalized cross-correlation. "
                "No colour gate, OCR, semantic classification, automatic "
                "scaling or rotation is applied; prepare each desired size "
                "and angle as its own template with a unique id. All templates "
                "measure the same immutable screenshot. A confirmed result "
                "carries matches with template_id, absolute screenshot x and "
                "y, width, height and score, plus completed_pixel_comparisons "
                "and the full screenshot dimensions image_width and "
                "image_height. "
                "A complete search with no matches answers an empty array. "
                "Invalid templates, exhausted work budget, timeout, cancellation "
                "or more than maximum_matches surviving suppression refuses "
                "the call instead of returning partial results. Nothing is "
                "captured, written or clicked.",
                &measuringDescriptor,
                &matchShapesArgumentMaterial,
                &matchShapesOutputMaterial,
            },
            FrameworkToolDefinition{
                k_observeTool,
                "Resolve one retained screenshot into the project's own "
                "vocabulary: the surfaces, ui_targets, bindings and action "
                "identities the installed RuntimeModel declares for what is on "
                "screen. screenshot_sha256 is the digest "
                "framework.screen.capture returned. A confirmed result carries "
                "observation_reference -- the handle every framework.ui Tool "
                "takes, which is SINGLE USE and expires -- together with "
                "state_resolution, the model's own reading of the frame, and "
                "the identity hashes it was read under. The flow is capture, "
                "observe, then act: one ui call spends the reference, and a "
                "second action needs a second observe.",
                &observeDescriptor,
                &observeArgumentMaterial,
                &observeOutputMaterial,
            },
            FrameworkToolDefinition{
                k_probeTool,
                "Measure one rectangle of a retained screenshot for a colour: "
                "how many of its pixels lie within tolerance of the given RGB, "
                "and what the rectangle's dominant colour is. Set removes to "
                "select the pixels that do NOT match. Nothing is written and "
                "no pixels are returned. A confirmed result carries "
                "rect_pixels, distinct_colours, the dominant_* channels and "
                "dominant_pixels, and -- when the colour key admits a "
                "tolerance ramp -- fully_selected_pixels, ramp_selected_pixels "
                "and selected_weight together.",
                &measuringDescriptor,
                &probeArgumentMaterial,
                &probeOutputMaterial,
            },
            FrameworkToolDefinition{
                k_readLinesTool,
                "Read the text inside one rectangle of a retained screenshot "
                "with OCR. A confirmed result carries lines, one entry per "
                "recognised line, each with its text, its x, y, width and "
                "height in screenshot pixels, a confidence from 0 to 1, and "
                "characters in text order, each with text and its own decoder "
                "confidence from 0 to 1. Character confidence is not copied "
                "from the line average and no character boxes are inferred. "
                "Nothing is captured and nothing is written: the rectangle is "
                "measured on the screenshot the caller named.",
                &measuringDescriptor,
                &readLinesArgumentMaterial,
                &readLinesOutputMaterial,
            },
            FrameworkToolDefinition{
                k_readSingleLineTool,
                "Read one rectangle of a retained screenshot as EXACTLY ONE "
                "LINE of text. THE CALLER ASSERTS THAT THE RECTANGLE HOLDS ONE "
                "LINE AND NOTHING CHECKS THE ASSERTION: no line detection runs, "
                "so a rectangle that in fact holds several lines comes back as "
                "ONE run of nonsense rather than as an error, and that run can "
                "carry a high confidence -- the score is the reader's certainty "
                "about the characters it emitted, never about the layout claim. "
                "Use this Tool where you drew the rectangle yourself and know "
                "what is inside it: a label, a counter, a cost digit, one cell "
                "you measured. Use framework.screen.read_lines instead for a "
                "region nobody can draw a rectangle inside. A confirmed result "
                "carries text_found, and -- when that is true -- text and "
                "confidence and characters with individual text and decoder "
                "confidence from 0 to 1; a reader that looked and saw nothing answers "
                "text_found false rather than failing. A rectangle too large "
                "for one recognition pass is refused by name, and that ceiling "
                "is on cost alone: it is not a check that the rectangle holds "
                "one line. Nothing is captured and nothing is written: the "
                "rectangle is measured on the screenshot the caller named.",
                &measuringDescriptor,
                &readSingleLineArgumentMaterial,
                &readSingleLineOutputMaterial,
            },
            FrameworkToolDefinition{
                k_sessionGetTool,
                "Read back what an earlier chunk of THIS SESSION stored under "
                "name with framework.session.set. A confirmed result carries "
                "name and present, and -- when present is true -- value, the "
                "exact value that was stored. A name this session never stored "
                "answers present false rather than failing, so a chunk can ask "
                "whether its state exists without treating the answer as an "
                "error.",
                &sessionReadDescriptor,
                &sessionEntryArgumentMaterial,
                &sessionGetOutputMaterial,
            },
            FrameworkToolDefinition{
                k_sessionListTool,
                "Report what this session's own state holds. Takes no "
                "arguments. A confirmed result carries names, every name "
                "stored so far in UTF-8 order; stored_bytes, what they weigh "
                "together; and maximum_names and maximum_bytes, the two "
                "ceilings a further framework.session.set is refused past. Use "
                "it to find out what an earlier chunk left behind when the "
                "names are not already known.",
                &sessionReadDescriptor,
                &noArgumentsMaterial,
                &sessionListOutputMaterial,
            },
            FrameworkToolDefinition{
                k_sessionSetTool,
                "Store value under name in this session's own state, so a "
                "LATER CHUNK OF THE SAME SESSION can read it back with "
                "framework.session.get. This is how one piece of work is "
                "written as several chunks: each chunk runs in a VM built for "
                "it and destroyed after it, so a global a chunk assigns is gone "
                "before the next chunk compiles, and this state is not. The "
                "value is any JSON but null and is stored byte for byte; the "
                "framework never reads into it, and the last write under a "
                "name wins. "
                "THE STATE IS THE SESSION'S AND DIES WITH IT: nothing is "
                "written to disk, and the next session starts empty -- use "
                "framework.project.write_text for something that must outlive "
                "this run. A confirmed result carries name and stored_bytes; a "
                "call that would take this session past a ceiling "
                "framework.session.list reports is refused, naming what was "
                "exceeded and what the limit was.",
                &sessionSetDescriptor,
                &sessionSetArgumentMaterial,
                &sessionSetOutputMaterial,
            },
            FrameworkToolDefinition{
                k_uiClickTool,
                "Click what one observation resolved, in the project's own "
                "vocabulary rather than in coordinates. observation_reference "
                "is the handle framework.screen.observe returned and IS SPENT "
                "BY THIS CALL; ui_target, binding and action are identifiers "
                "that observation minted, and a triple it did not carry is "
                "refused. A confirmed result carries delivered, the Host's "
                "verdict and the reason it recorded.",
                &uiClickDescriptor,
                &uiArgumentMaterial,
                &uiInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_uiDragTool,
                "Drag what one observation resolved, in the project's own "
                "vocabulary rather than in coordinates. "
                "observation_reference, ui_target, binding and action all come "
                "from one framework.screen.observe result, and this call "
                "spends the reference. A confirmed result carries delivered, "
                "the Host's verdict and the reason it recorded.",
                &uiDragDescriptor,
                &uiArgumentMaterial,
                &uiInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_uiHoldTool,
                "Engage what one observation resolved and hold it, releasing "
                "on every exit path. Set return_screen to capture or observe "
                "to learn what was on screen WHILE STILL ENGAGED; the result "
                "then carries screen, whose screenshot_sha256 names a retained "
                "screenshot every framework.screen measuring Tool accepts. "
                "observation_reference is spent by this "
                "call. A confirmed result carries action, delivered, held -- "
                "always false, because the release is part of the call -- and "
                "the Host's verdict.",
                &uiHoldDescriptor,
                &uiHoldArgumentMaterial,
                &uiHoldOutputMaterial,
            },
            FrameworkToolDefinition{
                k_uiKeyTool,
                "Send a key action that one observation resolved, in the "
                "project's own vocabulary rather than as a raw key name. "
                "observation_reference, ui_target, binding and action all come "
                "from one framework.screen.observe result, and this call "
                "spends the reference. A confirmed result carries delivered, "
                "the Host's verdict and the reason it recorded.",
                &uiKeyDescriptor,
                &uiArgumentMaterial,
                &uiInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_uiMoveTool,
                "Move to what one observation resolved, without pressing "
                "anything. observation_reference, ui_target, binding and "
                "action all come from one framework.screen.observe result, and "
                "this call spends the reference. A confirmed result carries "
                "delivered, the Host's verdict and the reason it recorded.",
                &uiMoveDescriptor,
                &uiArgumentMaterial,
                &uiInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_uiScrollTool,
                "Scroll what one observation resolved, in the project's own "
                "vocabulary rather than in wheel detents. "
                "observation_reference, ui_target, binding and action all come "
                "from one framework.screen.observe result, and this call "
                "spends the reference. A confirmed result carries delivered, "
                "the Host's verdict and the reason it recorded.",
                &uiScrollDescriptor,
                &uiArgumentMaterial,
                &uiInputOutputMaterial,
            },
            FrameworkToolDefinition{
                k_nowTool,
                "Read the Operator's wall clock and answer with the instant it "
                "read. Takes no arguments, observes no frame and delivers "
                "nothing. This is the only source of wall-clock time inside a "
                "run: the VM carries no clock of its own, because a pure-data "
                "VM must hold no nondeterministic source. A confirmed result "
                "carries read_at_unix_ms -- Unix milliseconds since "
                "1970-01-01T00:00:00Z, as a decimal string, on the same clock "
                "and in the same rendering as a framework.screen.capture "
                "receipt's created_at_unix_ms. The reading is a durable Tool "
                "outcome, so replaying this call position answers the instant "
                "that was recorded rather than a fresh one.",
                &nowDescriptor,
                &noArgumentsMaterial,
                &nowOutputMaterial,
            },
            FrameworkToolDefinition{
                k_statusTool,
                "Report what this run is and what it may still do. Takes no "
                "arguments, observes no frame and delivers nothing. A "
                "confirmed result carries session_id, controlled_target_id, "
                "the installed_generation this run is pinned to, and access, "
                "which is writable or read_only.",
                &statusDescriptor,
                &noArgumentsMaterial,
                &statusOutputMaterial,
            },
            FrameworkToolDefinition{
                k_waitTool,
                "Wait for duration_ms whole milliseconds before returning. The "
                "longest wait this Tool admits is stated in "
                "properties.duration_ms.maximum of its own input schema; a "
                "larger value is refused rather than clamped. A confirmed "
                "result carries the duration_ms asked for and completed, which "
                "is false when a cooperative stop ended the wait early -- a "
                "recorded fact about the call, not a failure.",
                &waitDescriptor,
                &waitArgumentMaterial,
                &waitOutputMaterial,
            },
        };

        [[nodiscard]]
        auto frameworkDefinition(std::string_view name)
            -> FrameworkToolDefinition const*
        {
            auto const found = std::ranges::find(
                k_frameworkTools,
                name,
                &FrameworkToolDefinition::name
            );
            return found == k_frameworkTools.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto stringArray(std::span<std::string const> values) -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(values.size());
            for (auto const& value : values)
            {
                rendered.emplace_back(json::Value::ofString(value));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        [[nodiscard]]
        auto effectBoundsMaterial(std::span<EffectBound const> bounds)
            -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(bounds.size());
            for (auto const& bound : bounds)
            {
                rendered.emplace_back(json::Value::ofObject({
                    {"maximum_risk",
                     json::Value::ofString(
                         std::string{riskWireName(bound.maximumRisk)}
                     )},
                    {"namespaced_type",
                     json::Value::ofString(bound.namespacedType)},
                    {"payload_schema_hash",
                     json::Value::ofString(bound.payloadSchemaHash.hex())},
                    {"scope_kind", json::Value::ofString(bound.scopeKind)},
                }));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        [[nodiscard]]
        auto descriptorMaterial(
            FrameworkToolDefinition const& definition,
            ToolDescriptor const& descriptor
        ) -> json::Value
        {
            return json::Value::ofObject({
                {"description",
                 json::Value::ofString(std::string{definition.description})},
                {"effect_bounds",
                 effectBoundsMaterial(descriptor.effectBounds)},
                {"idempotency",
                 json::Value::ofString(
                     std::string{
                         toolIdempotencyWireName(descriptor.idempotency)
                     }
                 )},
                {"mutability",
                 json::Value::ofString(
                     std::string{
                         toolMutabilityWireName(descriptor.mutability)
                     }
                 )},
                {"name", json::Value::ofString(std::string{definition.name})},
                {"input_schema", definition.argumentMaterial()},
                {"output_schema", definition.outputMaterial()},
                {"required_capabilities",
                 stringArray(descriptor.requiredCapabilities)},
                {"surface",
                 json::Value::ofString(
                     std::string{toolSurfaceWireName(descriptor.surface)}
                 )},
                {"timeout",
                 json::Value::ofObject({
                     {"maximum_elapsed_ms",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.timeout.maximumElapsedMillis
                          )
                      )},
                     {"on_timeout",
                      json::Value::ofString(
                          std::string{
                              timeoutActionWireName(
                                  descriptor.timeout.onTimeout
                              )
                          }
                      )},
                 })},
                {"tool_version",
                 json::Value::ofString(descriptor.toolVersion)},
                {"ui_action_bounds", stringArray(descriptor.uiActionBounds)},
            });
        }

        [[nodiscard]]
        auto describeTool(
            std::span<ToolCatalogEntry const> tools,
            std::string_view toolName
        ) -> Result<ToolDescriptor>
        {
            auto const found = std::ranges::find(
                tools,
                toolName,
                &ToolCatalogEntry::name
            );
            if (found == tools.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog declares no tool named "
                        + std::string{toolName}
                );
            }
            return found->descriptor;
        }

        [[nodiscard]]
        auto offerTools(
            std::span<ToolCatalogEntry const> tools,
            ControllerProfile profile,
            std::span<std::string const> heldCapabilities
        ) -> std::vector<OfferedTool>
        {
            auto offered = std::vector<OfferedTool>{};
            for (auto const& entry : tools)
            {
                if (!toolSurfaceAllowed(profile, entry.descriptor.surface))
                {
                    continue;
                }
                if (
                    missingRequiredToolCapability(
                        heldCapabilities,
                        entry.descriptor.requiredCapabilities
                    )
                )
                {
                    continue;
                }
                offered.emplace_back(OfferedTool{
                    .name    = entry.name,
                    .version = entry.descriptor.toolVersion,
                });
            }
            return offered;
        }
    }

    auto missingRequiredToolCapability(
        std::span<std::string const> heldCapabilities,
        std::span<std::string const> requiredCapabilities
    ) -> std::optional<std::string>
    {
        auto const missing = std::ranges::find_if(
            requiredCapabilities,
            [heldCapabilities](std::string const& capability)
            {
                return !std::ranges::contains(heldCapabilities, capability);
            }
        );
        if (missing == requiredCapabilities.end())
        {
            return std::nullopt;
        }
        return *missing;
    }

    CallerIdempotencyNamespace::CallerIdempotencyNamespace(std::string value)
        : m_value{std::move(value)}
    {
    }

    auto CallerIdempotencyNamespace::create(std::string value)
        -> Result<CallerIdempotencyNamespace>
    {
        if (
            value.empty()
            || value.size() > k_maximumCallerIdentityBytes
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "caller idempotency namespace must contain 1 to 256 bytes"
            );
        }
        return CallerIdempotencyNamespace{std::move(value)};
    }

    auto CallerIdempotencyNamespace::value() const noexcept
        -> std::string const&
    {
        return m_value;
    }

    RootRequestKey::RootRequestKey(std::string value)
        : m_value{std::move(value)}
    {
    }

    auto RootRequestKey::create(std::string value) -> Result<RootRequestKey>
    {
        if (
            value.empty()
            || value.size() > k_maximumCallerIdentityBytes
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "root request key must contain 1 to 256 bytes"
            );
        }
        return RootRequestKey{std::move(value)};
    }

    auto RootRequestKey::value() const noexcept -> std::string const&
    {
        return m_value;
    }

    ToolRootRequestIdentity::ToolRootRequestIdentity(
        CallerIdempotencyNamespace callerNamespace,
        RootRequestKey requestKey,
        CanonicalJson requestPreimage,
        ContentHash identity
    )
        : m_callerNamespace{std::move(callerNamespace)}
        , m_requestKey{std::move(requestKey)}
        , m_requestPreimage{std::move(requestPreimage)}
        , m_identity{identity}
    {
    }

    auto ToolRootRequestIdentity::create(
        std::string callerNamespace,
        std::string requestKey,
        CanonicalJson requestPreimage
    ) -> Result<ToolRootRequestIdentity>
    {
        UF_TRY_VALUE(
            validatedNamespace,
            CallerIdempotencyNamespace::create(std::move(callerNamespace))
        );
        UF_TRY_VALUE(
            validatedKey,
            RootRequestKey::create(std::move(requestKey))
        );
        auto const material = rootIdentityMaterial(
            validatedNamespace,
            validatedKey,
            requestPreimage
        );
        UF_TRY_VALUE(identity, sha256(std::as_bytes(std::span{material})));
        return ToolRootRequestIdentity{
            std::move(validatedNamespace),
            std::move(validatedKey),
            std::move(requestPreimage),
            identity,
        };
    }

    auto ToolRootRequestIdentity::identity() const -> ContentHash
    {
        return m_identity;
    }

    auto ToolRootRequestIdentity::callerNamespace() const noexcept
        -> CallerIdempotencyNamespace const&
    {
        return m_callerNamespace;
    }

    auto ToolRootRequestIdentity::requestKey() const noexcept
        -> RootRequestKey const&
    {
        return m_requestKey;
    }

    auto ToolRootRequestIdentity::requestPreimage() const noexcept
        -> CanonicalJson const&
    {
        return m_requestPreimage;
    }

    auto ToolRootRequestIdentity::relationTo(
        ToolRootRequestIdentity const& other
    ) const noexcept -> RootRequestRelation
    {
        if (
            m_callerNamespace != other.m_callerNamespace
            || m_requestKey != other.m_requestKey
        )
        {
            return RootRequestRelation::Distinct;
        }
        return m_requestPreimage.bytes() == other.m_requestPreimage.bytes()
            ? RootRequestRelation::SameRequest
            : RootRequestRelation::Conflict;
    }

    ToolCallParent::ToolCallParent(
        ContentHash rootIdentity,
        ContentHash identity
    )
        : m_rootIdentity{rootIdentity}
        , m_identity{identity}
    {
    }

    auto ToolCallParent::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolCallParent::identity() const -> ContentHash
    {
        return m_identity;
    }

    ValidatedToolInvocation::ValidatedToolInvocation(
        ToolProviderIdentity provider,
        std::string toolName,
        CanonicalJson canonicalArgs,
        ToolDescriptor descriptor
    )
        : m_provider{std::move(provider)}
        , m_toolName{std::move(toolName)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_descriptor{std::move(descriptor)}
    {
    }

    auto ValidatedToolInvocation::provider() const noexcept
        -> ToolProviderIdentity const&
    {
        return m_provider;
    }

    auto ValidatedToolInvocation::toolName() const noexcept -> std::string const&
    {
        return m_toolName;
    }

    auto ValidatedToolInvocation::canonicalArgs() const noexcept
        -> CanonicalJson const&
    {
        return m_canonicalArgs;
    }

    auto ValidatedToolInvocation::descriptor() const noexcept
        -> ToolDescriptor const&
    {
        return m_descriptor;
    }

    ToolCallPositionIdentity::ToolCallPositionIdentity(
        ContentHash identity,
        ContentHash rootIdentity,
        ContentHash parentIdentity,
        uint32 sequence,
        ToolExecutionIdentity executionIdentity,
        ToolProviderIdentity provider,
        std::string toolName,
        std::string toolVersion,
        std::string canonicalArgs,
        ContentHash canonicalArgsHash,
        std::optional<ContentHash> observationReference,
        ToolDescriptor descriptor
    )
        : m_identity{identity}
        , m_rootIdentity{rootIdentity}
        , m_parentIdentity{parentIdentity}
        , m_sequence{sequence}
        , m_executionIdentity{std::move(executionIdentity)}
        , m_provider{std::move(provider)}
        , m_toolName{std::move(toolName)}
        , m_toolVersion{std::move(toolVersion)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_canonicalArgsHash{canonicalArgsHash}
        , m_observationReference{std::move(observationReference)}
        , m_descriptor{std::move(descriptor)}
    {
    }

    auto ToolCallPositionIdentity::create(
        ContentHash const& rootIdentity,
        ToolCallParent const& parent,
        uint32 sequence,
        ToolExecutionIdentity const& executionIdentity,
        ValidatedToolInvocation const& invocation,
        std::optional<ContentHash> observationReference
    ) -> Result<ToolCallPositionIdentity>
    {
        if (parent.rootIdentity() != rootIdentity)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "tool call parent belongs to a different root request"
            );
        }
        auto const parentIdentity = parent.identity();
        auto const material       = callIdentityMaterial(
            rootIdentity,
            parentIdentity,
            sequence,
            executionIdentity,
            invocation,
            observationReference
        );
        UF_TRY_VALUE(identity, sha256(std::as_bytes(std::span{material})));
        return ToolCallPositionIdentity{
            identity,
            rootIdentity,
            parentIdentity,
            sequence,
            executionIdentity,
            invocation.provider(),
            invocation.toolName(),
            invocation.descriptor().toolVersion,
            invocation.canonicalArgs().bytes(),
            invocation.canonicalArgs().contentHash(),
            std::move(observationReference),
            invocation.descriptor(),
        };
    }

    auto ToolCallPositionIdentity::identity() const -> ContentHash
    {
        return m_identity;
    }

    auto ToolCallPositionIdentity::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolCallPositionIdentity::parentIdentity() const -> ContentHash
    {
        return m_parentIdentity;
    }

    auto ToolCallPositionIdentity::sequence() const noexcept -> uint32
    {
        return m_sequence;
    }

    auto ToolCallPositionIdentity::executionIdentity() const noexcept
        -> ToolExecutionIdentity const&
    {
        return m_executionIdentity;
    }

    auto ToolCallPositionIdentity::provider() const noexcept
        -> ToolProviderIdentity const&
    {
        return m_provider;
    }

    auto ToolCallPositionIdentity::toolName() const noexcept
        -> std::string const&
    {
        return m_toolName;
    }

    auto ToolCallPositionIdentity::toolVersion() const noexcept
        -> std::string const&
    {
        return m_toolVersion;
    }

    auto ToolCallPositionIdentity::canonicalArgs() const noexcept
        -> std::string const&
    {
        return m_canonicalArgs;
    }

    auto ToolCallPositionIdentity::observationReference() const noexcept
        -> std::optional<ContentHash> const&
    {
        return m_observationReference;
    }

    auto ToolCallPositionIdentity::canonicalArgsHash() const -> ContentHash
    {
        return m_canonicalArgsHash;
    }

    auto ToolCallPositionIdentity::descriptor() const noexcept
        -> ToolDescriptor const&
    {
        return m_descriptor;
    }

    ToolCallIssuingContext::ToolCallIssuingContext(
        ContentHash rootIdentity,
        ToolCallParent parent,
        ToolExecutionIdentity executionIdentity
    )
        : m_rootIdentity{rootIdentity}
        , m_parent{parent}
        , m_executionIdentity{std::move(executionIdentity)}
    {
    }

    auto ToolCallIssuingContext::forRoot(
        ToolRootRequestIdentity const& root,
        ToolExecutionIdentity executionIdentity
    ) -> ToolCallIssuingContext
    {
        return ToolCallIssuingContext{
            root.identity(),
            ToolCallParent{root.identity(), root.identity()},
            std::move(executionIdentity),
        };
    }

    auto ToolCallIssuingContext::forHandler(
        ToolCallPositionIdentity const& parent
    ) -> ToolCallIssuingContext
    {
        return ToolCallIssuingContext{
            parent.rootIdentity(),
            ToolCallParent{parent.rootIdentity(), parent.identity()},
            parent.executionIdentity(),
        };
    }

    auto ToolCallIssuingContext::issuedCalls() const noexcept -> uint32
    {
        return m_issuedCalls;
    }

    auto ToolCallIssuingContext::issueNext(
        ValidatedToolInvocation const& invocation,
        std::optional<ContentHash> observationReference
    ) -> Result<ToolCallPositionIdentity>
    {
        if (m_issuedCalls == std::numeric_limits<uint32>::max())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Tool call index is exhausted for this issuing context"
            );
        }
        UF_TRY_VALUE(
            position,
            ToolCallPositionIdentity::create(
                m_rootIdentity,
                m_parent,
                m_issuedCalls + 1U,
                m_executionIdentity,
                invocation,
                std::move(observationReference)
            )
        );
        ++m_issuedCalls;
        return position;
    }

    auto ToolCallIssuingContext::issue(
        ValidatedToolInvocation const& invocation
    ) -> Result<ToolCallPositionIdentity>
    {
        return issueNext(invocation, std::nullopt);
    }

    auto ToolCallIssuingContext::issueAgainstObservation(
        ValidatedToolInvocation const& invocation,
        SnapshotObservationReference const& observation
    ) -> Result<ToolCallPositionIdentity>
    {
        return issueNext(invocation, observation.identity());
    }

    auto ToolCallIssuingContext::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolCallIssuingContext::parent() const noexcept
        -> ToolCallParent const&
    {
        return m_parent;
    }

    auto toolSurfaceAllowed(
        ControllerProfile profile,
        ToolSurface surface
    ) noexcept -> bool
    {
        return !profile.semanticToolsOnly || surface == ToolSurface::Semantic;
    }

    ProjectToolCatalogSchemaOwner::ProjectToolCatalogSchemaOwner(
        ContentHash projectRegistrationHash,
        ContentHash toolCatalogHash,
        std::vector<ToolCatalogEntry> tools,
        ToolArgumentValidator validateArguments
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolCatalogHash{toolCatalogHash}
        , m_tools{std::move(tools)}
        , m_validateArguments{std::move(validateArguments)}
    {
    }

    auto ProjectToolCatalogSchemaOwner::create(
        ProjectIdentity const& project,
        std::string_view exactToolCatalogBytes,
        ToolCatalogReader const& readCatalog,
        ToolArgumentValidator validateArguments
    ) -> Result<ProjectToolCatalogSchemaOwner>
    {
        if (!readCatalog || !validateArguments)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectToolCatalogSchemaOwner requires both catalog readers"
            );
        }
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{exactToolCatalogBytes}))
        );
        if (catalogHash != project.toolCatalogHash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool declaration bytes do not match the registration's tool_catalog_hash"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            tools,
            readCatalog(),
            "reading the Tool Catalog's declared tools"
        );
        // A catalog that declares nothing is a value and not an absence: a
        // Project that binds no Tool states an empty catalog exactly as it
        // states an empty entry set and an empty binding union, and refusing
        // it here would leave such a Project with no legal document at all.
        for (auto const& entry : tools)
        {
            // The positive half, and the only half. A Project owns the
            // namespace it registered -- its plugin_id -- and a descriptor
            // naming anything outside it is declaring a Tool that is not this
            // registrant's, whether the namespace is the Framework's or another
            // Project's. The reserved-prefix refusal this replaced could only
            // catch the first of those two, and is unreachable behind this one
            // because a plugin_id inside `framework.` is refused at the
            // registration (manifest.cpp validateClaims).
            UF_TRY(validateToolNameOwnership(entry.name, project.pluginId()));
            if (entry.descriptor.toolVersion.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog descriptor must carry a tool version"
                );
            }
            if (entry.description.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog descriptor must carry a description"
                );
            }
        }
        std::ranges::sort(tools, {}, &ToolCatalogEntry::name);
        auto const repeated = std::ranges::adjacent_find(
            tools,
            {},
            &ToolCatalogEntry::name
        );
        if (repeated != tools.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool Catalog declares one tool name twice"
            );
        }
        return ProjectToolCatalogSchemaOwner{
            project.hash(),
            catalogHash,
            std::move(tools),
            std::move(validateArguments),
        };
    }

    auto ProjectToolCatalogSchemaOwner::projectRegistrationHash() const
        -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectToolCatalogSchemaOwner::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto ProjectToolCatalogSchemaOwner::toolNames() const
        -> std::vector<std::string>
    {
        auto names = std::vector<std::string>{};
        names.reserve(m_tools.size());
        for (auto const& tool : m_tools)
        {
            names.emplace_back(tool.name);
        }
        return names;
    }

    auto ProjectToolCatalogSchemaOwner::entries() const
        -> std::vector<ToolCatalogEntry>
    {
        return m_tools;
    }

    auto ProjectToolCatalogSchemaOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        UF_TRY_VALUE(descriptor, describe(toolName));
        UF_TRY_CONTEXT(
            m_validateArguments(toolName, canonicalArgs.bytes()),
            "validating the arguments against the schema this descriptor names"
        );
        return ValidatedToolInvocation{
            ToolProviderIdentity{ProjectToolProvider{
                .projectRegistrationHash = m_projectRegistrationHash,
                .toolCatalogHash         = m_toolCatalogHash,
            }},
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto ProjectToolCatalogSchemaOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        return withContext(
            describeTool(m_tools, toolName),
            "reading the Project Tool Catalog"
        );
    }

    auto ProjectToolCatalogSchemaOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        return offerTools(m_tools, profile, heldCapabilities);
    }

    ProjectToolBindingTable::ProjectToolBindingTable(
        ContentHash projectRegistrationHash,
        std::vector<ProjectToolBinding> bindings
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_bindings{std::move(bindings)}
    {
    }

    auto ProjectToolBindingTable::bind(
        ProjectIdentity const& project,
        ProjectToolCatalogSchemaOwner const& catalog,
        std::span<std::string const> exportedEntryPoints
    ) -> Result<ProjectToolBindingTable>
    {
        if (catalog.projectRegistrationHash() != project.hash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project Tool binding requires the Tool Catalog this "
                "registration pinned"
            );
        }

        auto const& bindings = project.projectToolBindings();
        auto const names     = catalog.toolNames();

        // The three-way join, in the one place that states it. The offline
        // project kit calls the same function against a declaration under
        // edit, so a directory this refuses is a directory `project check`
        // refuses first.
        UF_TRY(validateProjectToolBindings(names, bindings, exportedEntryPoints));

        // The registration's own half of the rule, which only a registration
        // can apply: a Tool declared here is a Tool a caller may address, and
        // one with no binding is a call nothing could answer.
        for (auto const& name : names)
        {
            auto const bound = std::ranges::find(
                bindings,
                name,
                &ProjectToolBinding::toolName
            );
            if (bound == bindings.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Project Tool " + name
                        + " is declared with no binding to a Project entry"
                );
            }
        }
        return ProjectToolBindingTable{project.hash(), bindings};
    }

    auto ProjectToolBindingTable::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectToolBindingTable::bindings() const noexcept
        -> std::vector<ProjectToolBinding> const&
    {
        return m_bindings;
    }

    auto ProjectToolBindingTable::entryPointFor(
        std::string_view toolName
    ) const -> Result<std::string>
    {
        auto const bound = std::ranges::find(
            m_bindings,
            toolName,
            &ProjectToolBinding::toolName
        );
        if (bound == m_bindings.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this Project registration binds no Tool named "
                    + std::string{toolName}
            );
        }
        return bound->entryPoint;
    }

    auto ProjectToolBindingTable::entryPoints() const -> std::vector<std::string>
    {
        auto entries = std::vector<std::string>{};
        entries.reserve(m_bindings.size());
        for (auto const& binding : m_bindings)
        {
            entries.emplace_back(binding.entryPoint);
        }
        std::ranges::sort(entries);
        auto const repeated = std::ranges::unique(entries);
        entries.erase(repeated.begin(), repeated.end());
        return entries;
    }

    FrameworkToolCatalogOwner::FrameworkToolCatalogOwner(
        ContentHash toolCatalogHash,
        std::string canonicalJcs,
        std::vector<ToolCatalogEntry> tools,
        std::vector<CompiledArgumentSchema> argumentSchemas
    )
        : m_toolCatalogHash{toolCatalogHash}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_tools{std::move(tools)}
        , m_argumentSchemas{std::move(argumentSchemas)}
    {
    }

    auto FrameworkToolCatalogOwner::create()
        -> Result<FrameworkToolCatalogOwner>
    {
        auto tools    = std::vector<ToolCatalogEntry>{};
        auto material = std::vector<json::Value>{};
        auto schemas  = std::vector<CompiledArgumentSchema>{};
        tools.reserve(k_frameworkTools.size());
        material.reserve(k_frameworkTools.size());
        schemas.reserve(k_frameworkTools.size());
        for (auto const& definition : k_frameworkTools)
        {
            UF_TRY_VALUE_CONTEXT(
                descriptor,
                definition.descriptor(),
                "building a Framework Tool Catalog descriptor"
            );
            auto inputSchema = definition.argumentMaterial();

            // The published bytes are what judges a call, not a second reading
            // of them: the schema is compiled from the exact material that
            // reaches tool_catalog_hash, so a contract this evaluator cannot
            // apply is a refusal here, at startup, rather than a constraint
            // nothing ever checked.
            UF_TRY_VALUE_CONTEXT(
                compiled,
                json::Schema::compile(json::Schema::Document{
                    .label      = definition.name,
                    .exactBytes = json::canonicalBytes(inputSchema),
                }),
                "compiling a Framework Tool's published argument schema"
            );
            schemas.emplace_back(CompiledArgumentSchema{
                .name   = std::string{definition.name},
                .schema = std::move(compiled),
            });

            material.emplace_back(descriptorMaterial(definition, descriptor));
            tools.emplace_back(ToolCatalogEntry{
                .name         = std::string{definition.name},
                .description  = std::string{definition.description},
                .inputSchema  = std::move(inputSchema),
                .outputSchema = definition.outputMaterial(),
                .descriptor   = std::move(descriptor),
            });
        }
        auto canonicalJcs = json::canonicalBytes(json::Value::ofObject({
            // One shape for every answer, stated once. A caller reads it here
            // and each Tool's own output_schema for the `result` inside it.
            {"answer_envelope", answerEnvelopeMaterial()},

            // This material is deliberately internal until the execution
            // adapters and durable runtime can publish one atomic wire cut.
            {"internal_generation", json::Value::ofNumber(0.0)},
            {"owner", json::Value::ofString("framework")},
            {"tools", json::Value::ofArray(std::move(material))},
        }));
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{canonicalJcs}))
        );
        return FrameworkToolCatalogOwner{
            catalogHash,
            std::move(canonicalJcs),
            std::move(tools),
            std::move(schemas),
        };
    }

    auto FrameworkToolCatalogOwner::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto FrameworkToolCatalogOwner::canonicalJcs() const noexcept
        -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto FrameworkToolCatalogOwner::toolNames() const -> std::vector<std::string>
    {
        auto names = std::vector<std::string>{};
        names.reserve(m_tools.size());
        for (auto const& tool : m_tools)
        {
            names.emplace_back(tool.name);
        }
        return names;
    }

    auto FrameworkToolCatalogOwner::entries() const
        -> std::vector<ToolCatalogEntry>
    {
        return m_tools;
    }

    auto FrameworkToolCatalogOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        auto const* const p_definition = frameworkDefinition(toolName);
        if (p_definition == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Framework Tool Catalog declares no tool named " + toolName
            );
        }
        auto const compiled = std::ranges::find(
            m_argumentSchemas,
            toolName,
            &CompiledArgumentSchema::name
        );
        // Every definition compiled its schema in create(), so a definition
        // this found without a schema beside it would be a catalog that was
        // built by something other than create().
        UF_CHECK(compiled != m_argumentSchemas.end());
        UF_TRY(adoptSchemaRefusal(
            compiled->schema.validate(canonicalArgs.value()),
            toolName
        ));
        UF_TRY_VALUE(descriptor, describe(toolName));
        return ValidatedToolInvocation{
            ToolProviderIdentity{FrameworkToolProvider{
                .toolCatalogHash = m_toolCatalogHash,
            }},
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto FrameworkToolCatalogOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        return withContext(
            describeTool(m_tools, toolName),
            "reading the Framework Tool Catalog"
        );
    }

    auto FrameworkToolCatalogOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        return offerTools(m_tools, profile, heldCapabilities);
    }

    auto toolIdentityPreimageMaterial() -> Result<std::string>
    {
        // Distinct probe digests, so a member that took another member's value
        // would move these bytes rather than hide inside them.
        auto const probe = [](std::string_view label) -> Result<ContentHash>
        {
            return sha256(std::as_bytes(std::span{label}));
        };
        UF_TRY_VALUE(rootProbe, probe("umbraflow-preimage-probe-root"));
        UF_TRY_VALUE(parentProbe, probe("umbraflow-preimage-probe-parent"));
        UF_TRY_VALUE(runProbe, probe("umbraflow-preimage-probe-run"));
        UF_TRY_VALUE(releaseProbe, probe("umbraflow-preimage-probe-release"));
        UF_TRY_VALUE(protocolProbe, probe("umbraflow-preimage-probe-protocol"));
        UF_TRY_VALUE(environmentProbe, probe("umbraflow-preimage-probe-environment"));
        UF_TRY_VALUE(observationProbe, probe("umbraflow-preimage-probe-observation"));
        UF_TRY_VALUE(registrationProbe, probe("umbraflow-preimage-probe-registration"));
        UF_TRY_VALUE(catalogProbe, probe("umbraflow-preimage-probe-catalog"));

        UF_TRY_VALUE(
            callerNamespace,
            CallerIdempotencyNamespace::create("umbraflow-preimage-probe-caller")
        );
        UF_TRY_VALUE(
            requestKey,
            RootRequestKey::create("umbraflow-preimage-probe-request")
        );
        UF_TRY_VALUE(
            requestPreimage,
            CanonicalJson::parseExact(R"({"probe":1})")
        );

        UF_TRY_VALUE(catalog, FrameworkToolCatalogOwner::create());
        UF_TRY_VALUE(
            arguments,
            CanonicalJson::parseExact(
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})"
            )
        );
        UF_TRY_VALUE(
            invocation,
            catalog.validate(std::string{k_observeTool}, std::move(arguments))
        );

        // Both provider arms, rendered through the same visitor the identity
        // uses, so the arm tags and their member order are covered even though
        // one coordinate can carry only one of them.
        auto providers = std::string{};
        std::visit(
            AppendProviderIdentity{providers},
            ToolProviderIdentity{FrameworkToolProvider{
                .toolCatalogHash = catalogProbe,
            }}
        );
        std::visit(
            AppendProviderIdentity{providers},
            ToolProviderIdentity{ProjectToolProvider{
                .projectRegistrationHash = registrationProbe,
                .toolCatalogHash         = catalogProbe,
            }}
        );

        // The framing rule itself, which no sample above can lose: a part is
        // its UTF-8 byte length, a colon, and the bytes.
        auto framing = std::string{};
        appendIdentityPart(framing, "umbraflow-preimage-probe-part");

        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = runProbe,
            .frameworkReleaseIdentity    = releaseProbe,
            .toolRuntimeProtocolIdentity = protocolProbe,
            .environmentIdentity         = environmentProbe,
        };
        return json::canonicalBytes(json::Value::ofObject({
            {"call_preimage_observed",
             json::Value::ofString(callIdentityMaterial(
                 rootProbe,
                 parentProbe,
                 7U,
                 execution,
                 invocation,
                 observationProbe
             ))},
            {"call_preimage_unobserved",
             json::Value::ofString(callIdentityMaterial(
                 rootProbe,
                 parentProbe,
                 7U,
                 execution,
                 invocation,
                 std::nullopt
             ))},
            {"part_framing", json::Value::ofString(std::move(framing))},
            {"provider_arms", json::Value::ofString(std::move(providers))},
            {"root_preimage",
             json::Value::ofString(rootIdentityMaterial(
                 callerNamespace,
                 requestKey,
                 requestPreimage
             ))},
        }));
    }
}
