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
        constexpr auto k_maximumRuntimeVariants      = std::size_t{4096} * 8U;
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

        // A recognizer as its own table row leaves it. Its variants arrive in
        // later rows, and the click offset cannot become a TemplateOffset until
        // one of those templates is known.
        struct ParsedRecognizer final
        {
            ElementId                id;
            ResourceName             name;
            PixelRect                searchRoi;
            detail::CapabilityFields capabilities;

            std::vector<RecognizerVariant>   variants{};
            std::vector<RuntimeVariantAsset> assets{};
        };

        struct ParsedVariant final
        {
            ElementId           elementId;
            RecognizerVariant   variant;
            RuntimeVariantAsset asset;
        };

        [[nodiscard]]
        auto parseRecognizer(
            detail::CanonicalTomlReader& reader
        ) -> Result<ParsedRecognizer>
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
            return ParsedRecognizer{
                .id           = id,
                .name         = std::move(name),
                .searchRoi    = searchRoi,
                .capabilities = capabilities,
            };
        }

        [[nodiscard]]
        auto parseVariant(
            detail::CanonicalTomlReader& reader
        ) -> Result<ParsedVariant>
        {
            UF_TRY_VALUE(elementIdText, reader.takeStringField("element_id"));
            UF_TRY_VALUE(elementId, detail::parseId<ElementId>(elementIdText));
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(kind, reader.takeStringField("kind"));
            if (kind != "gray_template")
            {
                return invalidManifest(
                    "runtime manifest P0 recognizer kind must be gray_template"
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

            return ParsedVariant{
                .elementId = elementId,
                .variant   = RecognizerVariant{
                    .name         = name,
                    .templateRect = templateRect,
                    .threshold    = threshold,
                },
                .asset = RuntimeVariantAsset{
                    .variantName  = std::move(name),
                    .templateHash = templateHash,
                    .sourceHash   = sourceHash,
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

            auto variant = std::optional<ResourceName>{};
            UF_TRY_VALUE(hasVariant, reader.nextIsField("variant"));
            if (hasVariant)
            {
                UF_TRY_VALUE(variantText, reader.takeStringField("variant"));
                UF_TRY_VALUE(name, ResourceName::create(std::move(variantText)));
                variant = std::move(name);
            }

            return PageReference{
                .pageId    = pageId,
                .elementId = elementId,
                .holding   = *holding,
                .exercised = exercised,
                .searchRoi = searchRoi,
                .variant   = std::move(variant),
            };
        }
    }

    RuntimeManifest::RuntimeManifest(
        RecognitionCatalog catalog,
        std::vector<RuntimeRecognizerAsset> assets
    ) noexcept
        : m_catalog{std::move(catalog)}
        , m_assets{std::move(assets)}
    {
    }

    auto RuntimeManifest::create(
        ProjectId projectId,
        ProjectFingerprint fingerprint,
        std::vector<RuntimeRecognizerSpec> recognizers,
        std::vector<PageSpec> pages,
        std::vector<PageReference> references
    ) -> Result<RuntimeManifest>
    {
        if (
            recognizers.size() > k_maximumRuntimeResources
            || pages.size() > k_maximumRuntimeResources
            || references.size() > k_maximumRuntimeReferences
        )
        {
            return invalidManifest(
                "runtime manifest exceeds the recognizer, page, or reference quota"
            );
        }

        std::ranges::sort(
            recognizers,
            {},
            [](RuntimeRecognizerSpec const& recognizer) -> ResourceId
            {
                return recognizer.definition.id().value();
            }
        );

        auto definitions = std::vector<RecognizerDefinition>{};
        auto assets      = std::vector<RuntimeRecognizerAsset>{};
        definitions.reserve(recognizers.size());
        for (auto& recognizer : recognizers)
        {
            auto const id       = recognizer.definition.id();
            auto const variants = recognizer.definition.variants();
            if (recognizer.variants.size() != variants.size())
            {
                return invalidManifest(
                    "runtime manifest needs exactly one template asset per declared variant"
                );
            }
            for (auto index = std::size_t{0}; index < variants.size(); ++index)
            {
                auto const& asset = checkedAt(recognizer.variants, index);
                if (asset.variantName != checkedAt(variants, index).name)
                {
                    return invalidManifest(
                        "runtime manifest template assets are not in variant order"
                    );
                }
                assets.emplace_back(
                    RuntimeRecognizerAsset{
                        .elementId    = id,
                        .variantName  = asset.variantName,
                        .templateHash = asset.templateHash,
                        .sourceHash   = asset.sourceHash,
                        .templatePath = templatePath(asset.templateHash),
                    }
                );
            }
            definitions.emplace_back(std::move(recognizer.definition));
        }
        if (assets.size() > k_maximumRuntimeVariants)
        {
            return invalidManifest("runtime manifest exceeds the variant quota");
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

    auto RuntimeManifest::assets() const noexcept -> std::span<RuntimeRecognizerAsset const>
    {
        return m_assets;
    }

    auto RuntimeManifest::findAsset(
        ElementId elementId,
        ResourceName const& variantName
    ) const noexcept -> RuntimeRecognizerAsset const*
    {
        auto const found = std::ranges::find_if(
            m_assets,
            [elementId, &variantName](RuntimeRecognizerAsset const& asset) noexcept -> bool
            {
                return (
                    asset.elementId == elementId
                    && asset.variantName == variantName
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

        for (auto const& recognizer : manifest.catalog().recognizers())
        {
            output += "\n[[recognizer]]\n";
            detail::appendStringField(
                output,
                "id",
                recognizer.id().value().toString()
            );
            detail::appendStringField(output, "name", recognizer.name().value());
            detail::appendRectField(output, "search_roi", recognizer.searchRoi());
            detail::appendCapabilityFields(output, recognizer.capabilities());
        }

        for (auto const& recognizer : manifest.catalog().recognizers())
        {
            for (auto const& variant : recognizer.variants())
            {
                auto const* p_asset = manifest.findAsset(
                    recognizer.id(),
                    variant.name
                );
                UF_CHECK_MSG(p_asset != nullptr, "runtime variant asset is missing");
                output += "\n[[variant]]\n";
                detail::appendStringField(
                    output,
                    "element_id",
                    recognizer.id().value().toString()
                );
                detail::appendStringField(output, "name", variant.name.value());
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
                    variant.templateRect
                );
                output += "min_similarity_bp = ";
                output += std::to_string(variant.threshold.basisPoints());
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
            if (auto const& pinned = reference.variant)
            {
                detail::appendStringField(output, "variant", pinned->value());
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

        auto parsedRecognizers = std::vector<ParsedRecognizer>{};
        auto pages             = std::vector<PageSpec>{};
        auto references        = std::vector<PageReference>{};
        auto variantCount      = std::size_t{0};
        auto section           = uint8{0};
        while (!reader.eof())
        {
            UF_TRY(reader.expect(""));
            auto const headerLine = reader.line();
            UF_TRY_VALUE(header, reader.take());
            auto rank = uint8{0};
            if (header == "[[recognizer]]")
            {
                rank = 1;
                if (parsedRecognizers.size() >= k_maximumRuntimeResources)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the recognizer quota"
                    );
                }
                UF_TRY_VALUE(recognizer, parseRecognizer(reader));
                parsedRecognizers.emplace_back(std::move(recognizer));
            }
            else if (header == "[[variant]]")
            {
                rank = 2;
                if (variantCount >= k_maximumRuntimeVariants)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the variant quota"
                    );
                }
                UF_TRY_VALUE(parsed, parseVariant(reader));
                auto const owner = std::ranges::find(
                    parsedRecognizers,
                    parsed.elementId,
                    &ParsedRecognizer::id
                );
                if (owner == parsedRecognizers.end())
                {
                    return invalidManifest(
                        "runtime manifest variant references an unknown element"
                    );
                }
                owner->variants.emplace_back(std::move(parsed.variant));
                owner->assets.emplace_back(std::move(parsed.asset));
                ++variantCount;
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
                    "runtime manifest recognizer, variant, page, and reference tables are out of order"
                );
            }
            section = rank;
        }

        auto recognizers = std::vector<RuntimeRecognizerSpec>{};
        recognizers.reserve(parsedRecognizers.size());
        for (auto& parsed : parsedRecognizers)
        {
            auto boundingTemplate = std::optional<PixelRect>{};
            if (!parsed.variants.empty())
            {
                boundingTemplate = parsed.variants.front().templateRect;
            }
            UF_TRY_VALUE(
                capabilities,
                detail::toElementCapabilities(parsed.capabilities, boundingTemplate)
            );
            UF_TRY_VALUE(
                definition,
                RecognizerDefinition::create(
                    fingerprint,
                    RecognizerSpec{
                        .id           = parsed.id,
                        .name         = std::move(parsed.name),
                        .capabilities = capabilities,
                        .searchRoi    = parsed.searchRoi,
                        .variants     = std::move(parsed.variants),
                    }
                )
            );
            recognizers.emplace_back(
                RuntimeRecognizerSpec{
                    .definition = std::move(definition),
                    .variants   = std::move(parsed.assets),
                }
            );
        }

        UF_TRY_VALUE(
            manifest,
            RuntimeManifest::create(
                std::move(projectId),
                fingerprint,
                std::move(recognizers),
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
