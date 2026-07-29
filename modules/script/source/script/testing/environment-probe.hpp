#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <optional>
#include <string_view>

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
    // its numeric result.
    //
    // It exists so the isolation claim can be stated as a difference rather than
    // an absence. Asserting only that a project script cannot see a framework
    // value proves nothing on its own -- the same assertion passes if the
    // framework never ran. Running the SAME expression on both sides turns it
    // into a real discriminator: the framework side must find the value the
    // project side must not.
    //
    // Luau-free header; not part of the public Engine surface, because nothing
    // in production may run arbitrary source under the framework environment.
    [[nodiscard]]
    auto runInEnvironment(
        std::string_view frameworkSource,
        std::string_view source,
        ProbeEnvironment environment
    ) -> Result<double>;

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
        // closed rather than abandoned -- lua_close runs every userdata
        // destructor, and nothing else in this probe can. A leaked VM leaves it
        // at zero.
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
