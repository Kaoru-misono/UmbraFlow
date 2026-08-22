#include "invoke.hpp"

#include "observe.hpp"
#include "platform/ocr-engine-binding.hpp"
#include "platform/target-binding.hpp"

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <utility>

namespace uf::cli
{
    auto invokeToolProduct(InvokeArgs const& args) -> Result<ToolInvokeReport>
    {
        // Before any window is enumerated, as observe and explore do it: a
        // model directory that will not build an engine must fail before a
        // capture session exists.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));
        if (!ocrEngine)
        {
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "the OCR model directory built no engine, so no Reader this "
                "project's model declares could run"
            );
        }

        UF_TRY_VALUE(bound, platform::bindTarget(WindowHandle{args.windowHandle}));

        return invokeTool(
            args,
            ObserveSources{
                .frameSource     = std::move(bound.frameSource),
                .actionSink      = std::move(bound.actionSink),
                .ocrEngine       = std::move(ocrEngine),
                .liveFingerprint = bound.liveFingerprint,
            }
        );
    }
}
