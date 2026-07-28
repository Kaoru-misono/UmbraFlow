#include "authoring-document.hpp"

#include "detail/annotation-fields.hpp"
#include "detail/canonical-toml.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>

#include <domain/error.hpp>

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
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_maximumAuthoringDocumentBytes = std::size_t{16} * 1024U * 1024U;
        constexpr auto k_maximumAuthoringResources     = std::size_t{4096};
        constexpr auto k_maximumAuthoringPlacements    = std::size_t{4096} * 16U;

        [[nodiscard]]
        auto invalidAuthoring(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto sourcePath(ContentHash const& hash) -> std::string
        {
            auto path = std::string{"assets/sources/"};
            path += hash.hex();
            path += ".png";
            return path;
        }

        [[nodiscard]]
        constexpr auto isDigit(char value) noexcept -> bool
        {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]]
        constexpr auto twoDigits(
            std::string_view value,
            std::size_t offset
        ) noexcept -> uint32
        {
            return (
                static_cast<uint32>(checkedAt(value, offset) - '0') * 10U
                + static_cast<uint32>(checkedAt(value, offset + 1U) - '0')
            );
        }

        [[nodiscard]]
        constexpr auto fourDigits(
            std::string_view value,
            std::size_t offset
        ) noexcept -> uint32
        {
            return twoDigits(value, offset) * 100U + twoDigits(value, offset + 2U);
        }

        [[nodiscard]]
        constexpr auto isLeapYear(uint32 year) noexcept -> bool
        {
            return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
        }

        [[nodiscard]]
        constexpr auto daysInMonth(uint32 year, uint32 month) noexcept -> uint32
        {
            constexpr auto days = std::array<uint32, 12>{
                31, 28, 31, 30, 31, 30,
                31, 31, 30, 31, 30, 31,
            };
            if (month == 2U && isLeapYear(year))
            {
                return 29U;
            }
            return checkedAt(days, static_cast<std::size_t>(month - 1U));
        }

        [[nodiscard]]
        auto isCanonicalRfc3339(std::string_view value) noexcept -> bool
        {
            if (
                value.size() < 20U
                || checkedAt(value, 4) != '-'
                || checkedAt(value, 7) != '-'
                || checkedAt(value, 10) != 'T'
                || checkedAt(value, 13) != ':'
                || checkedAt(value, 16) != ':'
            )
            {
                return false;
            }
            for (auto const index : std::array<std::size_t, 14>{
                0, 1, 2, 3, 5, 6, 8, 9,
                11, 12, 14, 15, 17, 18,
            })
            {
                if (!isDigit(checkedAt(value, index)))
                {
                    return false;
                }
            }

            auto const year   = fourDigits(value, 0);
            auto const month  = twoDigits(value, 5);
            auto const day    = twoDigits(value, 8);
            auto const hour   = twoDigits(value, 11);
            auto const minute = twoDigits(value, 14);
            auto const second = twoDigits(value, 17);
            if (
                year == 0U
                || month == 0U
                || month > 12U
                || day == 0U
                || day > daysInMonth(year, month)
                || hour > 23U
                || minute > 59U
                || second > 60U
            )
            {
                return false;
            }

            auto position = std::size_t{19};
            if (position < value.size() && checkedAt(value, position) == '.')
            {
                ++position;
                auto const fractionStart = position;
                while (
                    position < value.size()
                    && isDigit(checkedAt(value, position))
                )
                {
                    ++position;
                }
                if (position == fractionStart)
                {
                    return false;
                }
            }
            if (position >= value.size())
            {
                return false;
            }
            if (checkedAt(value, position) == 'Z')
            {
                return position + 1U == value.size();
            }
            if (
                checkedAt(value, position) != '+'
                && checkedAt(value, position) != '-'
            )
            {
                return false;
            }
            if (
                value.size() - position != 6U
                || checkedAt(value, position + 3U) != ':'
                || !isDigit(checkedAt(value, position + 1U))
                || !isDigit(checkedAt(value, position + 2U))
                || !isDigit(checkedAt(value, position + 4U))
                || !isDigit(checkedAt(value, position + 5U))
            )
            {
                return false;
            }
            return (
                twoDigits(value, position + 1U) <= 23U
                && twoDigits(value, position + 4U) <= 59U
            );
        }

        [[nodiscard]]
        auto parseSource(
            detail::CanonicalTomlReader& reader
        ) -> Result<AuthoringSource>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, detail::parseId<SourceId>(idText));
            UF_TRY_VALUE(path, reader.takeStringField("path"));
            UF_TRY_VALUE(hashText, reader.takeStringField("content_hash"));
            UF_TRY_VALUE(contentHash, ContentHash::parse(hashText));
            if (path != sourcePath(contentHash))
            {
                return invalidAuthoring(
                    "authoring source path does not match content_hash"
                );
            }
            UF_TRY_VALUE(
                fingerprint,
                detail::parseFingerprintFields(reader, "client_size", "dpi")
            );
            UF_TRY_VALUE(backend, reader.takeStringField("capture_backend"));
            auto provenance = SourceProvenance{ImportedSourceProvenance{}};
            if (backend == "wgc")
            {
                UF_TRY_VALUE(
                    generation,
                    reader.takeUnsigned64Field("target_generation")
                );
                UF_TRY_VALUE(capturedAt, reader.takeStringField("captured_at"));
                provenance = WgcSourceProvenance{
                    .targetGeneration = TargetGeneration::fromValue(generation),
                    .capturedAt       = std::move(capturedAt),
                };
            }
            else if (backend != "imported")
            {
                return invalidAuthoring(
                    std::format("unknown authoring capture_backend '{}'", backend)
                );
            }
            return AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = id,
                    .contentHash = contentHash,
                    .fingerprint = fingerprint,
                    .provenance  = std::move(provenance),
                }
            );
        }

        [[nodiscard]]
        auto parsePage(
            detail::CanonicalTomlReader& reader
        ) -> Result<PageSignature>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, detail::parseId<PageId>(idText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(
                requiredText,
                reader.takeStringArrayField("required")
            );
            UF_TRY_VALUE(
                required,
                detail::parseIds<RecognizerId>(requiredText)
            );
            UF_TRY_VALUE(
                forbiddenText,
                reader.takeStringArrayField("forbidden")
            );
            UF_TRY_VALUE(
                forbidden,
                detail::parseIds<RecognizerId>(forbiddenText)
            );
            return PageSignature::create(
                PageSpec{
                    .id        = id,
                    .name      = std::move(name),
                    .required  = std::move(required),
                    .forbidden = std::move(forbidden),
                }
            );
        }

        [[nodiscard]]
        auto parseClassification(
            std::string_view value
        ) -> Result<RegressionClassification>
        {
            if (value == "positive")
            {
                return RegressionClassification::Positive;
            }
            if (value == "negative")
            {
                return RegressionClassification::Negative;
            }
            if (value == "confusable")
            {
                return RegressionClassification::Confusable;
            }
            return invalidAuthoring(
                std::format("unknown regression classification '{}'", value)
            );
        }

        [[nodiscard]]
        auto parseRegression(
            detail::CanonicalTomlReader& reader
        ) -> Result<RegressionCase>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, detail::parseId<RegressionId>(idText));
            UF_TRY_VALUE(sourceIdText, reader.takeStringField("source_id"));
            UF_TRY_VALUE(sourceId, detail::parseId<SourceId>(sourceIdText));
            UF_TRY_VALUE(
                classificationText,
                reader.takeStringField("classification")
            );
            UF_TRY_VALUE(
                classification,
                parseClassification(classificationText)
            );
            UF_TRY_VALUE(
                outcome,
                reader.takeStringField("expected_outcome")
            );

            auto expectation = RegressionExpectation{UnknownRegression{}};
            if (outcome == "resolved")
            {
                UF_TRY_VALUE(
                    pageIdText,
                    reader.takeStringField("expected_page_id")
                );
                UF_TRY_VALUE(pageId, detail::parseId<PageId>(pageIdText));
                expectation = ResolvedRegression{pageId};
            }
            else if (outcome == "ambiguous")
            {
                expectation = AmbiguousRegression{};
            }
            else if (outcome != "unknown")
            {
                return invalidAuthoring(
                    std::format("unknown regression expected_outcome '{}'", outcome)
                );
            }
            return RegressionCase{
                RegressionSpec{
                    .id             = id,
                    .sourceId       = sourceId,
                    .classification = classification,
                    .expectation    = expectation,
                }
            };
        }

        [[nodiscard]]
        auto classificationText(
            RegressionClassification classification
        ) noexcept -> std::string_view
        {
            switch (classification)
            {
            case RegressionClassification::Positive:
                return "positive";
            case RegressionClassification::Negative:
                return "negative";
            case RegressionClassification::Confusable:
                return "confusable";
            }
            UF_UNREACHABLE_MSG("unknown regression classification");
        }

        template <typename Id>
        [[nodiscard]]
        auto resourceIdText(Id id) -> std::string
        {
            return id.value().toString();
        }

        [[nodiscard]]
        auto placementLess(
            AuthoringPlacement const& left,
            AuthoringPlacement const& right
        ) noexcept -> bool
        {
            if (left.pageId != right.pageId)
            {
                return left.pageId.value() < right.pageId.value();
            }
            return left.elementId.value() < right.elementId.value();
        }

        struct DerivedModel final
        {
            RecognitionCatalog                     catalog;
            std::vector<AuthoringRecognizerSource> recognizerSources;
        };

        // Builds the derived read model the compiler and UI still consume: each
        // element becomes a RecognizerDefinition whose allowed_page_ids are
        // inverted from the placements that reference it. This is the single
        // inversion the design asks for; every catalog() reader downstream gets
        // its page membership from here rather than joining placements itself.
        //
        // PERMANENT BRIDGE -- do not "clean this up". The placements-to-
        // allowed_page_ids inversion and the derived catalog() read model are the
        // load-bearing translation from the v2 authoring model to the FROZEN
        // runtime manifest schema (umbraflow-annotations/v1), whose recognizer
        // shape carries membership inverted on the recognizer. While that runtime
        // contract stands, this inversion must stay exactly here; deleting it
        // would either break the runtime manifest or scatter the join back across
        // every consumer, which is the state this design set out to remove.
        [[nodiscard]]
        auto deriveModel(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::span<Element const> elements,
            std::span<AuthoringPlacement const> placements,
            std::vector<PageSignature> pages
        ) -> Result<DerivedModel>
        {
            auto definitions       = std::vector<RecognizerDefinition>{};
            auto recognizerSources = std::vector<AuthoringRecognizerSource>{};
            definitions.reserve(elements.size());
            recognizerSources.reserve(elements.size());
            for (auto const& element : elements)
            {
                auto allowedPageIds = std::vector<PageId>{};
                for (auto const& placement : placements)
                {
                    if (placement.elementId == element.id())
                    {
                        allowedPageIds.emplace_back(placement.pageId);
                    }
                }

                auto defaultClick = std::optional<TemplateOffset>{};
                if (
                    auto const* p_interactive = std::get_if<InteractiveElement>(
                        &element.kind()
                    )
                )
                {
                    defaultClick = p_interactive->clickOffset;
                }

                UF_TRY_VALUE(
                    definition,
                    RecognizerDefinition::create(
                        fingerprint,
                        RecognizerSpec{
                            .id             = element.id(),
                            .name           = element.name(),
                            .annotationType = element.annotationType(),
                            .templateRect   = element.templateRect(),
                            .searchRoi      = element.searchRoi(),
                            .threshold      = element.threshold(),
                            .defaultClick   = defaultClick,
                            .allowedPageIds = std::move(allowedPageIds),
                        }
                    )
                );
                recognizerSources.emplace_back(
                    AuthoringRecognizerSource{
                        .recognizerId = element.id(),
                        .sourceId     = element.sourceId(),
                        .shared       = element.shared(),
                    }
                );
                definitions.emplace_back(std::move(definition));
            }

            UF_TRY_VALUE(
                catalog,
                RecognitionCatalog::create(
                    std::move(projectId),
                    fingerprint,
                    std::move(definitions),
                    std::move(pages)
                )
            );
            return DerivedModel{
                .catalog           = std::move(catalog),
                .recognizerSources = std::move(recognizerSources),
            };
        }

        [[nodiscard]]
        auto parseElement(
            detail::CanonicalTomlReader& reader,
            ProjectFingerprint fingerprint
        ) -> Result<Element>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, detail::parseId<RecognizerId>(idText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(typeText, reader.takeStringField("type"));
            auto const annotationType = detail::annotationTypeFromText(typeText);
            if (!annotationType)
            {
                return invalidAuthoring(
                    std::format("unknown authoring annotation type '{}'", typeText)
                );
            }
            UF_TRY_VALUE(sourceIdText, reader.takeStringField("source_id"));
            UF_TRY_VALUE(sourceId, detail::parseId<SourceId>(sourceIdText));
            UF_TRY_VALUE(kindText, reader.takeStringField("recognizer_kind"));
            if (kindText != "gray_template")
            {
                return invalidAuthoring(
                    "authoring P0 recognizer kind must be gray_template"
                );
            }
            UF_TRY_VALUE(
                templateRect,
                detail::parsePixelRectField(reader, "template_rect")
            );
            UF_TRY_VALUE(
                searchRoi,
                detail::parsePixelRectField(reader, "search_roi")
            );
            UF_TRY_VALUE(
                thresholdValue,
                reader.takeUnsigned32Field("min_similarity_bp")
            );
            UF_TRY_VALUE(threshold, SimilarityThreshold::create(thresholdValue));

            auto clickOffset = std::optional<TemplateOffset>{};
            UF_TRY_VALUE(hasDefaultClick, reader.nextIsField("default_click"));
            if (hasDefaultClick)
            {
                UF_TRY_VALUE(
                    values,
                    reader.takeUnsigned32ArrayField("default_click")
                );
                if (values.size() != 2U)
                {
                    return invalidAuthoring(
                        "authoring default_click must have two integers"
                    );
                }
                UF_TRY_VALUE(
                    offset,
                    TemplateOffset::create(
                        checkedAt(values, 0),
                        checkedAt(values, 1),
                        templateRect.width(),
                        templateRect.height()
                    )
                );
                clickOffset = offset;
            }
            if (
                *annotationType != AnnotationType::ActionTarget
                && clickOffset.has_value()
            )
            {
                return invalidAuthoring(
                    "only an interactive element may define a default click"
                );
            }

            auto shared = false;
            UF_TRY_VALUE(hasShared, reader.nextIsField("shared"));
            if (hasShared)
            {
                UF_TRY_VALUE(parsed, reader.takeBoolField("shared"));
                shared = parsed;
            }

            auto kind = ElementKind{AnchorElement{}};
            switch (*annotationType)
            {
            case AnnotationType::PageAnchor:
                kind = AnchorElement{};
                break;
            case AnnotationType::ActionTarget:
                kind = InteractiveElement{.clickOffset = clickOffset};
                break;
            case AnnotationType::InfoRegion:
                kind = InfoElement{};
                break;
            }

            return Element::create(
                fingerprint,
                Element::Spec{
                    .id           = id,
                    .name         = std::move(name),
                    .sourceId     = sourceId,
                    .templateRect = templateRect,
                    .searchRoi    = searchRoi,
                    .threshold    = threshold,
                    .kind         = std::move(kind),
                    .shared       = shared,
                }
            );
        }

        [[nodiscard]]
        auto parsePlacement(
            detail::CanonicalTomlReader& reader
        ) -> Result<AuthoringPlacement>
        {
            UF_TRY_VALUE(pageIdText, reader.takeStringField("page_id"));
            UF_TRY_VALUE(pageId, detail::parseId<PageId>(pageIdText));
            UF_TRY_VALUE(elementIdText, reader.takeStringField("element_id"));
            UF_TRY_VALUE(elementId, detail::parseId<RecognizerId>(elementIdText));
            UF_TRY_VALUE(
                searchRoi,
                detail::parsePixelRectField(reader, "search_roi")
            );
            return AuthoringPlacement{
                .pageId    = pageId,
                .elementId = elementId,
                .searchRoi = searchRoi,
            };
        }
    }

    auto annotationTypeOfKind(ElementKind const& kind) noexcept -> AnnotationType
    {
        return matchVariant(
            kind,
            [](AnchorElement const&) { return AnnotationType::PageAnchor; },
            [](InteractiveElement const&) { return AnnotationType::ActionTarget; },
            [](InfoElement const&) { return AnnotationType::InfoRegion; }
        );
    }

    Element::Element(Spec spec) noexcept
        : m_id{spec.id}
        , m_name{std::move(spec.name)}
        , m_sourceId{spec.sourceId}
        , m_templateRect{spec.templateRect}
        , m_searchRoi{spec.searchRoi}
        , m_threshold{spec.threshold}
        , m_kind{std::move(spec.kind)}
        , m_shared{spec.shared}
    {
    }

    auto Element::create(
        ProjectFingerprint fingerprint,
        Spec const& spec
    ) -> Result<Element>
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
            return invalidAuthoring(
                "element template_rect and search_roi must fit the project resolution"
            );
        }
        if (
            spec.templateRect.width() > spec.searchRoi.width()
            || spec.templateRect.height() > spec.searchRoi.height()
        )
        {
            return invalidAuthoring(
                "element template dimensions must fit inside search_roi"
            );
        }
        UF_TRY(
            spec.threshold.maximumSad(
                spec.templateRect.width(),
                spec.templateRect.height()
            )
        );

        // The click-inside-template rule that RecognizerDefinition enforces as a
        // cross-field check lives here as a fact of the interactive kind: no
        // other kind can carry a click to misplace.
        if (
            auto const* p_interactive = std::get_if<InteractiveElement>(&spec.kind)
        )
        {
            if (
                p_interactive->clickOffset.has_value()
                && (
                    p_interactive->clickOffset->x() >= spec.templateRect.width()
                    || p_interactive->clickOffset->y() >= spec.templateRect.height()
                )
            )
            {
                return invalidAuthoring(
                    "interactive click offset must be inside the element template"
                );
            }
        }

        return Element{spec};
    }

    auto Element::id() const -> RecognizerId { return m_id; }
    auto Element::name() const -> ResourceName { return m_name; }
    auto Element::sourceId() const -> SourceId { return m_sourceId; }
    auto Element::templateRect() const noexcept -> PixelRect { return m_templateRect; }
    auto Element::searchRoi() const noexcept -> PixelRect { return m_searchRoi; }
    auto Element::threshold() const noexcept -> SimilarityThreshold { return m_threshold; }
    auto Element::annotationType() const noexcept -> AnnotationType
    {
        return annotationTypeOfKind(m_kind);
    }
    auto Element::shared() const noexcept -> bool { return m_shared; }
    auto Element::kind() const noexcept -> ElementKind const& { return m_kind; }

    AuthoringSource::AuthoringSource(AuthoringSourceSpec const& spec)
        : m_id{spec.id}
        , m_contentHash{spec.contentHash}
        , m_relativePath{sourcePath(m_contentHash)}
        , m_fingerprint{spec.fingerprint}
        , m_provenance{spec.provenance}
    {
    }

    auto AuthoringSource::create(
        AuthoringSourceSpec const& spec
    ) -> Result<AuthoringSource>
    {
        auto const* p_wgc = std::get_if<WgcSourceProvenance>(
            &spec.provenance
        );
        if (
            p_wgc != nullptr
            && !isCanonicalRfc3339(p_wgc->capturedAt)
        )
        {
            return invalidAuthoring(
                "WGC source captured_at must be canonical RFC 3339"
            );
        }
        return AuthoringSource{spec};
    }

    auto AuthoringSource::id() const -> SourceId { return m_id; }
    auto AuthoringSource::contentHash() const -> ContentHash { return m_contentHash; }
    auto AuthoringSource::fingerprint() const noexcept -> ProjectFingerprint
    {
        return m_fingerprint;
    }
    auto AuthoringSource::relativePath() const noexcept -> std::string const&
    {
        return m_relativePath;
    }
    auto AuthoringSource::provenance() const noexcept -> SourceProvenance const&
    {
        return m_provenance;
    }

    RegressionCase::RegressionCase(RegressionSpec const& spec)
        : m_id{spec.id}
        , m_sourceId{spec.sourceId}
        , m_classification{spec.classification}
        , m_expectation{spec.expectation}
    {
    }

    auto RegressionCase::id() const -> RegressionId { return m_id; }
    auto RegressionCase::sourceId() const -> SourceId { return m_sourceId; }
    auto RegressionCase::classification() const noexcept -> RegressionClassification
    {
        return m_classification;
    }
    auto RegressionCase::expectation() const noexcept -> RegressionExpectation const&
    {
        return m_expectation;
    }

    AuthoringDocument::AuthoringDocument(
        RecognitionCatalog&& catalog,
        std::vector<AuthoringSource>&& sources,
        std::vector<Element>&& elements,
        std::vector<AuthoringPlacement>&& placements,
        std::vector<AuthoringRecognizerSource>&& recognizerSources,
        std::vector<RegressionCase>&& regressions
    ) noexcept
        : m_catalog{std::move(catalog)}
        , m_sources{std::move(sources)}
        , m_elements{std::move(elements)}
        , m_placements{std::move(placements)}
        , m_recognizerSources{std::move(recognizerSources)}
        , m_regressions{std::move(regressions)}
    {
    }

    auto AuthoringDocument::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<AuthoringSource> sources,
        std::vector<Element> elements,
        std::vector<PageSignature> pages,
        std::vector<AuthoringPlacement> placements,
        std::vector<RegressionCase> regressions
    ) -> Result<AuthoringDocument>
    {
        if (
            sources.size() > k_maximumAuthoringResources
            || elements.size() > k_maximumAuthoringResources
            || pages.size() > k_maximumAuthoringResources
            || placements.size() > k_maximumAuthoringPlacements
            || regressions.size() > k_maximumAuthoringResources
        )
        {
            return invalidAuthoring(
                "authoring document exceeds a source, element, page, placement, or regression quota"
            );
        }

        std::ranges::sort(
            sources,
            {},
            [](AuthoringSource const& source) -> ResourceId
            {
                return source.id().value();
            }
        );
        for (auto index = std::size_t{0}; index < sources.size(); ++index)
        {
            auto const& source = checkedAt(sources, index);
            if (source.fingerprint() != fingerprint)
            {
                return invalidAuthoring(
                    "every authoring source must match the project fingerprint"
                );
            }
            if (
                index != 0U
                && checkedAt(sources, index - 1U).id() == source.id()
            )
            {
                return invalidAuthoring("authoring source IDs must be unique");
            }
        }

        auto findSource = [&sources](SourceId id) noexcept -> AuthoringSource const*
        {
            auto const found = std::ranges::find(sources, id, &AuthoringSource::id);
            return found == sources.end() ? nullptr : &*found;
        };

        std::ranges::sort(
            elements,
            {},
            [](Element const& element) -> ResourceId
            {
                return element.id().value();
            }
        );
        for (auto const& element : elements)
        {
            if (findSource(element.sourceId()) == nullptr)
            {
                return invalidAuthoring(
                    "authoring element references an unknown source"
                );
            }
        }

        auto findElement = [&elements](RecognizerId id) noexcept -> Element const*
        {
            auto const found = std::ranges::find(elements, id, &Element::id);
            return found == elements.end() ? nullptr : &*found;
        };
        auto findPage = [&pages](PageId id) noexcept -> PageSignature const*
        {
            auto const found = std::ranges::find(pages, id, &PageSignature::id);
            return found == pages.end() ? nullptr : &*found;
        };

        std::ranges::sort(placements, placementLess);
        for (auto index = std::size_t{0}; index < placements.size(); ++index)
        {
            auto const& placement = checkedAt(placements, index);
            auto const* p_element = findElement(placement.elementId);
            auto const* p_page    = findPage(placement.pageId);
            if (p_element == nullptr)
            {
                return invalidAuthoring(
                    "authoring placement references an unknown element"
                );
            }
            if (p_page == nullptr)
            {
                return invalidAuthoring(
                    "authoring placement references an unknown page"
                );
            }
            if (
                placement.searchRoi.right() > fingerprint.width()
                || placement.searchRoi.bottom() > fingerprint.height()
            )
            {
                return invalidAuthoring(
                    "authoring placement search_roi must fit the project resolution"
                );
            }
            if (
                p_element->templateRect().width() > placement.searchRoi.width()
                || p_element->templateRect().height() > placement.searchRoi.height()
            )
            {
                return invalidAuthoring(
                    "authoring placement search_roi is smaller than the element template"
                );
            }
            if (
                index != 0U
                && checkedAt(placements, index - 1U).pageId == placement.pageId
                && checkedAt(placements, index - 1U).elementId == placement.elementId
            )
            {
                return invalidAuthoring(
                    "authoring page places the same element twice"
                );
            }
        }

        // A placement authorizes something the runtime may act on or read; a page
        // anchor is identity evidence and joins a page through its signature.
        for (auto const& placement : placements)
        {
            auto const* p_element = findElement(placement.elementId);
            UF_CHECK(p_element != nullptr);
            if (p_element->annotationType() == AnnotationType::PageAnchor)
            {
                return invalidAuthoring(
                    std::format(
                        "\"{}\" is a page anchor and cannot be placed; anchors join a page through its signature",
                        p_element->name().value()
                    )
                );
            }
        }

        // An element in a page's signature is an identity mark; a placement is an
        // interactive or info element the page carries. The same element cannot
        // be both on one page.
        for (auto const& placement : placements)
        {
            auto const* p_page = findPage(placement.pageId);
            UF_CHECK(p_page != nullptr);
            auto const inSignature = (
                std::ranges::contains(p_page->required(), placement.elementId)
                || std::ranges::contains(p_page->forbidden(), placement.elementId)
            );
            if (inSignature)
            {
                auto const* p_element = findElement(placement.elementId);
                UF_CHECK(p_element != nullptr);
                return invalidAuthoring(
                    std::format(
                        "\"{}\" is both a signature member and a placement on page \"{}\"",
                        p_element->name().value(),
                        p_page->name().value()
                    )
                );
            }
        }

        // The closure rule that replaces the old "action target must authorize a
        // page" field rule: an interactive element the runtime could click has to
        // appear somewhere it can be clicked.
        for (auto const& element : elements)
        {
            if (!std::holds_alternative<InteractiveElement>(element.kind()))
            {
                continue;
            }
            auto const placed = std::ranges::any_of(
                placements,
                [&element](AuthoringPlacement const& placement)
                {
                    return placement.elementId == element.id();
                }
            );
            if (!placed)
            {
                return invalidAuthoring(
                    std::format(
                        "interactive element \"{}\" must be placed on at least one page",
                        element.name().value()
                    )
                );
            }
        }

        std::ranges::sort(
            regressions,
            {},
            [](RegressionCase const& regression) -> ResourceId
            {
                return regression.id().value();
            }
        );
        for (auto index = std::size_t{0}; index < regressions.size(); ++index)
        {
            auto const& regression = checkedAt(regressions, index);
            if (findSource(regression.sourceId()) == nullptr)
            {
                return invalidAuthoring(
                    "authoring regression references an unknown source"
                );
            }
            if (
                index != 0U
                && checkedAt(regressions, index - 1U).id() == regression.id()
            )
            {
                return invalidAuthoring("authoring regression IDs must be unique");
            }
        }

        UF_TRY_VALUE(
            derived,
            deriveModel(
                std::move(projectId),
                fingerprint,
                elements,
                placements,
                std::move(pages)
            )
        );
        auto const& catalog = derived.catalog;
        for (auto const& regression : regressions)
        {
            auto const* p_resolved = std::get_if<ResolvedRegression>(
                &regression.expectation()
            );
            if (
                p_resolved != nullptr
                && catalog.findPage(p_resolved->pageId) == nullptr
            )
            {
                return invalidAuthoring(
                    "resolved regression references an unknown page"
                );
            }
        }

        auto allIds = std::vector<std::string>{};
        allIds.reserve(
            sources.size()
            + elements.size()
            + catalog.pages().size()
            + regressions.size()
        );
        for (auto const& source : sources)
        {
            allIds.emplace_back(resourceIdText(source.id()));
        }
        for (auto const& element : elements)
        {
            allIds.emplace_back(resourceIdText(element.id()));
        }
        for (auto const& page : catalog.pages())
        {
            allIds.emplace_back(resourceIdText(page.id()));
        }
        for (auto const& regression : regressions)
        {
            allIds.emplace_back(resourceIdText(regression.id()));
        }
        std::ranges::sort(allIds);
        if (std::adjacent_find(allIds.begin(), allIds.end()) != allIds.end())
        {
            return invalidAuthoring(
                "authoring resource IDs must be globally unique"
            );
        }

        auto document = AuthoringDocument{
            std::move(derived.catalog),
            std::move(sources),
            std::move(elements),
            std::move(placements),
            std::move(derived.recognizerSources),
            std::move(regressions)
        };
        if (serializeAuthoringDocument(document).size() > k_maximumAuthoringDocumentBytes)
        {
            return invalidAuthoring(
                "authoring document exceeds the 16 MiB serialized quota"
            );
        }
        return document;
    }

    auto AuthoringDocument::catalog() const noexcept -> RecognitionCatalog const&
    {
        return m_catalog;
    }
    auto AuthoringDocument::sources() const noexcept -> std::span<AuthoringSource const>
    {
        return m_sources;
    }
    auto AuthoringDocument::elements() const noexcept -> std::span<Element const>
    {
        return m_elements;
    }
    auto AuthoringDocument::placements() const noexcept
        -> std::span<AuthoringPlacement const>
    {
        return m_placements;
    }
    auto AuthoringDocument::recognizerSources() const noexcept
        -> std::span<AuthoringRecognizerSource const>
    {
        return m_recognizerSources;
    }
    auto AuthoringDocument::regressions() const noexcept -> std::span<RegressionCase const>
    {
        return m_regressions;
    }
    auto AuthoringDocument::findSource(
        SourceId id
    ) const noexcept -> AuthoringSource const*
    {
        auto const found = std::ranges::find(m_sources, id, &AuthoringSource::id);
        return found == m_sources.end() ? nullptr : &*found;
    }
    auto AuthoringDocument::findElement(
        RecognizerId id
    ) const noexcept -> Element const*
    {
        auto const found = std::ranges::find(m_elements, id, &Element::id);
        return found == m_elements.end() ? nullptr : &*found;
    }
    auto serializeAuthoringDocument(
        AuthoringDocument const& document
    ) -> std::string
    {
        auto output = std::string{};
        detail::appendStringField(output, "schema", k_authoringDocumentSchema);
        detail::appendStringField(
            output,
            "project_id",
            document.catalog().projectId().value()
        );
        detail::appendFingerprintFields(
            output,
            document.catalog().fingerprint(),
            "base_resolution",
            "base_dpi"
        );

        for (auto const& source : document.sources())
        {
            output += "\n[[source]]\n";
            detail::appendStringField(output, "id", resourceIdText(source.id()));
            detail::appendStringField(output, "path", source.relativePath());
            detail::appendStringField(
                output,
                "content_hash",
                source.contentHash().toString()
            );
            detail::appendFingerprintFields(
                output,
                source.fingerprint(),
                "client_size",
                "dpi"
            );
            auto const* p_wgc = std::get_if<WgcSourceProvenance>(
                &source.provenance()
            );
            if (p_wgc != nullptr)
            {
                detail::appendStringField(output, "capture_backend", "wgc");
                output += "target_generation = ";
                output += std::to_string(p_wgc->targetGeneration.value());
                output.push_back('\n');
                detail::appendStringField(
                    output,
                    "captured_at",
                    p_wgc->capturedAt
                );
            }
            else
            {
                detail::appendStringField(
                    output,
                    "capture_backend",
                    "imported"
                );
            }
        }

        // Elements no longer carry page membership; that is a page-side fact,
        // serialized as flat [[placement]] tables below.
        for (auto const& element : document.elements())
        {
            output += "\n[[annotation]]\n";
            detail::appendStringField(
                output,
                "id",
                resourceIdText(element.id())
            );
            detail::appendStringField(output, "name", element.name().value());
            detail::appendStringField(
                output,
                "type",
                detail::annotationTypeText(element.annotationType())
            );
            detail::appendStringField(
                output,
                "source_id",
                resourceIdText(element.sourceId())
            );
            detail::appendStringField(
                output,
                "recognizer_kind",
                "gray_template"
            );
            detail::appendRectField(output, "template_rect", element.templateRect());
            detail::appendRectField(output, "search_roi", element.searchRoi());
            output += "min_similarity_bp = ";
            output += std::to_string(element.threshold().basisPoints());
            output.push_back('\n');
            if (
                auto const* p_interactive = std::get_if<InteractiveElement>(
                    &element.kind()
                )
            )
            {
                if (auto const click = p_interactive->clickOffset)
                {
                    output += "default_click = ";
                    auto const values = std::array{click->x(), click->y()};
                    detail::appendUnsigned32Array(output, values);
                    output.push_back('\n');
                }
            }
            // Written only when set, so a project that marks nothing reusable
            // serializes exactly as it did before the field existed.
            if (element.shared())
            {
                detail::appendBoolField(output, "shared", true);
            }
        }

        for (auto const& page : document.catalog().pages())
        {
            output += "\n[[page]]\n";
            detail::appendStringField(output, "id", resourceIdText(page.id()));
            detail::appendStringField(output, "name", page.name().value());
            output += "required = ";
            detail::appendIdArray(output, page.required());
            output.push_back('\n');
            output += "forbidden = ";
            detail::appendIdArray(output, page.forbidden());
            output.push_back('\n');
        }

        // Placements are canonically ordered by page then element, mirroring the
        // id sorts elsewhere. A flat table keeps them inside the reader's
        // one-field-per-line grammar, which has no inline or nested tables.
        for (auto const& placement : document.placements())
        {
            output += "\n[[placement]]\n";
            detail::appendStringField(
                output,
                "page_id",
                resourceIdText(placement.pageId)
            );
            detail::appendStringField(
                output,
                "element_id",
                resourceIdText(placement.elementId)
            );
            detail::appendRectField(output, "search_roi", placement.searchRoi);
        }

        for (auto const& regression : document.regressions())
        {
            output += "\n[[regression]]\n";
            detail::appendStringField(
                output,
                "id",
                resourceIdText(regression.id())
            );
            detail::appendStringField(
                output,
                "source_id",
                resourceIdText(regression.sourceId())
            );
            detail::appendStringField(
                output,
                "classification",
                classificationText(regression.classification())
            );
            if (
                auto const* p_resolved = std::get_if<ResolvedRegression>(
                    &regression.expectation()
                )
            )
            {
                detail::appendStringField(
                    output,
                    "expected_outcome",
                    "resolved"
                );
                detail::appendStringField(
                    output,
                    "expected_page_id",
                    resourceIdText(p_resolved->pageId)
                );
            }
            else if (
                std::holds_alternative<UnknownRegression>(
                    regression.expectation()
                )
            )
            {
                detail::appendStringField(
                    output,
                    "expected_outcome",
                    "unknown"
                );
            }
            else
            {
                detail::appendStringField(
                    output,
                    "expected_outcome",
                    "ambiguous"
                );
            }
        }
        return output;
    }

    auto parseAuthoringDocument(
        std::string_view canonicalToml
    ) -> Result<AuthoringDocument>
    {
        if (
            canonicalToml.empty()
            || canonicalToml.size() > k_maximumAuthoringDocumentBytes
        )
        {
            return invalidAuthoring(
                "authoring document is empty or exceeds the 16 MiB quota"
            );
        }

        auto reader = detail::CanonicalTomlReader{
            "authoring document",
            std::string{canonicalToml}
        };
        UF_TRY_VALUE(schema, reader.takeStringField("schema"));
        if (schema != k_authoringDocumentSchema)
        {
            return invalidAuthoring(
                std::format("unsupported authoring document schema '{}'", schema)
            );
        }
        UF_TRY_VALUE(projectIdText, reader.takeStringField("project_id"));
        UF_TRY_VALUE(projectId, ProjectId::create(std::move(projectIdText)));
        UF_TRY_VALUE(
            fingerprint,
            detail::parseFingerprintFields(
                reader,
                "base_resolution",
                "base_dpi"
            )
        );

        auto sources     = std::vector<AuthoringSource>{};
        auto elements    = std::vector<Element>{};
        auto pages       = std::vector<PageSignature>{};
        auto placements  = std::vector<AuthoringPlacement>{};
        auto regressions = std::vector<RegressionCase>{};
        auto section     = uint8{0};
        while (!reader.eof())
        {
            UF_TRY(reader.expect(""));
            auto const headerLine = reader.line();
            UF_TRY_VALUE(header, reader.take());
            auto rank = uint8{0};
            if (header == "[[source]]")
            {
                rank = 1;
                if (sources.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring source quota exceeded");
                }
                UF_TRY_VALUE(source, parseSource(reader));
                sources.emplace_back(std::move(source));
            }
            else if (header == "[[annotation]]")
            {
                rank = 2;
                if (elements.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring annotation quota exceeded");
                }
                UF_TRY_VALUE(element, parseElement(reader, fingerprint));
                elements.emplace_back(std::move(element));
            }
            else if (header == "[[page]]")
            {
                rank = 3;
                if (pages.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring page quota exceeded");
                }
                UF_TRY_VALUE(page, parsePage(reader));
                pages.emplace_back(std::move(page));
            }
            else if (header == "[[placement]]")
            {
                rank = 4;
                if (placements.size() >= k_maximumAuthoringPlacements)
                {
                    return invalidAuthoring("authoring placement quota exceeded");
                }
                UF_TRY_VALUE(placement, parsePlacement(reader));
                placements.emplace_back(placement);
            }
            else if (header == "[[regression]]")
            {
                rank = 5;
                if (regressions.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring regression quota exceeded");
                }
                UF_TRY_VALUE(regression, parseRegression(reader));
                regressions.emplace_back(regression);
            }
            else
            {
                return invalidAuthoring(
                    std::format(
                        "authoring document line {} has unknown table header '{}'",
                        headerLine,
                        header
                    )
                );
            }
            if (rank < section)
            {
                return invalidAuthoring(
                    "authoring source, annotation, page, placement, and regression tables are out of order"
                );
            }
            section = rank;
        }

        UF_TRY_VALUE(
            document,
            AuthoringDocument::create(
                std::move(projectId),
                fingerprint,
                std::move(sources),
                std::move(elements),
                std::move(pages),
                std::move(placements),
                std::move(regressions)
            )
        );
        if (serializeAuthoringDocument(document) != canonicalToml)
        {
            return invalidAuthoring(
                "authoring document is valid data but not canonical GUI output"
            );
        }
        return document;
    }
}
