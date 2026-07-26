#include "authoring-compiler.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_rgbaBytesPerPixel            = std::size_t{4};
        constexpr auto k_maximumCompiledTemplateBytes = std::size_t{512} * 1024U * 1024U;
        constexpr auto k_maximumCompilationPixelWork  = std::size_t{256} * 1024U * 1024U;

        struct RecognizerWork final
        {
            SourceId    sourceId;
            std::size_t recognizerIndex{};
        };

        struct TemplateTaskKey final
        {
            SourceId  sourceId;
            PixelRect templateRect;
        };

        struct TemplateTaskLess final
        {
            [[nodiscard]]
            auto operator()(
                TemplateTaskKey const& left,
                TemplateTaskKey const& right
            ) const noexcept -> bool
            {
                if (left.sourceId != right.sourceId)
                {
                    return left.sourceId.value() < right.sourceId.value();
                }

                return (
                    std::tuple{
                        left.templateRect.x(),
                        left.templateRect.y(),
                        left.templateRect.width(),
                        left.templateRect.height()
                    }
                    < std::tuple{
                        right.templateRect.x(),
                        right.templateRect.y(),
                        right.templateRect.width(),
                        right.templateRect.height()
                    }
                );
            }
        };

        using TemplateTaskPlan        = std::set<TemplateTaskKey, TemplateTaskLess>;
        using GeneratedTemplateHashes = std::map<TemplateTaskKey, ContentHash, TemplateTaskLess>;

        [[nodiscard]]
        auto invalidCompilation(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto prepareTemplateTasks(
            std::span<AuthoringSource const> sources,
            std::span<RecognizerDefinition const> recognizers,
            std::span<RecognizerWork const> recognizerWork
        ) -> Result<TemplateTaskPlan>
        {
            auto templateTasks        = TemplateTaskPlan{};
            auto compilationPixelWork = std::size_t{0};
            for (auto const& source : sources)
            {
                auto const fingerprint  = source.fingerprint();
                auto const sourcePixels = checkedMultiply(
                    static_cast<std::size_t>(fingerprint.width()),
                    static_cast<std::size_t>(fingerprint.height())
                );
                if (!sourcePixels)
                {
                    return invalidCompilation(
                        "authoring compilation pixel-work calculation overflowed addressable memory"
                    );
                }

                auto const nextPixelWork = checkedAdd(
                    compilationPixelWork,
                    *sourcePixels
                );
                if (
                    !nextPixelWork
                    || *nextPixelWork > k_maximumCompilationPixelWork
                )
                {
                    return invalidCompilation(
                        "authoring compilation exceeds the 256 Mi-pixel work quota"
                    );
                }
                compilationPixelWork = *nextPixelWork;
            }

            for (auto const& work : recognizerWork)
            {
                auto const templateRect = checkedAt(
                    recognizers,
                    work.recognizerIndex
                ).templateRect();
                auto const insertion = templateTasks.emplace(
                    TemplateTaskKey{
                        .sourceId     = work.sourceId,
                        .templateRect = templateRect,
                    }
                );
                if (!insertion.second)
                {
                    continue;
                }

                auto const taskPixels = checkedMultiply(
                    static_cast<std::size_t>(templateRect.width()),
                    static_cast<std::size_t>(templateRect.height())
                );
                if (!taskPixels)
                {
                    return invalidCompilation(
                        "authoring template pixel-work calculation overflowed addressable memory"
                    );
                }

                auto const nextPixelWork = checkedAdd(
                    compilationPixelWork,
                    *taskPixels
                );
                if (
                    !nextPixelWork
                    || *nextPixelWork > k_maximumCompilationPixelWork
                )
                {
                    return invalidCompilation(
                        "authoring compilation exceeds the 256 Mi-pixel work quota"
                    );
                }
                compilationPixelWork = *nextPixelWork;
            }

            return templateTasks;
        }

        // The id for one per-placement runtime recognizer. An element placed on
        // several pages expands into one recognizer per page, and each needs an
        // id distinct from the element's own and stable across compiles. sha256
        // over the element id bytes concatenated with the page id bytes gives
        // both: deterministic for fixed inputs, and collision-resistant enough
        // that a clash with a real id is astronomically unlikely -- and if one
        // ever occurred it would fail loudly in RecognitionCatalog::create's
        // uniqueness guard rather than silently drop a recognizer.
        [[nodiscard]]
        auto derivedRecognizerId(
            RecognizerId elementId,
            PageId pageId
        ) -> RecognizerId
        {
            auto seed             = std::vector<std::byte>{};
            auto const elementHex = elementId.value().toString();
            auto const pageHex    = pageId.value().toString();
            seed.reserve(elementHex.size() + pageHex.size());
            for (auto const character : elementHex)
            {
                seed.emplace_back(static_cast<std::byte>(character));
            }
            for (auto const character : pageHex)
            {
                seed.emplace_back(static_cast<std::byte>(character));
            }
            // sha256 over an in-memory buffer cannot fail; the Result models a
            // streaming source, which this is not.
            auto const digest = sha256(seed);
            UF_CHECK_MSG(
                digest.has_value(),
                "hashing a derived recognizer id seed must not fail"
            );
            auto const digestBytes = digest->bytes();
            auto truncated         = std::array<std::byte, 16>{};
            for (auto index = std::size_t{0}; index < truncated.size(); ++index)
            {
                checkedAt(truncated, index) = static_cast<std::byte>(
                    checkedAt(digestBytes, index)
                );
            }
            return RecognizerId{
                ResourceId::fromBytes(std::span<std::byte const, 16>{truncated})
            };
        }

        // The name for one per-placement runtime recognizer: the element name and
        // the page name joined by an underscore. Both are already valid ASCII
        // Luau member keys, so their underscore-joined form is one too, and the
        // join can never spell a reserved word (each carries an underscore). The
        // page name is unique among pages, so the pair reads as "which element,
        // on which page"; any residual clash across elements fails loudly in the
        // catalog's name-uniqueness guard.
        [[nodiscard]]
        auto derivedRecognizerName(
            ResourceName const& elementName,
            ResourceName const& pageName
        ) -> Result<ResourceName>
        {
            return ResourceName::create(
                std::format("{}_{}", elementName.value(), pageName.value())
            );
        }

    }

    auto compileAuthoringDocument(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets
    ) -> Result<CompiledAuthoringProject>
    {
        auto const sources = document.sources();
        if (sourceAssets.size() != sources.size())
        {
            return invalidCompilation(
                "authoring compilation requires exactly one asset per source record"
            );
        }

        auto assetOrder = std::vector<std::size_t>{};
        assetOrder.reserve(sourceAssets.size());
        for (auto index = std::size_t{0}; index < sourceAssets.size(); ++index)
        {
            assetOrder.emplace_back(index);
        }
        std::ranges::sort(
            assetOrder,
            {},
            [&sourceAssets](std::size_t index) -> ResourceId
            {
                return checkedAt(sourceAssets, index).id.value();
            }
        );
        for (auto index = std::size_t{0}; index < sources.size(); ++index)
        {
            auto const& source    = checkedAt(sources, index);
            auto const assetIndex = checkedAt(assetOrder, index);
            auto const& asset     = checkedAt(sourceAssets, assetIndex);
            if (asset.id != source.id())
            {
                return invalidCompilation(
                    "authoring compilation requires one distinct matching asset per source"
                );
            }
        }

        auto const recognizers       = document.catalog().recognizers();
        auto const recognizerSources = document.recognizerSources();
        UF_CHECK_MSG(
            recognizers.size() == recognizerSources.size(),
            "authoring recognizer source closure is inconsistent"
        );
        auto recognizerWork = std::vector<RecognizerWork>{};
        recognizerWork.reserve(recognizers.size());
        for (auto index = std::size_t{0}; index < recognizers.size(); ++index)
        {
            auto const& recognizer   = checkedAt(recognizers, index);
            auto const& relationship = checkedAt(recognizerSources, index);
            UF_CHECK_MSG(
                relationship.recognizerId == recognizer.id(),
                "authoring recognizer source order is inconsistent"
            );
            recognizerWork.emplace_back(
                RecognizerWork{
                    .sourceId        = relationship.sourceId,
                    .recognizerIndex = index,
                }
            );
        }
        std::ranges::sort(
            recognizerWork,
            [](RecognizerWork const& left, RecognizerWork const& right) noexcept
            {
                if (left.sourceId != right.sourceId)
                {
                    return left.sourceId.value() < right.sourceId.value();
                }
                return left.recognizerIndex < right.recognizerIndex;
            }
        );
        UF_TRY_VALUE(
            templateTasks,
            prepareTemplateTasks(sources, recognizers, recognizerWork)
        );

        auto runtimeRecognizers = std::vector<RuntimeRecognizerSpec>{};
        auto templateAssets     = std::vector<TemplateAsset>{};
        runtimeRecognizers.reserve(document.catalog().recognizers().size());
        templateAssets.reserve(document.catalog().recognizers().size());
        auto compiledTemplateBytes = std::size_t{0};
        auto taskIterator          = templateTasks.begin();
        auto generatedTaskHashes   = GeneratedTemplateHashes{};
        for (auto sourceIndex = std::size_t{0}; sourceIndex < sources.size(); ++sourceIndex)
        {
            auto const& source      = checkedAt(sources, sourceIndex);
            auto const assetIndex   = checkedAt(assetOrder, sourceIndex);
            auto const& sourceAsset = checkedAt(sourceAssets, assetIndex);
            if (sourceAsset.pngBytes.size() > image::k_maximumPngFileBytes)
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} exceeds the PNG byte quota",
                        source.relativePath()
                    )
                );
            }
            UF_TRY_VALUE_CONTEXT(
                actualHash,
                sha256(sourceAsset.pngBytes),
                std::format("hashing authoring source {}", source.relativePath())
            );
            if (actualHash != source.contentHash())
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} content hash does not match its document record",
                        source.relativePath()
                    )
                );
            }
            UF_TRY_VALUE_CONTEXT(
                decoded,
                image::decodePng(
                    sourceAsset.pngBytes,
                    source.relativePath()
                ),
                std::format("decoding authoring source {}", source.relativePath())
            );
            auto const fingerprint = source.fingerprint();
            if (
                decoded.width != fingerprint.width()
                || decoded.height != fingerprint.height()
            )
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} decoded as {}x{}, expected {}x{}",
                        source.relativePath(),
                        decoded.width,
                        decoded.height,
                        fingerprint.width(),
                        fingerprint.height()
                    )
                );
            }
            auto const stride = checkedMultiply(
                static_cast<std::size_t>(decoded.width),
                k_rgbaBytesPerPixel
            );
            if (!stride)
            {
                return invalidCompilation(
                    "authoring source stride overflowed addressable memory"
                );
            }
            UF_TRY_VALUE_CONTEXT(
                bgraPixels,
                image::rgba8ToBgra8(std::move(decoded.pixels)),
                std::format("converting authoring source {}", source.relativePath())
            );
            while (
                taskIterator != templateTasks.end()
                && taskIterator->sourceId == source.id()
            )
            {
                UF_TRY_VALUE_CONTEXT(
                    generated,
                    generateTemplateAsset(
                        bgraPixels,
                        decoded.width,
                        decoded.height,
                        *stride,
                        taskIterator->templateRect
                    ),
                    std::format(
                        "generating template [{}, {}, {}, {}] from source {}",
                        taskIterator->templateRect.x(),
                        taskIterator->templateRect.y(),
                        taskIterator->templateRect.width(),
                        taskIterator->templateRect.height(),
                        source.relativePath()
                    )
                );
                if (generated.pngBytes.size() > image::k_maximumPngFileBytes)
                {
                    return invalidCompilation(
                        std::format(
                            "generated template [{}, {}, {}, {}] from source {} exceeds the PNG byte quota",
                            taskIterator->templateRect.x(),
                            taskIterator->templateRect.y(),
                            taskIterator->templateRect.width(),
                            taskIterator->templateRect.height(),
                            source.relativePath()
                        )
                    );
                }

                auto const generatedHash = generated.hash;
                auto const existing      = std::ranges::find(
                    templateAssets,
                    generatedHash,
                    &TemplateAsset::hash
                );
                if (existing == templateAssets.end())
                {
                    auto const nextTotal = checkedAdd(
                        compiledTemplateBytes,
                        generated.pngBytes.size()
                    );
                    if (
                        !nextTotal
                        || *nextTotal > k_maximumCompiledTemplateBytes
                    )
                    {
                        return invalidCompilation(
                            "compiled templates exceed the 512 MiB project quota"
                        );
                    }
                    compiledTemplateBytes = *nextTotal;
                    templateAssets.emplace_back(std::move(generated));
                }
                else if (existing->pngBytes != generated.pngBytes)
                {
                    return invalidCompilation(
                        "distinct template bytes produced the same content hash"
                    );
                }

                auto const insertion = generatedTaskHashes.emplace(
                    *taskIterator,
                    generatedHash
                );
                UF_CHECK_MSG(
                    insertion.second,
                    "authoring template task was generated more than once"
                );
                ++taskIterator;
            }
        }
        UF_CHECK_MSG(
            taskIterator == templateTasks.end(),
            "authoring recognizer source closure references an unknown source"
        );
        // Per-page search ROIs reach the runtime here, at generation time. The
        // derived catalog() stays one-recognizer-per-element -- it is the UI's
        // read model, and expanding it there would surface synthetic per-page
        // recognizers as phantom rows -- so the expansion lives in the compiler,
        // built from the v2 truth (elements + placements). An element placed on N
        // pages becomes N runtime recognizers, each carrying that page's own ROI
        // and allowed page. Templates still dedupe by (source, template rect), so
        // the N recognizers of one element share a single template asset.
        auto const elements   = document.elements();
        auto const placements = document.placements();
        for (auto const& element : elements)
        {
            auto const generated = generatedTaskHashes.find(
                TemplateTaskKey{
                    .sourceId     = element.sourceId(),
                    .templateRect = element.templateRect(),
                }
            );
            UF_CHECK_MSG(
                generated != generatedTaskHashes.end(),
                "authoring recognizer template task was not generated"
            );
            auto const* p_source = document.findSource(element.sourceId());
            UF_CHECK_MSG(
                p_source != nullptr,
                "authoring recognizer source closure references an unknown source"
            );
            auto const templateHash = generated->second;
            auto const sourceHash   = p_source->contentHash();

            auto defaultClick = std::optional<TemplateOffset>{};
            if (
                auto const* p_interactive = std::get_if<InteractiveElement>(
                    &element.kind()
                )
            )
            {
                defaultClick = p_interactive->clickOffset;
            }

            // The placements naming this element, in canonical order: placements()
            // is sorted by page then element, so filtering by element preserves
            // that order.
            auto elementPlacements = std::vector<AuthoringPlacement>{};
            for (auto const& placement : placements)
            {
                if (placement.elementId == element.id())
                {
                    elementPlacements.emplace_back(placement);
                }
            }

            auto buildSpec =
                [&](
                    RecognizerId id,
                    ResourceName name,
                    PixelRect searchRoi,
                    std::vector<PageId> allowedPageIds
                ) -> Result<RuntimeRecognizerSpec>
            {
                UF_TRY_VALUE(
                    definition,
                    RecognizerDefinition::create(
                        document.catalog().fingerprint(),
                        RecognizerSpec{
                            .id             = id,
                            .name           = std::move(name),
                            .annotationType = element.annotationType(),
                            .templateRect   = element.templateRect(),
                            .searchRoi      = searchRoi,
                            .threshold      = element.threshold(),
                            .defaultClick   = defaultClick,
                            .allowedPageIds = std::move(allowedPageIds),
                        }
                    )
                );
                return RuntimeRecognizerSpec{
                    .definition   = std::move(definition),
                    .templateHash = templateHash,
                    .sourceHash   = sourceHash,
                };
            };

            if (elementPlacements.size() <= 1U)
            {
                // Anchor, unplaced element, or a single placement -- one runtime
                // recognizer keeping the element's own id and name, exactly as the
                // derived catalog carries it today. A single placement contributes
                // its ROI and page; for migrated data that ROI equals the
                // element's own, so the manifest is byte-identical to before.
                auto searchRoi      = element.searchRoi();
                auto allowedPageIds = std::vector<PageId>{};
                if (!elementPlacements.empty())
                {
                    searchRoi = elementPlacements.front().searchRoi;
                    allowedPageIds.emplace_back(elementPlacements.front().pageId);
                }
                UF_TRY_VALUE(
                    spec,
                    buildSpec(
                        element.id(),
                        element.name(),
                        searchRoi,
                        std::move(allowedPageIds)
                    )
                );
                runtimeRecognizers.emplace_back(std::move(spec));
            }
            else
            {
                // N placements -> N runtime recognizers, one per page, each with
                // its own search ROI. Deterministic ids and names keep the
                // manifest stable across compiles of the same document.
                for (auto const& placement : elementPlacements)
                {
                    auto const* p_page = document.catalog().findPage(
                        placement.pageId
                    );
                    UF_CHECK_MSG(
                        p_page != nullptr,
                        "authoring placement references an unknown page"
                    );
                    UF_TRY_VALUE(
                        name,
                        derivedRecognizerName(element.name(), p_page->name())
                    );
                    UF_TRY_VALUE(
                        spec,
                        buildSpec(
                            derivedRecognizerId(element.id(), placement.pageId),
                            std::move(name),
                            placement.searchRoi,
                            std::vector<PageId>{placement.pageId}
                        )
                    );
                    runtimeRecognizers.emplace_back(std::move(spec));
                }
            }
        }
        std::ranges::sort(
            templateAssets,
            {},
            &TemplateAsset::relativePath
        );

        auto pages = std::vector<PageSignature>{
            document.catalog().pages().begin(),
            document.catalog().pages().end()
        };
        UF_TRY_VALUE_CONTEXT(
            runtimeManifest,
            RuntimeManifest::create(
                document.catalog().projectId(),
                document.catalog().fingerprint(),
                std::move(runtimeRecognizers),
                std::move(pages)
            ),
            "compiling the annotation runtime manifest"
        );
        auto runtimeManifestToml = serializeRuntimeManifest(runtimeManifest);
        return CompiledAuthoringProject{
            .runtimeManifest     = std::move(runtimeManifest),
            .runtimeManifestToml = std::move(runtimeManifestToml),
            .templateAssets      = std::move(templateAssets),
        };
    }
}
