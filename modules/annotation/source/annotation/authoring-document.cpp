#include "authoring-document.hpp"

#include "detail/annotation-fields.hpp"
#include "detail/canonical-toml.hpp"
#include "resource.hpp"

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
        constexpr auto k_maximumAuthoringAppearances      = std::size_t{4096} * 8U;
        constexpr auto k_maximumAuthoringReferences    = std::size_t{4096} * 16U;

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
        ) -> Result<PageSpec>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, detail::parseId<PageId>(idText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            return PageSpec{
                .id   = id,
                .name = std::move(name),
            };
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

        // An element as its own table row leaves it: the appearances that carry
        // its templates arrive in later rows, and the click offset cannot
        // become a TemplateOffset until one of them is known.
        struct ParsedElement final
        {
            ElementId                id;
            ResourceName             name;
            PixelRect                searchRoi;
            detail::CapabilityFields capabilities;

            std::vector<Appearance> appearances{};
        };

        struct ParsedAppearance final
        {
            ElementId  elementId;
            Appearance appearance;
        };

        [[nodiscard]]
        auto parseElement(
            detail::CanonicalTomlReader& reader
        ) -> Result<ParsedElement>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(id, detail::parseId<ElementId>(idText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(
                searchRoi,
                detail::parsePixelRectField(reader, "search_roi")
            );
            UF_TRY_VALUE(capabilities, detail::parseCapabilityFields(reader));
            return ParsedElement{
                .id           = id,
                .name         = std::move(name),
                .searchRoi    = searchRoi,
                .capabilities = capabilities,
            };
        }

        [[nodiscard]]
        auto parseAppearance(
            detail::CanonicalTomlReader& reader
        ) -> Result<ParsedAppearance>
        {
            UF_TRY_VALUE(elementIdText, reader.takeStringField("element_id"));
            UF_TRY_VALUE(elementId, detail::parseId<ElementId>(elementIdText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(sourceIdText, reader.takeStringField("source_id"));
            UF_TRY_VALUE(sourceId, detail::parseId<SourceId>(sourceIdText));
            UF_TRY_VALUE(kindText, reader.takeStringField("element_kind"));
            if (kindText != "gray_template")
            {
                return invalidAuthoring(
                    "authoring P0 element kind must be gray_template"
                );
            }
            UF_TRY_VALUE(
                templateRect,
                detail::parsePixelRectField(reader, "template_rect")
            );
            UF_TRY_VALUE(
                thresholdValue,
                reader.takeUnsigned32Field("min_similarity_bp")
            );
            UF_TRY_VALUE(threshold, SimilarityThreshold::create(thresholdValue));

            // The key and its tolerance are one fact in two lines: the array is
            // optional, and once it is there the tolerance is not, so a
            // half-written key fails rather than defaulting to something the
            // author never chose.
            auto colourKey = std::optional<ColourKey>{};
            UF_TRY_VALUE(hasColourKey, reader.nextIsField("colour_key"));
            if (hasColourKey)
            {
                UF_TRY_VALUE(
                    channels,
                    reader.takeUnsigned32ArrayField("colour_key")
                );
                if (channels.size() != 3U)
                {
                    return invalidAuthoring(
                        "authoring colour_key must have three channel values"
                    );
                }
                UF_TRY_VALUE(
                    tolerance,
                    reader.takeUnsigned32Field("colour_key_tolerance")
                );
                UF_TRY_VALUE(
                    key,
                    ColourKey::create(
                        checkedAt(channels, 0),
                        checkedAt(channels, 1),
                        checkedAt(channels, 2),
                        tolerance
                    )
                );
                colourKey = key;
            }

            UF_TRY_VALUE(
                appearance,
                Appearance::create(
                    Appearance::Spec{
                        .name         = std::move(name),
                        .sourceId     = sourceId,
                        .templateRect = templateRect,
                        .threshold    = threshold,
                        .colourKey    = colourKey,
                    }
                )
            );
            return ParsedAppearance{
                .elementId  = elementId,
                .appearance = std::move(appearance),
            };
        }

        [[nodiscard]]
        auto parseReference(
            detail::CanonicalTomlReader& reader
        ) -> Result<PageReference>
        {
            UF_TRY_VALUE(pageIdText, reader.takeStringField("page_id"));
            UF_TRY_VALUE(pageId, detail::parseId<PageId>(pageIdText));
            UF_TRY_VALUE(elementIdText, reader.takeStringField("element_id"));
            UF_TRY_VALUE(elementId, detail::parseId<ElementId>(elementIdText));
            UF_TRY_VALUE(holdingName, reader.takeStringField("holding"));
            auto const holding = detail::holdingFromText(holdingName);
            if (!holding)
            {
                return invalidAuthoring(
                    std::format("unknown page reference holding '{}'", holdingName)
                );
            }
            UF_TRY_VALUE(exercised, detail::parseExercisedFields(reader));

            auto searchRoi = std::optional<PixelRect>{};
            UF_TRY_VALUE(hasSearchRoi, reader.nextIsField("search_roi"));
            if (hasSearchRoi)
            {
                UF_TRY_VALUE(
                    refined,
                    detail::parsePixelRectField(reader, "search_roi")
                );
                searchRoi = refined;
            }

            auto appearance = std::optional<ResourceName>{};
            UF_TRY_VALUE(hasAppearance, reader.nextIsField("appearance"));
            if (hasAppearance)
            {
                UF_TRY_VALUE(appearanceText, reader.takeStringField("appearance"));
                UF_TRY_VALUE(name, ResourceName::create(std::move(appearanceText)));
                appearance = std::move(name);
            }

            return PageReference{
                .pageId     = pageId,
                .elementId  = elementId,
                .holding    = *holding,
                .exercised  = exercised,
                .searchRoi  = searchRoi,
                .appearance = std::move(appearance),
            };
        }

        [[nodiscard]]
        auto deriveCatalog(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::span<Element const> elements,
            std::vector<PageSpec> pages,
            std::vector<PageReference> references
        ) -> Result<RecognitionCatalog>
        {
            auto definitions = std::vector<CompiledElement>{};
            definitions.reserve(elements.size());
            for (auto const& element : elements)
            {
                auto appearances = std::vector<CompiledAppearance>{};
                appearances.reserve(element.appearances().size());
                for (auto const& appearance : element.appearances())
                {
                    appearances.emplace_back(runtimeAppearanceOf(appearance));
                }
                UF_TRY_VALUE(
                    definition,
                    CompiledElement::create(
                        fingerprint,
                        CompiledElementSpec{
                            .id           = element.id(),
                            .name         = element.name(),
                            .capabilities = element.capabilities(),
                            .searchRoi    = element.searchRoi(),
                            .appearances  = std::move(appearances),
                        }
                    )
                );
                definitions.emplace_back(std::move(definition));
            }

            return RecognitionCatalog::create(
                std::move(projectId),
                fingerprint,
                std::move(definitions),
                std::move(pages),
                std::move(references)
            );
        }
    }

    auto runtimeAppearanceOf(Appearance const& appearance) -> CompiledAppearance
    {
        return CompiledAppearance{
            .name         = appearance.name(),
            .templateRect = appearance.templateRect(),
            .threshold    = appearance.threshold(),
        };
    }

    Element::Element(Spec spec) noexcept
        : m_id{spec.id}
        , m_name{std::move(spec.name)}
        , m_capabilities{spec.capabilities}
        , m_searchRoi{spec.searchRoi}
        , m_appearances{std::move(spec.appearances)}
    {
    }

    auto Element::create(
        ProjectFingerprint fingerprint,
        Spec const& spec
    ) -> Result<Element>
    {
        auto runtimeAppearances = std::vector<CompiledAppearance>{};
        runtimeAppearances.reserve(spec.appearances.size());
        for (auto const& appearance : spec.appearances)
        {
            runtimeAppearances.emplace_back(runtimeAppearanceOf(appearance));
        }
        UF_TRY(
            validateElementShape(
                fingerprint,
                spec.searchRoi,
                runtimeAppearances,
                spec.capabilities
            )
        );

        return Element{spec};
    }

    auto Element::id() const -> ElementId { return m_id; }
    auto Element::name() const -> ResourceName { return m_name; }
    auto Element::capabilities() const noexcept -> ElementCapabilities const&
    {
        return m_capabilities;
    }
    auto Element::searchRoi() const noexcept -> PixelRect { return m_searchRoi; }
    auto Element::appearances() const noexcept -> std::span<Appearance const>
    {
        return m_appearances;
    }
    auto Element::findAppearance(
        ResourceName const& name
    ) const noexcept -> Appearance const*
    {
        auto const found = std::ranges::find(m_appearances, name, &Appearance::name);
        return found == m_appearances.end() ? nullptr : &*found;
    }

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
        std::vector<RegressionCase>&& regressions
    ) noexcept
        : m_catalog{std::move(catalog)}
        , m_sources{std::move(sources)}
        , m_elements{std::move(elements)}
        , m_regressions{std::move(regressions)}
    {
    }

    auto AuthoringDocument::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<AuthoringSource> sources,
        std::vector<Element> elements,
        std::vector<PageSpec> pages,
        std::vector<PageReference> references,
        std::vector<RegressionCase> regressions
    ) -> Result<AuthoringDocument>
    {
        if (
            sources.size() > k_maximumAuthoringResources
            || elements.size() > k_maximumAuthoringResources
            || pages.size() > k_maximumAuthoringResources
            || references.size() > k_maximumAuthoringReferences
            || regressions.size() > k_maximumAuthoringResources
        )
        {
            return invalidAuthoring(
                "authoring document exceeds a source, element, page, reference, or regression quota"
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
        auto totalAppearances = std::size_t{0};
        for (auto const& element : elements)
        {
            totalAppearances += element.appearances().size();
            for (auto const& appearance : element.appearances())
            {
                if (findSource(appearance.sourceId()) == nullptr)
                {
                    return invalidAuthoring(
                        std::format(
                            "appearance \"{}\" of \"{}\" references an unknown source",
                            appearance.name().value(),
                            element.name().value()
                        )
                    );
                }
            }
        }
        if (totalAppearances > k_maximumAuthoringAppearances)
        {
            return invalidAuthoring("authoring document exceeds the appearance quota");
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

        // Every invariant that spans elements, pages, and references is the
        // catalog's, so the derived read model and the compiled runtime
        // manifest are held to one set of rules stated once.
        UF_TRY_VALUE(
            catalog,
            deriveCatalog(
                std::move(projectId),
                fingerprint,
                elements,
                std::move(pages),
                std::move(references)
            )
        );
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
            std::move(catalog),
            std::move(sources),
            std::move(elements),
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
    auto AuthoringDocument::references() const noexcept -> std::span<PageReference const>
    {
        return m_catalog.references();
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
        ElementId id
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

        for (auto const& element : document.elements())
        {
            output += "\n[[element]]\n";
            detail::appendStringField(
                output,
                "id",
                resourceIdText(element.id())
            );
            detail::appendStringField(output, "name", element.name().value());
            detail::appendRectField(output, "search_roi", element.searchRoi());
            detail::appendCapabilityFields(output, element.capabilities());
        }

        // Appearances are a flat table keyed back to their element, which keeps
        // them inside the reader's one-field-per-line grammar, and they are
        // written in declaration order because that order is the tie-break rule
        // and reordering them would change which appearance wins a draw.
        for (auto const& element : document.elements())
        {
            for (auto const& appearance : element.appearances())
            {
                output += "\n[[appearance]]\n";
                detail::appendStringField(
                    output,
                    "element_id",
                    resourceIdText(element.id())
                );
                detail::appendStringField(output, "name", appearance.name().value());
                detail::appendStringField(
                    output,
                    "source_id",
                    resourceIdText(appearance.sourceId())
                );
                detail::appendStringField(
                    output,
                    "element_kind",
                    "gray_template"
                );
                detail::appendRectField(
                    output,
                    "template_rect",
                    appearance.templateRect()
                );
                output += "min_similarity_bp = ";
                output += std::to_string(appearance.threshold().basisPoints());
                output.push_back('\n');
                if (auto const key = appearance.colourKey())
                {
                    output += "colour_key = ";
                    auto const channels = std::array{
                        static_cast<uint32>(key->red()),
                        static_cast<uint32>(key->green()),
                        static_cast<uint32>(key->blue()),
                    };
                    detail::appendUnsigned32Array(output, channels);
                    output.push_back('\n');
                    output += "colour_key_tolerance = ";
                    output += std::to_string(key->tolerance());
                    output.push_back('\n');
                }
            }
        }

        // A page carries only its identity. Its required and forbidden sets are
        // derived from the references that exercise identify, so writing them
        // here would put the same fact on disk twice.
        for (auto const& page : document.catalog().pages())
        {
            output += "\n[[page]]\n";
            detail::appendStringField(output, "id", resourceIdText(page.id()));
            detail::appendStringField(output, "name", page.name().value());
        }

        for (auto const& reference : document.references())
        {
            output += "\n[[reference]]\n";
            detail::appendStringField(
                output,
                "page_id",
                resourceIdText(reference.pageId)
            );
            detail::appendStringField(
                output,
                "element_id",
                resourceIdText(reference.elementId)
            );
            detail::appendStringField(
                output,
                "holding",
                detail::holdingText(reference.holding)
            );
            detail::appendExercisedFields(output, reference.exercised);
            if (auto const refined = reference.searchRoi)
            {
                detail::appendRectField(output, "search_roi", *refined);
            }
            if (auto const& pinned = reference.appearance)
            {
                detail::appendStringField(output, "appearance", pinned->value());
            }
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

        auto sources         = std::vector<AuthoringSource>{};
        auto parsedElements  = std::vector<ParsedElement>{};
        auto pages           = std::vector<PageSpec>{};
        auto references      = std::vector<PageReference>{};
        auto regressions     = std::vector<RegressionCase>{};
        auto appearanceCount = std::size_t{0};
        auto section         = uint8{0};
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
            else if (header == "[[element]]")
            {
                rank = 2;
                if (parsedElements.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring element quota exceeded");
                }
                UF_TRY_VALUE(element, parseElement(reader));
                parsedElements.emplace_back(std::move(element));
            }
            else if (header == "[[appearance]]")
            {
                rank = 3;
                if (appearanceCount >= k_maximumAuthoringAppearances)
                {
                    return invalidAuthoring("authoring appearance quota exceeded");
                }
                UF_TRY_VALUE(parsed, parseAppearance(reader));
                auto const owner = std::ranges::find(
                    parsedElements,
                    parsed.elementId,
                    &ParsedElement::id
                );
                if (owner == parsedElements.end())
                {
                    return invalidAuthoring(
                        "authoring appearance references an unknown element"
                    );
                }
                owner->appearances.emplace_back(std::move(parsed.appearance));
                ++appearanceCount;
            }
            else if (header == "[[page]]")
            {
                rank = 4;
                if (pages.size() >= k_maximumAuthoringResources)
                {
                    return invalidAuthoring("authoring page quota exceeded");
                }
                UF_TRY_VALUE(page, parsePage(reader));
                pages.emplace_back(std::move(page));
            }
            else if (header == "[[reference]]")
            {
                rank = 5;
                if (references.size() >= k_maximumAuthoringReferences)
                {
                    return invalidAuthoring("authoring reference quota exceeded");
                }
                UF_TRY_VALUE(reference, parseReference(reader));
                references.emplace_back(std::move(reference));
            }
            else if (header == "[[regression]]")
            {
                rank = 6;
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
                    "authoring source, element, appearance, page, reference, and regression tables are out of order"
                );
            }
            section = rank;
        }

        auto elements = std::vector<Element>{};
        elements.reserve(parsedElements.size());
        for (auto& parsed : parsedElements)
        {
            auto boundingTemplate = std::optional<PixelRect>{};
            if (!parsed.appearances.empty())
            {
                boundingTemplate = parsed.appearances.front().templateRect();
            }
            UF_TRY_VALUE(
                capabilities,
                detail::toElementCapabilities(parsed.capabilities, boundingTemplate)
            );
            UF_TRY_VALUE(
                element,
                Element::create(
                    fingerprint,
                    Element::Spec{
                        .id           = parsed.id,
                        .name         = std::move(parsed.name),
                        .capabilities = capabilities,
                        .searchRoi    = parsed.searchRoi,
                        .appearances  = std::move(parsed.appearances),
                    }
                )
            );
            elements.emplace_back(std::move(element));
        }

        UF_TRY_VALUE(
            document,
            AuthoringDocument::create(
                std::move(projectId),
                fingerprint,
                std::move(sources),
                std::move(elements),
                std::move(pages),
                std::move(references),
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
