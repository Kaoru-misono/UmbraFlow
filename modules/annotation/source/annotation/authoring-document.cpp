#include "authoring-document.hpp"

#include "detail/canonical-toml.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>
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

        template <typename Id>
        [[nodiscard]]
        auto parseId(std::string_view value) -> Result<Id>
        {
            UF_TRY_VALUE(resourceId, ResourceId::parse(value));
            return Id{resourceId};
        }

        template <typename Id>
        [[nodiscard]]
        auto parseIds(
            std::vector<std::string> const& encoded
        ) -> Result<std::vector<Id>>
        {
            auto ids = std::vector<Id>{};
            ids.reserve(encoded.size());
            for (auto const& value : encoded)
            {
                UF_TRY_VALUE(id, parseId<Id>(value));
                ids.emplace_back(id);
            }
            return ids;
        }

        [[nodiscard]]
        auto parseFingerprint(
            detail::CanonicalTomlReader& reader,
            std::string_view resolutionKey,
            std::string_view dpiKey
        ) -> Result<ProjectFingerprint>
        {
            UF_TRY_VALUE(
                resolution,
                reader.takeUnsigned32ArrayField(resolutionKey)
            );
            UF_TRY_VALUE(dpi, reader.takeUnsigned32ArrayField(dpiKey));
            if (resolution.size() != 2U || dpi.size() != 2U)
            {
                return invalidAuthoring(
                    std::format(
                        "authoring document '{}' and '{}' must have two integers",
                        resolutionKey,
                        dpiKey
                    )
                );
            }
            return ProjectFingerprint::create(
                checkedAt(resolution, 0),
                checkedAt(resolution, 1),
                checkedAt(dpi, 0),
                checkedAt(dpi, 1)
            );
        }

        [[nodiscard]]
        auto parsePixelRectField(
            detail::CanonicalTomlReader& reader,
            std::string_view key
        ) -> Result<PixelRect>
        {
            UF_TRY_VALUE(values, reader.takeUnsigned32ArrayField(key));
            if (values.size() != 4U)
            {
                return invalidAuthoring(
                    std::format(
                        "authoring document '{}' must have four integers",
                        key
                    )
                );
            }
            auto const rect = PixelRect::create(
                checkedAt(values, 0),
                checkedAt(values, 1),
                checkedAt(values, 2),
                checkedAt(values, 3)
            );
            if (!rect)
            {
                return invalidAuthoring(
                    std::format(
                        "authoring document '{}' is not a valid rectangle",
                        key
                    )
                );
            }
            return *rect;
        }

        [[nodiscard]]
        auto parseAnnotationType(
            std::string_view value
        ) -> Result<AnnotationType>
        {
            if (value == "page_anchor")
            {
                return AnnotationType::PageAnchor;
            }
            if (value == "action_target")
            {
                return AnnotationType::ActionTarget;
            }
            if (value == "info_region")
            {
                return AnnotationType::InfoRegion;
            }
            return invalidAuthoring(
                std::format("unknown authoring annotation type '{}'", value)
            );
        }

        [[nodiscard]]
        auto parseSource(
            detail::CanonicalTomlReader& reader
        ) -> Result<AuthoringSource>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, parseId<SourceId>(idText));
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
                parseFingerprint(reader, "client_size", "dpi")
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
                    .m_targetGeneration = TargetGeneration::fromValue(generation),
                    .m_capturedAt       = std::move(capturedAt),
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
                    .m_id          = id,
                    .m_contentHash = contentHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = std::move(provenance),
                }
            );
        }

        [[nodiscard]]
        auto parseRecognizer(
            detail::CanonicalTomlReader& reader,
            ProjectFingerprint fingerprint
        ) -> Result<AuthoringRecognizerSpec>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, parseId<RecognizerId>(idText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(typeText, reader.takeStringField("type"));
            UF_TRY_VALUE(annotationType, parseAnnotationType(typeText));
            UF_TRY_VALUE(sourceIdText, reader.takeStringField("source_id"));
            UF_TRY_VALUE(sourceId, parseId<SourceId>(sourceIdText));
            UF_TRY_VALUE(kind, reader.takeStringField("recognizer_kind"));
            if (kind != "gray_template")
            {
                return invalidAuthoring(
                    "authoring P0 recognizer kind must be gray_template"
                );
            }
            UF_TRY_VALUE(
                templateRect,
                parsePixelRectField(reader, "template_rect")
            );
            UF_TRY_VALUE(
                searchRoi,
                parsePixelRectField(reader, "search_roi")
            );
            UF_TRY_VALUE(
                thresholdValue,
                reader.takeUnsigned32Field("min_similarity_bp")
            );
            UF_TRY_VALUE(
                threshold,
                SimilarityThreshold::create(thresholdValue)
            );

            auto defaultClick = std::optional<TemplateOffset>{};
            UF_TRY_VALUE(
                hasDefaultClick,
                reader.nextIsField("default_click")
            );
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
                defaultClick = offset;
            }

            auto pageIds = std::vector<PageId>{};
            UF_TRY_VALUE(hasPageIds, reader.nextIsField("page_ids"));
            if (hasPageIds)
            {
                UF_TRY_VALUE(
                    encoded,
                    reader.takeStringArrayField("page_ids")
                );
                UF_TRY_VALUE(parsed, parseIds<PageId>(encoded));
                pageIds = std::move(parsed);
            }

            UF_TRY_VALUE(
                definition,
                RecognizerDefinition::create(
                    fingerprint,
                    RecognizerSpec{
                        .m_id             = id,
                        .m_name           = std::move(name),
                        .m_annotationType = annotationType,
                        .m_templateRect   = templateRect,
                        .m_searchRoi      = searchRoi,
                        .m_threshold      = threshold,
                        .m_defaultClick   = defaultClick,
                        .m_allowedPageIds = std::move(pageIds),
                    }
                )
            );
            return AuthoringRecognizerSpec{
                .m_definition = std::move(definition),
                .m_sourceId   = sourceId,
            };
        }

        [[nodiscard]]
        auto parsePage(
            detail::CanonicalTomlReader& reader
        ) -> Result<PageSignature>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, parseId<PageId>(idText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(
                requiredText,
                reader.takeStringArrayField("required")
            );
            UF_TRY_VALUE(
                required,
                parseIds<RecognizerId>(requiredText)
            );
            UF_TRY_VALUE(
                forbiddenText,
                reader.takeStringArrayField("forbidden")
            );
            UF_TRY_VALUE(
                forbidden,
                parseIds<RecognizerId>(forbiddenText)
            );
            return PageSignature::create(
                PageSpec{
                    .m_id        = id,
                    .m_name      = std::move(name),
                    .m_required  = std::move(required),
                    .m_forbidden = std::move(forbidden),
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
            UF_TRY_VALUE(id, parseId<RegressionId>(idText));
            UF_TRY_VALUE(sourceIdText, reader.takeStringField("source_id"));
            UF_TRY_VALUE(sourceId, parseId<SourceId>(sourceIdText));
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
                UF_TRY_VALUE(pageId, parseId<PageId>(pageIdText));
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
                    .m_id             = id,
                    .m_sourceId       = sourceId,
                    .m_classification = classification,
                    .m_expectation    = expectation,
                }
            };
        }

        [[nodiscard]]
        auto annotationTypeText(AnnotationType type) noexcept -> std::string_view
        {
            switch (type)
            {
            case AnnotationType::PageAnchor:
                return "page_anchor";
            case AnnotationType::ActionTarget:
                return "action_target";
            case AnnotationType::InfoRegion:
                return "info_region";
            }
            UF_UNREACHABLE_MSG("unknown annotation type");
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

        auto appendFingerprintFields(
            std::string& output,
            ProjectFingerprint fingerprint,
            std::string_view resolutionKey,
            std::string_view dpiKey
        ) -> void
        {
            output += resolutionKey;
            output += " = ";
            auto const resolution = std::array{
                fingerprint.width(),
                fingerprint.height(),
            };
            detail::appendUnsigned32Array(output, resolution);
            output.push_back('\n');
            output += dpiKey;
            output += " = ";
            auto const dpi = std::array{
                fingerprint.dpiX(),
                fingerprint.dpiY(),
            };
            detail::appendUnsigned32Array(output, dpi);
            output.push_back('\n');
        }

        auto appendRectField(
            std::string& output,
            std::string_view key,
            PixelRect rect
        ) -> void
        {
            output += key;
            output += " = ";
            auto const values = std::array{
                rect.x(),
                rect.y(),
                rect.width(),
                rect.height(),
            };
            detail::appendUnsigned32Array(output, values);
            output.push_back('\n');
        }

        template <typename Id>
        auto appendIdArray(
            std::string& output,
            std::span<Id const> ids
        ) -> void
        {
            output.push_back('[');
            for (auto index = std::size_t{0}; index < ids.size(); ++index)
            {
                if (index != 0U)
                {
                    output += ", ";
                }
                detail::appendTomlString(
                    output,
                    checkedAt(ids, index).value().toString()
                );
            }
            output.push_back(']');
        }

        template <typename Id>
        [[nodiscard]]
        auto resourceIdText(Id id) -> std::string
        {
            return id.value().toString();
        }
    }

    AuthoringSource::AuthoringSource(AuthoringSourceSpec const& spec)
        : m_id{spec.m_id}
        , m_contentHash{spec.m_contentHash}
        , m_relativePath{sourcePath(m_contentHash)}
        , m_fingerprint{spec.m_fingerprint}
        , m_provenance{spec.m_provenance}
    {
    }

    auto AuthoringSource::create(
        AuthoringSourceSpec const& spec
    ) -> Result<AuthoringSource>
    {
        auto const* p_wgc = std::get_if<WgcSourceProvenance>(
            &spec.m_provenance
        );
        if (
            p_wgc != nullptr
            && !isCanonicalRfc3339(p_wgc->m_capturedAt)
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
        : m_id{spec.m_id}
        , m_sourceId{spec.m_sourceId}
        , m_classification{spec.m_classification}
        , m_expectation{spec.m_expectation}
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
        std::vector<AuthoringRecognizerSource>&& recognizerSources,
        std::vector<RegressionCase>&& regressions
    ) noexcept
        : m_catalog{std::move(catalog)}
        , m_sources{std::move(sources)}
        , m_recognizerSources{std::move(recognizerSources)}
        , m_regressions{std::move(regressions)}
    {
    }

    auto AuthoringDocument::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<AuthoringSource> sources,
        std::vector<AuthoringRecognizerSpec> recognizers,
        std::vector<PageSignature> pages,
        std::vector<RegressionCase> regressions
    ) -> Result<AuthoringDocument>
    {
        if (
            sources.size() > k_maximumAuthoringResources
            || recognizers.size() > k_maximumAuthoringResources
            || pages.size() > k_maximumAuthoringResources
            || regressions.size() > k_maximumAuthoringResources
        )
        {
            return invalidAuthoring(
                "authoring document exceeds a source, annotation, page, or regression quota"
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
            recognizers,
            {},
            [](AuthoringRecognizerSpec const& recognizer) -> ResourceId
            {
                return recognizer.m_definition.id().value();
            }
        );
        auto definitions       = std::vector<RecognizerDefinition>{};
        auto recognizerSources = std::vector<AuthoringRecognizerSource>{};
        definitions.reserve(recognizers.size());
        recognizerSources.reserve(recognizers.size());
        for (auto& recognizer : recognizers)
        {
            if (findSource(recognizer.m_sourceId) == nullptr)
            {
                return invalidAuthoring(
                    "authoring annotation references an unknown source"
                );
            }
            auto const recognizerId = recognizer.m_definition.id();
            recognizerSources.emplace_back(
                AuthoringRecognizerSource{
                    .m_recognizerId = recognizerId,
                    .m_sourceId     = recognizer.m_sourceId,
                }
            );
            definitions.emplace_back(std::move(recognizer.m_definition));
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
            catalog,
            RecognitionCatalog::create(
                std::move(projectId),
                fingerprint,
                std::move(definitions),
                std::move(pages)
            )
        );
        for (auto const& regression : regressions)
        {
            auto const* p_resolved = std::get_if<ResolvedRegression>(
                &regression.expectation()
            );
            if (
                p_resolved != nullptr
                && catalog.findPage(p_resolved->m_pageId) == nullptr
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
            + catalog.recognizers().size()
            + catalog.pages().size()
            + regressions.size()
        );
        for (auto const& source : sources)
        {
            allIds.emplace_back(resourceIdText(source.id()));
        }
        for (auto const& recognizer : catalog.recognizers())
        {
            allIds.emplace_back(resourceIdText(recognizer.id()));
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
            std::move(catalog),
            std::move(sources),
            std::move(recognizerSources),
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
        appendFingerprintFields(
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
            appendFingerprintFields(
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
                output += std::to_string(p_wgc->m_targetGeneration.value());
                output.push_back('\n');
                detail::appendStringField(
                    output,
                    "captured_at",
                    p_wgc->m_capturedAt
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

        auto const recognizers       = document.catalog().recognizers();
        auto const recognizerSources = document.recognizerSources();
        UF_CHECK_MSG(
            recognizers.size() == recognizerSources.size(),
            "authoring recognizer source closure is inconsistent"
        );
        for (auto index = std::size_t{0}; index < recognizers.size(); ++index)
        {
            auto const& recognizer   = checkedAt(recognizers, index);
            auto const& relationship = checkedAt(recognizerSources, index);
            UF_CHECK_MSG(
                relationship.m_recognizerId == recognizer.id(),
                "authoring recognizer source order is inconsistent"
            );
            output += "\n[[annotation]]\n";
            detail::appendStringField(
                output,
                "id",
                resourceIdText(recognizer.id())
            );
            detail::appendStringField(output, "name", recognizer.name().value());
            detail::appendStringField(
                output,
                "type",
                annotationTypeText(recognizer.annotationType())
            );
            detail::appendStringField(
                output,
                "source_id",
                resourceIdText(relationship.m_sourceId)
            );
            detail::appendStringField(
                output,
                "recognizer_kind",
                "gray_template"
            );
            appendRectField(output, "template_rect", recognizer.templateRect());
            appendRectField(output, "search_roi", recognizer.searchRoi());
            output += "min_similarity_bp = ";
            output += std::to_string(recognizer.threshold().basisPoints());
            output.push_back('\n');
            if (auto const click = recognizer.defaultClick())
            {
                output += "default_click = ";
                auto const values = std::array{click->x(), click->y()};
                detail::appendUnsigned32Array(output, values);
                output.push_back('\n');
            }
            if (!recognizer.allowedPageIds().empty())
            {
                output += "page_ids = ";
                appendIdArray(output, recognizer.allowedPageIds());
                output.push_back('\n');
            }
        }

        for (auto const& page : document.catalog().pages())
        {
            output += "\n[[page]]\n";
            detail::appendStringField(output, "id", resourceIdText(page.id()));
            detail::appendStringField(output, "name", page.name().value());
            output += "required = ";
            appendIdArray(output, page.required());
            output.push_back('\n');
            output += "forbidden = ";
            appendIdArray(output, page.forbidden());
            output.push_back('\n');
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
                    resourceIdText(p_resolved->m_pageId)
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
            parseFingerprint(reader, "base_resolution", "base_dpi")
        );

        auto sources     = std::vector<AuthoringSource>{};
        auto recognizers = std::vector<AuthoringRecognizerSpec>{};
        auto pages       = std::vector<PageSignature>{};
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
                if (recognizers.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring annotation quota exceeded");
                }
                UF_TRY_VALUE(
                    recognizer,
                    parseRecognizer(reader, fingerprint)
                );
                recognizers.emplace_back(std::move(recognizer));
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
            else if (header == "[[regression]]")
            {
                rank = 4;
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
                    "authoring source, annotation, page, and regression tables are out of order"
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
                std::move(recognizers),
                std::move(pages),
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
