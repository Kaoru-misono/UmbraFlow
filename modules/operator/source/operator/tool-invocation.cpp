#include "tool-invocation.hpp"

#include "snapshot-reference.hpp"

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
        // One input Tool, and its name is the act every one of its six arms
        // performs. `framework.input.coordinate` is gone rather than aliased:
        // two of the six -- `key` and `scroll` -- name no coordinate at all, so
        // the old name was false about a third of its own contract. Ruled in
        // docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md.
        constexpr auto k_deliverInputTool = std::string_view{
            "framework.input.deliver"
        };
        constexpr auto k_semanticInputTool = std::string_view{
            "framework.input.semantic_target"
        };
        constexpr auto k_observeTool = std::string_view{
            "framework.screen.observe"
        };
        // The three measuring Tools an observation's body issues. Each one names
        // a rectangle of THE FRAME THE ENCLOSING OBSERVE IS HOLDING and takes no
        // frame handle at all: a handle could be stored and a frame may not, so
        // they bind by call position to the innermost open frame and are refused
        // by name outside one
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        //
        // There is no `framework.screen.crop`. A crop's answer is the frame's
        // PIXELS, and a Tool result is canonical JSON inside a durable row, so a
        // cropping Tool would put a frame's pixels inside a hashed record --
        // which is the `framework.screen.capture` Tool this design deleted,
        // under another name. Keeping a piece of the screen is an AUTHORING
        // WRITE and is spelled as one, on framework.project.write's capture arm.
        constexpr auto k_censusGridTool = std::string_view{
            "framework.screen.census_grid"
        };
        constexpr auto k_probeTool = std::string_view{
            "framework.screen.probe"
        };
        constexpr auto k_readLinesTool = std::string_view{
            "framework.screen.read_lines"
        };
        constexpr auto k_projectReadTool = std::string_view{
            "framework.project.read"
        };
        constexpr auto k_projectWriteTool = std::string_view{
            "framework.project.write"
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
        // The type IS the delivering Tool's name, and deliberately so: one act
        // gets one spelling. Both input Tools declare it, because both deliver
        // the same input to the same target and differ only in what they aim
        // by -- which is what ToolSurface already says about them.
        constexpr auto k_inputEffectType = k_deliverInputTool;
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
        // The type IS the writing Tool's name, for the input line's reason: one
        // act gets one spelling.
        constexpr auto k_authoringEffectType = k_projectWriteTool;
        constexpr auto k_authoringEffectScope = std::string_view{
            "project_authoring_store"
        };

        constexpr auto k_frameworkToolVersion = std::string_view{"1"};

        // An observe's ceiling now covers ITS BODY as well as its own capture.
        // The frame is the scope of the call that opened it, so every child
        // measurement runs inside this call and is timed against this number; a
        // ceiling sized for a bare capture would refuse a body that measured
        // three rectangles. CALIBRATION: sixty seconds is well above what a
        // bounded body of measurements takes and well below any run budget.
        constexpr auto k_maximumObserveMillis = uint64{60'000U};

        // How many measuring calls one observation's body may issue. It is the
        // Framework's own declaration, sized so that a body may sweep a screen
        // rectangle by rectangle without the ceiling being the thing that
        // decides how finely.
        constexpr auto k_maximumObserveChildCalls = uint32{64U};

        constexpr auto k_maximumMeasureMillis = uint64{10'000U};
        constexpr auto k_maximumProjectMillis = uint64{10'000U};
        constexpr auto k_maximumWaitMillis = uint64{60'000U};
        constexpr auto k_maximumAuditMillis = uint64{1'000U};
        constexpr auto k_maximumStatusMillis = uint64{1'000U};
        constexpr auto k_maximumInputMillis = uint64{15'000U};

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

        using FrameworkArgumentValidator = Status (*)(CanonicalJson const&);
        using FrameworkArgumentMaterial = json::Value (*)();

        // Result rather than a plain descriptor: a mutating Framework Tool
        // declares an effect bound, and an effect bound names the sha256 of a
        // payload schema. Deriving that digest can fail, and a factory that
        // could not say so would have to invent a hash.
        using FrameworkDescriptorFactory = Result<ToolDescriptor> (*)();

        struct FrameworkToolDefinition final
        {
            std::string_view          name{};
            FrameworkDescriptorFactory descriptor{};
            FrameworkArgumentValidator validateArguments{};
            FrameworkArgumentMaterial argumentMaterial{};
        };

        [[nodiscard]]
        auto invalidFrameworkArguments(std::string message)
            -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
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
            Tag,
            KeyName,
            SurfacePixel,
            Milliseconds,
            SignedCount,
            ColourChannel,
            Count,
            Flag,
            Name,
            Text,
        };

        struct ArgumentMember final
        {
            std::string_view   name{};
            ArgumentMemberKind kind{ArgumentMemberKind::Tag};
        };

        // Every member any Framework Tool's contract may carry, spelled once. A
        // member name means the same thing in every contract that carries it,
        // so the kind is a property of the name rather than of the pair.
        constexpr auto k_argumentMembers = std::array{
            ArgumentMember{"action", ArgumentMemberKind::Tag},
            ArgumentMember{"cell_height", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"cell_width", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"colour_blue", ArgumentMemberKind::ColourChannel},
            ArgumentMember{"colour_green", ArgumentMemberKind::ColourChannel},
            ArgumentMember{"colour_red", ArgumentMemberKind::ColourChannel},
            ArgumentMember{"content", ArgumentMemberKind::Text},
            ArgumentMember{"height", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"key", ArgumentMemberKind::KeyName},
            ArgumentMember{"notches", ArgumentMemberKind::SignedCount},
            ArgumentMember{"path", ArgumentMemberKind::Name},
            ArgumentMember{"removes", ArgumentMemberKind::Flag},
            ArgumentMember{"to_x", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"to_y", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"tolerance", ArgumentMemberKind::Count},
            ArgumentMember{"travel_ms", ArgumentMemberKind::Milliseconds},
            ArgumentMember{"width", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"x", ArgumentMemberKind::SurfacePixel},
            ArgumentMember{"y", ArgumentMemberKind::SurfacePixel},
        };

        constexpr auto k_clickArmMembers = std::array{
            std::string_view{"action"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_dragArmMembers = std::array{
            std::string_view{"action"},
            std::string_view{"to_x"},
            std::string_view{"to_y"},
            std::string_view{"travel_ms"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_holdArmMembers = std::array{
            std::string_view{"action"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_keyArmMembers = std::array{
            std::string_view{"action"},
            std::string_view{"key"},
        };
        constexpr auto k_moveArmMembers = std::array{
            std::string_view{"action"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_scrollArmMembers = std::array{
            std::string_view{"action"},
            std::string_view{"notches"},
        };

        // The exact member list one measuring or authoring Tool requires. Every
        // one is REQUIRED: a Framework argument contract is a closed object, so
        // an absent member is a malformed call rather than a defaulted one.
        //
        // Declared in UTF-8 order of the member names, so the rendered contract
        // material is already sorted.
        constexpr auto k_rectangleMembers = std::array{
            std::string_view{"height"},
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
            std::string_view{"tolerance"},
            std::string_view{"width"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_projectReadMembers = std::array{
            std::string_view{"path"},
        };

        // framework.project.write's two arms. One act -- a change to the
        // project's own authoring store -- with two sources for what is
        // written: text the chunk is holding, and the pixels of the frame this
        // call captures. A second write Tool would be a second spelling of one
        // act, and a `capture` that ANSWERED with the pixels would be the
        // deleted screen-capture Tool.
        constexpr auto k_captureWriteMembers = std::array{
            std::string_view{"action"},
            std::string_view{"height"},
            std::string_view{"path"},
            std::string_view{"width"},
            std::string_view{"x"},
            std::string_view{"y"},
        };
        constexpr auto k_textWriteMembers = std::array{
            std::string_view{"action"},
            std::string_view{"content"},
            std::string_view{"path"},
        };

        struct InputArm final
        {
            std::string_view                  action{};
            std::span<std::string_view const> members{};
        };

        // The CLOSED enumeration of input verbs, and the tagged union over it.
        // `action` decides the arm and the arm decides the members, all of
        // which are REQUIRED: this is a sum type, not an object whose absent
        // members mean an older behaviour.
        //
        // `x` and `y` live in the four arms that aim and in no other. A `key`
        // or a `scroll` names no position -- a wheel lands on whatever the
        // target already believes is hovered -- so making them carry
        // coordinates would be an accident dressed as a method.
        //
        // Declared in UTF-8 order of the action names, so the rendered contract
        // material is already sorted and has one spelling rather than one per
        // declaration order.
        constexpr auto k_inputArms = std::array{
            InputArm{"click", k_clickArmMembers},
            InputArm{"drag", k_dragArmMembers},
            InputArm{"hold", k_holdArmMembers},
            InputArm{"key", k_keyArmMembers},
            InputArm{"move", k_moveArmMembers},
            InputArm{"scroll", k_scrollArmMembers},
        };

        auto inputBodyDeclaration() -> ToolBodyDeclaration
        {
            return ToolBodyDeclaration{
                .takesBody = false,
                .taggedBy  = "action",
                .arms      = {
                    ToolBodyArm{.name = "click", .takesBody = false},
                    ToolBodyArm{.name = "drag", .takesBody = false},
                    ToolBodyArm{.name = "hold", .takesBody = true},
                    ToolBodyArm{.name = "key", .takesBody = false},
                    ToolBodyArm{.name = "move", .takesBody = false},
                    ToolBodyArm{.name = "scroll", .takesBody = false},
                },
            };
        }

        constexpr auto k_projectWriteArms = std::array{
            InputArm{"capture", k_captureWriteMembers},
            InputArm{"text", k_textWriteMembers},
        };

        auto projectWriteBodyDeclaration() -> ToolBodyDeclaration
        {
            return ToolBodyDeclaration{
                .takesBody = false,
                .taggedBy  = "action",
                .arms      = {
                    ToolBodyArm{.name = "capture", .takesBody = false},
                    ToolBodyArm{.name = "text", .takesBody = false},
                },
            };
        }

        [[nodiscard]]
        auto armActionNames(std::span<InputArm const> arms) -> std::string
        {
            auto names = std::string{};
            for (auto const& arm : arms)
            {
                if (!names.empty())
                {
                    names += ", ";
                }
                names += arm.action;
            }
            return names;
        }

        [[nodiscard]]
        auto memberList(std::span<std::string_view const> members) -> std::string
        {
            auto rendered = std::string{};
            for (auto const& member : members)
            {
                if (!rendered.empty())
                {
                    rendered += ", ";
                }
                rendered += member;
            }
            return rendered;
        }

        [[nodiscard]]
        auto armFor(std::span<InputArm const> arms, std::string_view action)
            -> InputArm const*
        {
            auto const found = std::ranges::find(
                arms,
                action,
                &InputArm::action
            );
            return found == arms.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto argumentMemberKind(std::string_view member) -> ArgumentMemberKind
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
            return found->kind;
        }

        [[nodiscard]]
        auto argumentMemberValid(
            ArgumentMemberKind kind,
            json::Value const& value
        ) -> bool
        {
            switch (kind)
            {
            case ArgumentMemberKind::Tag:
            case ArgumentMemberKind::KeyName:
            case ArgumentMemberKind::Name:
                return value.kind() == json::ValueKind::String
                    && !value.string().empty();
            case ArgumentMemberKind::Text:
                return value.kind() == json::ValueKind::String;
            case ArgumentMemberKind::SurfacePixel:
                return value.isInteger()
                    && value.number() >= 0.0
                    && value.number()
                        <= static_cast<double>(
                            std::numeric_limits<uint32>::max()
                        );
            case ArgumentMemberKind::Milliseconds:
            case ArgumentMemberKind::Count:
                return value.isInteger() && value.number() >= 0.0;
            case ArgumentMemberKind::SignedCount:
                return value.isInteger();
            case ArgumentMemberKind::ColourChannel:
                return value.isInteger()
                    && value.number() >= 0.0
                    && value.number() <= 255.0;
            case ArgumentMemberKind::Flag:
                return value.kind() == json::ValueKind::Boolean;
            }
            UF_UNREACHABLE_MSG("Unknown Framework argument member kind");
        }

        // One closed object contract: exactly these members, each valid for the
        // kind its name carries. Every measuring and authoring contract is this
        // shape, so it is judged once here rather than once per Tool.
        [[nodiscard]]
        auto requireExactMembers(
            CanonicalJson const& arguments,
            std::string_view toolName,
            std::span<std::string_view const> members
        ) -> Status
        {
            auto const& value = arguments.value();
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != members.size()
            )
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " requires exactly "
                    + memberList(members)
                );
            }
            for (auto const& member : members)
            {
                auto const* const p_value = value.find(member);
                if (
                    p_value == nullptr
                    || !argumentMemberValid(argumentMemberKind(member), *p_value)
                )
                {
                    return invalidFrameworkArguments(
                        std::string{toolName} + " requires exactly "
                        + memberList(members) + ", and '" + std::string{member}
                        + "' is missing or malformed"
                    );
                }
            }
            return ok();
        }

        // The tagged union, judged tag first, for whichever Tool declares one.
        // Every refusal names what was wrong: an action outside the enumeration
        // is named against the whole closed set, and an arm short a member is
        // named against the exact member list its own tag requires.
        [[nodiscard]]
        auto requireTaggedArm(
            CanonicalJson const& arguments,
            std::string_view toolName,
            std::span<InputArm const> arms
        ) -> Status
        {
            auto const& value = arguments.value();
            if (value.kind() != json::ValueKind::Object)
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " arguments must be an object"
                );
            }
            auto const* const p_action = value.find("action");
            if (
                p_action == nullptr
                || p_action->kind() != json::ValueKind::String
            )
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " requires an action naming one of "
                    + armActionNames(arms)
                );
            }
            auto const* const p_arm = armFor(arms, p_action->string());
            if (p_arm == nullptr)
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " action '"
                    + std::string{p_action->string()}
                    + "' is outside the closed set " + armActionNames(arms)
                );
            }
            if (value.members().size() != p_arm->members.size())
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " action '"
                    + std::string{p_arm->action} + "' requires exactly "
                    + memberList(p_arm->members)
                );
            }
            for (auto const& member : p_arm->members)
            {
                auto const* const p_value = value.find(member);
                if (
                    p_value == nullptr
                    || !argumentMemberValid(argumentMemberKind(member), *p_value)
                )
                {
                    return invalidFrameworkArguments(
                        std::string{toolName} + " action '"
                        + std::string{p_arm->action} + "' requires exactly "
                        + memberList(p_arm->members) + ", and '"
                        + std::string{member} + "' is missing or malformed"
                    );
                }
            }
            return ok();
        }

        // The payload shape a Framework input effect carries. It is rendered
        // here rather than read from a file so that the Framework owns its own
        // effect schema exactly the way it owns its argument contracts, and so
        // that its digest is derived from material already inside
        // tool_catalog_hash.
        [[nodiscard]]
        auto inputEffectPayloadMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
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

        // The bound BOTH Framework input Tools declare, and there is one of it.
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
        auto inputEffectBounds() -> Result<std::vector<EffectBound>>
        {
            UF_TRY_VALUE(payloadSchemaHash, inputEffectPayloadSchemaHash());
            auto bounds = std::vector<EffectBound>{};
            bounds.emplace_back(EffectBound{
                .namespacedType    = std::string{k_inputEffectType},
                .scopeKind         = std::string{k_inputEffectScope},
                .payloadSchemaHash = payloadSchemaHash,
                .maximumRisk       = Risk::Critical,
            });
            return bounds;
        }

        // AN OBSERVATION HOLDS ITS FRAME, and this declaration is what lets it
        // have a body at all
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        // A grant is minted from a parent whose child_tool_names are non-empty
        // and whose row is still dispatching, so a body's measurements are
        // recorded as this call's children only because the three names below
        // are inside tool_catalog_hash.
        //
        // THE CEILINGS ARE READ-ONLY, and that is not a placeholder. A mutating
        // child under this parent could never be admitted whatever this said:
        // admission also matches a child's effect against the ADMITTED ROOT
        // EFFECT ENVELOPE, an observe declares no effect bound so its envelope
        // is empty, and giving it one would make it a mutating call that a
        // deny-all artifact refuses -- which would take read-only screen
        // observation away from the very session that has no policy yet. So the
        // held child measurements are read-only, exactly as V4 rules them, and
        // an input or an authoring write is issued at the top of the run
        // instead.
        //
        // Declared in UTF-8 order of the names, so the rendered declaration is
        // already sorted.
        [[nodiscard]]
        auto observeDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .childEffects         = ChildEffectDeclaration{
                    .childToolNames = {
                        std::string{k_censusGridTool},
                        std::string{k_probeTool},
                        std::string{k_readLinesTool},
                    },
                    .maximumChildSurface    = ToolSurface::Privileged,
                    .maximumChildMutability = ToolMutability::ReadOnly,
                    .maximumChildRisk       = Risk::ReadOnly,
                    .maximumChildCalls      = k_maximumObserveChildCalls,
                },
                .body = ToolBodyDeclaration{.takesBody = true},
                .limits = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumObserveMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumObserveMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // The three measuring Tools. Each is PRIVILEGED because its arguments
        // and its answer are the machine's vocabulary -- a rectangle of pixels,
        // a colour channel -- and never the project's; that label is what an
        // Operator's privileged_surface_tools list judges at the top of a run,
        // and it is deliberately not judged for a child, whose surface is
        // bounded by its parent's declaration instead. So an observe's body
        // measures under deny-all, and a top-level measurement needs the
        // Operator's grant AND still finds no open frame.
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
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumMeasureMillis,
                },
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
        auto projectReadDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumProjectMillis,
                },
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
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
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
        auto authoringEffectBounds() -> Result<std::vector<EffectBound>>
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
                .namespacedType    = std::string{k_authoringEffectType},
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

        // Writing the project's own authoring store, from text or from the
        // pixels of a frame this call captures.
        //
        // PRIVILEGED, because its capture arm names a rectangle of the screen,
        // and a Tool is judged by the more restricted of the vocabularies it
        // speaks. Mutating, so the Operator's policy decides its effect. Under
        // deny-all both refusals fire and both name what was missing.
        [[nodiscard]]
        auto projectWriteDescriptor() -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, authoringEffectBounds());
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {},
                .body                 = projectWriteBodyDeclaration(),
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumProjectMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumProjectMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability = ToolMutability::Mutating,
                .surface    = ToolSurface::Privileged,
                // A second write of the same bytes to the same path leaves the
                // store where the first left it, which is what delivery-safe
                // means: redelivering costs nothing beyond the write.
                .idempotency = ToolIdempotency::DeliverySafe,
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
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumStatusMillis,
                },
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
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumAuditMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumAuditMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Native input against a snapshot-scoped semantic target. Semantic
        // surface deliberately: this is the input an ordinary online actor is
        // meant to reach, and the target it names was resolved by Framework
        // against an observation Framework minted.
        //
        // NonIdempotent because the question the field answers is what
        // REDELIVERING one call would cost, and a second delivered click is a
        // second click. Single use of the observation authority is a separate
        // and stronger guarantee, and neither substitutes for the other.
        [[nodiscard]]
        auto semanticInputDescriptor() -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, inputEffectBounds());
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {std::string{k_semanticInputTool}},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 1U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumInputMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumInputMillis,
                    .onTimeout            = TimeoutAction::Reobserve,
                },
                .mutability  = ToolMutability::Mutating,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::NonIdempotent,
            };
        }

        // The whole input vocabulary in machine terms: six verbs, one tagged
        // contract, and a point measured against the target surface this
        // registration declared rather than a target the model declared.
        // Section 3.2 keeps low-level input a Privileged surface, so being a
        // Framework Tool does not make it generally available -- an Operator
        // that has not listed it under privileged_surface_tools cannot reach it
        // at the top of a run at all, and that grant is the "distinct
        // privileged authority stating plainly that nothing measured it" this
        // Tool's placeholder verdict was waiting for.
        [[nodiscard]]
        auto deliverInputDescriptor() -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, inputEffectBounds());
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {std::string{k_deliverInputTool}},
                .childEffects         = ChildEffectDeclaration{
                    .childToolNames = {
                        std::string{k_observeTool},
                    },
                    .maximumChildSurface    = ToolSurface::Semantic,
                    .maximumChildMutability = ToolMutability::ReadOnly,
                    .maximumChildRisk       = Risk::ReadOnly,
                    .maximumChildCalls      = k_maximumObserveChildCalls,
                },
                .body = inputBodyDeclaration(),
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 1U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumInputMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumInputMillis,
                    .onTimeout            = TimeoutAction::Reobserve,
                },
                .mutability  = ToolMutability::Mutating,
                .surface     = ToolSurface::Privileged,
                .idempotency = ToolIdempotency::NonIdempotent,
            };
        }

        [[nodiscard]]
        auto waitDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 1U,
                    .maximumElapsedMillis = k_maximumWaitMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_waitTimeoutMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Three Framework Tools take no arguments at all. The tool name is a
        // parameter so that each still refuses in its own words, while the one
        // rule they share is stated once.
        [[nodiscard]]
        auto requireNoArguments(
            CanonicalJson const& arguments,
            std::string_view toolName
        ) -> Status
        {
            auto const& value = arguments.value();
            if (
                value.kind() != json::ValueKind::Object
                || !value.members().empty()
            )
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " arguments must be exactly {}"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateObserveArguments(CanonicalJson const& arguments) -> Status
        {
            return requireNoArguments(arguments, k_observeTool);
        }

        [[nodiscard]]
        auto validateStatusArguments(CanonicalJson const& arguments) -> Status
        {
            return requireNoArguments(arguments, k_statusTool);
        }

        // The record is the whole of the call's durable outcome, so it is the
        // whole of the call's arguments. There is deliberately no attribution
        // member: per R5 the root and position are supplied by the seam, and a
        // caller that could state them could attribute its record to another
        // call's position.
        [[nodiscard]]
        auto validateAuditArguments(CanonicalJson const& arguments) -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_record = value.find("record");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 1U
                || p_record == nullptr
                || p_record->kind() != json::ValueKind::Object
            )
            {
                return invalidFrameworkArguments(
                    "framework.audit.record requires only an object record"
                );
            }
            return ok();
        }

        // observation_reference is an OBJECT and never a string carrying JSON.
        // Per R3 a handle stays plain JSON data, so the reference travels as
        // the value it is; re-rendering that member canonically reproduces the
        // exact bytes the Framework minted, which is what the resolution
        // boundary recognises it by.
        [[nodiscard]]
        auto validateSemanticInputArguments(CanonicalJson const& arguments)
            -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_reference = value.find(
                k_observationReferenceArgument
            );
            auto const* const p_target = value.find("semantic_target");
            auto const* const p_action = value.find("ui_action");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 3U
                || p_reference == nullptr
                || p_reference->kind() != json::ValueKind::Object
                || p_target == nullptr
                || p_target->kind() != json::ValueKind::String
                || p_target->string().empty()
                || p_action == nullptr
                || p_action->kind() != json::ValueKind::String
                || p_action->string().empty()
            )
            {
                return invalidFrameworkArguments(
                    "framework.input.semantic_target requires an object "
                    "observation_reference and a non-empty semantic_target "
                    "and ui_action"
                );
            }
            return ok();
        }

        // The tagged union, judged tag first. A verb this Tool cannot validate
        // would be an unbound call it could not refuse by name, which is the
        // hole the free string left.
        [[nodiscard]]
        auto validateDeliverInputArguments(CanonicalJson const& arguments)
            -> Status
        {
            return requireTaggedArm(arguments, k_deliverInputTool, k_inputArms);
        }

        [[nodiscard]]
        auto validateReadLinesArguments(CanonicalJson const& arguments) -> Status
        {
            return requireExactMembers(
                arguments,
                k_readLinesTool,
                k_rectangleMembers
            );
        }

        [[nodiscard]]
        auto validateProbeArguments(CanonicalJson const& arguments) -> Status
        {
            return requireExactMembers(arguments, k_probeTool, k_probeMembers);
        }

        [[nodiscard]]
        auto validateCensusGridArguments(CanonicalJson const& arguments)
            -> Status
        {
            return requireExactMembers(
                arguments,
                k_censusGridTool,
                k_censusGridMembers
            );
        }

        [[nodiscard]]
        auto validateProjectReadArguments(CanonicalJson const& arguments)
            -> Status
        {
            return requireExactMembers(
                arguments,
                k_projectReadTool,
                k_projectReadMembers
            );
        }

        [[nodiscard]]
        auto validateProjectWriteArguments(CanonicalJson const& arguments)
            -> Status
        {
            return requireTaggedArm(
                arguments,
                k_projectWriteTool,
                k_projectWriteArms
            );
        }

        [[nodiscard]]
        auto validateWaitArguments(CanonicalJson const& arguments) -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_duration = value.find("duration_ms");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 1U
                || p_duration == nullptr
                || !p_duration->isInteger()
                || p_duration->number() < 0.0
                || p_duration->number()
                    > static_cast<double>(k_maximumWaitMillis)
            )
            {
                return invalidFrameworkArguments(
                    "framework.workflow.wait requires only integer duration_ms "
                    "in [0, 60000]"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto noArgumentsMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto requiredMembersMaterial(std::vector<json::Value> required)
            -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"required", json::Value::ofArray(std::move(required))},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto auditArgumentMaterial() -> json::Value
        {
            return requiredMembersMaterial({
                json::Value::ofString("record"),
            });
        }

        [[nodiscard]]
        auto semanticInputArgumentMaterial() -> json::Value
        {
            return requiredMembersMaterial({
                json::Value::ofString(
                    std::string{k_observationReferenceArgument}
                ),
                json::Value::ofString("semantic_target"),
                json::Value::ofString("ui_action"),
            });
        }

        // One closed object contract as catalog material: the exact member list
        // is inside tool_catalog_hash, so widening one moves the recorded
        // identity of every call.
        [[nodiscard]]
        auto exactMembersMaterial(std::span<std::string_view const> members)
            -> json::Value
        {
            auto required = std::vector<json::Value>{};
            required.reserve(members.size());
            for (auto const& member : members)
            {
                required.emplace_back(json::Value::ofString(std::string{member}));
            }
            return requiredMembersMaterial(std::move(required));
        }

        [[nodiscard]]
        auto readLinesArgumentMaterial() -> json::Value
        {
            return exactMembersMaterial(k_rectangleMembers);
        }

        [[nodiscard]]
        auto probeArgumentMaterial() -> json::Value
        {
            return exactMembersMaterial(k_probeMembers);
        }

        [[nodiscard]]
        auto censusGridArgumentMaterial() -> json::Value
        {
            return exactMembersMaterial(k_censusGridMembers);
        }

        [[nodiscard]]
        auto projectReadArgumentMaterial() -> json::Value
        {
            return exactMembersMaterial(k_projectReadMembers);
        }

        // The tagged union as catalog material. The tag and every arm's exact
        // member list are inside tool_catalog_hash, so widening one arm or
        // adding a seventh verb moves the recorded identity of every call --
        // which is the whole reason the enumeration is closed.
        [[nodiscard]]
        auto taggedArmMaterial(std::span<InputArm const> armList) -> json::Value
        {
            auto arms = std::vector<json::Member>{};
            arms.reserve(armList.size());
            for (auto const& arm : armList)
            {
                auto required = std::vector<json::Value>{};
                required.reserve(arm.members.size());
                for (auto const& member : arm.members)
                {
                    required.emplace_back(
                        json::Value::ofString(std::string{member})
                    );
                }
                arms.emplace_back(
                    std::string{arm.action},
                    json::Value::ofObject({
                        {"required", json::Value::ofArray(std::move(required))},
                    })
                );
            }
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"arms", json::Value::ofObject(std::move(arms))},
                {"tag", json::Value::ofString("action")},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto deliverInputArgumentMaterial() -> json::Value
        {
            return taggedArmMaterial(k_inputArms);
        }

        [[nodiscard]]
        auto projectWriteArgumentMaterial() -> json::Value
        {
            return taggedArmMaterial(k_projectWriteArms);
        }

        [[nodiscard]]
        auto waitArgumentMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"maximum_duration_ms",
                 json::Value::ofNumber(
                     static_cast<double>(k_maximumWaitMillis)
                 )},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("duration_ms"),
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
                &auditDescriptor,
                &validateAuditArguments,
                &auditArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_deliverInputTool,
                &deliverInputDescriptor,
                &validateDeliverInputArguments,
                &deliverInputArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_semanticInputTool,
                &semanticInputDescriptor,
                &validateSemanticInputArguments,
                &semanticInputArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_projectReadTool,
                &projectReadDescriptor,
                &validateProjectReadArguments,
                &projectReadArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_projectWriteTool,
                &projectWriteDescriptor,
                &validateProjectWriteArguments,
                &projectWriteArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_censusGridTool,
                &measuringDescriptor,
                &validateCensusGridArguments,
                &censusGridArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_observeTool,
                &observeDescriptor,
                &validateObserveArguments,
                &noArgumentsMaterial,
            },
            FrameworkToolDefinition{
                k_probeTool,
                &measuringDescriptor,
                &validateProbeArguments,
                &probeArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_readLinesTool,
                &measuringDescriptor,
                &validateReadLinesArguments,
                &readLinesArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_statusTool,
                &statusDescriptor,
                &validateStatusArguments,
                &noArgumentsMaterial,
            },
            FrameworkToolDefinition{
                k_waitTool,
                &waitDescriptor,
                &validateWaitArguments,
                &waitArgumentMaterial,
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

        // A declaration is catalog material like every other bound: its bytes
        // reach tool_catalog_hash through the descriptor, so widening what a
        // Tool may delegate moves the catalog identity.
        [[nodiscard]]
        auto childEffectsMaterial(ChildEffectDeclaration const& declaration)
            -> json::Value
        {
            return json::Value::ofObject({
                {"child_tool_names", stringArray(declaration.childToolNames)},
                {"maximum_child_calls",
                 json::Value::ofNumber(
                     static_cast<double>(declaration.maximumChildCalls)
                 )},
                {"maximum_child_mutability",
                 json::Value::ofString(
                     std::string{
                         toolMutabilityWireName(
                             declaration.maximumChildMutability
                         )
                     }
                 )},
                {"maximum_child_risk",
                 json::Value::ofString(
                     std::string{riskWireName(declaration.maximumChildRisk)}
                 )},
                {"maximum_child_surface",
                 json::Value::ofString(
                     std::string{
                         toolSurfaceWireName(declaration.maximumChildSurface)
                     }
                 )},
            });
        }

        [[nodiscard]]
        auto bodyMaterial(ToolBodyDeclaration const& declaration) -> json::Value
        {
            if (declaration.taggedBy.empty())
            {
                return json::Value::ofBoolean(declaration.takesBody);
            }
            auto arms = std::vector<json::Member>{};
            arms.reserve(declaration.arms.size());
            for (auto const& arm : declaration.arms)
            {
                arms.emplace_back(
                    arm.name,
                    json::Value::ofBoolean(arm.takesBody)
                );
            }
            return json::Value::ofObject({
                {"arms", json::Value::ofObject(std::move(arms))},
                {"tag", json::Value::ofString(declaration.taggedBy)},
            });
        }

        [[nodiscard]]
        auto bodyMatchesArgumentContract(
            ToolBodyDeclaration const& declaration,
            json::Value const& argumentContract
        ) -> Status
        {
            auto const* const p_tag  = argumentContract.find("tag");
            auto const* const p_arms = argumentContract.find("arms");
            auto const tagged = p_tag != nullptr || p_arms != nullptr;
            if (!tagged)
            {
                if (!declaration.taggedBy.empty())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "an untagged argument contract has a per-arm body declaration"
                    );
                }
                return ok();
            }
            if (
                p_tag == nullptr || p_arms == nullptr
                || p_tag->kind() != json::ValueKind::String
                || p_arms->kind() != json::ValueKind::Object
                || declaration.taggedBy != p_tag->string()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "a tagged argument contract and its body declaration name different tags"
                );
            }

            auto contractArms = std::vector<std::string>{};
            contractArms.reserve(p_arms->members().size());
            for (auto const& [name, ignored] : p_arms->members())
            {
                static_cast<void>(ignored);
                contractArms.emplace_back(name);
            }
            auto bodyArms = std::vector<std::string>{};
            bodyArms.reserve(declaration.arms.size());
            std::ranges::transform(
                declaration.arms,
                std::back_inserter(bodyArms),
                &ToolBodyArm::name
            );
            std::ranges::sort(contractArms);
            std::ranges::sort(bodyArms);
            if (contractArms != bodyArms)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "a tagged Tool body declaration must state every argument arm exactly once"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto descriptorMaterial(
            FrameworkToolDefinition const& definition,
            ToolDescriptor const& descriptor
        ) -> json::Value
        {
            return json::Value::ofObject({
                {"argument_contract", definition.argumentMaterial()},
                {"body", bodyMaterial(descriptor.body)},
                {"child_effects", childEffectsMaterial(descriptor.childEffects)},
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
                {"workflow_limits",
                 json::Value::ofObject({
                     {"maximum_dispatches",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumDispatches
                          )
                      )},
                     {"maximum_elapsed_ms",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumElapsedMillis
                          )
                      )},
                     {"maximum_observations",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumObservations
                          )
                      )},
                     {"maximum_steps",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumSteps
                          )
                      )},
                     {"maximum_waits",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumWaits
                          )
                      )},
                 })},
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

    auto ToolCallPositionIdentity::asParent() const -> ToolCallParent
    {
        return ToolCallParent{m_rootIdentity, m_identity};
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
        ToolCallPositionIdentity const& handlerCall
    ) -> ToolCallIssuingContext
    {
        return ToolCallIssuingContext{
            handlerCall.rootIdentity(),
            handlerCall.asParent(),
            handlerCall.executionIdentity(),
        };
    }

    auto ToolCallIssuingContext::issueNext(
        ValidatedToolInvocation const& invocation,
        std::optional<ContentHash> observationReference
    ) -> Result<ToolCallPositionIdentity>
    {
        if (m_issuedChildren == std::numeric_limits<uint32>::max())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Tool call child index is exhausted for this issuing context"
            );
        }
        UF_TRY_VALUE(
            position,
            ToolCallPositionIdentity::create(
                m_rootIdentity,
                m_parent,
                m_issuedChildren + 1U,
                m_executionIdentity,
                invocation,
                std::move(observationReference)
            )
        );
        ++m_issuedChildren;
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

    auto ToolCallIssuingContext::issuedChildren() const noexcept -> uint32
    {
        return m_issuedChildren;
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
            UF_TRY_CONTEXT(
                childEffectDeclarationValid(entry.descriptor.childEffects),
                "reading the child_effects of " + entry.name
            );
            UF_TRY_CONTEXT(
                toolBodyDeclarationValid(entry.descriptor.body),
                "reading the body declaration of " + entry.name
            );
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
        std::vector<ToolCatalogEntry> tools
    )
        : m_toolCatalogHash{toolCatalogHash}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_tools{std::move(tools)}
    {
    }

    auto FrameworkToolCatalogOwner::create()
        -> Result<FrameworkToolCatalogOwner>
    {
        auto tools    = std::vector<ToolCatalogEntry>{};
        auto material = std::vector<json::Value>{};
        tools.reserve(k_frameworkTools.size());
        material.reserve(k_frameworkTools.size());
        for (auto const& definition : k_frameworkTools)
        {
            UF_TRY_VALUE_CONTEXT(
                descriptor,
                definition.descriptor(),
                "building a Framework Tool Catalog descriptor"
            );
            UF_TRY_CONTEXT(
                childEffectDeclarationValid(descriptor.childEffects),
                "building the child_effects of " + std::string{definition.name}
            );
            UF_TRY_CONTEXT(
                toolBodyDeclarationValid(descriptor.body),
                "building the body declaration of "
                    + std::string{definition.name}
            );
            UF_TRY_CONTEXT(
                bodyMatchesArgumentContract(
                    descriptor.body,
                    definition.argumentMaterial()
                ),
                "matching the body declaration to the argument contract of "
                    + std::string{definition.name}
            );
            material.emplace_back(descriptorMaterial(definition, descriptor));
            tools.emplace_back(ToolCatalogEntry{
                .name       = std::string{definition.name},
                .descriptor = std::move(descriptor),
            });
        }
        auto canonicalJcs = json::canonicalBytes(json::Value::ofObject({
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
        UF_TRY(p_definition->validateArguments(canonicalArgs));
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
        UF_TRY_VALUE(arguments, CanonicalJson::parseExact("{}"));
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
