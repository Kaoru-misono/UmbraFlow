#pragma once

#include <script/engine.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::script::testing
{
    // Which of a booted VM's two environments a probe run executes in.
    enum class ProbeEnvironment : uint8
    {
        Framework,
        Project,
    };

    // Test seam: boot a VM whose framework bundle is the single module `probe`
    // with body `frameworkSource`, then run `source` in `environment` and return
    // its numeric result. It states the isolation claim as a difference rather
    // than an absence: asserting only that a project script cannot see a
    // framework value passes just as well if the framework never ran, whereas
    // the SAME expression on both sides discriminates -- the framework side must
    // find the value the project side must not.
    //
    // Luau-free header; not part of the public Engine surface, because nothing
    // in production may run arbitrary source under the framework environment.
    [[nodiscard]]
    auto runInEnvironment(
        std::string_view frameworkSource,
        std::string_view source,
        ProbeEnvironment environment
    ) -> Result<double>;

    // Every name a chunk running under the trusted framework environment can
    // reach: that environment's own keys plus the main globals its __index
    // chains to, sorted and deduplicated.
    struct FrameworkGlobalsProbe final
    {
        // The same boot with nothing to load, which is the baseline: the
        // standard library the sandbox left standing plus whatever the host
        // installer registered.
        std::vector<std::string> beforeLoad{};

        // The same boot after the given modules loaded. Equality with
        // `beforeLoad` is the invariant: loading a Framework module must add no
        // name to any VM's global namespace.
        std::vector<std::string> afterLoad{};
    };

    // Test seam: boot the real sandbox twice -- once with no framework modules
    // and once with `modules` -- and report the reachable global names each boot
    // left. Both boots run the production installSandbox, so the baseline is the
    // same machine and not a reimplementation of its ordering.
    [[nodiscard]]
    auto probeFrameworkGlobals(std::span<FrameworkModule const> modules)
        -> Result<FrameworkGlobalsProbe>;

    // What booting a VM whose host-table installer fails left behind.
    struct InstallerFailureProbe final
    {
        // The failure Engine::create reported, or empty if create unexpectedly
        // succeeded. The test compares it against the error the installer
        // returned, so "create fails with the installer's own error" is checked
        // rather than merely "create fails".
        std::optional<Error> failure{};

        // Times the VM ran the destructor of a host userdata the failing
        // installer registered before returning its error. Exactly one is
        // host-visible proof that the VM create() had already allocated was
        // closed rather than abandoned: only lua_close runs that destructor, so
        // a leaked VM leaves this at zero.
        uint64 finalizedHostObjects{0};
    };

    // Test seam: run Engine::create with an installer that registers one
    // finalizable host object and then fails with `kind` and `message`.
    [[nodiscard]]
    auto probeInstallerFailure(
        AutomationErrorKind kind,
        std::string_view message
    ) -> InstallerFailureProbe;
}
