#include <task/capability-surface.hpp>
#include <task/cycle-ledger.hpp>
#include <task/task-context.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>
#include <annotation/recognition.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; a manifest-driven module has no CMakeLists to mark them
// external, so wrap the includes exactly as modules/script's ffi layer does.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <lua.h>
#include <lualib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::task
{
    namespace
    {
        // Each of these strings is one host object kind's __metatable and
        // __tostring label -- the only string a script can obtain from such an
        // object, naming the kind and nothing else (no id, no address). Every
        // label is rooted at `uf`, the same script-visible root the DATA table is
        // registered under, so an object names the surface it came from.
        //
        // The first six are also VM registry keys: a handle kind's metatable is
        // shared by every handle of that kind and is registered under its label.
        // k_errorType is not, because a Tier B error carrier's metatable holds
        // that one error's fields and so exists per instance; the label is still
        // what a script sees, and the label is what the framework compares
        // against, but the carrier's identity to C++ is its userdata tag.
        constexpr auto k_recognizerType   = "uf.recognizer";
        constexpr auto k_pageRefType      = "uf.page";
        constexpr auto k_cycleType        = "uf.cycle";
        constexpr auto k_resolvedPageType = "uf.resolved_page";
        constexpr auto k_hitType          = "uf.hit";
        constexpr auto k_deadlineType     = "uf.deadline";
        constexpr auto k_errorType        = "uf.error";

        // The single global name the DATA installer registers, and therefore the
        // one host name the project environment whitelists. Spelled once here so
        // the registration and the whitelist entry cannot drift apart. The
        // private surface has no counterpart here on purpose: it is registered
        // under no name at all.
        constexpr auto k_ufRoot = "uf";

        // The field of the private capability surface carrying k_errorType to
        // the framework. It is data, not capability, and it is on the surface
        // for one reason: it is the only table the framework is handed and no
        // project script can name, so the label reaches ctx:try without ever
        // being spelled in Luau. The C++ constant above is then the single
        // source of that string -- rename it and the mint, the __metatable, and
        // the framework's comparison all move together, because the framework
        // compares against whatever this hands it.
        constexpr auto k_errorTagField = "error_tag";

        // The Luau userdata tag every Tier B error carrier is minted under, and
        // the whole of how C++ recognizes one. A tag is a property of the object
        // the VM itself stores, so it cannot be copied onto a script-built value:
        // table.clone takes tables, setmetatable takes tables, and newproxy --
        // the one base-library way to mint a userdata -- is removed from both
        // environments. That is what the frozen error TABLE could not offer,
        // where identity rested on a metatable a clone could carry along.
        //
        // Any value below LUA_UTAG_LIMIT would do. Zero is what plain
        // lua_newuserdata stamps, so the first non-default tag is used instead;
        // the handle kinds are minted by lua_newuserdatadtor, which stamps
        // UTAG_IDTOR (== LUA_UTAG_LIMIT) and can therefore never collide.
        constexpr auto k_errorUserdataTag = 1;
        static_assert(k_errorUserdataTag > 0 && k_errorUserdataTag < LUA_UTAG_LIMIT);

        // Pushes an error kind's wire spelling as a Lua string. The domain returns
        // a view rather than a C string, so push it with its length instead of
        // relying on the literal's terminator.
        auto pushWireName(lua_State* state, AutomationErrorKind kind) -> void
        {
            auto const name = automationErrorWireName(kind);
            lua_pushlstring(state, name.data(), name.size());
        }

        // The `retryable` field of a Tier B error table reuses the domain's own
        // unwind axis rather than inventing a parallel table: a kind whose
        // FailureResponse is Retry is retryable, every other response is not.
        [[nodiscard]]
        auto retryableOf(AutomationErrorKind kind) noexcept -> bool
        {
            return failureResponse(kind) == FailureResponse::Retry;
        }

        // The payload an action handle carries. It is placement-constructed into
        // a host-owned full userdata and destroyed by destroyBox at GC.
        //
        // The move-only Observation is never stored in a handle: it lives in the
        // TaskContext's CycleLedger, and the only handle that names it is the
        // ticket. A hit therefore carries just the ordinal of the cycle that
        // found it, which is the whole of its staleness check -- with at most one
        // cycle open, an ordinal that is not the open one names a cycle that no
        // longer exists, and there is no second cycle it could have come from.
        struct HitBox final
        {
            uint64              cycleOrdinal{};
            engine::ActionFound action;
        };

        // Runs at VM garbage collection to destroy the value placement-constructed
        // into a handle by pushBoxed.
        //
        // No handle's destructor releases a frame. Collecting a ticket, a page or
        // a hit frees only the few bytes of the box: the frame behind them is
        // released by cycle_close or by the click that consumes the cycle,
        // and by the ledger's own destructor when the generation is torn down.
        // That is the point of the cycle protocol -- host memory whose release
        // timing was decided by the Lua collector was wrong by design.
        template <typename T>
        auto destroyBox(void* storage) -> void
        {
            // SAFETY: `storage` is the userdata block lua_newuserdatadtor handed
            // pushBoxed, where exactly one T was placement-constructed via
            // std::construct_at. Destroy it once here; the VM frees the block
            // afterwards.
            std::destroy_at(static_cast<T*>(storage));
        }

        // Bound as the __newindex of every handle metatable and of every Tier B
        // error carrier's: rejects every write with a clear error so a host
        // object can never be mutated from a script. It is belt over braces --
        // a userdata with no __newindex already refuses a write -- and it is
        // here for the message, so an author sees why rather than "attempt to
        // index".
        auto denyWrite(lua_State* state) -> int
        {
            // luaL_error is l_noret: it longjmps out of this frame and never
            // returns, so this lua_CFunction never falls off its int-returning end.
            luaL_error(state, "task handles are read-only");
        }

        // Bound as every handle metatable's __tostring: returns the fixed kind
        // label carried as upvalue 1, so tostring(handle) never leaks the handle
        // address or its internal id and stays deterministic.
        auto handleToString(lua_State* state) -> int
        {
            lua_pushvalue(state, lua_upvalueindex(1));
            return 1;
        }

        // Creates one opaque host-owned userdata carrying `value`, attaches the
        // shared protected metatable registered under `metatableType`, and leaves
        // the handle on the stack top.
        template <typename T>
        auto pushBoxed(
            lua_State* state,
            T value,
            void (*dtor)(void*),
            char const* metatableType
        ) -> void
        {
            // SAFETY: lua_newuserdatadtor allocates sizeof(T) VM-owned bytes whose
            // data region is 16-byte aligned for these sizes (Udata::data is
            // alignas(8), and >=16-byte blocks get 16-byte alignment), so every T
            // here (alignment 8) is suitably aligned; we placement-construct the
            // value into it and the registered dtor destroys it once at GC. No Lua
            // allocation runs between the allocation and the construction, so a
            // collection can never see the block uninitialised. The block outlives
            // every script use of the handle.
            static_assert(alignof(T) <= 16);
            void* storage = lua_newuserdatadtor(state, sizeof(T), dtor);
            std::construct_at(static_cast<T*>(storage), std::move(value));

            luaL_getmetatable(state, metatableType);
            lua_setmetatable(state, -2);
        }

        // True when the value at `index` is a full userdata whose metatable is the
        // one registered under `metatableType`. Pointer identity against the
        // registry entry is robust: a script cannot fabricate a userdata carrying a
        // chosen metatable, and __metatable protection blocks setmetatable.
        [[nodiscard]]
        auto isKind(lua_State* state, int index, char const* metatableType) -> bool
        {
            if (lua_type(state, index) != LUA_TUSERDATA)
            {
                return false;
            }
            if (lua_getmetatable(state, index) == 0)
            {
                return false;
            }
            luaL_getmetatable(state, metatableType);
            bool const same = lua_rawequal(state, -1, -2) != 0;
            lua_pop(state, 2);
            return same;
        }

        // What a Tier B error carrier stores in its own userdata block. The kind
        // is the whole payload: it is the one field the host has to be able to
        // read back out of a value a script handed around, and reading it here
        // -- rather than off the script-visible `kind` string -- is what keeps
        // the decode structural. Trivially destructible, so the carrier needs no
        // destructor and can be minted with a plain tagged userdata.
        struct TierBError final
        {
            AutomationErrorKind kind{};
        };

        // The automation kind of the Tier B error carrier at `index`, or nullopt
        // when that value is not one. The test is the userdata tag and nothing
        // else: no field is read, so a script-built look-alike carrying `kind`,
        // `message` and `retryable` is not a Tier B error here however faithfully
        // it copies one.
        [[nodiscard]]
        auto tierBErrorKind(
            lua_State* state,
            int index
        ) -> std::optional<AutomationErrorKind>
        {
            if (lua_type(state, index) != LUA_TUSERDATA)
            {
                return std::nullopt;
            }
            if (lua_userdatatag(state, index) != k_errorUserdataTag)
            {
                return std::nullopt;
            }
            // SAFETY: the tag is stamped by the VM at creation and cannot be set
            // from script, and raiseTierB below is the only place that stamps
            // this one, so the block holds exactly one live TierBError.
            auto const* p_error =
                static_cast<TierBError const*>(lua_touserdata(state, index));
            return p_error->kind;
        }

        // Mints one Tier B error carrier and raises it. Never returns.
        //
        // The carrier is a host-minted userdata under k_errorUserdataTag, not a
        // table. The three fields a script reads -- kind, message, retryable --
        // live in a frozen table behind the carrier's own protected metatable's
        // __index, which is a TABLE and never a function (that rule is what keeps
        // a future move to a yield protocol from acquiring an unyieldable hole,
        // and it is checked by deepFreezeMetatable rather than trusted here).
        //
        // The metatable is per instance because the fields are: with __index
        // restricted to a table there is nowhere else per-error data could live.
        // That costs two small tables per raised error, which is the right side
        // of the trade -- errors are exceptional, and the alternative is a
        // function __index the design rules out.
        //
        // Identity does not rest on that metatable. C++ reads the tag, and the
        // framework's ctx:try asks whether the value is a userdata wearing this
        // label; a project script can produce neither.
        [[noreturn]]
        auto raiseTierB(
            lua_State* state,
            AutomationErrorKind kind,
            std::string const& message
        ) -> void
        {
            // SAFETY: lua_newuserdatatagged allocates sizeof(TierBError) VM-owned
            // bytes, aligned as lua_newuserdatadtor's blocks are (see pushBoxed),
            // and TierBError is trivially destructible, so no destructor has to be
            // registered for the tag and nothing leaks when the VM frees the
            // block. No Lua allocation runs between the allocation and the
            // construction, so a collection can never see the block uninitialised.
            static_assert(std::is_trivially_destructible_v<TierBError>);
            void* storage = lua_newuserdatatagged(
                state,
                sizeof(TierBError),
                k_errorUserdataTag
            );
            std::construct_at(static_cast<TierBError*>(storage), TierBError{.kind = kind});
            int const carrier = lua_gettop(state);

            lua_createtable(state, 0, 3);
            int const fields = lua_gettop(state);
            pushWireName(state, kind);
            lua_setfield(state, fields, "kind");
            lua_pushlstring(state, message.data(), message.size());
            lua_setfield(state, fields, "message");
            lua_pushboolean(state, retryableOf(kind) ? 1 : 0);
            lua_setfield(state, fields, "retryable");

            lua_createtable(state, 0, 4);
            int const metatable = lua_gettop(state);

            lua_pushvalue(state, fields);
            lua_setfield(state, metatable, "__index");

            lua_pushcfunction(state, &denyWrite, "uf_error_newindex");
            lua_setfield(state, metatable, "__newindex");

            // tostring() names the kind and the message and nothing else, so an
            // error that reaches the host uncaught still carries its cause into
            // the run report. It is a fixed string built here rather than a
            // formatter reading the carrier, so it stays deterministic and leaks
            // no address.
            auto label = std::string{k_errorType};
            label += "(";
            label += automationErrorWireName(kind);
            label += "): ";
            label += message;
            lua_pushlstring(state, label.data(), label.size());
            lua_pushcclosure(state, &handleToString, "uf_error_tostring", 1);
            lua_setfield(state, metatable, "__tostring");

            lua_pushstring(state, k_errorType);
            lua_setfield(state, metatable, "__metatable");

            if (!script::deepFreezeMetatable(state, metatable))
            {
                // Unreachable while the metatable is the one built directly
                // above: it carries __metatable and a table __index by
                // construction. Raising a plain string rather than a half-frozen
                // carrier keeps a broken host from handing a script a mutable
                // object that answers to the Tier B label.
                lua_pushstring(
                    state,
                    "uf: a Tier B error carrier could not be frozen"
                );
                lua_error(state);
            }

            lua_setmetatable(state, carrier);
            lua_settop(state, carrier);

            // lua_error takes the value on top of the stack (the carrier) and
            // longjmps; it is l_noret, so control never returns here.
            lua_error(state);
        }

        // Validates that the value at `index` is a handle of the expected kind and
        // returns a pointer to its payload. A wrong type is a structured Tier B
        // InvalidResource, so the whole script-facing failure model is one shape.
        template <typename T>
        [[nodiscard]]
        auto checkBox(
            lua_State* state,
            int index,
            char const* metatableType,
            char const* expected
        ) -> T*
        {
            if (!isKind(state, index, metatableType))
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{"expected a "} + expected + " handle"
                );
            }
            // SAFETY: isKind confirmed the value at `index` is a full userdata
            // carrying this kind's shared protected metatable, which only
            // pushBoxed<T> attaches, so the block holds exactly one live T.
            // raiseTierB never returns, so this cast runs only on a valid handle.
            return static_cast<T*>(lua_touserdata(state, index));
        }

        // Tier C: latch the fatal flag so the host discards this VM generation,
        // then raise a plain non-table sentinel. It carries no uf.error
        // metatable, so ctx:try refuses to swallow it and re-raises it unchanged.
        //
        // A project pcall CAN catch this value, and that is accepted: control is
        // not what the sentinel protects. The latch is. Every primitive enters
        // through guardFatal, so a script that swallowed the sentinel and kept
        // running is refused at its next primitive call, before any capture or
        // any click.
        [[noreturn]]
        auto raiseCancelled(lua_State* state, TaskContext* context) -> void
        {
            context->markFatal();
            lua_pushstring(state, "uf: task cancelled");
            lua_error(state);
        }

        // Maps an engine failure to a raised Lua error: Cancelled takes the Tier C
        // sentinel; every other kind becomes a Tier B error table. Never returns.
        [[noreturn]]
        auto raiseFromError(
            lua_State* state,
            TaskContext* context,
            Error const& error
        ) -> void
        {
            auto const kind =
                automationErrorKind(error).value_or(AutomationErrorKind::InternalInvariant);
            if (kind == AutomationErrorKind::Cancelled)
            {
                raiseCancelled(state, context);
            }
            raiseTierB(state, kind, std::string{error.message()});
        }

        // Re-raises the Tier C sentinel if a prior verb already latched a fatal
        // cancellation, so a script that swallowed the sentinel cannot drive one
        // more engine verb before the host tears the generation down.
        auto guardFatal(lua_State* state, TaskContext* context) -> void
        {
            if (context->fatal())
            {
                raiseCancelled(state, context);
            }
        }

        // Reads the TaskContext bound as upvalue 1 of a primitive.
        [[nodiscard]]
        auto boundContext(lua_State* state) -> TaskContext*
        {
            // SAFETY: upvalue 1 of every primitive closure is the lightuserdata
            // installPrimitive installed -- a pointer to the host-owned
            // TaskContext that outlives the VM by the installer's lifetime
            // contract. No other value is ever stored there, so casting the opaque
            // pointer back to TaskContext* is sound. The primitive runs only on
            // the VM's owning thread, so the access is race-free.
            return static_cast<TaskContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(1))
            );
        }

        // The automation kind of an engine failure, defaulting an unclassified
        // error to InternalInvariant. Mirrors raiseFromError's own mapping so the
        // HostCall trace records exactly the kind the Tier ladder will raise.
        [[nodiscard]]
        auto kindOf(Error const& error) noexcept -> AutomationErrorKind
        {
            return automationErrorKind(error)
                .value_or(AutomationErrorKind::InternalInvariant);
        }

        // Which primitive ran and what it was handed. Every task.native_call
        // carries it, so a primitive the host fails before the engine is reached
        // -- a ticket or a hit naming a cycle that is no longer open, which is
        // the only failure that produces no engine event at all -- still names
        // the cycle the script tried to use. A call-scoped parameter type: `verb`
        // views a string literal and the struct never outlives the call that
        // builds it.
        struct NativeCallIdentity final
        {
            std::string_view                        verb;
            std::optional<uint64>                   cycleOrdinal{};
            std::optional<uint64>                   hitCycleOrdinal{};
            std::optional<annotation::RecognizerId> recognizerId{};

            // The pause a settle declared, in whole milliseconds. It is the only
            // argument of a primitive that carries no handle at all, and a replay
            // cannot reconstruct the run without it.
            std::optional<uint64> durationMillis{};
        };

        [[nodiscard]]
        auto nativeCallEvent(
            NativeCallIdentity const& call,
            trace::NativeCallOutcome outcome,
            std::optional<AutomationErrorKind> errorKind
        ) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind       = trace::TraceEventKind::TaskNativeCall,
                .nativeCall = trace::TraceEvent::NativeCall{
                    .verb            = std::string{call.verb},
                    .outcome         = outcome,
                    .cycleOrdinal    = call.cycleOrdinal,
                    .hitCycleOrdinal = call.hitCycleOrdinal,
                    .durationMillis  = call.durationMillis,
                },
                .recognizerId = call.recognizerId,
                .errorKind    = errorKind,
            };
        }

        // Emits the task.native_call trace event for a verb at its exit. A sink
        // failure aborts the verb as a Tier B IoFailure: losing trace evidence is
        // a hard error, not a silent drop, matching the engine's throw-instant
        // emit discipline (D4).
        auto traceHostCall(
            lua_State* state,
            TaskContext* context,
            NativeCallIdentity const& call,
            trace::NativeCallOutcome outcome
        ) -> void
        {
            auto status = context->emitTrace(
                nativeCallEvent(call, outcome, std::nullopt)
            );
            if (!status)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::IoFailure,
                    std::string{status.error().message()}
                );
            }
        }

        // Records a failed verb and then raises that verb's own error. Emitting
        // first and raising the sink's failure instead would report the wrong
        // cause: a script asking why its click failed would be told the trace file
        // was unwritable. The verb is failing either way, so its own cause wins and
        // the sink failure is latched on the context, where the host reads it after
        // the run rather than losing it silently.
        //
        // For a cancellation this also keeps the Tier C sentinel on the raise path,
        // which latches fatal before anything else can run. That ordering is now
        // load-bearing rather than defensive: ctx:try is pure Luau and consults
        // nothing, so the latch set here is the only thing standing between a
        // swallowed sentinel and one more primitive call. Never returns.
        auto traceHostCallFailure(
            lua_State* state,
            TaskContext* context,
            NativeCallIdentity const& call,
            Error const& error
        ) -> void
        {
            auto status = context->emitTrace(
                nativeCallEvent(
                    call,
                    trace::NativeCallOutcome::Failed,
                    kindOf(error)
                )
            );
            if (!status)
            {
                context->latchTraceFailure();
            }
            raiseFromError(state, context, error);
        }

        // Every primitive below is a plain function of the private capability
        // table, called as native.<verb>(...) from the framework, so its
        // arguments start at stack index 1. They were method calls on the uf
        // root before the surface went private, which is why the design writes
        // them as bare signatures: cycle_close(ticket), not uf:cycle_close.
        //
        // cycle_open() -> ticket. Observes one frame and opens the
        // generation's single observation cycle over it. A capture failure maps
        // through the Tier ladder (Tier B, or Tier C for a cancellation);
        // opening while a cycle is already open is a framework bug and fails
        // InternalInvariant rather than becoming a Tier B failure a script could
        // catch and retry.
        //
        // It takes no deadline yet. The plan's cycle_open(deadline) needs
        // IFrameSource::capture() to accept one, which is stage 3 work; a
        // parameter parsed and discarded here would tell a script its capture is
        // bounded when nothing bounds it.
        auto cycleOpenFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            // cycle_open mints the ordinal rather than being handed one, so its
            // identity is the primitive alone.
            auto const call = NativeCallIdentity{.verb = "cycle_open"};

            auto result = context->openCycle();
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            pushBoxed<CycleTicket>(
                state,
                *result,
                &destroyBox<CycleTicket>,
                k_cycleType
            );
            return 1;
        }

        // cycle_close(ticket) -> (). Releases the frame the cycle retains,
        // deterministically and at a moment the host chose. Idempotent: a ticket
        // that names no open cycle -- closed already, consumed by a click, or
        // minted by another generation -- records an Empty call and returns.
        auto cycleCloseFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const call = NativeCallIdentity{
                .verb           = "cycle_close",
                .cycleOrdinal = ticket->ordinal,
            };

            bool const released = context->closeCycle(*ticket);
            traceHostCall(
                state,
                context,
                call,
                released ? trace::NativeCallOutcome::Succeeded
                         : trace::NativeCallOutcome::Empty
            );
            return 0;
        }

        // cycle_page(ticket) -> resolved-page handle, or nil when the frame
        // resolved to Unknown or Ambiguous (both already traced by the engine, so
        // neither raises). A successful resolution is also recorded in the ledger
        // as this cycle's click authorization evidence, which is the only way a
        // click can ever be authorized.
        auto cyclePageFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const call = NativeCallIdentity{
                .verb           = "cycle_page",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cyclePage(*ticket);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            auto resolved = *std::move(result);
            if (!resolved.has_value())
            {
                traceHostCall(state, context, call, trace::NativeCallOutcome::Empty);
                lua_pushnil(state);
                return 1;
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            pushBoxed<annotation::ResolvedPage>(
                state,
                *std::move(resolved),
                &destroyBox<annotation::ResolvedPage>,
                k_resolvedPageType
            );
            return 1;
        }

        // cycle_find(ticket, recognizer) -> hit handle, or nil for a
        // completed miss (Tier A). A non-recognizer argument is a Tier B
        // InvalidResource.
        auto cycleFindFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto* recognizer =
                checkBox<annotation::RecognizerId>(state, 2, k_recognizerType, "recognizer");
            auto const call = NativeCallIdentity{
                .verb           = "cycle_find",
                .cycleOrdinal = ticket->ordinal,
                .recognizerId   = *recognizer,
            };

            auto result = context->cycleFind(*ticket, *recognizer);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            auto found = *std::move(result);
            if (!found.has_value())
            {
                traceHostCall(state, context, call, trace::NativeCallOutcome::Empty);
                lua_pushnil(state);
                return 1;
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            pushBoxed<HitBox>(
                state,
                HitBox{
                    .cycleOrdinal = ticket->ordinal,
                    .action       = *std::move(found),
                },
                &destroyBox<HitBox>,
                k_hitType
            );
            return 1;
        }

        // cycle_click(ticket, hit) -> (). Consumes the cycle and delivers
        // the click.
        //
        // There is deliberately NO page argument. The host requires that this
        // ticket already resolved a page and uses that as the authorization
        // evidence, so a script cannot hand over evidence from another frame:
        // there is no parameter to hand it through, and with at most one cycle
        // open there is no other frame to take it from. A cycle that never
        // resolved a page fails ActionRejected.
        //
        // Both ordinals reach the wire: they agree on every delivered click, and
        // differ exactly when a hit from a spent cycle was refused.
        auto cycleClickFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto* hit    = checkBox<HitBox>(state, 2, k_hitType, "hit");

            auto const call = NativeCallIdentity{
                .verb              = "cycle_click",
                .cycleOrdinal    = ticket->ordinal,
                .hitCycleOrdinal = hit->cycleOrdinal,
            };

            auto result = context->cycleClick(*ticket, hit->cycleOrdinal, hit->action);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // page:is(pageReference) -> bool. page:is desugars to page.is(page, ref).
        auto isFn(lua_State* state) -> int
        {
            auto* page =
                checkBox<annotation::ResolvedPage>(state, 1, k_resolvedPageType, "page");
            auto* reference =
                checkBox<annotation::PageId>(state, 2, k_pageRefType, "page reference");
            lua_pushboolean(state, page->pageId() == *reference ? 1 : 0);
            return 1;
        }

        // Converts a script's millisecond count into the monotonic Duration the
        // host times with. `what` names the script-facing spelling the count
        // came from, because that is what the author wrote to get here.
        //
        // The Duration is steady_clock's tick -- nanoseconds on every supported
        // standard library -- so the conversion multiplies by the ms-to-tick
        // ratio. A raw duration_cast would overflow the signed tick rep
        // (undefined behaviour) far below the millisecond count's own int64
        // limit, and before any monotonic-overflow guard downstream could run.
        // Build the Duration through the checked helpers instead: a NaN,
        // negative, non-finite, or out-of-range count -- or one whose tick
        // product would not fit -- is a Tier B InvalidResource here rather than
        // silent wraparound.
        [[nodiscard]]
        auto millisToDuration(
            lua_State* state,
            double millis,
            std::string const& what
        ) -> MonotonicInstant::Duration
        {
            using Duration  = MonotonicInstant::Duration;
            using MsToTicks = std::ratio_divide<std::milli, Duration::period>;
            static_assert(MsToTicks::den == 1, "Duration must be no coarser than a millisecond");

            auto ticks = std::optional<Duration::rep>{};
            if (millis >= 0.0)
            {
                if (auto const count = checkedIntegralCast<Duration::rep>(millis))
                {
                    ticks = checkedMultiply<Duration::rep>(
                        *count,
                        static_cast<Duration::rep>(MsToTicks::num)
                    );
                }
            }
            if (!ticks)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " must be a finite, non-negative millisecond count within range"
                );
            }
            return Duration{*ticks};
        }

        // Reads a required millisecond argument at `index`. A missing or
        // non-numeric argument is a Tier B InvalidResource, keeping the
        // script-facing failure model single-shaped.
        [[nodiscard]]
        auto checkMillisDuration(
            lua_State* state,
            int index,
            std::string const& what
        ) -> MonotonicInstant::Duration
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " requires a number of milliseconds"
                );
            }
            return millisToDuration(state, lua_tonumber(state, index), what);
        }

        // Reads an optional millisecond field from the wait options table at
        // `optsIndex`. Returns nullopt when the options are absent (nil/none) or
        // the named field is nil, so the caller applies the TaskContext default. A
        // present-but-non-numeric or out-of-range value is a Tier B
        // InvalidResource, keeping the script-facing failure model single-shaped.
        [[nodiscard]]
        auto readWaitDuration(
            lua_State* state,
            int optsIndex,
            char const* field
        ) -> std::optional<MonotonicInstant::Duration>
        {
            if (lua_isnoneornil(state, optsIndex))
            {
                return std::nullopt;
            }
            if (lua_type(state, optsIndex) != LUA_TTABLE)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "wait_for_page options must be a table"
                );
            }

            auto const what = std::string{"wait_for_page option '"} + field + "'";

            lua_getfield(state, optsIndex, field);
            if (lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                return std::nullopt;
            }
            if (lua_type(state, -1) != LUA_TNUMBER)
            {
                lua_pop(state, 1);
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " must be a number of milliseconds"
                );
            }
            double const millis = lua_tonumber(state, -1);
            lua_pop(state, 1);

            return millisToDuration(state, millis, what);
        }

        // wait_for_page(pageReference, options) -> { page = resolved-page,
        // cycle = ticket }. A timeout raises a Tier B error; a cancellation takes
        // the Tier C sentinel.
        //
        // The wait leaves the generation's one cycle OPEN over the frame that
        // resolved the page, with that page already recorded as the cycle's
        // authorization evidence. The caller therefore finds and clicks on the
        // returned ticket and must close it, exactly as if it had opened the
        // cycle itself. This engine-side wait loop is deleted in stage 3, when
        // the Luau framework polls cycle_open / cycle_page itself.
        auto waitForPageFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* reference =
                checkBox<annotation::PageId>(state, 1, k_pageRefType, "page reference");
            auto const timeout      = readWaitDuration(state, 2, "timeout_ms");
            auto const pollInterval = readWaitDuration(state, 2, "poll_interval_ms");

            // wait_for_page mints the ordinal its own cycle opens under, so like
            // cycle_open it is handed none.
            auto const call = NativeCallIdentity{.verb = "wait_for_page"};

            auto result = context->waitForPage(*reference, timeout, pollInterval);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);

            auto wait = *std::move(result);
            lua_createtable(state, 0, 2);
            int const table = lua_gettop(state);

            pushBoxed<annotation::ResolvedPage>(
                state,
                std::move(wait.page),
                &destroyBox<annotation::ResolvedPage>,
                k_resolvedPageType
            );
            lua_setfield(state, table, "page");

            pushBoxed<CycleTicket>(
                state,
                wait.ticket,
                &destroyBox<CycleTicket>,
                k_cycleType
            );
            lua_setfield(state, table, "cycle");
            return 1;
        }

        // Raises the Tier C sentinel when the run's cancel source has already
        // requested a stop. The three time primitives below reach no engine
        // verb, so nothing downstream would fail closed for them: this is where
        // they do it, and it runs both before a pause and after one, so a stop
        // that lands mid-sleep ends the primitive on the terminal path rather
        // than as a normal return. Every later primitive is then refused by
        // guardFatal, before any capture.
        auto guardCancelled(lua_State* state, TaskContext* context) -> void
        {
            if (context->cancellationRequested())
            {
                raiseCancelled(state, context);
            }
        }

        // deadline(ms) -> deadline handle. Mints the absolute instant `ms` from
        // now as an opaque host handle.
        //
        // The instant is absolute and host-minted for the same reason a cycle
        // ticket is: a script that could name the value could also renew it, and
        // a wait budget a script can extend is not a budget. It reads no clock
        // back to Lua -- `now()` was deleted with this primitive's arrival --
        // so the only thing a script can do with a deadline is hand it to wait.
        auto deadlineFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);
            guardCancelled(state, context);

            auto const duration = checkMillisDuration(state, 1, "ctx:deadline");
            auto const instant  = MonotonicInstant::now().checkedAdd(duration);
            if (!instant)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:deadline overflows the monotonic clock"
                );
            }

            pushBoxed<MonotonicInstant>(
                state,
                *instant,
                &destroyBox<MonotonicInstant>,
                k_deadlineType
            );
            return 1;
        }

        // wait(deadline, interval_ms) -> bool. Pauses for one poll interval and
        // reports whether the deadline still has budget: false means it expired
        // and the framework's wait loop is over.
        //
        // It polls nothing itself. What is re-observed between two calls is the
        // framework's business, which is what keeps this an indivisible effect
        // rather than a policy loop smuggled into C++ (design section 18's
        // primitive admission rule). Bounded by min(interval, time to deadline),
        // and it never sleeps at all once the deadline has passed.
        //
        // The interval is clamped up to k_minWaitPollInterval so a framework bug
        // asking for zero cannot turn the observation cycle into a busy wait.
        auto waitFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* deadline =
                checkBox<MonotonicInstant>(state, 1, k_deadlineType, "deadline");
            auto const requested =
                checkMillisDuration(state, 2, "ctx:wait poll interval");
            auto const interval = std::max(requested, k_minWaitPollInterval);

            guardCancelled(state, context);
            bool const budgetRemains = context->waitUntil(*deadline, interval);
            guardCancelled(state, context);

            lua_pushboolean(state, budgetRemains ? 1 : 0);
            return 1;
        }

        // settle(ms) -> (). A declarative bounded pause, and the only one: it
        // reaches no engine verb, so its whole content is the duration, which is
        // why it is traced. Design section 10 makes that duration part of the
        // replay input -- a run cannot be reproduced from a pause nobody wrote
        // down.
        //
        // A request beyond k_maxSettleDuration is Tier B rather than a framework
        // invariant failure: a project asking to settle for ten minutes is a
        // project error, and section 9 reserves the invariant kind for failures a
        // project cannot cause. It is refused before the pause and untraced, like
        // every other argument rejection.
        auto settleFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const duration = checkMillisDuration(state, 1, "ctx:settle");
            if (duration > k_maxSettleDuration)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:settle exceeds the host's settle ceiling; wait against a "
                    "deadline instead of sleeping"
                );
            }

            // The duration came from a whole millisecond count and is capped far
            // below any tick-count ceiling, so the round trip back to
            // milliseconds is exact and non-negative.
            auto const call = NativeCallIdentity{
                .verb           = "settle",
                .durationMillis = static_cast<uint64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                        .count()
                ),
            };

            guardCancelled(state, context);
            context->settle(duration);
            if (context->cancellationRequested())
            {
                // Record the abandoned settle before taking the terminal path,
                // so the trace shows the pause that was cut short rather than
                // ending on a verb that never reported anything.
                auto const cancelled = fail(
                    AutomationErrorKind::Cancelled,
                    "cancelled while settling"
                );
                traceHostCallFailure(state, context, call, cancelled.error());
            }

            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // The largest magnitude a random bound may take. Beyond 2^53 a Lua
        // number (an IEEE double) can no longer represent every integer, so the
        // value the script passed would already be rounded and a returned integer
        // might not round-trip. Bounding here keeps every result an exact integer.
        constexpr auto k_maxExactInteger = int64{9007199254740992};

        // Reads a random bound at `index` as an integer. A non-number, a
        // non-integral value, or one outside +/-2^53 is a Tier B InvalidResource,
        // keeping the script-facing failure model single-shaped. The messages name
        // the script-facing spelling ctx:random, because that is what a project
        // author wrote to get here.
        [[nodiscard]]
        auto checkRandomInteger(lua_State* state, int index) -> int64
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:random bounds must be integers"
                );
            }
            double const raw   = lua_tonumber(state, index);
            auto const   value = checkedIntegralCast<int64>(raw);
            if (!value || *value > k_maxExactInteger || *value < -k_maxExactInteger)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:random bounds must be whole numbers within +/-2^53"
                );
            }
            return *value;
        }

        // random([m [, n]]) -> number. Mirrors Lua's math.random over the
        // host's deterministic seeded RNG: no argument yields a double in [0, 1);
        // one argument m yields an integer in [1, m]; two arguments yield an integer
        // in [m, n]. A bad shape -- a non-integer bound, m < 1, or m > n -- is a
        // Tier B InvalidResource, matching math.random's own rejection of an empty
        // interval.
        auto randomFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);

            bool const hasLow  = !lua_isnoneornil(state, 1);
            bool const hasHigh = !lua_isnoneornil(state, 2);

            if (!hasLow && !hasHigh)
            {
                lua_pushnumber(state, context->nextRandomUnitDouble());
                return 1;
            }

            int64 const low = checkRandomInteger(state, 1);
            if (!hasHigh)
            {
                if (low < 1)
                {
                    raiseTierB(
                        state,
                        AutomationErrorKind::InvalidResource,
                        "ctx:random(m) requires m >= 1"
                    );
                }
                lua_pushnumber(
                    state,
                    static_cast<double>(context->nextRandomInRange(int64{1}, low))
                );
                return 1;
            }

            int64 const high = checkRandomInteger(state, 2);
            if (low > high)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:random(m, n) requires m <= n"
                );
            }
            lua_pushnumber(
                state,
                static_cast<double>(context->nextRandomInRange(low, high))
            );
            return 1;
        }

        // Starts a fresh handle metatable on the stack with an empty __index method
        // table, __newindex deny, and the fixed __tostring/__metatable label. The
        // caller adds any methods, then finishMetatable freezes and registers it.
        auto beginMetatable(lua_State* state, char const* label) -> void
        {
            lua_newtable(state);
            int const metatable = lua_gettop(state);

            lua_newtable(state);
            lua_setfield(state, metatable, "__index");

            lua_pushcfunction(state, &denyWrite, "uf_handle_newindex");
            lua_setfield(state, metatable, "__newindex");

            lua_pushstring(state, label);
            lua_pushcclosure(state, &handleToString, "uf_handle_tostring", 1);
            lua_setfield(state, metatable, "__tostring");

            lua_pushstring(state, label);
            lua_setfield(state, metatable, "__metatable");
        }

        // Adds `function` to the __index method table of the metatable at
        // `metatable`. When `context` is non-null the closure carries it as
        // lightuserdata upvalue 1, which is how the engine verbs reach the session.
        auto addMethod(
            lua_State* state,
            int metatable,
            char const* name,
            lua_CFunction function,
            TaskContext* context
        ) -> void
        {
            lua_getfield(state, metatable, "__index");
            if (context != nullptr)
            {
                lua_pushlightuserdata(state, context);
                lua_pushcclosure(state, function, name, 1);
            }
            else
            {
                lua_pushcfunction(state, function, name);
            }
            lua_setfield(state, -2, name);
            lua_pop(state, 1);
        }

        // Freezes the metatable on the stack top and stores it in the registry
        // under `registryType`, popping it.
        //
        // The freeze is script::deepFreezeMetatable, the sandbox's own walk in
        // its metatable-checking form, so the design's runtime rules -- a
        // __metatable field on every metatable, an __index that is a table and
        // never a function, frozen before anything wears it -- are enforced on
        // these at construction rather than restated here as a convention.
        //
        // The metatable-checking form is the one that applies: a handle kind's
        // metatable is registered here and attached to each handle later, so the
        // plain deepFreeze would only ever see it as an ordinary table and would
        // never check it as the metatable it is about to become.
        [[nodiscard]]
        auto finishMetatable(lua_State* state, char const* registryType) -> Status
        {
            UF_TRY(script::deepFreezeMetatable(state, -1));
            lua_setfield(state, LUA_REGISTRYINDEX, registryType);
            return ok();
        }

        // Registers the metatables of the handle kinds the DATA surface mints:
        // the recognizer and page references named under uf.recognizers and
        // uf.pages. Both exist whether or not a session is bound, because a
        // reference is an identity rather than a capability.
        [[nodiscard]]
        auto installResourceMetatables(lua_State* state) -> Status
        {
            beginMetatable(state, k_recognizerType);
            UF_TRY(finishMetatable(state, k_recognizerType));

            beginMetatable(state, k_pageRefType);
            return finishMetatable(state, k_pageRefType);
        }

        // Registers the metatables only a bound session can ever mint: the cycle
        // ticket, the hit, the resolved page and the deadline. They belong to the
        // private capability installer because every one of them is produced by a
        // primitive, so a VM with no session has nothing that could wear them.
        //
        // The Tier B error carrier is absent on purpose. Its metatable is per
        // instance -- it holds that error's own kind, message and retryable
        // behind a table __index -- so there is nothing shared to register here,
        // and its identity is the userdata tag rather than a registry entry.
        //
        // Only the resolved page carries a method. A ticket and a hit are pure
        // names the host hands back to itself, so every operation on a cycle is a
        // primitive taking the ticket.
        [[nodiscard]]
        auto installSessionMetatables(lua_State* state) -> Status
        {
            beginMetatable(state, k_cycleType);
            UF_TRY(finishMetatable(state, k_cycleType));

            beginMetatable(state, k_hitType);
            UF_TRY(finishMetatable(state, k_hitType));

            beginMetatable(state, k_deadlineType);
            UF_TRY(finishMetatable(state, k_deadlineType));

            beginMetatable(state, k_resolvedPageType);
            {
                int const metatable = lua_gettop(state);
                addMethod(state, metatable, "is", &isFn, nullptr);
            }
            return finishMetatable(state, k_resolvedPageType);
        }

        // Populates `uf[fieldName]` with a name table of opaque handles, one per
        // spec, each carrying the metatable registered under `metatableType`.
        template <typename Spec, typename Id>
        auto installResourceTable(
            lua_State* state,
            int root,
            char const* fieldName,
            char const* metatableType,
            std::vector<Spec> const& specs
        ) -> void
        {
            lua_newtable(state);
            int const table = lua_gettop(state);
            for (auto const& spec : specs)
            {
                pushBoxed<Id>(state, spec.id, &destroyBox<Id>, metatableType);
                lua_setfield(state, table, spec.name.c_str());
            }
            lua_setfield(state, root, fieldName);
        }

        // Populates `uf.errors` with one constant per AutomationErrorKind. Both
        // the key and the value are that kind's domain wire spelling, so a script
        // writes `err.kind == uf.errors.timeout` and compares the exact string
        // the Tier B error and the trace line both carry.
        //
        // The table is built here, from the same domain function, rather than
        // generated into a .luau file by a build step. A generator would parse the
        // enum and emit a third artifact that has to be kept in sync; reading the
        // enum at install time leaves one source of truth by construction, with no
        // parse, no codegen and nothing to go stale. Iteration is over the
        // reflected entries, which is the repository's existing enumeration of the
        // kinds, so a new kind appears here with no edit to this function.
        auto installErrorKindTable(lua_State* state, int root) -> void
        {
            lua_newtable(state);
            int const table = lua_gettop(state);
            for (auto const& entry : enumEntries<AutomationErrorKind>())
            {
                pushWireName(state, entry.value);
                pushWireName(state, entry.value);
                lua_rawset(state, table);
            }
            lua_setfield(state, root, "errors");
        }

        // Binds one primitive (`fieldName`) into the private surface table at
        // `surface`, as a C closure carrying the TaskContext as lightuserdata
        // upvalue 1.
        auto installPrimitive(
            lua_State* state,
            int surface,
            char const* fieldName,
            lua_CFunction function,
            char const* debugName,
            TaskContext* context
        ) -> void
        {
            lua_pushlightuserdata(state, context);
            lua_pushcclosure(state, function, debugName, 1);
            lua_setfield(state, surface, fieldName);
        }

        // Assembles the frozen global uf table: recognizers, pages and error
        // kinds. Every entry is data. Nothing here can observe or act, which is
        // why it is safe as a project global.
        [[nodiscard]]
        auto buildUfData(
            lua_State* state,
            std::vector<RecognizerHandleSpec> const& recognizers,
            std::vector<PageHandleSpec> const& pages
        ) -> Status
        {
            UF_TRY(installResourceMetatables(state));

            lua_newtable(state);
            int const root = lua_gettop(state);

            installResourceTable<RecognizerHandleSpec, annotation::RecognizerId>(
                state,
                root,
                "recognizers",
                k_recognizerType,
                recognizers
            );
            installResourceTable<PageHandleSpec, annotation::PageId>(
                state,
                root,
                "pages",
                k_pageRefType,
                pages
            );
            installErrorKindTable(state, root);

            UF_TRY(script::deepFreeze(state, root));
            lua_setglobal(state, k_ufRoot);
            return ok();
        }

        // Assembles the private capability surface and LEAVES IT ON THE STACK,
        // which is the contract script::PrivateCapabilityInstaller states: the
        // boot hands this one table to the framework bundle as a chunk argument
        // and then drops it, so no name in either environment ever refers to it.
        //
        // It is frozen for the same reason the uf table is: a framework bug
        // that rebound a primitive would silently redefine what "click" means,
        // and freezing turns that into an immediate error instead.
        [[nodiscard]]
        auto buildPrivateSurface(lua_State* state, TaskContext* context) -> Status
        {
            UF_TRY(installSessionMetatables(state));

            lua_newtable(state);
            int const surface = lua_gettop(state);

            installPrimitive(
                state,
                surface,
                "cycle_open",
                &cycleOpenFn,
                "uf_cycle_open",
                context
            );
            installPrimitive(
                state,
                surface,
                "cycle_close",
                &cycleCloseFn,
                "uf_cycle_close",
                context
            );
            installPrimitive(
                state,
                surface,
                "cycle_page",
                &cyclePageFn,
                "uf_cycle_page",
                context
            );
            installPrimitive(
                state,
                surface,
                "cycle_find",
                &cycleFindFn,
                "uf_cycle_find",
                context
            );
            installPrimitive(
                state,
                surface,
                "cycle_click",
                &cycleClickFn,
                "uf_cycle_click",
                context
            );
            installPrimitive(
                state,
                surface,
                "wait_for_page",
                &waitForPageFn,
                "uf_wait_for_page",
                context
            );
            installPrimitive(
                state,
                surface,
                "deadline",
                &deadlineFn,
                "uf_deadline",
                context
            );
            installPrimitive(state, surface, "wait", &waitFn, "uf_wait", context);
            installPrimitive(
                state,
                surface,
                "settle",
                &settleFn,
                "uf_settle",
                context
            );
            installPrimitive(
                state,
                surface,
                "random",
                &randomFn,
                "uf_random",
                context
            );

            // The one non-primitive entry: the Tier B label, so ctx:try can ask
            // whether a caught value wears it without the framework spelling the
            // string itself. See k_errorTagField.
            lua_pushstring(state, k_errorType);
            lua_setfield(state, surface, k_errorTagField);

            return script::deepFreeze(state, surface);
        }
    }

    auto CapabilitySurface::projectGlobals() -> std::vector<std::string>
    {
        return std::vector<std::string>{std::string{k_ufRoot}};
    }

    auto CapabilitySurface::raisedErrorClassifier() -> script::RaisedErrorClassifier
    {
        return [](lua_State* state, int index) -> std::optional<AutomationErrorKind>
        {
            return tierBErrorKind(state, index);
        };
    }

    auto CapabilitySurface::installer() const -> script::HostTableInstaller
    {
        // The installer owns its own snapshot of the specs so it stays valid
        // independently of this surface's lifetime; the specs are plain values.
        auto recognizers = m_recognizers;
        auto pages       = m_pages;
        return [recognizers = std::move(recognizers), pages = std::move(pages)](
                   lua_State* state
               ) -> Status
        {
            return buildUfData(state, recognizers, pages);
        };
    }

    auto CapabilitySurface::privateCapabilities(TaskContext& context)
        -> script::PrivateCapabilityInstaller
    {
        // Lifetime: the closure stores this address, which the caller guarantees
        // outlives the VM this installer configures (see the header contract). The
        // TaskContext is non-movable, so the pointer stays valid for the VM's life;
        // it is a non-owning observation, never an owner.
        TaskContext* const contextPtr = &context;
        return [contextPtr](lua_State* state) -> Status
        {
            return buildPrivateSurface(state, contextPtr);
        };
    }
}
