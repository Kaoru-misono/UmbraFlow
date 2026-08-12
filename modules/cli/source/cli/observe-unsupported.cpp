#include "observe.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

namespace uf::cli
{
    // The composition binds a live Windows target through the controller module,
    // which only builds on Windows. Other hosts fail before opening resources.
    //
    // observeProject above is deliberately not split: it takes its ports as
    // arguments, so every host compiles the composition itself and only the
    // binding of a live desktop is missing here.
    auto observeProduct(ObserveArgs const&) -> Result<ObservedState>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow observe is unsupported on this host"
        );
    }
}
