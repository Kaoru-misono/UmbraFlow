#include "invoke.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

namespace uf::cli
{
    // The composition binds a live Windows target through the controller
    // module, which only builds on Windows. Other hosts fail before opening
    // resources, exactly as observeProduct does.
    //
    // invokeTool is deliberately not split: it takes its ports as arguments, so
    // every host compiles the whole Tool Runtime path and only the binding of a
    // live desktop is missing here.
    auto invokeToolProduct(InvokeArgs const&) -> Result<ToolInvokeReport>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow invoke is unsupported on this host"
        );
    }
}
