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
            SourceId    m_sourceId;
            std::size_t m_recognizerIndex{};
        };

        struct TemplateTaskKey final
        {
            SourceId  m_sourceId;
            PixelRect m_templateRect;
        };

        struct TemplateTaskLess final
        {
            [[nodiscard]]
            auto operator()(
                TemplateTaskKey const& left,
                TemplateTaskKey const& right
            ) const noexcept -> bool
            {
                if (left.m_sourceId != right.m_sourceId)
                {
                    return left.m_sourceId.value() < right.m_sourceId.value();
                }

                return (
                    std::tuple{
                        left.m_templateRect.x(),
                        left.m_templateRect.y(),
                        left.m_templateRect.width(),
                        left.m_templateRect.height()
                    }
                    < std::tuple{
                        right.m_templateRect.x(),
                        right.m_templateRect.y(),
                        right.m_templateRect.width(),
                        right.m_templateRect.height()
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
                    work.m_recognizerIndex
                ).templateRect();
                auto const insertion = templateTasks.emplace(
                    TemplateTaskKey{
                        .m_sourceId     = work.m_sourceId,
                        .m_templateRect = templateRect,
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
                return checkedAt(sourceAssets, index).m_id.value();
            }
        );
        for (auto index = std::size_t{0}; index < sources.size(); ++index)
        {
            auto const& source    = checkedAt(sources, index);
            auto const assetIndex = checkedAt(assetOrder, index);
            auto const& asset     = checkedAt(sourceAssets, assetIndex);
            if (asset.m_id != source.id())
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
                relationship.m_recognizerId == recognizer.id(),
                "authoring recognizer source order is inconsistent"
            );
            recognizerWork.emplace_back(
                RecognizerWork{
                    .m_sourceId        = relationship.m_sourceId,
                    .m_recognizerIndex = index,
                }
            );
        }
        std::ranges::sort(
            recognizerWork,
            [](RecognizerWork const& left, RecognizerWork const& right) noexcept
            {
                if (left.m_sourceId != right.m_sourceId)
                {
                    return left.m_sourceId.value() < right.m_sourceId.value();
                }
                return left.m_recognizerIndex < right.m_recognizerIndex;
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
            if (sourceAsset.m_pngBytes.size() > image::k_maximumPngFileBytes)
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
                sha256(sourceAsset.m_pngBytes),
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
                    sourceAsset.m_pngBytes,
                    source.relativePath()
                ),
                std::format("decoding authoring source {}", source.relativePath())
            );
            auto const fingerprint = source.fingerprint();
            if (
                decoded.m_width != fingerprint.width()
                || decoded.m_height != fingerprint.height()
            )
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} decoded as {}x{}, expected {}x{}",
                        source.relativePath(),
                        decoded.m_width,
                        decoded.m_height,
                        fingerprint.width(),
                        fingerprint.height()
                    )
                );
            }
            auto const stride = checkedMultiply(
                static_cast<std::size_t>(decoded.m_width),
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
                image::rgba8ToBgra8(std::move(decoded.m_pixels)),
                std::format("converting authoring source {}", source.relativePath())
            );
            while (
                taskIterator != templateTasks.end()
                && taskIterator->m_sourceId == source.id()
            )
            {
                UF_TRY_VALUE_CONTEXT(
                    generated,
                    generateTemplateAsset(
                        bgraPixels,
                        decoded.m_width,
                        decoded.m_height,
                        *stride,
                        taskIterator->m_templateRect
                    ),
                    std::format(
                        "generating template [{}, {}, {}, {}] from source {}",
                        taskIterator->m_templateRect.x(),
                        taskIterator->m_templateRect.y(),
                        taskIterator->m_templateRect.width(),
                        taskIterator->m_templateRect.height(),
                        source.relativePath()
                    )
                );
                if (generated.m_pngBytes.size() > image::k_maximumPngFileBytes)
                {
                    return invalidCompilation(
                        std::format(
                            "generated template [{}, {}, {}, {}] from source {} exceeds the PNG byte quota",
                            taskIterator->m_templateRect.x(),
                            taskIterator->m_templateRect.y(),
                            taskIterator->m_templateRect.width(),
                            taskIterator->m_templateRect.height(),
                            source.relativePath()
                        )
                    );
                }

                auto const generatedHash = generated.m_hash;
                auto const existing      = std::ranges::find(
                    templateAssets,
                    generatedHash,
                    &TemplateAsset::m_hash
                );
                if (existing == templateAssets.end())
                {
                    auto const nextTotal = checkedAdd(
                        compiledTemplateBytes,
                        generated.m_pngBytes.size()
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
                else if (existing->m_pngBytes != generated.m_pngBytes)
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
                work.m_recognizerIndex
            );
            auto const generated = generatedTaskHashes.find(
                TemplateTaskKey{
                    .m_sourceId     = work.m_sourceId,
                    .m_templateRect = recognizer.templateRect(),
                }
            );
            UF_CHECK_MSG(
                generated != generatedTaskHashes.end(),
                "authoring recognizer template task was not generated"
            );
            auto const* p_source = document.findSource(work.m_sourceId);
            UF_CHECK_MSG(
                p_source != nullptr,
                "authoring recognizer source closure references an unknown source"
            );
            runtimeRecognizers.emplace_back(
                RuntimeRecognizerSpec{
                    .m_definition   = recognizer,
                    .m_templateHash = generated->second,
                    .m_sourceHash   = p_source->contentHash(),
                }
            );
        }
        std::ranges::sort(
            templateAssets,
            {},
            &TemplateAsset::m_relativePath
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
            .m_runtimeManifest     = std::move(runtimeManifest),
            .m_runtimeManifestToml = std::move(runtimeManifestToml),
            .m_templateAssets      = std::move(templateAssets),
        };
    }
}
