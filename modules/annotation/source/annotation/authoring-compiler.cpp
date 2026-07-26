#include "authoring-compiler.hpp"

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
        for (auto const& work : recognizerWork)
        {
            auto const& recognizer = checkedAt(
                recognizers,
                work.recognizerIndex
            );
            auto const generated = generatedTaskHashes.find(
                TemplateTaskKey{
                    .sourceId     = work.sourceId,
                    .templateRect = recognizer.templateRect(),
                }
            );
            UF_CHECK_MSG(
                generated != generatedTaskHashes.end(),
                "authoring recognizer template task was not generated"
            );
            auto const* p_source = document.findSource(work.sourceId);
            UF_CHECK_MSG(
                p_source != nullptr,
                "authoring recognizer source closure references an unknown source"
            );
            runtimeRecognizers.emplace_back(
                RuntimeRecognizerSpec{
                    .definition   = recognizer,
                    .templateHash = generated->second,
                    .sourceHash   = p_source->contentHash(),
                }
            );
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
