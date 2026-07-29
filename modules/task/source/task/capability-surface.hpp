#pragma once

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <annotation/catalog.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace uf::task
{
    class TaskContext;

    // One action target the catalog exposes to scripts under
    // uf.recognizers.<name>. It pairs the recognizer's validated Luau member-key
    // name with its stable identity; the name becomes the table key and the id
    // is baked opaquely into the userdata handle. A plain value type, freely
    // copyable, so the installer can own its own snapshot.
    struct RecognizerHandleSpec final
    {
        std::string              name;
        annotation::RecognizerId id;
    };

    // One page the catalog exposes to scripts under uf.pages.<name>, with the
    // same shape and role as RecognizerHandleSpec.
    struct PageHandleSpec final
    {
        std::string        name;
        annotation::PageId id;
    };

    // Builds and owns the two surfaces one project's recognition catalog gives a
    // task VM, and keeps them apart.
    //
    // The PUBLIC one is data: the recursively read-only uf.recognizers and
    // uf.pages name tables of opaque handles, plus the uf.errors table of
    // error-kind constants. It is a global a project script may name, because
    // naming a recognizer confers nothing -- a handle is an identity, not a
    // capability.
    //
    // The PRIVATE one is capability: the observation-cycle primitives. It is
    // never registered as a global and never becomes a key of any table either
    // environment can reach; the boot hands it to the trusted framework as a
    // chunk argument, so it survives only as a closure upvalue there.
    //
    // Construction validates every exposed name (fail-closed) and captures the
    // {name, id} pairs.
    //
    // uf.errors takes no catalog input: it is one string constant per
    // AutomationErrorKind, keyed and valued by that kind's domain wire spelling,
    // built at install time from the same function the trace and a Tier B error
    // use.
    //
    // No Luau type appears in this header: the Luau work lives behind the ffi
    // boundary and is reached only through the returned installers.
    class CapabilitySurface final
    {
        std::vector<RecognizerHandleSpec> m_recognizers;
        std::vector<PageHandleSpec>       m_pages;

        CapabilitySurface(
            std::vector<RecognizerHandleSpec> recognizers,
            std::vector<PageHandleSpec> pages
        ) noexcept;

    public:
        // Enumerates the catalog's action-target recognizers and its pages,
        // checks each exposed name is unique within its table, and captures the
        // {name, id} pairs. A duplicate name fails InvalidResource rather than
        // silently overwriting a handle (annotation-design 3.4). Names are
        // already valid direct Luau member keys by annotation::ResourceName's
        // construction invariant, so the surface never observes an illegal key.
        //
        // Page anchors never enter uf.recognizers: scripts reference anchors
        // only through pages, and frame:find authorizes action targets alone. An
        // info_region recognizer has no script verb in this wave, so it is not
        // exposed either; it joins uf.recognizers when its read verb lands.
        [[nodiscard]]
        static auto create(
            annotation::RecognitionCatalog const& catalog
        ) -> Result<CapabilitySurface>;

        // The global names either installer registers, in the shape
        // script::EngineConfig::projectGlobals takes. It is what carries the
        // uf root across the boundary the project environment draws: that
        // environment is an explicit whitelist with no __index chain to the main
        // globals, so a name the installer wrote as a global is invisible to a
        // project script until it is listed here.
        //
        // Static because it describes the installer, not one catalog, and it is
        // needed at the same call site that builds the EngineConfig. A name
        // listed here that the installer did not register fails VM creation, so
        // the two statements cannot silently disagree.
        [[nodiscard]]
        static auto projectGlobals() -> std::vector<std::string>;

        // The decoder script::EngineConfig::classifyRaisedError takes: it reads
        // the automation kind out of a Tier B error carrier a run raised and
        // nobody caught, so the run is reported and traced under the kind that
        // actually failed rather than as a malformed script.
        //
        // It decides on the carrier's userdata tag alone. That is the whole
        // reason the carrier is host-minted userdata: a table forged by a
        // project script carries no tag, so it can name no kind here however
        // exactly it copies a real error's fields.
        //
        // Static for the same reason projectGlobals() is -- the carrier's shape
        // is a property of the host, not of one catalog.
        [[nodiscard]]
        static auto raisedErrorClassifier() -> script::RaisedErrorClassifier;

        // A host-table installer suitable for script::EngineConfig::installHostTables.
        // Invoked once per task VM before the sandbox freezes the globals, it
        // builds the frozen global uf table: uf.recognizers, uf.pages and
        // uf.errors. The returned installer owns its own copy of the handle
        // specs, so it stays valid independently of this surface's lifetime.
        //
        // It takes no TaskContext because none of what it builds can act. That
        // is the point of the split: the data surface is the same table whether
        // or not a session is bound.
        [[nodiscard]]
        auto installer() const -> script::HostTableInstaller;

        // The private capability surface for a live task, suitable for
        // script::EngineConfig::installPrivateCapabilities: the observation-cycle
        // primitives (cycle_open / cycle_close / cycle_page / cycle_find /
        // cycle_click, plus wait_for_page, now and random), each bound to
        // `context`'s EngineSession and its cycle ledger. It also registers the
        // handle metatables only a bound session can mint -- the cycle ticket,
        // the hit, and the resolved page with its page:is method -- and carries
        // the Tier B error label to the framework, which is the only piece of
        // the surface that is data rather than capability.
        //
        // The returned installer captures a raw pointer to `context`. The caller
        // MUST keep `context` alive for at least as long as the script::Engine the
        // installer configures, because the VM's host functions dereference that
        // pointer on every primitive call; the TaskContext is non-movable so the
        // address stays stable.
        //
        // Static for the same reason projectGlobals() is: the primitives take
        // host-minted handles and scalars only, so nothing about them varies with
        // one catalog. The catalog decides which handles exist, never what a
        // primitive can do with one.
        [[nodiscard]]
        static auto privateCapabilities(TaskContext& context)
            -> script::PrivateCapabilityInstaller;

        [[nodiscard]]
        auto recognizerCount() const noexcept -> std::size_t;

        [[nodiscard]]
        auto pageCount() const noexcept -> std::size_t;

        // The action-target recognizer handles this surface exposes under
        // uf.recognizers, in catalog order. These are exactly the names a
        // uf.recognizers.<name> literal may resolve against, so the pre-VM
        // script validator (script-validator.hpp) checks every reference here
        // rather than against the wider catalog, which also holds page anchors
        // that never become findable handles. The returned span borrows this
        // surface's storage and stays valid only while the surface is alive.
        [[nodiscard]]
        auto recognizers() const noexcept UF_LIFETIME_BOUND
            -> std::span<RecognizerHandleSpec const>;

        // The page handles this surface exposes under uf.pages, the resolution
        // set for every uf.pages.<name> literal, with the same borrow contract
        // as recognizers().
        [[nodiscard]]
        auto pages() const noexcept UF_LIFETIME_BOUND -> std::span<PageHandleSpec const>;
    };
}
