#include "catalog.hpp"

#include "resource.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/safety/checked-access.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <vision/frame-analysis.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_luauReservedWords = std::array{
            std::string_view{"and"},
            std::string_view{"break"},
            std::string_view{"do"},
            std::string_view{"else"},
            std::string_view{"elseif"},
            std::string_view{"end"},
            std::string_view{"false"},
            std::string_view{"for"},
            std::string_view{"function"},
            std::string_view{"if"},
            std::string_view{"in"},
            std::string_view{"local"},
            std::string_view{"nil"},
            std::string_view{"not"},
            std::string_view{"or"},
            std::string_view{"repeat"},
            std::string_view{"return"},
            std::string_view{"then"},
            std::string_view{"true"},
            std::string_view{"until"},
            std::string_view{"while"},
        };

        [[nodiscard]]
        constexpr auto hexValue(char value) noexcept -> std::optional<uint8>
        {
            if (value >= '0' && value <= '9')
            {
                return static_cast<uint8>(value - '0');
            }

            if (value >= 'a' && value <= 'f')
            {
                return static_cast<uint8>(value - 'a' + 10);
            }

            if (value >= 'A' && value <= 'F')
            {
                return static_cast<uint8>(value - 'A' + 10);
            }

            return std::nullopt;
        }

        [[nodiscard]]
        constexpr auto isUuidHyphen(std::size_t index) noexcept -> bool
        {
            return index == 8 || index == 13 || index == 18 || index == 23;
        }

        [[nodiscard]]
        constexpr auto isAsciiIdentifierStart(char value) noexcept -> bool
        {
            return (
                (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || value == '_'
            );
        }

        [[nodiscard]]
        constexpr auto isAsciiIdentifierContinue(char value) noexcept -> bool
        {
            return isAsciiIdentifierStart(value) || (value >= '0' && value <= '9');
        }

        [[nodiscard]]
        constexpr auto isLuauReservedWord(std::string_view value) noexcept -> bool
        {
            return std::ranges::find(k_luauReservedWords, value) != k_luauReservedWords.end();
        }

        template <typename Id>
        [[nodiscard]]
        auto lessId(Id const& left, Id const& right) noexcept -> bool
        {
            return left.value() < right.value();
        }

        template <typename Id>
        [[nodiscard]]
        auto hasDuplicateIds(std::span<Id const> ids) noexcept -> bool
        {
            return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
        }

        [[nodiscard]]
        auto sameSignature(PageSignature const& left, PageSignature const& right) noexcept -> bool
        {
            return (
                std::ranges::equal(left.required(), right.required())
                && std::ranges::equal(left.forbidden(), right.forbidden())
            );
        }

        [[nodiscard]]
        auto invalidCatalog(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto referenceLess(
            PageReference const& left,
            PageReference const& right
        ) noexcept -> bool
        {
            if (left.pageId != right.pageId)
            {
                return left.pageId.value() < right.pageId.value();
            }
            return left.elementId.value() < right.elementId.value();
        }

        [[nodiscard]]
        auto rectWithin(PixelRect rect, ProjectFingerprint fingerprint) noexcept -> bool
        {
            return (
                rect.right() <= fingerprint.width()
                && rect.bottom() <= fingerprint.height()
            );
        }

        [[nodiscard]]
        auto templateFits(PixelRect templateRect, PixelRect searchRoi) noexcept -> bool
        {
            return (
                templateRect.width() <= searchRoi.width()
                && templateRect.height() <= searchRoi.height()
            );
        }
    }

    auto ResourceId::parse(std::string_view value) -> Result<ResourceId>
    {
        if (value.size() != 36)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "resource UUID must contain exactly 36 characters"
            );
        }

        auto bytes = std::array<uint8, 16>{};
        auto nibbleIndex = std::size_t{0};
        for (auto index = std::size_t{0}; index < value.size(); ++index)
        {
            if (isUuidHyphen(index))
            {
                if (value[index] != '-')
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "resource UUID has an invalid separator"
                    );
                }

                continue;
            }

            auto const nibble = hexValue(value[index]);
            if (!nibble)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "resource UUID contains a non-hexadecimal character"
                );
            }

            auto const byteIndex = nibbleIndex / 2;
            auto& byte = checkedAt(bytes, byteIndex);
            if (nibbleIndex % 2 == 0)
            {
                byte = static_cast<uint8>(*nibble << 4);
            }
            else
            {
                byte = static_cast<uint8>(byte | *nibble);
            }
            ++nibbleIndex;
        }

        return ResourceId{bytes};
    }

    auto ResourceId::fromBytes(
        std::span<std::byte const, 16> bytes
    ) noexcept -> ResourceId
    {
        auto storage = std::array<uint8, 16>{};
        for (auto index = std::size_t{0}; index < storage.size(); ++index)
        {
            checkedAt(storage, index) = std::to_integer<uint8>(bytes[index]);
        }

        return ResourceId{storage};
    }

    auto ResourceId::toString() const -> std::string
    {
        static constexpr auto k_hexDigits = std::string_view{"0123456789abcdef"};

        auto result = std::string{};
        result.reserve(36);
        for (auto index = std::size_t{0}; index < m_bytes.size(); ++index)
        {
            if (index == 4 || index == 6 || index == 8 || index == 10)
            {
                result.push_back('-');
            }

            auto const value = checkedAt(m_bytes, index);
            result.push_back(k_hexDigits[value >> 4]);
            result.push_back(k_hexDigits[value & uint8{0x0F}]);
        }

        return result;
    }

    ProjectId::ProjectId(std::string value) noexcept
        : m_value{std::move(value)}
    {
    }

    auto ProjectId::create(std::string value) -> Result<ProjectId>
    {
        if (value.empty() || !isValidUtf8(value))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "project ID must be non-empty valid UTF-8"
            );
        }

        return ProjectId{std::move(value)};
    }

    auto ProjectId::value() const noexcept -> std::string const& { return m_value; }

    ResourceName::ResourceName(std::string value) noexcept
        : m_value{std::move(value)}
    {
    }

    auto ResourceName::create(std::string value) -> Result<ResourceName>
    {
        auto const valid = (
            !value.empty()
            && isAsciiIdentifierStart(value.front())
            && std::ranges::all_of(
                value | std::views::drop(1),
                isAsciiIdentifierContinue
            )
            && !isLuauReservedWord(value)
        );
        if (!valid)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "resource name must be a direct ASCII Luau member key"
            );
        }

        return ResourceName{std::move(value)};
    }

    auto ResourceName::value() const noexcept -> std::string const& { return m_value; }

    auto SimilarityThreshold::create(uint32 basisPoints) -> Result<SimilarityThreshold>
    {
        if (basisPoints > k_basisPointMaximum)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("similarity threshold {} exceeds 10000 basis points", basisPoints)
            );
        }

        return SimilarityThreshold{basisPoints};
    }

    auto SimilarityThreshold::maximumSad(
        uint32 templateWidth,
        uint32 templateHeight
    ) const -> Result<uint64>
    {
        if (templateWidth == 0 || templateHeight == 0)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "cannot compute a SAD threshold for an empty template"
            );
        }

        auto const pixels = checkedMultiply(
            static_cast<uint64>(templateWidth),
            static_cast<uint64>(templateHeight)
        );
        auto const distanceBasisPoints = static_cast<uint64>(
            k_basisPointMaximum - m_basisPoints
        );
        auto const scaledDistance = checkedMultiply(distanceBasisPoints, uint64{255});
        if (!pixels || !scaledDistance)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "template dimensions overflow the SAD threshold calculation"
            );
        }

        auto const numerator = checkedMultiply(*scaledDistance, *pixels);
        if (!numerator)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "template dimensions overflow the SAD threshold calculation"
            );
        }

        return *numerator / static_cast<uint64>(k_basisPointMaximum);
    }

    auto TemplateOffset::create(
        uint32 x,
        uint32 y,
        uint32 templateWidth,
        uint32 templateHeight
    ) -> Result<TemplateOffset>
    {
        if (x >= templateWidth || y >= templateHeight)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "template click offset ({}, {}) outside {}x{} template",
                    x,
                    y,
                    templateWidth,
                    templateHeight
                )
            );
        }

        return TemplateOffset{x, y};
    }

    auto ColourKey::create(
        uint32 red,
        uint32 green,
        uint32 blue,
        uint32 tolerance
    ) -> Result<ColourKey>
    {
        if (
            red > k_maximumChannel
            || green > k_maximumChannel
            || blue > k_maximumChannel
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "colour key channels must each be between 0 and 255"
            );
        }
        if (tolerance > k_maximumTolerance)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "colour key tolerance must be between 0 and 765"
            );
        }
        return ColourKey{
            static_cast<uint8>(red),
            static_cast<uint8>(green),
            static_cast<uint8>(blue),
            tolerance
        };
    }

    // Delegated rather than duplicated. The ramp had two byte-identical copies
    // for a while -- one here, one behind probeColour, which needs it to answer
    // how many pixels a key selects. Two copies of one rule is exactly what
    // drifts, and the drift would be silent: authoring would bake one mask and
    // the probe would report another. annotation depends on vision, so the call
    // goes this way and the rule lives once.
    auto ColourKey::alphaFor(
        uint8 red,
        uint8 green,
        uint8 blue
    ) const noexcept -> uint8
    {
        return colourKeyAlpha(
            Bgra8Pixel{.blue = blue, .green = green, .red = red, .alpha = 255},
            m_red,
            m_green,
            m_blue,
            m_tolerance
        );
    }

    auto validateElementShape(
        ProjectFingerprint fingerprint,
        PixelRect searchRoi,
        std::span<CompiledAppearance const> appearances,
        ElementCapabilities const& capabilities
    ) -> Status
    {
        if (!rectWithin(searchRoi, fingerprint))
        {
            return invalidCatalog(
                "element search_roi must fit the project resolution"
            );
        }

        for (auto const& appearance : appearances)
        {
            if (!rectWithin(appearance.templateRect, fingerprint))
            {
                return invalidCatalog(
                    std::format(
                        "appearance \"{}\" template_rect must fit the project resolution",
                        appearance.name.value()
                    )
                );
            }
            if (!templateFits(appearance.templateRect, searchRoi))
            {
                return invalidCatalog(
                    std::format(
                        "appearance \"{}\" template must fit inside the element search_roi",
                        appearance.name.value()
                    )
                );
            }
            UF_TRY(
                appearance.threshold.maximumSad(
                    appearance.templateRect.width(),
                    appearance.templateRect.height()
                )
            );
        }

        for (auto left = std::size_t{0}; left < appearances.size(); ++left)
        {
            for (auto right = left + 1U; right < appearances.size(); ++right)
            {
                if (checkedAt(appearances, left).name == checkedAt(appearances, right).name)
                {
                    return invalidCatalog("element appearance names must be unique");
                }
            }
        }

        // A rectangle with no pixels of its own is located by the page that was
        // recognised, so it cannot be part of what recognises that page.
        if (appearances.empty() && capabilities.hasIdentify())
        {
            return invalidCatalog(
                "an element with no appearances cannot exercise identify: it has no pixels to be evidence"
            );
        }

        auto const& interact = capabilities.interact();
        if (interact.has_value() && interact->clickOffset.has_value())
        {
            // The offset is template-local, so with no template there is
            // nothing for it to be local to.
            if (appearances.empty())
            {
                return invalidCatalog(
                    "an element with no appearances cannot define a click offset: there is no template to measure it from"
                );
            }
            for (auto const& appearance : appearances)
            {
                if (
                    interact->clickOffset->x() >= appearance.templateRect.width()
                    || interact->clickOffset->y() >= appearance.templateRect.height()
                )
                {
                    return invalidCatalog(
                        std::format(
                            "click offset must be inside every appearance template, and falls outside \"{}\"",
                            appearance.name.value()
                        )
                    );
                }
            }
        }

        return ok();
    }

    CompiledElement::CompiledElement(CompiledElementSpec spec) noexcept
        : m_id{spec.id}
        , m_name{std::move(spec.name)}
        , m_capabilities{spec.capabilities}
        , m_searchRoi{spec.searchRoi}
        , m_appearances{std::move(spec.appearances)}
    {
    }

    auto CompiledElement::create(
        ProjectFingerprint fingerprint,
        CompiledElementSpec const& spec
    ) -> Result<CompiledElement>
    {
        UF_TRY(
            validateElementShape(
                fingerprint,
                spec.searchRoi,
                spec.appearances,
                spec.capabilities
            )
        );

        return CompiledElement{spec};
    }

    auto CompiledElement::id() const -> ElementId { return m_id; }
    auto CompiledElement::name() const -> ResourceName { return m_name; }
    auto CompiledElement::capabilities() const noexcept -> ElementCapabilities const&
    {
        return m_capabilities;
    }
    auto CompiledElement::searchRoi() const noexcept -> PixelRect { return m_searchRoi; }
    auto CompiledElement::appearances() const noexcept -> std::span<CompiledAppearance const>
    {
        return m_appearances;
    }
    auto CompiledElement::findAppearance(
        ResourceName const& name
    ) const noexcept -> CompiledAppearance const*
    {
        auto const found = std::ranges::find(
            m_appearances,
            name,
            &CompiledAppearance::name
        );
        return found == m_appearances.end() ? nullptr : &*found;
    }

    PageSignature::PageSignature(
        PageSpec spec,
        std::vector<ElementId> required,
        std::vector<ElementId> forbidden
    ) noexcept
        : m_id{spec.id}
        , m_name{std::move(spec.name)}
        , m_required{std::move(required)}
        , m_forbidden{std::move(forbidden)}
    {
    }

    auto PageSignature::id() const -> PageId { return m_id; }
    auto PageSignature::name() const -> ResourceName { return m_name; }
    auto PageSignature::required() const noexcept -> std::span<ElementId const>
    {
        return m_required;
    }
    auto PageSignature::forbidden() const noexcept -> std::span<ElementId const>
    {
        return m_forbidden;
    }

    RecognitionCatalog::RecognitionCatalog(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<CompiledElement> elements,
        std::vector<PageSignature> pages,
        std::vector<PageReference> references,
        std::vector<ElementId> pageAnchorOrder
    ) noexcept
        : m_projectId{std::move(projectId)}
        , m_fingerprint{fingerprint}
        , m_elements{std::move(elements)}
        , m_pages{std::move(pages)}
        , m_references{std::move(references)}
        , m_pageAnchorOrder{std::move(pageAnchorOrder)}
    {
    }

    auto RecognitionCatalog::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<CompiledElement> elements,
        std::vector<PageSpec> pages,
        std::vector<PageReference> references
    ) -> Result<RecognitionCatalog>
    {
        std::ranges::sort(
            elements,
            {},
            [](CompiledElement const& element) -> ResourceId
            {
                return element.id().value();
            }
        );
        std::ranges::sort(
            pages,
            {},
            [](PageSpec const& page) -> ResourceId
            {
                return page.id.value();
            }
        );
        std::ranges::sort(references, referenceLess);

        for (auto index = std::size_t{1}; index < elements.size(); ++index)
        {
            if (
                checkedAt(elements, index - 1U).id()
                == checkedAt(elements, index).id()
            )
            {
                return invalidCatalog("element IDs must be unique");
            }
        }

        for (auto leftIndex = std::size_t{0}; leftIndex < elements.size(); ++leftIndex)
        {
            for (
                auto rightIndex = leftIndex + 1U;
                rightIndex < elements.size();
                ++rightIndex
            )
            {
                if (
                    checkedAt(elements, leftIndex).name()
                    == checkedAt(elements, rightIndex).name()
                )
                {
                    return invalidCatalog("element names must be unique");
                }
            }
        }

        for (auto index = std::size_t{1}; index < pages.size(); ++index)
        {
            if (checkedAt(pages, index - 1U).id == checkedAt(pages, index).id)
            {
                return invalidCatalog("page IDs must be unique");
            }
        }

        for (auto leftIndex = std::size_t{0}; leftIndex < pages.size(); ++leftIndex)
        {
            for (auto rightIndex = leftIndex + 1U; rightIndex < pages.size(); ++rightIndex)
            {
                if (checkedAt(pages, leftIndex).name == checkedAt(pages, rightIndex).name)
                {
                    return invalidCatalog("page names must be unique");
                }
            }
        }

        for (auto const& page : pages)
        {
            for (auto const& element : elements)
            {
                if (
                    page.id.value() == element.id().value()
                    || page.name == element.name()
                )
                {
                    return invalidCatalog(
                        "resource IDs and names must be globally unique"
                    );
                }
            }
        }

        auto findElement = [&elements](
            ElementId id
        ) noexcept -> CompiledElement const*
        {
            auto const found = std::ranges::find(
                elements,
                id,
                &CompiledElement::id
            );
            return found == elements.end() ? nullptr : &*found;
        };
        auto findPage = [&pages](PageId id) noexcept -> PageSpec const*
        {
            auto const found = std::ranges::find(pages, id, &PageSpec::id);
            return found == pages.end() ? nullptr : &*found;
        };

        for (auto index = std::size_t{0}; index < references.size(); ++index)
        {
            auto const& reference = checkedAt(references, index);
            if (
                index != 0U
                && checkedAt(references, index - 1U).pageId == reference.pageId
                && checkedAt(references, index - 1U).elementId == reference.elementId
            )
            {
                return invalidCatalog("a page references the same element twice");
            }

            auto const* p_page = findPage(reference.pageId);
            if (p_page == nullptr)
            {
                return invalidCatalog("page reference names an unknown page");
            }
            auto const* p_element = findElement(reference.elementId);
            if (p_element == nullptr)
            {
                return invalidCatalog("page reference names an unknown element");
            }

            // Two levels, one direction: the element declares what it can do,
            // and the reference declares what this page does with it.
            if (!reference.exercised.isSubsetOf(p_element->capabilities()))
            {
                return invalidCatalog(
                    std::format(
                        "page \"{}\" exercises a capability \"{}\" does not declare",
                        p_page->name.value(),
                        p_element->name().value()
                    )
                );
            }

            // The anchor pass reads the element-level region. Letting a
            // signature member narrow it per page would search the same pixels
            // a second time in the same cycle, which is exactly the cost the
            // capability merge exists to remove.
            if (reference.exercised.hasIdentify() && reference.searchRoi.has_value())
            {
                return invalidCatalog(
                    std::format(
                        "page \"{}\" exercises identify on \"{}\" and may not refine its search_roi",
                        p_page->name.value(),
                        p_element->name().value()
                    )
                );
            }

            if (auto const refined = reference.searchRoi)
            {
                UF_TRY(
                    validateElementShape(
                        fingerprint,
                        *refined,
                        p_element->appearances(),
                        p_element->capabilities()
                    )
                );
            }

            if (auto const& pinned = reference.appearance)
            {
                if (p_element->findAppearance(*pinned) == nullptr)
                {
                    return invalidCatalog(
                        std::format(
                            "page \"{}\" pins appearance \"{}\", which \"{}\" does not declare",
                            p_page->name.value(),
                            pinned->value(),
                            p_element->name().value()
                        )
                    );
                }
            }
        }

        for (auto const& element : elements)
        {
            // Owned is the author saying an element belongs to one page. Two
            // pages claiming to own the same one is the contradiction the flag
            // this replaced could hold without anything noticing.
            auto owners = std::size_t{0};
            for (auto const& reference : references)
            {
                if (
                    reference.elementId == element.id()
                    && reference.holding == Holding::Owned
                )
                {
                    ++owners;
                }
            }
            if (owners > 1U)
            {
                return invalidCatalog(
                    std::format(
                        "\"{}\" is owned by more than one page",
                        element.name().value()
                    )
                );
            }

            // The closure that replaces "an action target must authorize a
            // page": an element the runtime could be asked to click has to be
            // reachable somewhere it can be clicked. An element that is only
            // read, or only identifies, needs no such edge.
            if (!element.capabilities().hasInteract())
            {
                continue;
            }
            auto const exercised = std::ranges::any_of(
                references,
                [&element](PageReference const& reference)
                {
                    return (
                        reference.elementId == element.id()
                        && reference.exercised.hasInteract()
                    );
                }
            );
            if (!exercised)
            {
                return invalidCatalog(
                    std::format(
                        "\"{}\" declares interact but no page exercises it",
                        element.name().value()
                    )
                );
            }
        }

        // The signature is derived here and stored nowhere else. References are
        // sorted by page then element, so each page's members come out already
        // ordered by ID, and one reference per (page, element) makes required
        // and forbidden structurally incapable of overlapping or repeating.
        auto signatures      = std::vector<PageSignature>{};
        auto pageAnchorOrder = std::vector<ElementId>{};
        signatures.reserve(pages.size());
        for (auto& page : pages)
        {
            auto required  = std::vector<ElementId>{};
            auto forbidden = std::vector<ElementId>{};
            for (auto const& reference : references)
            {
                if (reference.pageId != page.id)
                {
                    continue;
                }
                auto const& identify = reference.exercised.identify();
                if (!identify.has_value())
                {
                    continue;
                }
                switch (identify->role)
                {
                case SignatureRole::Required:
                    required.emplace_back(reference.elementId);
                    break;
                case SignatureRole::Forbidden:
                    forbidden.emplace_back(reference.elementId);
                    break;
                }
                pageAnchorOrder.emplace_back(reference.elementId);
            }

            if (required.empty() && forbidden.empty())
            {
                return invalidCatalog(
                    std::format(
                        "page \"{}\" has no reference exercising identify, so nothing can recognise it",
                        page.name.value()
                    )
                );
            }
            signatures.emplace_back(
                PageSignature{
                    std::move(page),
                    std::move(required),
                    std::move(forbidden)
                }
            );
        }

        for (auto leftIndex = std::size_t{0}; leftIndex < signatures.size(); ++leftIndex)
        {
            for (
                auto rightIndex = leftIndex + 1U;
                rightIndex < signatures.size();
                ++rightIndex
            )
            {
                if (
                    sameSignature(
                        checkedAt(signatures, leftIndex),
                        checkedAt(signatures, rightIndex)
                    )
                )
                {
                    return invalidCatalog(
                        "two pages have the same required and forbidden signature"
                    );
                }
            }
        }

        std::ranges::sort(pageAnchorOrder, lessId<ElementId>);
        pageAnchorOrder.erase(
            std::unique(pageAnchorOrder.begin(), pageAnchorOrder.end()),
            pageAnchorOrder.end()
        );

        return RecognitionCatalog{
            std::move(projectId),
            fingerprint,
            std::move(elements),
            std::move(signatures),
            std::move(references),
            std::move(pageAnchorOrder)
        };
    }

    auto RecognitionCatalog::projectId() const noexcept -> ProjectId const&
    {
        return m_projectId;
    }

    auto RecognitionCatalog::fingerprint() const noexcept -> ProjectFingerprint
    {
        return m_fingerprint;
    }

    auto RecognitionCatalog::elements() const noexcept -> std::span<CompiledElement const>
    {
        return m_elements;
    }

    auto RecognitionCatalog::pages() const noexcept -> std::span<PageSignature const>
    {
        return m_pages;
    }

    auto RecognitionCatalog::references() const noexcept -> std::span<PageReference const>
    {
        return m_references;
    }

    auto RecognitionCatalog::findElement(
        ElementId id
    ) const noexcept -> CompiledElement const*
    {
        auto const found = std::ranges::find(
            m_elements,
            id,
            &CompiledElement::id
        );
        return found == m_elements.end() ? nullptr : &*found;
    }

    auto RecognitionCatalog::findPage(PageId id) const noexcept -> PageSignature const*
    {
        auto const found = std::ranges::find(m_pages, id, &PageSignature::id);
        return found == m_pages.end() ? nullptr : &*found;
    }

    auto RecognitionCatalog::findReference(
        PageId pageId,
        ElementId elementId
    ) const noexcept -> PageReference const*
    {
        auto const found = std::ranges::find_if(
            m_references,
            [pageId, elementId](PageReference const& reference) noexcept -> bool
            {
                return (
                    reference.pageId == pageId
                    && reference.elementId == elementId
                );
            }
        );
        return found == m_references.end() ? nullptr : &*found;
    }

    auto RecognitionCatalog::pageAnchorOrder() const noexcept -> std::span<ElementId const>
    {
        return m_pageAnchorOrder;
    }
}
