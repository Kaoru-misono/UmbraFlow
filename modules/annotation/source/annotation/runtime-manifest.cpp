#include "runtime-manifest.hpp"

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
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_maximumRuntimeManifestBytes = std::size_t{16} * 1024U * 1024U;
        constexpr auto k_maximumRuntimeResources     = std::size_t{4096};

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

        [[nodiscard]]
        auto parsePixelRectField(
            detail::CanonicalTomlReader& reader,
            std::string_view key
        ) -> Result<PixelRect>
        {
            UF_TRY_VALUE(values, reader.takeUnsigned32ArrayField(key));
            if (values.size() != 4U)
            {
                return invalidManifest(
                    std::format("runtime manifest '{}' must have four integers", key)
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
                return invalidManifest(
                    std::format("runtime manifest '{}' is not a valid rectangle", key)
                );
            }
            return *rect;
        }

        [[nodiscard]]
        auto parseRecognizerIds(
            std::vector<std::string> const& encoded
        ) -> Result<std::vector<RecognizerId>>
        {
            auto ids = std::vector<RecognizerId>{};
            ids.reserve(encoded.size());
            for (auto const& value : encoded)
            {
                UF_TRY_VALUE(id, ResourceId::parse(value));
                ids.emplace_back(RecognizerId{id});
            }
            return ids;
        }

        [[nodiscard]]
        auto parsePageIds(
            std::vector<std::string> const& encoded
        ) -> Result<std::vector<PageId>>
        {
            auto ids = std::vector<PageId>{};
            ids.reserve(encoded.size());
            for (auto const& value : encoded)
            {
                UF_TRY_VALUE(id, ResourceId::parse(value));
                ids.emplace_back(PageId{id});
            }
            return ids;
        }

        [[nodiscard]]
        auto parseAnnotationType(std::string_view value) -> Result<AnnotationType>
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
            return invalidManifest(
                std::format("runtime manifest has unknown annotation_type '{}'", value)
            );
        }

        [[nodiscard]]
        auto parseRecognizer(
            detail::CanonicalTomlReader& reader,
            ProjectFingerprint fingerprint
        ) -> Result<RuntimeRecognizerSpec>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(resourceId, ResourceId::parse(idText));
            auto const id = RecognizerId{resourceId};
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(typeText, reader.takeStringField("annotation_type"));
            UF_TRY_VALUE(annotationType, parseAnnotationType(typeText));
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
            UF_TRY_VALUE(templateRect, parsePixelRectField(reader, "template_rect"));
            UF_TRY_VALUE(searchRoi, parsePixelRectField(reader, "search_roi"));
            UF_TRY_VALUE(
                thresholdValue,
                reader.takeUnsigned32Field("min_similarity_bp")
            );
            UF_TRY_VALUE(threshold, SimilarityThreshold::create(thresholdValue));

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
                    return invalidManifest(
                        "runtime manifest default_click must have two integers"
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

            auto allowedPageIds = std::vector<PageId>{};
            UF_TRY_VALUE(
                hasAllowedPages,
                reader.nextIsField("allowed_page_ids")
            );
            if (hasAllowedPages)
            {
                UF_TRY_VALUE(
                    encoded,
                    reader.takeStringArrayField("allowed_page_ids")
                );
                UF_TRY_VALUE(parsed, parsePageIds(encoded));
                allowedPageIds = std::move(parsed);
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
                        .m_allowedPageIds = std::move(allowedPageIds),
                    }
                )
            );
            return RuntimeRecognizerSpec{
                .m_definition   = std::move(definition),
                .m_templateHash = templateHash,
                .m_sourceHash   = sourceHash,
            };
        }

        [[nodiscard]]
        auto parsePage(
            detail::CanonicalTomlReader& reader
        ) -> Result<PageSignature>
        {
            UF_TRY_VALUE(idText, reader.takeStringField("id"));
            UF_TRY_VALUE(resourceId, ResourceId::parse(idText));
            auto const id = PageId{resourceId};
            UF_TRY_VALUE(nameText, reader.takeStringField("name"));
            UF_TRY_VALUE(name, ResourceName::create(std::move(nameText)));
            UF_TRY_VALUE(
                requiredText,
                reader.takeStringArrayField("required")
            );
            UF_TRY_VALUE(required, parseRecognizerIds(requiredText));
            UF_TRY_VALUE(
                forbiddenText,
                reader.takeStringArrayField("forbidden")
            );
            UF_TRY_VALUE(forbidden, parseRecognizerIds(forbiddenText));
            return PageSignature::create(
                PageSpec{
                    .m_id        = id,
                    .m_name      = std::move(name),
                    .m_required  = std::move(required),
                    .m_forbidden = std::move(forbidden),
                }
            );
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
        std::vector<PageSignature> pages
    ) -> Result<RuntimeManifest>
    {
        if (
            recognizers.size() > k_maximumRuntimeResources
            || pages.size() > k_maximumRuntimeResources
        )
        {
            return invalidManifest(
                "runtime manifest exceeds the recognizer or page quota"
            );
        }

        std::ranges::sort(
            recognizers,
            {},
            [](RuntimeRecognizerSpec const& recognizer) -> ResourceId
            {
                return recognizer.m_definition.id().value();
            }
        );

        auto definitions = std::vector<RecognizerDefinition>{};
        auto assets      = std::vector<RuntimeRecognizerAsset>{};
        definitions.reserve(recognizers.size());
        assets.reserve(recognizers.size());
        for (auto& recognizer : recognizers)
        {
            auto const id = recognizer.m_definition.id();
            assets.emplace_back(
                RuntimeRecognizerAsset{
                    .m_id           = id,
                    .m_templateHash = recognizer.m_templateHash,
                    .m_sourceHash   = recognizer.m_sourceHash,
                    .m_templatePath = templatePath(recognizer.m_templateHash),
                }
            );
            definitions.emplace_back(std::move(recognizer.m_definition));
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
        RecognizerId id
    ) const noexcept -> RuntimeRecognizerAsset const*
    {
        auto const found = std::ranges::find(
            m_assets,
            id,
            &RuntimeRecognizerAsset::m_id
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

        auto const fingerprint = manifest.catalog().fingerprint();
        output += "base_resolution = ";
        auto const resolution = std::array{
            fingerprint.width(),
            fingerprint.height(),
        };
        detail::appendUnsigned32Array(output, resolution);
        output.push_back('\n');
        output += "base_dpi = ";
        auto const dpi = std::array{
            fingerprint.dpiX(),
            fingerprint.dpiY(),
        };
        detail::appendUnsigned32Array(output, dpi);
        output.push_back('\n');

        for (auto const& recognizer : manifest.catalog().recognizers())
        {
            auto const* p_asset = manifest.findAsset(recognizer.id());
            UF_CHECK_MSG(p_asset != nullptr, "runtime recognizer asset is missing");
            output += "\n[[recognizer]]\n";
            detail::appendStringField(
                output,
                "id",
                recognizer.id().value().toString()
            );
            detail::appendStringField(output, "name", recognizer.name().value());
            detail::appendStringField(
                output,
                "annotation_type",
                annotationTypeText(recognizer.annotationType())
            );
            detail::appendStringField(output, "kind", "gray_template");
            detail::appendStringField(output, "template", p_asset->m_templatePath);
            detail::appendStringField(
                output,
                "template_hash",
                p_asset->m_templateHash.toString()
            );
            detail::appendStringField(
                output,
                "source_hash",
                p_asset->m_sourceHash.toString()
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
                output += "allowed_page_ids = ";
                appendIdArray(output, recognizer.allowedPageIds());
                output.push_back('\n');
            }
        }

        for (auto const& page : manifest.catalog().pages())
        {
            output += "\n[[page]]\n";
            detail::appendStringField(output, "id", page.id().value().toString());
            detail::appendStringField(output, "name", page.name().value());
            output += "required = ";
            appendIdArray(output, page.required());
            output.push_back('\n');
            output += "forbidden = ";
            appendIdArray(output, page.forbidden());
            output.push_back('\n');
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
            resolution,
            reader.takeUnsigned32ArrayField("base_resolution")
        );
        UF_TRY_VALUE(dpi, reader.takeUnsigned32ArrayField("base_dpi"));
        if (resolution.size() != 2U || dpi.size() != 2U)
        {
            return invalidManifest(
                "runtime manifest base_resolution and base_dpi must have two integers"
            );
        }
        UF_TRY_VALUE(
            fingerprint,
            ProjectFingerprint::create(
                checkedAt(resolution, 0),
                checkedAt(resolution, 1),
                checkedAt(dpi, 0),
                checkedAt(dpi, 1)
            )
        );

        auto recognizers  = std::vector<RuntimeRecognizerSpec>{};
        auto pages        = std::vector<PageSignature>{};
        auto parsingPages = false;
        while (!reader.eof())
        {
            UF_TRY(reader.expect(""));
            auto const headerLine = reader.line();
            UF_TRY_VALUE(header, reader.take());
            if (header == "[[recognizer]]")
            {
                if (parsingPages)
                {
                    return invalidManifest(
                        "runtime manifest recognizers must precede pages"
                    );
                }
                if (recognizers.size() >= k_maximumRuntimeResources)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the recognizer quota"
                    );
                }
                UF_TRY_VALUE(recognizer, parseRecognizer(reader, fingerprint));
                recognizers.emplace_back(std::move(recognizer));
                continue;
            }
            if (header == "[[page]]")
            {
                parsingPages = true;
                if (pages.size() >= k_maximumRuntimeResources)
                {
                    return invalidManifest(
                        "runtime manifest exceeds the page quota"
                    );
                }
                UF_TRY_VALUE(page, parsePage(reader));
                pages.emplace_back(std::move(page));
                continue;
            }
            return invalidManifest(
                std::format(
                    "runtime manifest line {} has unknown table header '{}'",
                    headerLine,
                    header
                )
            );
        }

        UF_TRY_VALUE(
            manifest,
            RuntimeManifest::create(
                std::move(projectId),
                fingerprint,
                std::move(recognizers),
                std::move(pages)
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
