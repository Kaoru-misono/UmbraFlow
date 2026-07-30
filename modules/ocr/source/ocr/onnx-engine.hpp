#pragma once

#include "engine.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <memory>

namespace uf::ocr
{
    // How many threads one engine may use inside a single inference call.
    //
    // CALIBRATION: four is a conservative placeholder measured against the first
    // real frames, where one line cost 2-13 ms. It is a ceiling on latency
    // rather than a throughput setting: this engine is called from a run's own
    // thread, inside an observation whose frame carries a 750 ms lease, so the
    // budget that matters is how long one call blocks that run.
    inline constexpr auto k_defaultOcrThreadCount = uint32{4};

    // Where one engine's model files live, and how much of itself it is allowed
    // to use.
    //
    // The paths are the caller's because binding an engine to a filesystem
    // layout is a composition-root job, exactly as binding a capture target is.
    // modules/ocr ships the port and the adapter; it does not decide where a
    // product keeps its weights.
    struct OnnxEngineConfig final
    {
        // The recognition model and the file carrying its character dictionary
        // and input geometry. They are separate paths because they are separate
        // files in the release, but they are one decision: a dictionary from a
        // different model spells every output wrong while failing nothing.
        std::filesystem::path recognitionModel{};
        std::filesystem::path recognitionConfig{};

        uint32 threadCount{k_defaultOcrThreadCount};
    };

    // Builds an engine over ONNX Runtime.
    //
    // Every failure a model can present happens here: a missing file, a model
    // whose input is not the shape this adapter feeds, a config with no readable
    // dictionary. That is deliberate -- a successfully created engine has
    // already proved it can run, so a later read fails only on its own inputs.
    //
    // Returns the port rather than the concrete type, so nothing above this line
    // depends on which runtime is behind it. Declared unconditionally and
    // defined only where an adapter exists; a platform without one fails to
    // link rather than silently gaining a different engine.
    [[nodiscard]]
    auto createOnnxEngine(
        OnnxEngineConfig const& config
    ) -> Result<std::unique_ptr<IOcrEngine>>;
}
