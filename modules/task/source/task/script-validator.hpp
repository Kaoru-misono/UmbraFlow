#pragma once

#include <core/error/result.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    class CapabilitySurface;

    // The resource references one task script makes, enumerated by the pre-VM
    // AST validator. Both lists are deduplicated and sorted, so a host trace and
    // a test can assert on a stable set independent of source order or repeated
    // uses. A script that touches no resources yields two empty lists and still
    // validates. No Luau type appears here: the parse and traversal live behind
    // the ffi boundary and are reached only through validateScriptResources.
    struct ScriptResourceReport final
    {
        std::vector<std::string> recognizers{};
        std::vector<std::string> pages{};
    };

    // Proves, before any Luau VM is created, that `source` (compiled under chunk
    // name `chunkName`) reaches the umbra capability namespace only through the
    // canonical literal spellings -- umbra.recognizers.<name>,
    // umbra.pages.<name> and umbra.errors.<kind> direct member access -- plus
    // umbra:<verb>(...) method calls, and that every named resource resolves
    // against `surface`. This is the S0 resource closure (annotation-design 4):
    // every reference is enumerated and closed here, so a missing resource is
    // caught before the VM exists; runtime nil remains only defense in depth.
    //
    // An error-kind leaf is resolved against AutomationErrorKind's wire spellings
    // for the same reason, so a typo fails here rather than becoming a nil that
    // silently loses every comparison. It contributes nothing to the report: the
    // kinds are host vocabulary fixed for the binary, not project resources.
    //
    // Every other contact with the umbra global is rejected: a namespace or
    // handle alias (local r = umbra, local r = umbra.recognizers), a computed
    // index (umbra.recognizers[name]), dynamic traversal (pairs(umbra)), or
    // passing/returning it. A syntax error is likewise rejected. Every failure
    // is AutomationErrorKind::InvalidResource and names the offending source
    // location.
    [[nodiscard]]
    auto validateScriptResources(
        std::string_view source,
        std::string_view chunkName,
        CapabilitySurface const& surface
    ) -> Result<ScriptResourceReport>;
}
