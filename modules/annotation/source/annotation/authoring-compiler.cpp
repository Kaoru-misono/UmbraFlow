#include "authoring-compiler.hpp"

#include "resource.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
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
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_rgbaBytesPerPixel            = std::size_t{4};
        constexpr auto k_maximumCompiledTemplateBytes = std::size_t{512} * 1024U * 1024U;
        constexpr auto k_maximumCompilationPixelWork  = std::size_t{256} * 1024U * 1024U;

        // The colour key is part of the identity of a template, not a property
        // of one: the same rectangle of the same screen masked two different
        // ways is two different template images, and each needs its own asset.
        struct TemplateTaskKey final
        {
            SourceId                 sourceId;
            PixelRect                templateRect;
            std::optional<ColourKey> colourKey{};
        };

        // ColourKey carries equality but no order, because nothing about colours
        // is ordered; this is a sort key and only has to be total and stable.
        [[nodiscard]]
        auto colourKeyOrder(
            std::optional<ColourKey> const& key
        ) noexcept -> std::tuple<bool, uint32, uint32, uint32, uint32>
        {
            if (!key)
            {
                return {false, 0U, 0U, 0U, 0U};
            }
            return {
                true,
                key->red(),
                key->green(),
                key->blue(),
                key->tolerance()
            };
        }

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

                auto const leftRect = std::tuple{
                    left.templateRect.x(),
                    left.templateRect.y(),
                    left.templateRect.width(),
                    left.templateRect.height()
                };
                auto const rightRect = std::tuple{
                    right.templateRect.x(),
                    right.templateRect.y(),
                    right.templateRect.width(),
                    right.templateRect.height()
                };
                if (leftRect != rightRect)
                {
                    return leftRect < rightRect;
                }
                return colourKeyOrder(left.colourKey) < colourKeyOrder(right.colourKey);
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
        auto taskKeyOf(
            Appearance const& appearance
        ) -> TemplateTaskKey
        {
            return TemplateTaskKey{
                .sourceId     = appearance.sourceId(),
                .templateRect = appearance.templateRect(),
                .colourKey    = appearance.colourKey(),
            };
        }

        [[nodiscard]]
        auto prepareTemplateTasks(
            std::span<AuthoringSource const> sources,
            std::span<Element const> elements
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

            for (auto const& element : elements)
            {
                for (auto const& appearance : element.appearances())
                {
                    auto const insertion = templateTasks.emplace(taskKeyOf(appearance));
                    if (!insertion.second)
                    {
                        continue;
                    }

                    auto const templateRect = appearance.templateRect();
                    auto const taskPixels   = checkedMultiply(
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
            }

            return templateTasks;
        }

        // The template PNG's alpha channel IS the mask the matcher weights by,
        // so baking a colour key means writing that channel and nothing else.
        // Templates are cropped as BGRA, where alpha is the fourth byte.
        [[nodiscard]]
        auto applyColourKeyAlpha(
            std::vector<std::byte> croppedBgra,
            ColourKey const& key
        ) -> std::vector<std::byte>
        {
            auto const pixelCount = croppedBgra.size() / k_rgbaBytesPerPixel;
            for (auto pixel = std::size_t{0}; pixel < pixelCount; ++pixel)
            {
                auto const base  = pixel * k_rgbaBytesPerPixel;
                auto const blue  = std::to_integer<uint8>(checkedAt(croppedBgra, base));
                auto const green = std::to_integer<uint8>(
                    checkedAt(croppedBgra, base + 1U)
                );
                auto const red = std::to_integer<uint8>(
                    checkedAt(croppedBgra, base + 2U)
                );
                checkedAt(croppedBgra, base + 3U) = static_cast<std::byte>(
                    key.alphaFor(red, green, blue)
                );
            }
            return croppedBgra;
        }

        // One template task's pixels. Without a colour key this is exactly the
        // call the compiler has always made, so an unkeyed template's bytes do
        // not move. With one, the crop is masked first and then handed to the
        // same generator as a whole image -- the second crop is the identity,
        // and the caller's error context already names the real rectangle, so
        // nothing is lost by generating it at the origin.
        [[nodiscard]]
        auto generateTaskTemplate(
            std::span<std::byte const> sourceBgra,
            uint32 sourceWidth,
            uint32 sourceHeight,
            std::size_t sourceStride,
            TemplateTaskKey const& task
        ) -> Result<TemplateAsset>
        {
            if (!task.colourKey)
            {
                return generateTemplateAsset(
                    sourceBgra,
                    sourceWidth,
                    sourceHeight,
                    sourceStride,
                    task.templateRect
                );
            }

            UF_TRY_VALUE(
                cropped,
                image::cropBgra8(
                    sourceBgra,
                    sourceWidth,
                    sourceHeight,
                    sourceStride,
                    task.templateRect
                )
            );
            auto const masked = applyColourKeyAlpha(
                std::move(cropped),
                *task.colourKey
            );
            auto const maskedStride = checkedMultiply(
                static_cast<std::size_t>(task.templateRect.width()),
                k_rgbaBytesPerPixel
            );
            if (!maskedStride)
            {
                return invalidCompilation(
                    "masked template stride overflowed addressable memory"
                );
            }
            UF_TRY_VALUE(
                wholeCrop,
                PixelRect::create(
                    0U,
                    0U,
                    task.templateRect.width(),
                    task.templateRect.height()
                )
            );
            return generateTemplateAsset(
                masked,
                task.templateRect.width(),
                task.templateRect.height(),
                *maskedStride,
                wholeCrop
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

        auto const elements = document.elements();
        UF_TRY_VALUE(templateTasks, prepareTemplateTasks(sources, elements));

        auto templateAssets        = std::vector<TemplateAsset>{};
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
                    generateTaskTemplate(
                        bgraPixels,
                        decoded.width,
                        decoded.height,
                        *stride,
                        *taskIterator
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
            "authoring appearance source closure references an unknown source"
        );

        // One compiled element per authored element, under the element's own
        // id. Templates still dedupe by (source, rectangle, colour key), so two
        // elements cutting the same pixels share one asset.
        auto runtimeElements = std::vector<RuntimeElementSpec>{};
        runtimeElements.reserve(elements.size());
        for (auto const& element : elements)
        {
            auto appearances = std::vector<CompiledAppearance>{};
            auto assets      = std::vector<RuntimeAppearanceAsset>{};
            appearances.reserve(element.appearances().size());
            assets.reserve(element.appearances().size());
            for (auto const& appearance : element.appearances())
            {
                auto const generated = generatedTaskHashes.find(taskKeyOf(appearance));
                UF_CHECK_MSG(
                    generated != generatedTaskHashes.end(),
                    "authoring appearance template task was not generated"
                );
                auto const* p_source = document.findSource(appearance.sourceId());
                UF_CHECK_MSG(
                    p_source != nullptr,
                    "authoring appearance source closure references an unknown source"
                );
                appearances.emplace_back(runtimeAppearanceOf(appearance));
                assets.emplace_back(
                    RuntimeAppearanceAsset{
                        .appearanceName  = appearance.name(),
                        .templateHash = generated->second,
                        .sourceHash   = p_source->contentHash(),
                    }
                );
            }

            UF_TRY_VALUE(
                definition,
                CompiledElement::create(
                    document.catalog().fingerprint(),
                    CompiledElementSpec{
                        .id           = element.id(),
                        .name         = element.name(),
                        .capabilities = element.capabilities(),
                        .searchRoi    = element.searchRoi(),
                        .appearances  = std::move(appearances),
                    }
                )
            );
            runtimeElements.emplace_back(
                RuntimeElementSpec{
                    .definition  = std::move(definition),
                    .appearances = std::move(assets),
                }
            );
        }
        std::ranges::sort(
            templateAssets,
            {},
            &TemplateAsset::relativePath
        );

        auto pages = std::vector<PageSpec>{};
        pages.reserve(document.catalog().pages().size());
        for (auto const& page : document.catalog().pages())
        {
            pages.emplace_back(
                PageSpec{
                    .id   = page.id(),
                    .name = page.name(),
                }
            );
        }
        auto references = std::vector<PageReference>{
            document.references().begin(),
            document.references().end()
        };

        UF_TRY_VALUE_CONTEXT(
            runtimeManifest,
            RuntimeManifest::create(
                document.catalog().projectId(),
                document.catalog().fingerprint(),
                std::move(runtimeElements),
                std::move(pages),
                std::move(references)
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
