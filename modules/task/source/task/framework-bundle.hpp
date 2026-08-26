#pragma once

#include <core/error/result.hpp>

#include <script/engine.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // One module of the trusted Luau framework, embedded into the binary at build
    // time by scripts/embed_luau.py from modules/task/runtime.
    //
    // Lifetime contract: every view below borrows a string literal with static
    // storage duration inside the generated translation unit, so an entry and its
    // views stay valid for the whole process and may be stored without further
    // arrangement. This is the deliberate exception to the project's "do not store
    // views" rule -- the backing owner is the program image itself.
    struct FrameworkBundleEntry final
    {
        // The Luau module name: the .luau file stem, never a path.
        std::string_view name{};

        // The module's UTF-8 source text, byte for byte as it is on disk.
        std::string_view source{};

        // Lowercase hex SHA-256 of `source`, computed by the generator at build
        // time. tests/task/test-framework-bundle.cpp recomputes it with sha256,
        // which keeps the build-time and run-time hash definitions from drifting.
        std::string_view sourceHash{};
    };

    // The framework bundle, ordered by the generator's sort over the source
    // paths, so the sequence is identical on every machine and every build.
    [[nodiscard]]
    auto frameworkBundleEntries() noexcept -> std::span<FrameworkBundleEntry const>;

    // Lowercase hex SHA-256 over the whole embedded Luau release: every module's
    // bytes AND the linkage the release declares for it.
    //
    // Recipe, recomputable by hand. For each entry of frameworkBundleEntries(),
    // in that span's order, append these ten items in this order -- five
    // fields, each followed by its own terminator:
    //
    //   1. the module's publication name (the .luau file stem) in UTF-8;
    //   2. one 0x00 byte;
    //   3. the module's declaration tier, one of the ASCII tokens `pure`,
    //      `internal-pure`, `trusted`, `scoped`, or `undeclared` when no table
    //      in framework-bundle.cpp declares the module;
    //   4. one 0x00 byte;
    //   5. the module's reserved resolver alias in UTF-8, empty when the tier is
    //      `undeclared`;
    //   6. one 0x00 byte;
    //   7. the module's declared dependency depth as unpadded ASCII decimal
    //      with no sign;
    //   8. one 0x00 byte;
    //   9. the module's source bytes exactly as they are on disk;
    //  10. one 0x00 byte.
    //
    // The bundle hash is the SHA-256 of that concatenation, in lowercase hex.
    // Every field is NUL-terminated rather than NUL-separated: a source may be
    // empty, and with separators alone the boundary between one entry's source
    // and the next entry's name would not be recoverable from the byte string.
    //
    // Three of those fields are not the module's bytes, and they are the point.
    // A reserved alias, a dependency depth, and the table a module is declared
    // in are all release linkage that lives in C++ rather than in the .luau file
    // -- docs/standards/luau.md fixes that -- so a bundle that hashed sources
    // alone could ship a renamed alias, a reordered depth, or a trusted-only
    // module newly exposed to every Project VM under an unmoved digest.
    //
    // This is deliberately NOT a second spelling of `plugin_environment_hash`.
    // That digest is registration identity: it covers the closure a Project
    // executes in -- the pure SDK tier's names, source hashes, visibility and
    // depths, plus the release-owned limit and contract literals -- and refuses
    // a registration whose derived identity differs. It says nothing about the
    // modules a Project can never resolve. This digest is release-build
    // identity: it covers all four tiers of the embedded bundle, including the
    // trusted-only RuntimeModel modules and the four scoped facades, neither of
    // which enters any registration-level digest today. A change to the alias
    // of `@umbraflow/internal/observe` moves this and moves nothing else in the
    // tree.
    //
    // It stamps a trace so one run is attributable to an exact framework build,
    // and catches a bundle that went accidentally stale. It is NOT a security
    // property and must not be described as one: the digest is compiled into the
    // same binary as the bytes it certifies, so anything able to change those
    // bytes is equally able to change the digest that certifies them.
    //
    // It returns a fresh string rather than a static view because the digest is
    // computed here, from the bundle beside the declaration tables, rather than
    // baked into the generated translation unit: the tables the recipe reads are
    // C++ and the generator cannot see them.
    [[nodiscard]]
    auto frameworkBundleHash() -> Result<std::string>;

    // The framework's semantic version, declared as [embed].luau_version in
    // modules/task/manifest.txt and stamped into the bundle at build time.
    [[nodiscard]]
    auto frameworkVersion() noexcept -> std::string_view;

    // The same bundle, in the shape script::EngineConfig::frameworkModules takes,
    // so every VM that boots this framework builds the list one way. The views it
    // carries are the bundle's own, so they live for the whole process and the
    // returned vector may be handed to a config that outlives this call. Only the
    // vector itself is fresh, and this runs once per VM.
    [[nodiscard]]
    auto frameworkScriptModules() -> std::vector<script::FrameworkModule>;

    // The pure SDK closure admitted to pure-program VMs. Public names
    // use their reserved resolver spelling; internal release-owned data modules
    // use reserved names that only another Framework module may resolve. Source
    // views still borrow the generated static literals.
    [[nodiscard]]
    auto pureFrameworkScriptModules()
        -> Result<std::vector<script::FrameworkModule>>;

    // The Framework closure a script::ScopedToolProgram or
    // script::ScopedToolSession admits: the whole pure SDK plus the scoped
    // modules under their reserved names. The scoped ones are project-visible
    // because both program shapes refuse a catalog whose modules Project code
    // cannot resolve -- a Tool face nothing can require is a native seam with
    // no caller.
    //
    // Deliberately NOT a superset that any other environment may take: this list
    // is the only place they are admitted, and pureFrameworkScriptModules()
    // and frameworkScriptModules() both exclude them, so a pure program's require
    // fails in the resolver naming the module and no trusted Engine VM ever runs
    // their source.
    [[nodiscard]]
    auto scopedFrameworkScriptModules()
        -> Result<std::vector<script::FrameworkModule>>;

    // The read-only JSON resource name the host bakes a run's pinned Tool
    // catalog into. Re-exported from script::scopedToolCatalogResourceName so
    // the host that bakes those bytes names them exactly as the scoped program
    // type that reads them does; a bundle test binds it to the spelling inside
    // the embedded `catalog` module as well, which is the one place a Luau
    // source cannot read the C++ constant.
    [[nodiscard]]
    auto scopedToolCatalogResourceName() noexcept -> std::string_view;

    // Business execution is closed until OperatorSession exists, so this list is
    // deliberately empty. Loading trusted modules into a VM does not make their
    // exports project-visible; publication remains an explicit whitelist.
    [[nodiscard]]
    auto frameworkProjectGlobals() -> std::vector<std::string>;

    // What the Host's trusted RuntimeModel resolver publishes: the modules that
    // read a pinned model and resolve a state, and nothing that acts. This is
    // module wiring rather than an explore/runtime authority split: interactive
    // code never boots this environment, and the resolver has no Tool Runtime
    // primitive to call.
    [[nodiscard]]
    auto runtimeProjectGlobals() -> std::vector<std::string>;

    // Parses one framework module with the same vendored Luau parser the host uses
    // at load time, and reports the first syntax error together with its line and
    // column. `chunkName` only labels the diagnostic.
    //
    // This is the repository's syntax gate for .luau sources: the bundle test runs
    // it over every embedded entry, so a malformed framework module fails under
    // `ctest -L CI` rather than at the VM. A failure is InternalInvariant rather
    // than InvalidResource, because the framework is first-party and compiled into
    // the binary -- a syntax error there is a broken host, not bad user input.
    //
    // No Luau type appears in this header; the parse lives behind the module's ffi
    // boundary.
    [[nodiscard]]
    auto checkFrameworkModuleSyntax(
        std::string_view source,
        std::string_view chunkName
    ) -> Status;
}
