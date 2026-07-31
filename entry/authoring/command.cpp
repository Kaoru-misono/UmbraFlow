#include "command.hpp"

#include <annotation/capabilities.hpp>
#include <annotation/resource.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::authoring
{
    namespace
    {
        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto parseUnsigned(
            std::string_view value,
            std::string_view flag
        ) -> Result<uint64>
        {
            auto parsed             = uint64{};
            auto const* const begin = std::to_address(value.begin());
            auto const* const end   = std::to_address(value.end());
            auto const result       = std::from_chars(begin, end, parsed);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return invalid(
                    std::format("{} expects an integer, got \"{}\"", flag, value)
                );
            }
            return parsed;
        }

        [[nodiscard]]
        auto parseUnsigned32(
            std::string_view value,
            std::string_view flag
        ) -> Result<uint32>
        {
            UF_TRY_VALUE(parsed, parseUnsigned(value, flag));
            auto const narrowed = checkedCast<uint32>(parsed);
            if (!narrowed)
            {
                return invalid(
                    std::format("{} value {} does not fit 32 bits", flag, parsed)
                );
            }
            return *narrowed;
        }

        // Splits a flag value on a single separator into exactly Count integer
        // fields. Both shapes this CLI accepts are that: "x,y,w,h" and "r,g,b"
        // for a rectangle and a colour, "WxH" for a resolution. Reporting the
        // expected field count is what turns a typo into a message an agent can
        // act on rather than a silently truncated rectangle.
        template <std::size_t Count>
        [[nodiscard]]
        auto parseFields(
            std::string_view value,
            std::string_view flag,
            char separator,
            std::string_view shape
        ) -> Result<std::array<uint32, Count>>
        {
            auto fields    = std::array<uint32, Count>{};
            auto remaining = value;
            for (auto index = std::size_t{0}; index < Count; ++index)
            {
                auto const isLast = (index + 1U == Count);
                auto const cut    = remaining.find(separator);
                if (isLast == (cut != std::string_view::npos))
                {
                    return invalid(
                        std::format(
                            "{} expects {}, got \"{}\"",
                            flag,
                            shape,
                            value
                        )
                    );
                }

                auto const field = isLast ? remaining : remaining.substr(0, cut);
                UF_TRY_VALUE(parsed, parseUnsigned32(field, flag));
                checkedAt(fields, index) = parsed;
                if (!isLast)
                {
                    remaining = remaining.substr(cut + 1U);
                }
            }
            return fields;
        }

        [[nodiscard]]
        auto parseRect(
            std::string_view value,
            std::string_view flag
        ) -> Result<PixelRect>
        {
            UF_TRY_VALUE(
                fields,
                parseFields<4>(value, flag, ',', "x,y,w,h")
            );
            auto const [x, y, width, height] = fields;
            return PixelRect::create(x, y, width, height);
        }

        [[nodiscard]]
        auto positional(
            std::span<std::string const> raw,
            std::size_t index,
            std::string_view what
        ) -> Result<std::string>
        {
            if (index >= raw.size() || raw[index].empty())
            {
                return invalid(std::format("missing required argument <{}>", what));
            }
            if (raw[index].starts_with("--"))
            {
                return invalid(
                    std::format(
                        "expected <{}>, got flag \"{}\"",
                        what,
                        raw[index]
                    )
                );
            }
            return raw[index];
        }

        [[nodiscard]]
        auto require(
            std::optional<std::string> value,
            std::string_view flag
        ) -> Result<std::string>
        {
            if (!value || value->empty())
            {
                return invalid(std::format("missing required argument {}", flag));
            }
            return *std::move(value);
        }

        // The capability vocabulary, spelled the way the authoring document
        // spells it, so an author reads one set of names on the command line and
        // in the file rather than learning a CLI dialect.
        enum class CapabilityName : uint8
        {
            Identify,
            Interact,
            Read,
        };

        [[nodiscard]]
        auto capabilityNamed(
            std::string_view text
        ) noexcept -> std::optional<CapabilityName>
        {
            constexpr auto k_names = std::array{
                std::pair{std::string_view{"identify"}, CapabilityName::Identify},
                std::pair{std::string_view{"interact"}, CapabilityName::Interact},
                std::pair{std::string_view{"read"}, CapabilityName::Read},
            };
            auto const found = std::ranges::find(
                k_names,
                text,
                &std::pair<std::string_view, CapabilityName>::first
            );
            if (found == k_names.end())
            {
                return std::nullopt;
            }
            return found->second;
        }

        [[nodiscard]]
        auto signatureRoleNamed(
            std::string_view text
        ) noexcept -> std::optional<annotation::SignatureRole>
        {
            using Role = annotation::SignatureRole;

            constexpr auto k_roles = std::array{
                std::pair{std::string_view{"required"}, Role::Required},
                std::pair{std::string_view{"forbidden"}, Role::Forbidden},
            };
            auto const found = std::ranges::find(
                k_roles,
                text,
                &std::pair<std::string_view, Role>::first
            );
            if (found == k_roles.end())
            {
                return std::nullopt;
            }
            return found->second;
        }

        [[nodiscard]]
        auto repeatedCapability(std::string_view name) -> std::unexpected<Error>
        {
            return invalid(
                std::format("--capability {} was given twice", name)
            );
        }

        [[nodiscard]]
        auto refuseSignatureRole(bool present, std::string_view name) -> Status
        {
            if (!present)
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "--capability {} takes no \":role\"; only identify is "
                    "evidence for or against a page",
                    name
                )
            );
        }

        // One --capability token folded into the set read so far. The role is a
        // suffix on identify rather than a flag of its own, so a role stated
        // without identify -- which the model's ExercisedIdentify has nowhere to
        // put -- cannot be typed at all.
        [[nodiscard]]
        auto withCapability(
            StatedCapabilities const& collected,
            std::string_view token
        ) -> Result<StatedCapabilities>
        {
            auto const cut     = token.find(':');
            auto const named   = token.substr(0U, cut);
            auto const hasRole = (cut != std::string_view::npos);

            auto const capability = capabilityNamed(named);
            if (!capability)
            {
                return invalid(
                    std::format(
                        "--capability expects identify, interact or read, "
                        "got \"{}\"",
                        named
                    )
                );
            }

            auto updated = collected;
            switch (*capability)
            {
            case CapabilityName::Identify:
            {
                if (updated.identify)
                {
                    return repeatedCapability("identify");
                }
                auto role = annotation::SignatureRole::Required;
                if (hasRole)
                {
                    auto const roleText = token.substr(cut + 1U);
                    auto const parsed   = signatureRoleNamed(roleText);
                    if (!parsed)
                    {
                        return invalid(
                            std::format(
                                "--capability identify takes :required or "
                                ":forbidden, got \"{}\"",
                                roleText
                            )
                        );
                    }
                    role = *parsed;
                }
                updated.identify = role;
                break;
            }
            case CapabilityName::Interact:
                UF_TRY(refuseSignatureRole(hasRole, "interact"));
                if (updated.interact)
                {
                    return repeatedCapability("interact");
                }
                updated.interact = true;
                break;
            case CapabilityName::Read:
                UF_TRY(refuseSignatureRole(hasRole, "read"));
                if (updated.read)
                {
                    return repeatedCapability("read");
                }
                updated.read = true;
                break;
            }
            return updated;
        }

        [[nodiscard]]
        auto declaresAnyCapability(
            StatedCapabilities const& capabilities
        ) noexcept -> bool
        {
            return (
                capabilities.identify.has_value()
                || capabilities.interact
                || capabilities.read
            );
        }

        // Every flag a subcommand that draws a rectangle accepts, collected as
        // written. --key and --tolerance build one ColourKey between them, so
        // neither can become a domain value until the whole line has been read.
        //
        // The two numeric flags keep their defaults out of this record and in
        // buildElementDraw, so "not typed" stays distinguishable from "typed at
        // the default". A verb that mints no template has to be able to refuse a
        // threshold rather than apply it to nothing.
        struct DrawOptions final
        {
            std::optional<std::string> source{};
            std::optional<std::string> rect{};
            std::optional<std::string> searchRoi{};
            std::optional<std::string> key{};

            std::optional<uint32> tolerance{};
            std::optional<uint32> similarityBasisPoints{};

            StatedCapabilities capabilities{};
        };

        [[nodiscard]]
        auto parseDrawOptions(
            std::span<std::string const> raw
        ) -> Result<DrawOptions>
        {
            auto options = DrawOptions{};
            auto index   = std::size_t{0};
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--source")
                {
                    options.source = value;
                }
                else if (flag == "--rect")
                {
                    options.rect = value;
                }
                else if (flag == "--search-roi")
                {
                    options.searchRoi = value;
                }
                else if (flag == "--key")
                {
                    options.key = value;
                }
                else if (flag == "--capability")
                {
                    UF_TRY_VALUE(
                        applied,
                        withCapability(options.capabilities, value)
                    );
                    options.capabilities = applied;
                }
                else if (flag == "--tolerance")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned32(value, flag));
                    options.tolerance = parsed;
                }
                else if (flag == "--min-similarity-bp")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned32(value, flag));
                    options.similarityBasisPoints = parsed;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }
            return options;
        }

        [[nodiscard]]
        auto buildElementDraw(
            std::string name,
            DrawOptions const& options
        ) -> Result<ElementDraw>
        {
            UF_TRY_VALUE(source, require(options.source, "--source"));
            UF_TRY_VALUE(rectText, require(options.rect, "--rect"));
            UF_TRY_VALUE(templateRect, parseRect(rectText, "--rect"));

            auto searchRoi = std::optional<PixelRect>{};
            if (options.searchRoi)
            {
                UF_TRY_VALUE(parsed, parseRect(*options.searchRoi, "--search-roi"));
                searchRoi = parsed;
            }

            auto colourKey = std::optional<annotation::ColourKey>{};
            if (options.key)
            {
                UF_TRY_VALUE(
                    channels,
                    parseFields<3>(*options.key, "--key", ',', "r,g,b")
                );
                auto const [red, green, blue] = channels;
                UF_TRY_VALUE(
                    created,
                    annotation::ColourKey::create(
                        red,
                        green,
                        blue,
                        options.tolerance.value_or(k_defaultColourTolerance)
                    )
                );
                colourKey = created;
            }

            UF_TRY_VALUE(
                threshold,
                annotation::SimilarityThreshold::create(
                    options.similarityBasisPoints.value_or(
                        k_defaultSimilarityBasisPoints
                    )
                )
            );

            return ElementDraw{
                .name         = std::move(name),
                .source       = std::move(source),
                .templateRect = templateRect,
                .searchRoi    = searchRoi,
                .colourKey    = colourKey,
                .threshold    = threshold,
            };
        }

        // The <draw> flags that describe a template, listed as the author typed
        // them. Every one of the five says something about pixels a matcher will
        // compare, so a verb that mints no template names all of them at once
        // rather than accepting one and dropping it where nobody looks.
        [[nodiscard]]
        auto templateFlagsGiven(DrawOptions const& options) -> std::string
        {
            struct Typed final
            {
                std::string_view name{};
                bool             given{};
            };

            auto const flags = std::array{
                Typed{.name = "--source", .given = options.source.has_value()},
                Typed{
                    .name  = "--search-roi",
                    .given = options.searchRoi.has_value(),
                },
                Typed{.name = "--key", .given = options.key.has_value()},
                Typed{
                    .name  = "--tolerance",
                    .given = options.tolerance.has_value(),
                },
                Typed{
                    .name  = "--min-similarity-bp",
                    .given = options.similarityBasisPoints.has_value(),
                },
            };

            auto listed = std::string{};
            for (auto const& flag : flags)
            {
                if (!flag.given)
                {
                    continue;
                }
                if (!listed.empty())
                {
                    listed += ", ";
                }
                listed += flag.name;
            }
            return listed;
        }

        // One rectangle with no appearance of its own, which is what page add
        // authors when nothing about the capability set asks for pixels.
        [[nodiscard]]
        auto buildAddRegion(
            std::string const& root,
            std::string const& page,
            std::string name,
            DrawOptions const& options
        ) -> Result<AuthoringCommand>
        {
            auto const described = templateFlagsGiven(options);
            if (!described.empty())
            {
                return invalid(
                    std::format(
                        "{} describe pixels to compare, and this element has "
                        "none: page add mints an appearance only for "
                        "--capability identify, because identify is the one "
                        "capability whose evidence IS the pixels. interact and "
                        "read say WHERE a rectangle is, so the page that is "
                        "recognised locates it and --rect is the whole of it",
                        described
                    )
                );
            }

            UF_TRY_VALUE(rectText, require(options.rect, "--rect"));
            UF_TRY_VALUE(region, parseRect(rectText, "--rect"));
            return AddRegion{
                .root         = std::filesystem::path{root},
                .page         = page,
                .name         = std::move(name),
                .capabilities = options.capabilities,
                .region       = region,
            };
        }

        // `element appearance` draws with the same options page add draws with,
        // minus the two that are not an appearance's to state. Sharing the
        // parser rather than the flag names is the point: an author who has
        // drawn one rectangle can draw a second look at it without learning a
        // second vocabulary.
        [[nodiscard]]
        auto parseAddAppearance(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(root, positional(raw, 0, "root"));
            UF_TRY_VALUE(element, positional(raw, 1, "element"));
            UF_TRY_VALUE(name, positional(raw, 2, "appearance"));
            UF_TRY_VALUE(options, parseDrawOptions(raw.subspan(3U)));

            if (declaresAnyCapability(options.capabilities))
            {
                return invalid(
                    "element appearance takes no --capability: what an element "
                    "may be used for was settled when it was drawn, and a "
                    "second look at the same pixels does not change it"
                );
            }
            if (options.searchRoi)
            {
                return invalid(
                    "element appearance takes no --search-roi: the search "
                    "region belongs to the element and every appearance of it "
                    "is searched in that one region, so this template has to "
                    "fit the region already drawn"
                );
            }

            UF_TRY_VALUE(draw, buildElementDraw(std::move(name), options));
            return AddAppearance{
                .root    = std::filesystem::path{root},
                .element = std::move(element),
                .draw    = std::move(draw),
            };
        }

        [[nodiscard]]
        auto parseElementCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(verb, positional(raw, 0, "appearance"));
            if (verb == "appearance")
            {
                return parseAddAppearance(raw.subspan(1U));
            }
            return invalid(std::format("unknown element verb \"{}\"", verb));
        }

        [[nodiscard]]
        auto parseInitProject(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(root, positional(raw, 0, "root"));

            auto projectId  = std::optional<std::string>{};
            auto resolution = std::optional<std::string>{};
            auto density    = std::optional<std::string>{};

            auto index = std::size_t{1};
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--project-id")
                {
                    projectId = value;
                }
                else if (flag == "--resolution")
                {
                    resolution = value;
                }
                else if (flag == "--dpi")
                {
                    density = value;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            UF_TRY_VALUE(requiredId, require(std::move(projectId), "--project-id"));
            UF_TRY_VALUE(
                requiredResolution,
                require(std::move(resolution), "--resolution")
            );
            UF_TRY_VALUE(
                extent,
                parseFields<2>(requiredResolution, "--resolution", 'x', "WxH")
            );
            auto const [width, height] = extent;
            auto dpi = k_defaultSourceDpi;
            if (density.has_value())
            {
                UF_TRY_VALUE(parsed, parseFields<1>(*density, "--dpi", 'x', "N"));
                dpi = parsed.front();
            }
            UF_TRY_VALUE(
                fingerprint,
                annotation::ProjectFingerprint::create(width, height, dpi, dpi)
            );

            return InitProject{
                .root        = std::filesystem::path{root},
                .projectId   = std::move(requiredId),
                .fingerprint = fingerprint,
            };
        }

        [[nodiscard]]
        auto parseRootOnly(
            std::span<std::string const> raw,
            std::string_view verb
        ) -> Result<std::filesystem::path>
        {
            UF_TRY_VALUE(root, positional(raw, 0, "root"));
            if (raw.size() > 1U)
            {
                return invalid(
                    std::format(
                        "project {} takes only <root>, got \"{}\"",
                        verb,
                        raw[1]
                    )
                );
            }
            return std::filesystem::path{root};
        }

        [[nodiscard]]
        auto parseProjectCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(verb, positional(raw, 0, "init|show|save"));
            auto const rest = raw.subspan(1U);

            if (verb == "init")
            {
                return parseInitProject(rest);
            }
            if (verb == "show")
            {
                UF_TRY_VALUE(root, parseRootOnly(rest, "show"));
                return ShowProject{.root = std::move(root)};
            }
            if (verb == "save")
            {
                UF_TRY_VALUE(root, parseRootOnly(rest, "save"));
                return SaveProject{.root = std::move(root)};
            }
            return invalid(std::format("unknown project verb \"{}\"", verb));
        }

        // `page reference` names an element instead of drawing one, so it reads
        // its own short argument list rather than the <draw> options. It shares
        // --capability with them, and deliberately the same parser: an author
        // spells a capability one way across this tool, and the page side is
        // where the identify role was always going to be typed.
        [[nodiscard]]
        auto parseReferenceElement(
            std::string const& root,
            std::string const& page,
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(element, positional(raw, 0, "element"));

            auto searchRoi  = std::optional<std::string>{};
            auto appearance = std::optional<std::string>{};
            auto stated     = StatedCapabilities{};
            auto index      = std::size_t{1};
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--search-roi")
                {
                    searchRoi = value;
                }
                else if (flag == "--appearance")
                {
                    appearance = value;
                }
                else if (flag == "--capability")
                {
                    UF_TRY_VALUE(applied, withCapability(stated, value));
                    stated = applied;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            // Two flags the author typed on one line that the model cannot
            // hold together. Refused here rather than after the project is
            // opened, because both are on the command line and neither can be
            // dropped for them: each is a measurement they made.
            if (stated.identify && searchRoi)
            {
                return invalid(
                    "--capability identify and --search-roi cannot be combined: "
                    "the anchor pass reads the element's own search region "
                    "before any page is known, so refining it here would search "
                    "the same pixels twice a cycle"
                );
            }

            // A pin binds the page-scoped searches -- the click path reads it
            // off this row. The anchor pass runs before any page is known, so it
            // folds across every appearance whatever a reference says; a page
            // that exercises identify and nothing else therefore has no path a
            // pin could bind, and accepting one would be accepting a flag that
            // does nothing. A page that identifies AND clicks keeps it, because
            // the click half is exactly what it binds.
            if (
                stated.identify
                && !stated.interact
                && !stated.read
                && appearance
            )
            {
                return invalid(
                    "--capability identify alone and --appearance cannot be "
                    "combined: the anchor pass runs before any page is known, "
                    "so it folds across every appearance whatever this page "
                    "pins, and there is no other search on this page for the "
                    "pin to bind"
                );
            }

            auto refined = std::optional<PixelRect>{};
            if (searchRoi)
            {
                UF_TRY_VALUE(parsed, parseRect(*searchRoi, "--search-roi"));
                refined = parsed;
            }

            // Absent and empty are different answers, so the flag being unused
            // is carried as absence rather than as a set with nothing in it:
            // the edit layer reads absence as "every use a placement carries on
            // its own" and an empty set as a page using the element for
            // nothing.
            auto exercised = std::optional<StatedCapabilities>{};
            if (declaresAnyCapability(stated))
            {
                exercised = stated;
            }

            return ReferenceElement{
                .root       = std::filesystem::path{root},
                .page       = page,
                .element    = std::move(element),
                .exercised  = exercised,
                .searchRoi  = refined,
                .appearance = std::move(appearance),
            };
        }

        [[nodiscard]]
        auto parsePageCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(verb, positional(raw, 0, "create|add|reference"));
            UF_TRY_VALUE(root, positional(raw, 1, "root"));
            UF_TRY_VALUE(page, positional(raw, 2, "page"));

            if (verb == "reference")
            {
                return parseReferenceElement(root, page, raw.subspan(3U));
            }

            auto const isCreate = (verb == "create");
            if (!isCreate && verb != "add")
            {
                return invalid(std::format("unknown page verb \"{}\"", verb));
            }

            UF_TRY_VALUE(
                name,
                positional(raw, 3, isCreate ? "anchor" : "name")
            );
            UF_TRY_VALUE(options, parseDrawOptions(raw.subspan(4U)));

            // A page's first element identifies it -- PageSignature has no
            // representation for a page nothing names -- so `create` has no
            // capability to choose and refuses the flag rather than accepting it
            // and quietly meaning something else than it does on `add`.
            if (isCreate && declaresAnyCapability(options.capabilities))
            {
                return invalid(
                    "page create authors the mark that identifies the new page, "
                    "so it takes no --capability; use page add for the rest"
                );
            }
            if (!isCreate && !declaresAnyCapability(options.capabilities))
            {
                return invalid(
                    "page add needs at least one --capability "
                    "(identify[:required|:forbidden], interact, read); an "
                    "element nothing can reach is not a thing the model holds"
                );
            }

            // The rule the whole verb turns on: pixels are minted only when a
            // capability needs pixels, and identify is the only one that does.
            // A page's first mark identifies it by definition, so create is
            // always the drawing half.
            if (!isCreate && !options.capabilities.identify)
            {
                return buildAddRegion(root, page, std::move(name), options);
            }

            UF_TRY_VALUE(draw, buildElementDraw(std::move(name), options));
            if (isCreate)
            {
                return CreatePage{
                    .root   = std::filesystem::path{root},
                    .page   = std::move(page),
                    .anchor = std::move(draw),
                };
            }
            return AddElement{
                .root         = std::filesystem::path{root},
                .page         = std::move(page),
                .capabilities = options.capabilities,
                .draw         = std::move(draw),
            };
        }

        [[nodiscard]]
        auto parseMatchCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(root, positional(raw, 0, "root"));
            // An author names the element they drew, and that same name is what
            // the compiled catalog answers to, so the argument, the usage text,
            // and the field all say element.
            UF_TRY_VALUE(element, positional(raw, 1, "element"));

            auto frame  = std::optional<std::string>{};
            auto page   = std::optional<std::string>{};
            auto budget = cli::k_defaultPixelComparisonBudget;

            auto index = std::size_t{2};
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--frame")
                {
                    frame = value;
                }
                else if (flag == "--page")
                {
                    page = value;
                }
                else if (flag == "--budget")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned(value, flag));
                    budget = parsed;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            UF_TRY_VALUE(requiredFrame, require(std::move(frame), "--frame"));
            return MatchElement{
                .root    = std::filesystem::path{root},
                .element = std::move(element),
                .frame   = std::filesystem::path{requiredFrame},
                .page    = std::move(page),
                .budget  = budget,
            };
        }

        [[nodiscard]]
        auto parseCheckCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(root, positional(raw, 0, "root"));

            auto budget = cli::k_defaultPixelComparisonBudget;

            auto index = std::size_t{1};
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--budget")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned(value, flag));
                    budget = parsed;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            return CheckModel{
                .root   = std::filesystem::path{root},
                .budget = budget,
            };
        }

        // The PNG paths a frames subcommand opens with. They are positional and
        // variable in number, so they end at the first flag rather than at a
        // fixed index. How many are needed is each subcommand's own question --
        // a stability or probe scan compares frames and needs two, a census
        // counts the colours of one -- so this reports what it found and leaves
        // that judgment to the caller.
        struct FrameArguments final
        {
            std::vector<std::filesystem::path> frames{};

            // How many arguments the paths consumed, so the flags after them
            // are read by the same loop every other subcommand uses.
            std::size_t consumed{};
        };

        [[nodiscard]]
        auto parseFrameArguments(
            std::span<std::string const> raw
        ) -> Result<FrameArguments>
        {
            auto arguments = FrameArguments{};
            while (arguments.consumed < raw.size())
            {
                if (raw[arguments.consumed].starts_with("--"))
                {
                    break;
                }
                UF_TRY_VALUE(path, positional(raw, arguments.consumed, "png"));
                arguments.frames.emplace_back(std::move(path));
                ++arguments.consumed;
            }
            if (arguments.frames.empty())
            {
                return invalid("missing required argument <png>");
            }
            return arguments;
        }

        // A scan compares frames, and one frame is stable everywhere and agrees
        // with itself about every colour. Refusing that here rather than letting
        // the vision module refuse it keeps the answer an argument error the
        // caller can fix, instead of an internal invariant.
        [[nodiscard]]
        auto requireComparableFrames(
            FrameArguments const& arguments,
            std::string_view verb
        ) -> Status
        {
            if (arguments.frames.size() < 2U)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "frames {} compares frames and needs at least two <png>, got {}",
                        verb,
                        arguments.frames.size()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto parseStabilityCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(arguments, parseFrameArguments(raw));
            UF_TRY(requireComparableFrames(arguments, "stability"));

            auto rect          = std::optional<std::string>{};
            auto grayTolerance = uint32{0};
            auto minimumGap    = uint32{0};

            auto index = arguments.consumed;
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--rect")
                {
                    rect = value;
                }
                else if (flag == "--gray-tolerance")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned32(value, flag));
                    grayTolerance = parsed;
                }
                else if (flag == "--gap")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned32(value, flag));
                    minimumGap = parsed;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            auto analysed = std::optional<PixelRect>{};
            if (rect)
            {
                UF_TRY_VALUE(parsed, parseRect(*rect, "--rect"));
                analysed = parsed;
            }

            return AnalyseFrameStability{
                .frames        = std::move(arguments.frames),
                .rect          = analysed,
                .grayTolerance = grayTolerance,
                .minimumGap    = minimumGap,
            };
        }

        [[nodiscard]]
        auto parseProbeCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(arguments, parseFrameArguments(raw));
            UF_TRY(requireComparableFrames(arguments, "probe"));

            auto rect      = std::optional<std::string>{};
            auto key       = std::optional<std::string>{};
            auto tolerance = k_defaultColourTolerance;

            auto index = arguments.consumed;
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--rect")
                {
                    rect = value;
                }
                else if (flag == "--key")
                {
                    key = value;
                }
                else if (flag == "--tolerance")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned32(value, flag));
                    tolerance = parsed;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            UF_TRY_VALUE(rectText, require(std::move(rect), "--rect"));
            UF_TRY_VALUE(analysed, parseRect(rectText, "--rect"));
            UF_TRY_VALUE(keyText, require(std::move(key), "--key"));
            UF_TRY_VALUE(
                channels,
                parseFields<3>(keyText, "--key", ',', "r,g,b")
            );
            auto const [red, green, blue] = channels;
            UF_TRY_VALUE(
                colourKey,
                annotation::ColourKey::create(red, green, blue, tolerance)
            );

            return ProbeFrameColour{
                .frames = std::move(arguments.frames),
                .rect   = analysed,
                .key    = colourKey,
            };
        }

        [[nodiscard]]
        auto parseCensusCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(arguments, parseFrameArguments(raw));
            if (arguments.frames.size() != 1U)
            {
                return invalid(
                    std::format(
                        "frames census counts one frame's colours and takes one "
                        "<png>, got {}",
                        arguments.frames.size()
                    )
                );
            }

            auto rect    = std::optional<std::string>{};
            auto entries = k_defaultCensusEntries;

            auto index = arguments.consumed;
            while (index < raw.size())
            {
                auto const& flag = raw[index];
                if (index + 1U >= raw.size())
                {
                    return invalid(std::format("missing value for {}", flag));
                }
                auto const& value = raw[index + 1U];

                if (flag == "--rect")
                {
                    rect = value;
                }
                else if (flag == "--top")
                {
                    UF_TRY_VALUE(parsed, parseUnsigned32(value, flag));
                    entries = parsed;
                }
                else
                {
                    return invalid(std::format("unknown argument \"{}\"", flag));
                }
                index += 2U;
            }

            UF_TRY_VALUE(rectText, require(std::move(rect), "--rect"));
            UF_TRY_VALUE(analysed, parseRect(rectText, "--rect"));

            return CensusFrameColours{
                .frame          = std::move(arguments.frames.front()),
                .rect           = analysed,
                .maximumEntries = entries,
            };
        }

        [[nodiscard]]
        auto parseFramesCommand(
            std::span<std::string const> raw
        ) -> Result<AuthoringCommand>
        {
            UF_TRY_VALUE(verb, positional(raw, 0, "stability|probe|census"));
            auto const rest = raw.subspan(1U);

            if (verb == "stability")
            {
                return parseStabilityCommand(rest);
            }
            if (verb == "probe")
            {
                return parseProbeCommand(rest);
            }
            if (verb == "census")
            {
                return parseCensusCommand(rest);
            }
            return invalid(std::format("unknown frames verb \"{}\"", verb));
        }
    }

    auto parseAuthoringCommand(
        std::span<std::string const> raw
    ) -> Result<AuthoringCommand>
    {
        UF_TRY_VALUE(
            group,
            positional(raw, 0, "project|page|element|match|check|frames")
        );
        auto const rest = raw.subspan(1U);

        if (group == "project")
        {
            return parseProjectCommand(rest);
        }
        if (group == "page")
        {
            return parsePageCommand(rest);
        }
        if (group == "element")
        {
            return parseElementCommand(rest);
        }
        if (group == "match")
        {
            return parseMatchCommand(rest);
        }
        if (group == "check")
        {
            return parseCheckCommand(rest);
        }
        if (group == "frames")
        {
            return parseFramesCommand(rest);
        }
        return invalid(std::format("unknown subcommand \"{}\"", group));
    }

    auto authoringUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  umbra-authoring project init  ROOT --project-id ID "
            "--resolution WxH\n"
            "  umbra-authoring project show  ROOT\n"
            "  umbra-authoring project save  ROOT\n"
            "  umbra-authoring page create    ROOT PAGE ANCHOR <draw>\n"
            "  umbra-authoring page add       ROOT PAGE NAME "
            "--capability C... <draw>\n"
            "  umbra-authoring page reference ROOT PAGE ELEMENT "
            "[--capability C...]\n"
            "                                 [--search-roi x,y,w,h] "
            "[--appearance NAME]\n"
            "  umbra-authoring element appearance ROOT ELEMENT NAME <draw>\n"
            "  umbra-authoring match ROOT ELEMENT --frame PNG [--page PAGE]\n"
            "                                     [--budget N]\n"
            "  umbra-authoring check ROOT [--budget N]\n"
            "  umbra-authoring frames stability PNG PNG... [--rect x,y,w,h]\n"
            "                                   [--gray-tolerance N] [--gap N]\n"
            "  umbra-authoring frames probe     PNG PNG... --rect x,y,w,h\n"
            "                                   --key r,g,b [--tolerance N]\n"
            "  umbra-authoring frames census    PNG --rect x,y,w,h [--top N]\n"
            "\n"
            "<draw> options:\n"
            "  --capability C          What these pixels may be used for. Give\n"
            "                          the flag once per capability; a set is\n"
            "                          the point, so an element that names its\n"
            "                          page AND can be clicked is\n"
            "                          --capability identify --capability "
            "interact,\n"
            "                          matched once a cycle rather than twice.\n"
            "                          C is one of:\n"
            "                            identify[:required|:forbidden]\n"
            "                                     joins this page's signature "
            "as\n"
            "                                     evidence for it, or against "
            "it;\n"
            "                                     default :required. The role "
            "is\n"
            "                                     the PAGE's, not the "
            "element's --\n"
            "                                     one mark is required by one "
            "page\n"
            "                                     and forbidden by another\n"
            "                            interact this page may click it\n"
            "                            read     this page may read text out "
            "of\n"
            "                                     it\n"
            "                          page add requires at least one; page "
            "create\n"
            "                          takes none, because a page's first mark\n"
            "                          identifies it by definition\n"
            "                          Only identify mints an appearance. Give\n"
            "                          page add interact or read alone and the\n"
            "                          element gets none: --rect is then its "
            "whole\n"
            "                          geometry, the page that is recognised "
            "locates\n"
            "                          it, and every other <draw> option is "
            "refused\n"
            "  --source HASH-OR-PATH   Screen the rectangle was measured on:\n"
            "                          a 64-hex source content hash the project\n"
            "                          already holds, or a PNG path to ingest\n"
            "  --rect x,y,w,h          Template rectangle, in source pixels\n"
            "  --search-roi x,y,w,h    Where to look for it; default: whole screen\n"
            "  --key r,g,b             Colour key selecting the template pixels\n"
            "                          that count; default: every pixel counts\n"
            "  --tolerance N           Colour distance still fully counted, 0..765;\n"
            "                          default: 12\n"
            "  --min-similarity-bp N   Similarity threshold in basis points, "
            "0..10000;\n"
            "                          default: 9000\n"
            "\n"
            "<frames> options:\n"
            "  --rect x,y,w,h          The rectangle analysed inside every "
            "frame;\n"
            "                          stability defaults to the whole first "
            "frame\n"
            "  --gray-tolerance N      Grey spread across frames a pixel may "
            "still\n"
            "                          be called stable at; default: 0, exact\n"
            "  --gap N                 Consecutive unstable rows or columns "
            "that\n"
            "                          split one region in two; default: 0, "
            "which\n"
            "                          reports one box over every stable pixel\n"
            "  --key r,g,b             The colour a probe measures the "
            "selection of\n"
            "  --tolerance N           Colour distance still fully selected, "
            "0..765;\n"
            "                          default: 12\n"
            "  --top N                 Dominant colours a census reports; "
            "default: 8\n"
            "\n"
            "project init also takes --dpi N: the display density of the window\n"
            "the screenshots came from, default 96. It has to match the target,\n"
            "because AuthoringDocument refuses a source whose fingerprint differs\n"
            "from the project's and the runtime refuses to deliver a click when\n"
            "the live fingerprint differs from the catalog's.\n"
            "\n"
            "A page is created with the first anchor that identifies it, because a\n"
            "page with an empty signature is not a thing the annotation model can\n"
            "represent.\n"
            "\n"
            "Both drawing verbs measure the mask their --key produces on the frame\n"
            "it was drawn from and report it under \"mask\", in the same two counts\n"
            "frames probe reports. A mask under 50 selected pixels, or one covering\n"
            "half the rectangle or more, also carries a \"warning\": the first is too\n"
            "small to locate anything and the second selected the fill rather than\n"
            "the figure drawn on it, and both look perfect until check searches them\n"
            "against a screen they must not match. It is a hint and not a refusal --\n"
            "the element is authored either way, and check is the gate.\n"
            "\n"
            "page reference puts an element the project ALREADY holds onto a second\n"
            "page: one element, two pages, one search and one template to correct.\n"
            "It is the only verb that produces a borrowed element -- everything\n"
            "drawn is owned by the page it was drawn on -- so redrawing a menu\n"
            "button per page is the thing it exists to replace.\n"
            "\n"
            "Its --capability says what THIS page exercises on the element, out of\n"
            "the same vocabulary page add takes, and a page may exercise only what\n"
            "the element declares. Without the flag the page takes every use a\n"
            "placement carries on its own -- interact and read, whichever the\n"
            "element declares -- and an element that only identifies is refused,\n"
            "because it joins a page through that page's signature rather than by\n"
            "being placed on it. identify is never inherited either, since which\n"
            "way the evidence points is a question the element has no answer to:\n"
            "  --capability identify:required   this page is on screen only when\n"
            "                                   the mark is\n"
            "  --capability identify:forbidden  this page is on screen only when\n"
            "                                   the mark is NOT\n"
            "are how a second page takes an existing mark into its own signature,\n"
            "and one mark may be required by one page and forbidden by another.\n"
            "\n"
            "--capability, --search-roi and --appearance refine THIS page's use of\n"
            "the element; none of them edits the element, which stays one\n"
            "rectangle and one set of appearances every page sees. Without\n"
            "--search-roi the page searches the element's own region and keeps\n"
            "following it when a later correction moves it. It cannot be combined\n"
            "with --capability identify: the anchor pass reads the element's own\n"
            "region, before any page is known.\n"
            "\n"
            "--appearance NAME pins which appearance this page expects, for the case\n"
            "where the PAGE decides it -- a back arrow drawn white on one screen\n"
            "and dark on another. The page then searches once instead of once per\n"
            "appearance, and cannot answer with the wrong one. Leave it off when\n"
            "the runtime state decides instead (a speed button reading 1x, 2x or\n"
            "3x): every appearance is then searched and the script reads which\n"
            "one matched. It is refused for a page that exercises identify and\n"
            "nothing else, because the anchor pass runs before any page is known\n"
            "and folds across every appearance whatever a reference pins.\n"
            "\n"
            "element appearance adds a SECOND look at one element's pixels: same\n"
            "id, same region, same capabilities, one more entry in its ordered\n"
            "appearance list. It is what the back arrow above needs, and drawing\n"
            "the element twice is what it replaces -- two ids, two templates, two\n"
            "searches a cycle and a script left choosing between them. An element\n"
            "with no appearance may be given one, which turns a rectangle located\n"
            "by its page into one a find re-verifies. It takes the same <draw>\n"
            "options minus --capability (settled when the element was drawn) and\n"
            "--search-roi (the region is the element's, and every appearance of it\n"
            "is searched in that one region).\n"
            "\n"
            "match --page names the page a click target is located on, because a\n"
            "refined search region and a pinned appearance both belong to a page's\n"
            "reference. It is only needed when more than one page clicks the\n"
            "element, and it is refused for an element that identifies: the anchor\n"
            "pass runs before any page is known. match itself is refused for an\n"
            "element with no appearance: there is nothing to compare, so the\n"
            "answer would be a hit on every frame ever handed to it.\n"
            "\n"
            "check is the falsification matrix. It searches every declared\n"
            "appearance against every screen the project holds, on and off the\n"
            "screens it belongs to, and it searches each element once more the\n"
            "way the runtime does -- every appearance folded into one answer.\n"
            "The off-diagonal cells are the point: a template always matches the\n"
            "image it was cut from, so the only evidence it identifies one screen\n"
            "rather than another is what it does on the others, and an appearance\n"
            "that matches everywhere disappears behind a sibling that matches\n"
            "correctly. \"findings\" is what is wrong and \"accepted\" is whether\n"
            "it is empty; a failing model still answers ok, because the findings\n"
            "ARE the answer and an error document would throw them away.\n"
            "\n"
            "One JSON document is written to stdout per invocation, success or\n"
            "failure; a failure adds one rendered line to stderr.\n";
    }
}
