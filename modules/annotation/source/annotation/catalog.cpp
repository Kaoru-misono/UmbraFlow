#include "catalog.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_luauReservedWords = std::array{
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
            return std::ranges::find(g_luauReservedWords, value) != g_luauReservedWords.end();
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
            if (nibbleIndex % 2 == 0)
            {
                bytes[byteIndex] = static_cast<uint8>(*nibble << 4);
            }
            else
            {
                bytes[byteIndex] = static_cast<uint8>(bytes[byteIndex] | *nibble);
            }
            ++nibbleIndex;
        }

        return ResourceId{bytes};
    }

    auto ResourceId::toString() const -> std::string
    {
        static constexpr auto s_hexDigits = std::string_view{"0123456789abcdef"};

        auto result = std::string{};
        result.reserve(36);
        for (auto index = std::size_t{0}; index < m_bytes.size(); ++index)
        {
            if (index == 4 || index == 6 || index == 8 || index == 10)
            {
                result.push_back('-');
            }

            auto const value = m_bytes[index];
            result.push_back(s_hexDigits[value >> 4]);
            result.push_back(s_hexDigits[value & uint8{0x0F}]);
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
        if (basisPoints > s_basisPointMaximum)
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
            s_basisPointMaximum - m_basisPoints
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

        return *numerator / static_cast<uint64>(s_basisPointMaximum);
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
        : m_id{std::move(spec.m_id)}
        , m_name{std::move(spec.m_name)}
        , m_annotationType{spec.m_annotationType}
        , m_templateRect{spec.m_templateRect}
        , m_searchRoi{spec.m_searchRoi}
        , m_threshold{spec.m_threshold}
        , m_defaultClick{spec.m_defaultClick}
        , m_allowedPageIds{std::move(spec.m_allowedPageIds)}
    {
    }

    auto RecognizerDefinition::create(
        ProjectFingerprint fingerprint,
        RecognizerSpec spec
    ) -> Result<RecognizerDefinition>
    {
        auto const templateWithinProject = (
            spec.m_templateRect.right() <= fingerprint.width()
            && spec.m_templateRect.bottom() <= fingerprint.height()
        );
        auto const searchWithinProject = (
            spec.m_searchRoi.right() <= fingerprint.width()
            && spec.m_searchRoi.bottom() <= fingerprint.height()
        );
        if (!templateWithinProject || !searchWithinProject)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "recognizer template_rect and search_roi must fit the project resolution"
            );
        }

        if (
            spec.m_templateRect.width() > spec.m_searchRoi.width()
            || spec.m_templateRect.height() > spec.m_searchRoi.height()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "recognizer template dimensions must fit inside search_roi"
            );
        }

        UF_TRY(
            spec.m_threshold.maximumSad(
                spec.m_templateRect.width(),
                spec.m_templateRect.height()
            )
        );

        if (
            spec.m_annotationType != AnnotationType::ActionTarget
            && spec.m_defaultClick.has_value()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "only action_target recognizers may define a default click"
            );
        }

        if (
            spec.m_defaultClick.has_value()
            && (
                spec.m_defaultClick->x() >= spec.m_templateRect.width()
                || spec.m_defaultClick->y() >= spec.m_templateRect.height()
            )
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "default click must be inside the recognizer template"
            );
        }

        if (
            spec.m_annotationType == AnnotationType::PageAnchor
            && !spec.m_allowedPageIds.empty()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page_anchor membership must be expressed by page signatures"
            );
        }

        if (
            spec.m_annotationType == AnnotationType::ActionTarget
            && spec.m_allowedPageIds.empty()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "action_target recognizer must authorize at least one page"
            );
        }

        std::ranges::sort(spec.m_allowedPageIds, lessId<PageId>);
        if (hasDuplicateIds<PageId>(spec.m_allowedPageIds))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "recognizer contains duplicate allowed page IDs"
            );
        }

        return RecognizerDefinition{std::move(spec)};
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
        : m_id{std::move(spec.m_id)}
        , m_name{std::move(spec.m_name)}
        , m_required{std::move(spec.m_required)}
        , m_forbidden{std::move(spec.m_forbidden)}
    {
    }

    auto PageSignature::create(PageSpec spec) -> Result<PageSignature>
    {
        std::ranges::sort(spec.m_required, lessId<RecognizerId>);
        std::ranges::sort(spec.m_forbidden, lessId<RecognizerId>);
        if (
            hasDuplicateIds<RecognizerId>(spec.m_required)
            || hasDuplicateIds<RecognizerId>(spec.m_forbidden)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page signature contains duplicate recognizer IDs"
            );
        }

        for (auto const id : spec.m_required)
        {
            if (containsId<RecognizerId>(spec.m_forbidden, id))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "page required and forbidden recognizer sets overlap"
                );
            }
        }

        return PageSignature{std::move(spec)};
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
