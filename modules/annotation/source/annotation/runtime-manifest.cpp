#include "runtime-manifest.hpp"

#include "detail/annotation-fields.hpp"
#include "detail/canonical-toml.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
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
        constexpr auto k_maximumRuntimeManifestBytes = std::size_t{16} * 1024U * 1024U;
        constexpr auto k_maximumRuntimeResources     = std::size_t{4096};
        constexpr auto k_maximumRuntimeAppearances      = std::size_t{4096} * 8U;
        constexpr auto k_maximumRuntimeReferences    = std::size_t{4096} * 16U;

        [[nodiscard]]
        auto invalidManifest(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto templatePath(ContentHash const& hash) -> std::string
        {
            auto path = std::string{"assets/templates/"};
            path += hash.hex();
            path += ".png";
            return path;
        }

        // An element as its own table row leaves it. Its appearances arrive in
        // later rows, and the click offset cannot become a TemplateOffset until
        // one of those templates is known.
        struct ParsedElement final
        {
            ElementId                id;
            ResourceName             name;
            PixelRect                searchRoi;
            detail::CapabilityFields capabilities;

            std::vector<CompiledAppearance>     appearances{};
            std::vector<RuntimeAppearanceAsset> assets{};
        };

        struct ParsedAppearance final
        {
            ElementId              elementId;
            CompiledAppearance     appearance;
            RuntimeAppearanceAsset asset;
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
            UF_TRY_VALUE(kind, reader.takeStringField("kind"));
            if (kind != "gray_template")
            {
                return invalidManifest(
                    "runtime manifest P0 element kind must be gray_template"
                );
            }
            UF_TRY_VALUE(path, reader.takeStringField("template"));
            UF_TRY_VALUE(
                templateHashText,
                reader.takeStringField("template_hash")
            );
            UF_TRY_VALUE(templateHash, ContentHash::parse(templateHashText));
            if (path != templatePath(templateHash))
            {
                return invalidManifest(
                    "runtime manifest template path does not match template_hash"
                );
            }
            UF_TRY_VALUE(
                sourceHashText,
                reader.takeStringField("source_hash")
            );
            UF_TRY_VALUE(sourceHash, ContentHash::parse(sourceHashText));
            UF_TRY_VALUE(
                templateRect,
                detail::parsePixelRectField(reader, "template_rect")
            );
            UF_TRY_VALUE(
                thresholdValue,
                reader.takeUnsigned32Field("min_similarity_bp")
            );
            UF_TRY_VALUE(threshold, SimilarityThreshold::create(thresholdValue));

            return ParsedAppearance{
                .elementId = elementId,
                .appearance   = CompiledAppearance{
                    .name         = name,
                    .templateRect = templateRect,
                    .threshold    = threshold,
                },
                .asset = RuntimeAppearanceAsset{
                    .appearanceName = std::move(name),
                    .templateHash   = templateHash,
                    .sourceHash     = sourceHash,
                },
            };
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
                return invalidManifest(
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
    }

    RuntimeManifest::RuntimeManifest(
        RecognitionCatalog catalog,
        std::vector<RuntimeElementAsset> assets
    ) noexcept
        : m_catalog{std::move(catalog)}
        , m_assets{std::move(assets)}
    {
    }

    auto RuntimeManifest::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<RuntimeElementSpec> elements,
        std::vector<PageSpec> pages,
        std::vector<PageReference> references
    ) -> Result<RuntimeManifest>
    {
        if (
            elements.size() > k_maximumRuntimeResources
            || pages.size() > k_maximumRuntimeResources
            || references.size() > k_maximumRuntimeReferences
        )
        {
            return invalidManifest(
                "runtime manifest exceeds the element, page, or reference quota"
            );
        }

        std::ranges::sort(
            elements,
            {},
            [](RuntimeElementSpec const& element) -> ResourceId
            {
                return element.definition.id().value();
            }
        );

        auto definitions = std::vector<CompiledElement>{};
        auto assets      = std::vector<RuntimeElementAsset>{};
        definitions.reserve(elements.size());
        for (auto& element : elements)
        {
            auto const id       = element.definition.id();
            auto const appearances = element.definition.appearances();
            if (element.appearances.size() != appearances.size())
            {
                return invalidManifest(
                    "runtime manifest needs exactly one template asset per declared appearance"
                );
            }
            for (auto index = std::size_t{0}; index < appearances.size(); ++index)
            {
                auto const& asset = checkedAt(element.appearances, index);
                if (asset.appearanceName != checkedAt(appearances, index).name)
                {
                    return invalidManifest(
                        "runtime manifest template assets are not in appearance order"
                    );
                }
                assets.emplace_back(
                    RuntimeElementAsset{
                        .elementId      = id,
                        .appearanceName = asset.appearanceName,
                        .templateHash   = asset.templateHash,
                        .sourceHash     = asset.sourceHash,
                        .templatePath   = templatePath(asset.templateHash),
                    }
                );
            }
            definitions.emplace_back(std::move(element.definition));
        }
        if (assets.size() > k_maximumRuntimeAppearances)
        {
            return invalidManifest("runtime manifest exceeds the appearance quota");
        }

        UF_TRY_VALUE(
            catalog,
            RecognitionCatalog::create(
                std::move(projectId),
                fingerprint,
                std::move(definitions),
                std::move(pages),
                std::move(references)
            )
        );
        auto manifest = RuntimeManifest{
            std::move(catalog),
            std::move(assets)
        };
        if (serializeRuntimeManifest(manifest).size() > k_maximumRuntimeManifestBytes)
        {
            return invalidManifest(
                "runtime manifest exceeds the 16 MiB serialized quota"
            );
        }
        return manifest;
    }

    auto RuntimeManifest::catalog() const noexcept -> RecognitionCatalog const&
    {
        return m_catalog;
    }

    auto RuntimeManifest::assets() const noexcept -> std::span<RuntimeElementAsset const>
    {
        return m_assets;
    }

    auto RuntimeManifest::findAsset(
        ElementId elementId,
        ResourceName const& appearanceName
    ) const noexcept -> RuntimeElementAsset const*
    {
        auto const found = std::ranges::find_if(
            m_assets,
            [elementId, &appearanceName](RuntimeElementAsset const& asset) noexcept -> bool
            {
                return (
                    asset.elementId == elementId
                    && asset.appearanceName == appearanceName
                );
            }
        );
        return found == m_assets.end() ? nullptr : &*found;
    }

    auto serializeRuntimeManifest(RuntimeManifest const& manifest) -> std::string
    {
        auto output = std::string{};
        detail::appendStringField(output, "schema", k_runtimeManifestSchema);
        detail::appendStringField(
            output,
            "project_id",
            manifest.catalog().projectId().value()
        );

        detail::appendFingerprintFields(
            output,
            manifest.catalog().fingerprint(),
            "base_resolution",
            "base_dpi"
        );

        for (auto const& element : manifest.catalog().elements())
        {
            output += "\n[[element]]\n";
            detail::appendStringField(
                output,
                "id",
                element.id().value().toString()
            );
            detail::appendStringField(output, "name", element.name().value());
            detail::appendRectField(output, "search_roi", element.searchRoi());
            detail::appendCapabilityFields(output, element.capabilities());
        }

        for (auto const& element : manifest.catalog().elements())
        {
            for (auto const& appearance : element.appearances())
            {
                auto const* p_asset = manifest.findAsset(
                    element.id(),
                    appearance.name
                );
                UF_CHECK_MSG(p_asset != nullptr, "runtime appearance asset is missing");
                output += "\n[[appearance]]\n";
                detail::appendStringField(
                    output,
                    "element_id",
                    element.id().value().toString()
                );
                detail::appendStringField(output, "name", appearance.name.value());
                detail::appendStringField(output, "kind", "gray_template");
                detail::appendStringField(output, "template", p_asset->templatePath);
                detail::appendStringField(
                    output,
                    "template_hash",
                    p_asset->templateHash.toString()
                );
                detail::appendStringField(
                    output,
                    "source_hash",
                    p_asset->sourceHash.toString()
                );
                detail::appendRectField(
                    output,
                    "template_rect",
                    appearance.templateRect
                );
                output += "min_similarity_bp = ";
                output += std::to_string(appearance.threshold.basisPoints());
                output.push_back('\n');
            }
        }

        for (auto const& page : manifest.catalog().pages())
        {
            output += "\n[[page]]\n";
            detail::appendStringField(output, "id", page.id().value().toString());
            detail::appendStringField(output, "name", page.name().value());
        }

        for (auto const& reference : manifest.catalog().references())
        {
            output += "\n[[reference]]\n";
            detail::appendStringField(
                output,
                "page_id",
                reference.pageId.value().toString()
            );
            detail::appendStringField(
                output,
                "element_id",
                reference.elementId.value().toString()
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
        return output;
    }

    auto parseRuntimeManifest(
        std::string_view canonicalToml
    ) -> Result<RuntimeManifest>
    {
        if (
            canonicalToml.empty()
            || canonicalToml.size() > k_maximumRuntimeManifestBytes
        )
        {
            return invalidManifest(
                "runtime manifest is empty or exceeds the 16 MiB quota"
            );
        }

        auto reader = detail::CanonicalTomlReader{
            "runtime manifest",
            std::string{canonicalToml}
        };
        UF_TRY_VALUE(schema, reader.takeStringField("schema"));
        if (schema != k_runtimeManifestSchema)
        {
            return invalidManifest(
                std::format("unsupported runtime manifest schema '{}'", schema)
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

        auto parsedElements  = std::vector<ParsedElement>{};
        auto pages           = std::vector<PageSpec>{};
        auto references      = std::vector<PageReference>{};
        auto appearanceCount = std::size_t{0};
        auto section         = uint8{0};
        while (!reader.eof())
        {
            UF_TRY(reader.expect(""));
            auto const headerLine = reader.line();
            UF_TRY_VALUE(header, reader.take());
            auto rank = uint8{0};
            if (header == "[[element]]")
            {
                rank = 1;
                if (parsedElements.size() >= k_maximumRuntimeResources)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the element quota"
                    );
                }
                UF_TRY_VALUE(element, parseElement(reader));
                parsedElements.emplace_back(std::move(element));
            }
            else if (header == "[[appearance]]")
            {
                rank = 2;
                if (appearanceCount >= k_maximumRuntimeAppearances)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the appearance quota"
                    );
                }
                UF_TRY_VALUE(parsed, parseAppearance(reader));
                auto const owner = std::ranges::find(
                    parsedElements,
                    parsed.elementId,
                    &ParsedElement::id
                );
                if (owner == parsedElements.end())
                {
                    return invalidManifest(
                        "runtime manifest appearance references an unknown element"
                    );
                }
                owner->appearances.emplace_back(std::move(parsed.appearance));
                owner->assets.emplace_back(std::move(parsed.asset));
                ++appearanceCount;
            }
            else if (header == "[[page]]")
            {
                rank = 3;
                if (pages.size() >= k_maximumRuntimeResources)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the page quota"
                    );
                }
                UF_TRY_VALUE(page, parsePage(reader));
                pages.emplace_back(std::move(page));
            }
            else if (header == "[[reference]]")
            {
                rank = 4;
                if (references.size() >= k_maximumRuntimeReferences)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the reference quota"
                    );
                }
                UF_TRY_VALUE(reference, parseReference(reader));
                references.emplace_back(std::move(reference));
            }
            else
            {
                return invalidManifest(
                    std::format(
                        "runtime manifest line {} has unknown table header '{}'",
                        headerLine,
                        header
                    )
                );
            }
            if (rank < section)
            {
                return invalidManifest(
                    "runtime manifest element, appearance, page, and reference tables are out of order"
                );
            }
            section = rank;
        }

        auto elements = std::vector<RuntimeElementSpec>{};
        elements.reserve(parsedElements.size());
        for (auto& parsed : parsedElements)
        {
            auto boundingTemplate = std::optional<PixelRect>{};
            if (!parsed.appearances.empty())
            {
                boundingTemplate = parsed.appearances.front().templateRect;
            }
            UF_TRY_VALUE(
                capabilities,
                detail::toElementCapabilities(parsed.capabilities, boundingTemplate)
            );
            UF_TRY_VALUE(
                definition,
                CompiledElement::create(
                    fingerprint,
                    CompiledElementSpec{
                        .id           = parsed.id,
                        .name         = std::move(parsed.name),
                        .capabilities = capabilities,
                        .searchRoi    = parsed.searchRoi,
                        .appearances  = std::move(parsed.appearances),
                    }
                )
            );
            elements.emplace_back(
                RuntimeElementSpec{
                    .definition  = std::move(definition),
                    .appearances = std::move(parsed.assets),
                }
            );
        }

        UF_TRY_VALUE(
            manifest,
            RuntimeManifest::create(
                std::move(projectId),
                fingerprint,
                std::move(elements),
                std::move(pages),
                std::move(references)
            )
        );
        if (serializeRuntimeManifest(manifest) != canonicalToml)
        {
            return invalidManifest(
                "runtime manifest is valid data but not canonical generated TOML"
            );
        }
        return manifest;
    }
}
