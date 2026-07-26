#include "catalog.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/safety/checked-access.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

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

        template <typename Id>
        [[nodiscard]]
        auto containsId(std::span<Id const> ids, Id id) noexcept -> bool
        {
            return std::ranges::find(ids, id) != ids.end();
        }

        [[nodiscard]]
        auto sameSignature(PageSignature const& left, PageSignature const& right) noexcept -> bool
        {
            return (
                std::ranges::equal(left.required(), right.required())
                && std::ranges::equal(left.forbidden(), right.forbidden())
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

    auto ProjectFingerprint::create(
        uint32 width,
        uint32 height,
        uint32 dpiX,
        uint32 dpiY
    ) -> Result<ProjectFingerprint>
    {
        if (width == 0 || height == 0 || dpiX == 0 || dpiY == 0)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "project fingerprint must be non-zero: {}x{} at {}x{} DPI",
                    width,
                    height,
                    dpiX,
                    dpiY
                )
            );
        }

        return ProjectFingerprint{width, height, dpiX, dpiY};
    }

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

    RecognizerDefinition::RecognizerDefinition(RecognizerSpec spec) noexcept
        : m_id{spec.id}
        , m_name{std::move(spec.name)}
        , m_annotationType{spec.annotationType}
        , m_templateRect{spec.templateRect}
        , m_searchRoi{spec.searchRoi}
        , m_threshold{spec.threshold}
        , m_defaultClick{spec.defaultClick}
        , m_allowedPageIds{std::move(spec.allowedPageIds)}
    {
    }

    auto RecognizerDefinition::create(
        ProjectFingerprint fingerprint,
        RecognizerSpec const& spec
    ) -> Result<RecognizerDefinition>
    {
        auto const templateWithinProject = (
            spec.templateRect.right() <= fingerprint.width()
            && spec.templateRect.bottom() <= fingerprint.height()
        );
        auto const searchWithinProject = (
            spec.searchRoi.right() <= fingerprint.width()
            && spec.searchRoi.bottom() <= fingerprint.height()
        );
        if (!templateWithinProject || !searchWithinProject)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "recognizer template_rect and search_roi must fit the project resolution"
            );
        }

        if (
            spec.templateRect.width() > spec.searchRoi.width()
            || spec.templateRect.height() > spec.searchRoi.height()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "recognizer template dimensions must fit inside search_roi"
            );
        }

        UF_TRY(
            spec.threshold.maximumSad(
                spec.templateRect.width(),
                spec.templateRect.height()
            )
        );

        if (
            spec.annotationType != AnnotationType::ActionTarget
            && spec.defaultClick.has_value()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "only action_target recognizers may define a default click"
            );
        }

        if (
            spec.defaultClick.has_value()
            && (
                spec.defaultClick->x() >= spec.templateRect.width()
                || spec.defaultClick->y() >= spec.templateRect.height()
            )
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "default click must be inside the recognizer template"
            );
        }

        if (
            spec.annotationType == AnnotationType::PageAnchor
            && !spec.allowedPageIds.empty()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page_anchor membership must be expressed by page signatures"
            );
        }

        if (
            spec.annotationType == AnnotationType::ActionTarget
            && spec.allowedPageIds.empty()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "action_target recognizer must authorize at least one page"
            );
        }

        auto normalizedSpec = spec;
        std::ranges::sort(normalizedSpec.allowedPageIds, lessId<PageId>);
        if (hasDuplicateIds<PageId>(normalizedSpec.allowedPageIds))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "recognizer contains duplicate allowed page IDs"
            );
        }

        return RecognizerDefinition{std::move(normalizedSpec)};
    }

    auto RecognizerDefinition::id() const -> RecognizerId { return m_id; }
    auto RecognizerDefinition::name() const -> ResourceName { return m_name; }
    auto RecognizerDefinition::annotationType() const noexcept -> AnnotationType
    {
        return m_annotationType;
    }
    auto RecognizerDefinition::templateRect() const noexcept -> PixelRect { return m_templateRect; }
    auto RecognizerDefinition::searchRoi() const noexcept -> PixelRect { return m_searchRoi; }
    auto RecognizerDefinition::threshold() const noexcept -> SimilarityThreshold
    {
        return m_threshold;
    }
    auto RecognizerDefinition::defaultClick() const noexcept -> std::optional<TemplateOffset>
    {
        return m_defaultClick;
    }
    auto RecognizerDefinition::allowedPageIds() const noexcept -> std::span<PageId const>
    {
        return m_allowedPageIds;
    }

    PageSignature::PageSignature(PageSpec spec) noexcept
        : m_id{spec.id}
        , m_name{std::move(spec.name)}
        , m_required{std::move(spec.required)}
        , m_forbidden{std::move(spec.forbidden)}
    {
    }

    auto PageSignature::create(PageSpec const& spec) -> Result<PageSignature>
    {
        auto normalizedSpec = spec;
        if (
            normalizedSpec.required.empty()
            && normalizedSpec.forbidden.empty()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page signature must contain at least one required or forbidden recognizer"
            );
        }

        std::ranges::sort(normalizedSpec.required, lessId<RecognizerId>);
        std::ranges::sort(normalizedSpec.forbidden, lessId<RecognizerId>);
        if (
            hasDuplicateIds<RecognizerId>(normalizedSpec.required)
            || hasDuplicateIds<RecognizerId>(normalizedSpec.forbidden)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page signature contains duplicate recognizer IDs"
            );
        }

        for (auto const id : normalizedSpec.required)
        {
            if (containsId<RecognizerId>(normalizedSpec.forbidden, id))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "page required and forbidden recognizer sets overlap"
                );
            }
        }

        return PageSignature{std::move(normalizedSpec)};
    }

    auto PageSignature::id() const -> PageId { return m_id; }
    auto PageSignature::name() const -> ResourceName { return m_name; }
    auto PageSignature::required() const noexcept -> std::span<RecognizerId const>
    {
        return m_required;
    }
    auto PageSignature::forbidden() const noexcept -> std::span<RecognizerId const>
    {
        return m_forbidden;
    }

    RecognitionCatalog::RecognitionCatalog(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<RecognizerDefinition> recognizers,
        std::vector<PageSignature> pages,
        std::vector<RecognizerId> pageAnchorOrder
    ) noexcept
        : m_projectId{std::move(projectId)}
        , m_fingerprint{fingerprint}
        , m_recognizers{std::move(recognizers)}
        , m_pages{std::move(pages)}
        , m_pageAnchorOrder{std::move(pageAnchorOrder)}
    {
    }

    auto RecognitionCatalog::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<RecognizerDefinition> recognizers,
        std::vector<PageSignature> pages
    ) -> Result<RecognitionCatalog>
    {
        std::ranges::sort(
            recognizers,
            {},
            [](RecognizerDefinition const& recognizer) -> ResourceId
            {
                return recognizer.id().value();
            }
        );
        std::ranges::sort(
            pages,
            {},
            [](PageSignature const& page) -> ResourceId
            {
                return page.id().value();
            }
        );

        for (auto index = std::size_t{1}; index < recognizers.size(); ++index)
        {
            auto const& previous = recognizers[index - 1];
            auto const& current = recognizers[index];
            if (previous.id() == current.id())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "recognizer IDs must be unique"
                );
            }
        }

        for (auto leftIndex = std::size_t{0}; leftIndex < recognizers.size(); ++leftIndex)
        {
            for (
                auto rightIndex = leftIndex + 1;
                rightIndex < recognizers.size();
                ++rightIndex
            )
            {
                if (recognizers[leftIndex].name() == recognizers[rightIndex].name())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "recognizer names must be unique"
                    );
                }
            }
        }

        for (auto index = std::size_t{1}; index < pages.size(); ++index)
        {
            auto const& previous = pages[index - 1];
            auto const& current = pages[index];
            if (previous.id() == current.id())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "page IDs must be unique"
                );
            }
        }

        for (auto leftIndex = std::size_t{0}; leftIndex < pages.size(); ++leftIndex)
        {
            for (
                auto rightIndex = leftIndex + 1;
                rightIndex < pages.size();
                ++rightIndex
            )
            {
                if (pages[leftIndex].name() == pages[rightIndex].name())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "page names must be unique"
                    );
                }
            }
        }

        for (auto const& page : pages)
        {
            for (auto const& recognizer : recognizers)
            {
                if (
                    page.id().value() == recognizer.id().value()
                    || page.name() == recognizer.name()
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "resource IDs and names must be globally unique"
                    );
                }
            }
        }

        auto findRecognizer = [&recognizers](
            RecognizerId id
        ) noexcept -> RecognizerDefinition const*
        {
            auto const found = std::ranges::find(
                recognizers,
                id,
                &RecognizerDefinition::id
            );
            return found == recognizers.end() ? nullptr : &*found;
        };
        auto findPage = [&pages](PageId id) noexcept -> PageSignature const*
        {
            auto const found = std::ranges::find(pages, id, &PageSignature::id);
            return found == pages.end() ? nullptr : &*found;
        };

        auto pageAnchorOrder = std::vector<RecognizerId>{};
        for (auto const& page : pages)
        {
            for (auto const id : page.required())
            {
                auto const* p_recognizer = findRecognizer(id);
                if (
                    p_recognizer == nullptr
                    || p_recognizer->annotationType() != AnnotationType::PageAnchor
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "page signatures may reference only existing page_anchor recognizers"
                    );
                }
                pageAnchorOrder.emplace_back(id);
            }
            for (auto const id : page.forbidden())
            {
                auto const* p_recognizer = findRecognizer(id);
                if (
                    p_recognizer == nullptr
                    || p_recognizer->annotationType() != AnnotationType::PageAnchor
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "page signatures may reference only existing page_anchor recognizers"
                    );
                }
                pageAnchorOrder.emplace_back(id);
            }
        }

        for (auto const& recognizer : recognizers)
        {
            for (auto const pageId : recognizer.allowedPageIds())
            {
                if (findPage(pageId) == nullptr)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "recognizer allowed_page_ids contains an unknown page"
                    );
                }
            }
        }

        for (auto leftIndex = std::size_t{0}; leftIndex < pages.size(); ++leftIndex)
        {
            for (
                auto rightIndex = leftIndex + 1;
                rightIndex < pages.size();
                ++rightIndex
            )
            {
                if (sameSignature(pages[leftIndex], pages[rightIndex]))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "two pages have the same required and forbidden signature"
                    );
                }
            }
        }

        std::ranges::sort(pageAnchorOrder, lessId<RecognizerId>);
        pageAnchorOrder.erase(
            std::unique(pageAnchorOrder.begin(), pageAnchorOrder.end()),
            pageAnchorOrder.end()
        );

        return RecognitionCatalog{
            std::move(projectId),
            fingerprint,
            std::move(recognizers),
            std::move(pages),
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

    auto RecognitionCatalog::recognizers() const noexcept -> std::span<RecognizerDefinition const>
    {
        return m_recognizers;
    }

    auto RecognitionCatalog::pages() const noexcept -> std::span<PageSignature const>
    {
        return m_pages;
    }

    auto RecognitionCatalog::findRecognizer(
        RecognizerId id
    ) const noexcept -> RecognizerDefinition const*
    {
        auto const found = std::ranges::find(
            m_recognizers,
            id,
            &RecognizerDefinition::id
        );
        return found == m_recognizers.end() ? nullptr : &*found;
    }

    auto RecognitionCatalog::findPage(PageId id) const noexcept -> PageSignature const*
    {
        auto const found = std::ranges::find(m_pages, id, &PageSignature::id);
        return found == m_pages.end() ? nullptr : &*found;
    }

    auto RecognitionCatalog::pageAnchorOrder() const noexcept -> std::span<RecognizerId const>
    {
        return m_pageAnchorOrder;
    }
}
