#pragma once

#include <core/error/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace uf::project
{
    // The document a project writes to say which framework build it runs on,
    // and the directory that build is installed into. Both are at the root of
    // the source tree, beside umbraflow-project.json.
    inline constexpr auto k_kitConfigName = std::string_view{
        "umbraflow-kit.json"
    };
    inline constexpr auto k_bundleDirectory = std::string_view{
        "umbraflow-bin"
    };

    // What one `project upgrade` was asked to install.
    //
    // An empty releaseOverride is the operator saying nothing on the command
    // line, at which point umbraflow-kit.json's own `release` decides. That
    // member is required and has no default, so the absent case here is never
    // "nobody stated a release" -- it is "the document's statement stands".
    struct ProjectUpgradeSpec final
    {
        std::filesystem::path sourceDirectory{};
        std::string           releaseOverride{};
    };

    // What one completed upgrade installed.
    //
    // projectExecutable is answered because the adaptation report that follows
    // an upgrade has to be the NEW binary's. The schema a declaration is judged
    // against is compiled into the executable, so the process that installed a
    // bundle is the one process that cannot speak for what it installed.
    struct InstalledRelease final
    {
        std::string           release{};
        std::filesystem::path bundleDirectory{};
        std::filesystem::path projectExecutable{};
    };

    // Stages the release the spec selects under <source>/work/release-staging,
    // verifies every artifact this host needs against the sha256 the manifest
    // declares, and only then swaps it into <source>/umbraflow-bin.
    //
    // IT DOES NOT REFUSE ON INCOMPATIBILITY. A release whose shape the
    // project's declaration does not match is installed anyway, because the
    // newly installed binary is the only thing that can tell the author whether
    // an adaptation is correct: refusing to install would leave the author
    // editing blind and re-downloading on every attempt. Install first, report
    // second.
    //
    // THE SWAP IS TWO RENAMES AND STOPS THERE. A directory holding a running
    // executable can be renamed on Windows and cannot be deleted, and the
    // process performing an upgrade is normally running out of the bundle it is
    // replacing. So the previous bundle is renamed aside and left; deleting it
    // is the next `project` invocation's first act, by which time nothing holds
    // it. The leftover is an artifact of that platform constraint and is not a
    // rollback: a bundle is fully reconstructible from an immutable release, so
    // rolling back is pinning `release` to the older name and upgrading again.
    [[nodiscard]]
    auto upgradeReleaseBundle(ProjectUpgradeSpec const& spec)
        -> Result<InstalledRelease>;

    // Brings the bundle directories in one source tree back to one live bundle,
    // and answers what it had to say about it -- empty when there was nothing
    // to do.
    //
    // Two states, and they are told apart by whether a live bundle exists
    // rather than by anything written down:
    //
    // - umbraflow-bin present: every umbraflow-bin.* beside it is the residue
    //   of a completed upgrade and is removed. One that is still held by a
    //   running process is named and left, because the process holding it is
    //   normally the caller's own parent and refusing every command until it
    //   exits would make the tool unusable at the one moment it is needed.
    // - umbraflow-bin absent with exactly one umbraflow-bin.* beside it: an
    //   upgrade was interrupted between its two renames. It is finished by
    //   putting that bundle back, and said so. More than one, and there is
    //   nothing here that can choose; the refusal names them.
    [[nodiscard]]
    auto reconcileBundleDirectories(
        std::filesystem::path const& sourceDirectory
    ) -> Result<std::string>;
}
