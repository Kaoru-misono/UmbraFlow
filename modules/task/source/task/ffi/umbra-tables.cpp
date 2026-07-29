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

#include <chrono>
#include <memory>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <unordered_set>
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
        // Every handle kind's shared metatable is registered in the VM registry
        // under one of these keys, and the same string is the kind's __metatable
        // and __tostring label -- the only string a script can obtain from a
        // handle, naming the kind and nothing else (no id, no address). The
        // recognizer and page-reference spellings are unchanged from the resource
        // wave so existing scripts and tests keep observing them.
        constexpr auto k_recognizerType   = "umbra.recognizer";
        constexpr auto k_pageRefType      = "umbra.page";
        constexpr auto k_cycleType        = "umbra.cycle";
        constexpr auto k_resolvedPageType = "umbra.resolved_page";
        constexpr auto k_hitType          = "umbra.hit";
        constexpr auto k_errorType        = "umbra.error";

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
        // released by umbra:cycle_close or by the click that consumes the cycle,
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

        // Recursively marks the table at `index`, every table reachable from its
        // values, and every metatable on the way, read-only. This mirrors
        // modules/script's internal deepFreeze (ffi/sandbox.hpp), reimplemented
        // here because that routine lives behind script's ffi boundary rather than
        // as a public capability. Cycle-safe: each table once.
        auto freezeInto(
            lua_State* state,
            int index,
            std::unordered_set<void const*>& visited
        ) -> void
        {
            int const table = lua_absindex(state, index);
            if (!visited.insert(lua_topointer(state, table)).second)
            {
                return;
            }

            // Freeze the metatable BEFORE the table: a still-writable metatable
            // would let a script rewrite __index/__newindex and monkey-patch
            // around the frozen table it guards.
            if (lua_getmetatable(state, table) != 0)
            {
                freezeInto(state, -1, visited);
                lua_pop(state, 1);
            }

            lua_setreadonly(state, table, 1);

            lua_pushnil(state);
            while (lua_next(state, table) != 0)
            {
                if (lua_istable(state, -1))
                {
                    freezeInto(state, -1, visited);
                }
                lua_pop(state, 1);
            }
        }

        auto freezeRecursive(lua_State* state, int index) -> void
        {
            auto visited = std::unordered_set<void const*>{};
            freezeInto(state, index, visited);
        }

        // Bound as every handle metatable's __newindex: rejects every write with a
        // clear error so a handle can never be mutated from a script.
        auto denyWrite(lua_State* state) -> int
        {
            // luaL_error is l_noret: it longjmps out of this frame and never
            // returns, so this lua_CFunction never falls off its int-returning end.
            luaL_error(state, "umbra capability handles are read-only");
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

        // True when the value at `index` is a table whose metatable is the shared
        // umbra.error metatable -- i.e. a Tier B error table this binding raised.
        // A Tier B error is a table, not a userdata handle, so isKind (which
        // requires userdata) cannot recognize it; this is the table-shaped analogue
        // umbra:try uses to tell an automation error apart from a script's own.
        [[nodiscard]]
        auto isErrorTable(lua_State* state, int index) -> bool
        {
            if (lua_type(state, index) != LUA_TTABLE)
            {
                return false;
            }
            if (lua_getmetatable(state, index) == 0)
            {
                return false;
            }
            luaL_getmetatable(state, k_errorType);
            bool const same = lua_rawequal(state, -1, -2) != 0;
            lua_pop(state, 2);
            return same;
        }

        // Builds a frozen Tier B error table { kind, message, retryable } under the
        // shared protected umbra.error metatable and raises it. `try` (a later
        // wave) identifies these by that metatable; scripts can neither forge nor
        // mutate them. Never returns.
        [[noreturn]]
        auto raiseTierB(
            lua_State* state,
            AutomationErrorKind kind,
            std::string const& message
        ) -> void
        {
            lua_createtable(state, 0, 3);
            int const table = lua_gettop(state);

            pushWireName(state, kind);
            lua_setfield(state, table, "kind");
            lua_pushstring(state, message.c_str());
            lua_setfield(state, table, "message");
            lua_pushboolean(state, retryableOf(kind) ? 1 : 0);
            lua_setfield(state, table, "retryable");

            luaL_getmetatable(state, k_errorType);
            lua_setmetatable(state, table);
            lua_setreadonly(state, table, 1);

            // lua_error takes the value on top of the stack (the frozen table) and
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
        // then raise a plain non-table sentinel. It carries no umbra.error
        // metatable, so a future umbra:try refuses to swallow it. Full pcall
        // immunity needs the shared lua_break cancel source and lands with the task
        // runner in a later wave; until then the fatal flag is the host's signal.
        [[noreturn]]
        auto raiseCancelled(lua_State* state, TaskContext* context) -> void
        {
            context->markFatal();
            lua_pushstring(state, "umbra: task cancelled");
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

        // Reads the TaskContext bound as upvalue 1 of an observation/action verb.
        [[nodiscard]]
        auto boundContext(lua_State* state) -> TaskContext*
        {
            // SAFETY: upvalue 1 of every verb closure is the lightuserdata
            // installMetatables/buildUmbra installed -- a pointer to the host-owned
            // TaskContext that outlives the VM by the installer's lifetime
            // contract. No other value is ever stored there, so casting the opaque
            // pointer back to TaskContext* is sound. The verb runs only on the VM's
            // owning thread, so the access is race-free.
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
        // For a cancellation this also keeps the Tier C sentinel on the raise path.
        // That is defense in depth rather than a live hole -- umbra:try re-checks
        // the shared stop token and the VM interrupt breaks the thread anyway, so
        // no test here can distinguish the orders. It is written this way because
        // depending on those layers to cover a downgrade is how a later change to
        // either one turns into a swallowed cancel. Never returns.
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

        // umbra:cycle_open() -> ticket. Observes one frame and opens the
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

        // umbra:cycle_close(ticket) -> (). Releases the frame the cycle retains,
        // deterministically and at a moment the host chose. Idempotent: a ticket
        // that names no open cycle -- closed already, consumed by a click, or
        // minted by another generation -- records an Empty call and returns.
        // umbra:cycle_close desugars to umbra.cycle_close(umbra, ticket), so the
        // ticket is argument 2.
        auto cycleCloseFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 2, k_cycleType, "cycle");
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

        // umbra:cycle_page(ticket) -> resolved-page handle, or nil when the frame
        // resolved to Unknown or Ambiguous (both already traced by the engine, so
        // neither raises). A successful resolution is also recorded in the ledger
        // as this cycle's click authorization evidence, which is the only way a
        // click can ever be authorized.
        auto cyclePageFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 2, k_cycleType, "cycle");
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

        // umbra:cycle_find(ticket, recognizer) -> hit handle, or nil for a
        // completed miss (Tier A). A non-recognizer argument is a Tier B
        // InvalidResource.
        auto cycleFindFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 2, k_cycleType, "cycle");
            auto* recognizer =
                checkBox<annotation::RecognizerId>(state, 3, k_recognizerType, "recognizer");
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

        // umbra:cycle_click(ticket, hit) -> (). Consumes the cycle and delivers
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

            auto* ticket = checkBox<CycleTicket>(state, 2, k_cycleType, "cycle");
            auto* hit    = checkBox<HitBox>(state, 3, k_hitType, "hit");

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
                    std::string{"wait_for_page option '"} + field
                        + "' must be a number of milliseconds"
                );
            }
            double const millis = lua_tonumber(state, -1);
            lua_pop(state, 1);

            // The wait Duration is steady_clock's tick -- nanoseconds on every
            // supported standard library -- so turning a millisecond count into it
            // multiplies by the ms-to-tick ratio. A raw duration_cast would overflow
            // the signed tick rep (undefined behaviour) far below the millisecond
            // count's own int64 limit, and before waitForPage's monotonic-overflow
            // guard could ever run. Build the Duration through the checked helpers
            // instead: a NaN, negative, non-finite, or out-of-range count -- or one
            // whose tick product would not fit -- is a Tier B InvalidResource here
            // rather than silent wraparound.
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
                    std::string{"wait_for_page option '"} + field
                        + "' must be a finite, non-negative millisecond count within range"
                );
            }
            return Duration{*ticks};
        }

        // umbra:wait_for_page(pageReference, options) -> { page = resolved-page,
        // cycle = ticket }. It desugars to umbra.wait_for_page(umbra, ref, opts),
        // so the page reference is argument 2 and the optional options table
        // argument 3. A timeout raises a Tier B error; a cancellation takes the
        // Tier C sentinel.
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
                checkBox<annotation::PageId>(state, 2, k_pageRefType, "page reference");
            auto const timeout      = readWaitDuration(state, 3, "timeout_ms");
            auto const pollInterval = readWaitDuration(state, 3, "poll_interval_ms");

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

        // umbra:try(fn) -> (ok, err). Runs `fn` under a protected call. A completed
        // run yields (true, nil). A Tier B automation error -- identified by the
        // protected umbra.error metatable -- is returned as (false, errorTable). A
        // cancellation is never recoverable through try: it re-raises the
        // non-catchable Tier C sentinel so the generation stays spent. Every other
        // failure is the script's own error (a raised value, a nil index, a
        // break-across-C-call boundary surfaced by a hard cancel inside a C frame)
        // and propagates unchanged, so a script bug is never masked. umbra:try
        // desugars to umbra.try(umbra, fn), so `fn` is argument 2.
        auto tryFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            if (lua_type(state, 2) != LUA_TFUNCTION)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "umbra:try expects a function to run"
                );
            }

            lua_pushvalue(state, 2);
            int const status = lua_pcall(state, 0, 0, 0);

            // A cancellation must never be recoverable through try. The single
            // cancel source is observable on the context (its stop token), and an
            // engine verb that hit Cancelled also latched fatal. lua_pcall runs
            // `fn` inside a C frame, so a hard break surfaces as a catchable
            // break-across-C-call error rather than an uncatchable LUA_BREAK;
            // consulting the stop token and the fatal latch -- not the raised value
            // -- is how the binding classifies it. Check both before returning any
            // value or classifying an ordinary error, then re-raise the Tier C
            // sentinel; the armed VM interrupt keeps re-breaking the thread too.
            if (context->cancelled() || context->fatal())
            {
                raiseCancelled(state, context);
            }

            if (status == LUA_OK)
            {
                lua_pushboolean(state, 1);
                lua_pushnil(state);
                return 2;
            }

            // The failed pcall left the error value on the stack top. A Tier B
            // automation error is a table carrying the protected umbra.error
            // metatable; return it as (false, errorTable).
            if (isErrorTable(state, -1))
            {
                lua_pushboolean(state, 0);
                lua_pushvalue(state, -2);
                return 2;
            }

            // Not a Tier B automation error: re-raise the script's own error so it
            // propagates past try unchanged.
            lua_error(state);
        }

        // umbra:now() -> number. The task's virtualized logical clock in whole
        // milliseconds: a strictly increasing, fully reproducible ordinal, not real
        // elapsed time (see DeterministicClock). It has no engine effect, so unlike
        // the automation verbs it is not gated on the cancellation/fatal latch: it
        // neither drives nor observes the session, and the armed VM interrupt
        // already stops a cancelled generation on its own. Reading advances the
        // logical clock, which is why nowMillis is a non-const context call.
        auto nowFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            lua_pushnumber(state, static_cast<double>(context->nowMillis()));
            return 1;
        }

        // The largest magnitude an umbra:random bound may take. Beyond 2^53 a Lua
        // number (an IEEE double) can no longer represent every integer, so the
        // value the script passed would already be rounded and a returned integer
        // might not round-trip. Bounding here keeps every result an exact integer.
        constexpr auto k_maxExactInteger = int64{9007199254740992};

        // Reads a umbra:random bound at `index` as an integer. A non-number, a
        // non-integral value, or one outside +/-2^53 is a Tier B InvalidResource,
        // keeping the script-facing failure model single-shaped.
        [[nodiscard]]
        auto checkRandomInteger(lua_State* state, int index) -> int64
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "umbra:random bounds must be integers"
                );
            }
            double const raw   = lua_tonumber(state, index);
            auto const   value = checkedIntegralCast<int64>(raw);
            if (!value || *value > k_maxExactInteger || *value < -k_maxExactInteger)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "umbra:random bounds must be whole numbers within +/-2^53"
                );
            }
            return *value;
        }

        // umbra:random([m [, n]]) -> number. Mirrors Lua's math.random over the
        // host's deterministic seeded RNG: no argument yields a double in [0, 1);
        // one argument m yields an integer in [1, m]; two arguments yield an integer
        // in [m, n]. The arguments desugar to umbra.random(umbra, ...), so they sit
        // at stack indices 2 and 3. A bad shape -- a non-integer bound, m < 1, or
        // m > n -- is a Tier B InvalidResource, matching math.random's own rejection
        // of an empty interval.
        auto randomFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);

            bool const hasLow  = !lua_isnoneornil(state, 2);
            bool const hasHigh = !lua_isnoneornil(state, 3);

            if (!hasLow && !hasHigh)
            {
                lua_pushnumber(state, context->nextRandomUnitDouble());
                return 1;
            }

            int64 const low = checkRandomInteger(state, 2);
            if (!hasHigh)
            {
                if (low < 1)
                {
                    raiseTierB(
                        state,
                        AutomationErrorKind::InvalidResource,
                        "umbra:random(m) requires m >= 1"
                    );
                }
                lua_pushnumber(
                    state,
                    static_cast<double>(context->nextRandomInRange(int64{1}, low))
                );
                return 1;
            }

            int64 const high = checkRandomInteger(state, 3);
            if (low > high)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "umbra:random(m, n) requires m <= n"
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

            lua_pushcfunction(state, &denyWrite, "umbra_handle_newindex");
            lua_setfield(state, metatable, "__newindex");

            lua_pushstring(state, label);
            lua_pushcclosure(state, &handleToString, "umbra_handle_tostring", 1);
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
        auto finishMetatable(lua_State* state, char const* registryType) -> void
        {
            freezeRecursive(state, -1);
            lua_setfield(state, LUA_REGISTRYINDEX, registryType);
        }

        // The Tier B error metatable is a data table's guard, not a handle's: it
        // needs only __metatable protection (the table itself is frozen readonly),
        // no __index, __newindex, or __tostring.
        auto installErrorMetatable(lua_State* state) -> void
        {
            lua_newtable(state);
            int const metatable = lua_gettop(state);
            lua_pushstring(state, k_errorType);
            lua_setfield(state, metatable, "__metatable");
            finishMetatable(state, k_errorType);
        }

        // Registers every handle kind's shared, frozen, protected metatable in the
        // VM registry. The resource kinds are always registered; the cycle and
        // action kinds, and the error guard, only when a session is bound.
        //
        // Only the resolved page carries a method. A ticket and a hit are pure
        // names the host hands back to itself, so every operation on a cycle is a
        // root primitive taking the ticket -- which is also the shape the private
        // capability surface takes once the Luau framework owns these calls.
        auto installMetatables(lua_State* state, TaskContext* context) -> void
        {
            beginMetatable(state, k_recognizerType);
            finishMetatable(state, k_recognizerType);

            beginMetatable(state, k_pageRefType);
            finishMetatable(state, k_pageRefType);

            if (context == nullptr)
            {
                return;
            }

            installErrorMetatable(state);

            beginMetatable(state, k_cycleType);
            finishMetatable(state, k_cycleType);

            beginMetatable(state, k_hitType);
            finishMetatable(state, k_hitType);

            beginMetatable(state, k_resolvedPageType);
            {
                int const metatable = lua_gettop(state);
                addMethod(state, metatable, "is", &isFn, nullptr);
            }
            finishMetatable(state, k_resolvedPageType);
        }

        // Populates `umbra[fieldName]` with a name table of opaque handles, one per
        // spec, each carrying the metatable registered under `metatableType`.
        template <typename Spec, typename Id>
        auto installResourceTable(
            lua_State* state,
            int umbra,
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
            lua_setfield(state, umbra, fieldName);
        }

        // Populates `umbra.errors` with one constant per AutomationErrorKind. Both
        // the key and the value are that kind's domain wire spelling, so a script
        // writes `err.kind == umbra.errors.timeout` and compares the exact string
        // the Tier B error and the trace line both carry.
        //
        // The table is built here, from the same domain function, rather than
        // generated into a .luau file by a build step. A generator would parse the
        // enum and emit a third artifact that has to be kept in sync; reading the
        // enum at install time leaves one source of truth by construction, with no
        // parse, no codegen and nothing to go stale. Iteration is over the
        // reflected entries, which is the repository's existing enumeration of the
        // kinds, so a new kind appears here with no edit to this function.
        auto installErrorKindTable(lua_State* state, int umbra) -> void
        {
            lua_newtable(state);
            int const table = lua_gettop(state);
            for (auto const& entry : enumEntries<AutomationErrorKind>())
            {
                pushWireName(state, entry.value);
                pushWireName(state, entry.value);
                lua_rawset(state, table);
            }
            lua_setfield(state, umbra, "errors");
        }

        // Binds one umbra-rooted verb (`fieldName`) to a C closure carrying the
        // TaskContext as lightuserdata upvalue 1.
        auto installUmbraVerb(
            lua_State* state,
            int umbra,
            char const* fieldName,
            lua_CFunction function,
            char const* debugName,
            TaskContext* context
        ) -> void
        {
            lua_pushlightuserdata(state, context);
            lua_pushcclosure(state, function, debugName, 1);
            lua_setfield(state, umbra, fieldName);
        }

        // Assembles the whole frozen global umbra table. With a null context it is
        // the resource-only surface (recognizers and pages, no verbs); with a
        // context it also wires the observation-cycle primitives to the session.
        auto buildUmbra(
            lua_State* state,
            std::vector<RecognizerHandleSpec> const& recognizers,
            std::vector<PageHandleSpec> const& pages,
            TaskContext* context
        ) -> void
        {
            installMetatables(state, context);

            lua_newtable(state);
            int const umbra = lua_gettop(state);

            installResourceTable<RecognizerHandleSpec, annotation::RecognizerId>(
                state,
                umbra,
                "recognizers",
                k_recognizerType,
                recognizers
            );
            installResourceTable<PageHandleSpec, annotation::PageId>(
                state,
                umbra,
                "pages",
                k_pageRefType,
                pages
            );

            // Independent of the session: the kind constants are the same strings
            // whether or not verbs are bound, so both installer overloads expose
            // them and a script sees one surface shape.
            installErrorKindTable(state, umbra);

            if (context != nullptr)
            {
                installUmbraVerb(
                    state,
                    umbra,
                    "cycle_open",
                    &cycleOpenFn,
                    "umbra_cycle_open",
                    context
                );
                installUmbraVerb(
                    state,
                    umbra,
                    "cycle_close",
                    &cycleCloseFn,
                    "umbra_cycle_close",
                    context
                );
                installUmbraVerb(
                    state,
                    umbra,
                    "cycle_page",
                    &cyclePageFn,
                    "umbra_cycle_page",
                    context
                );
                installUmbraVerb(
                    state,
                    umbra,
                    "cycle_find",
                    &cycleFindFn,
                    "umbra_cycle_find",
                    context
                );
                installUmbraVerb(
                    state,
                    umbra,
                    "cycle_click",
                    &cycleClickFn,
                    "umbra_cycle_click",
                    context
                );
                installUmbraVerb(
                    state,
                    umbra,
                    "wait_for_page",
                    &waitForPageFn,
                    "umbra_wait_for_page",
                    context
                );
                installUmbraVerb(state, umbra, "try", &tryFn, "umbra_try", context);
                installUmbraVerb(state, umbra, "now", &nowFn, "umbra_now", context);
                installUmbraVerb(
                    state,
                    umbra,
                    "random",
                    &randomFn,
                    "umbra_random",
                    context
                );
            }

            freezeRecursive(state, umbra);
            lua_setglobal(state, "umbra");
        }
    }

    auto CapabilitySurface::installer() const -> script::HostTableInstaller
    {
        // The installer owns its own snapshot of the specs so it stays valid
        // independently of this surface's lifetime; the specs are plain values.
        auto recognizers = m_recognizers;
        auto pages       = m_pages;
        return [recognizers = std::move(recognizers), pages = std::move(pages)](
                   lua_State* state
               ) -> void
        {
            buildUmbra(state, recognizers, pages, nullptr);
        };
    }

    auto CapabilitySurface::installer(TaskContext& context) const
        -> script::HostTableInstaller
    {
        auto recognizers = m_recognizers;
        auto pages       = m_pages;

        // Lifetime: the closure stores this address, which the caller guarantees
        // outlives the VM this installer configures (see the header contract). The
        // TaskContext is non-movable, so the pointer stays valid for the VM's life;
        // it is a non-owning observation, never an owner.
        TaskContext* const contextPtr = &context;
        return [recognizers = std::move(recognizers),
                pages = std::move(pages),
                contextPtr](lua_State* state) -> void
        {
            buildUmbra(state, recognizers, pages, contextPtr);
        };
    }
}
