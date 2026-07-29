#include <task/capability-surface.hpp>
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
#include <variant>
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
        constexpr auto k_frameType        = "umbra.frame";
        constexpr auto k_outcomeType      = "umbra.outcome";
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

        // The payloads carried by the observation and action handles. Each is
        // placement-constructed into a host-owned full userdata and destroyed by
        // destroyBox at GC. The move-only Observation is never stored here: it
        // lives in the TaskContext, keyed by `seq`, so every handle carries only
        // the sequence of the capture it descends from -- enough to reject a
        // cross-frame mix and to fail closed once its frame has been consumed.
        struct FrameBox final
        {
            ObservationSeq seq;
            // SAFETY: a non-owning observation of the host TaskContext that
            // retains this frame's Observation. The installer's lifetime contract
            // (see task-context.hpp) guarantees the context outlives the VM, so
            // this pointer stays valid until the last frame handle is collected.
            // It is never an owner; destroyFrameBox uses it only to release the
            // observation this handle pins when the handle dies.
            TaskContext* context;
        };

        struct OutcomeBox final
        {
            ObservationSeq          seq;
            annotation::PageOutcome outcome;
        };

        struct ResolvedPageBox final
        {
            ObservationSeq           seq;
            annotation::ResolvedPage page;
        };

        struct HitBox final
        {
            ObservationSeq      seq;
            engine::ActionFound action;
        };

        // Runs at VM garbage collection to destroy the value placement-constructed
        // into a handle by pushBoxed.
        template <typename T>
        auto destroyBox(void* storage) -> void
        {
            // SAFETY: `storage` is the userdata block lua_newuserdatadtor handed
            // pushBoxed, where exactly one T was placement-constructed via
            // std::construct_at. Destroy it once here; the VM frees the block
            // afterwards.
            std::destroy_at(static_cast<T*>(storage));
        }

        // Runs at VM garbage collection for a frame handle: releases the
        // observation the frame pins in its TaskContext, then destroys the box.
        // This is what binds the observation's lifetime to its Lua handle. A
        // frame whose click already consumed the observation releases a seq that
        // is no longer live, which TaskContext::release treats as a no-op, so a
        // consumed frame's later collection never double-frees.
        auto destroyFrameBox(void* storage) -> void
        {
            // SAFETY: `storage` is the userdata block lua_newuserdatadtor handed
            // pushBoxed, holding exactly one FrameBox placement-constructed via
            // std::construct_at. Its `context` observes the host TaskContext,
            // which the installer's lifetime contract keeps alive past the VM
            // (see task-context.hpp), so releasing through it here is valid.
            // Destroy the box once; the VM frees the block afterwards.
            auto* const box = static_cast<FrameBox*>(storage);
            box->context->release(box->seq);
            std::destroy_at(box);
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

        // Enforces the frame-retention guardrail before a verb retains one more
        // observation. A script that keeps only the frame it is inspecting never
        // reaches the cap. A polling loop that drops each frame reaches it only
        // with dead frame handles still awaiting collection, so a full VM
        // collection runs their finalisers -- each releasing the observation it
        // pinned -- and the verb proceeds. A cap that still stands after the
        // collection means the script genuinely holds that many frames at once,
        // each pinning a whole screenshot in host memory; that is a Tier B
        // InvalidResource naming the cause rather than a silent memory blowup. A
        // zero cap disables the guard.
        auto guardObservationBudget(lua_State* state, TaskContext* context) -> void
        {
            auto const cap = context->maxLiveObservations();
            if (cap == 0 || context->liveObservationCount() < cap)
            {
                return;
            }

            lua_gc(state, LUA_GCCOLLECT, 0);
            if (context->liveObservationCount() < cap)
            {
                return;
            }

            raiseTierB(
                state,
                AutomationErrorKind::InvalidResource,
                "too many live frames retained at once; each frame pins a whole "
                "screenshot in host memory until it is clicked or goes out of "
                "scope, so keep only the frame you are inspecting"
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

        // Which verb ran and what it was handed. Every task.native_call carries
        // it, so a verb the host fails before the engine is reached -- a stale or
        // cross-frame observation, which is the only failure that produces no
        // engine event at all -- still names the frame the script tried to use.
        // A call-scoped parameter type: `verb` views a string literal and the
        // struct never outlives the call that builds it.
        struct NativeCallIdentity final
        {
            std::string_view                        verb;
            std::optional<uint64>                   observationSeq{};
            std::optional<uint64>                   hitObservationSeq{};
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
                    .verb              = std::string{call.verb},
                    .outcome           = outcome,
                    .observationSeq    = call.observationSeq,
                    .hitObservationSeq = call.hitObservationSeq,
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

        // umbra:capture() -> frame handle. A capture failure maps through the Tier
        // ladder (Tier B, or Tier C for a cancellation).
        auto captureFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);
            guardObservationBudget(state, context);

            // capture mints its own sequence rather than being handed one, so its
            // identity is the verb alone.
            auto const call = NativeCallIdentity{.verb = "capture"};

            auto result = context->capture();
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            pushBoxed<FrameBox>(
                state,
                FrameBox{.seq = *result, .context = context},
                &destroyFrameBox,
                k_frameType
            );
            return 1;
        }

        // frame:resolve_page() -> outcome handle carrying the PageOutcome.
        auto resolvePageFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* frame = checkBox<FrameBox>(state, 1, k_frameType, "frame");
            auto const call = NativeCallIdentity{
                .verb           = "resolve_page",
                .observationSeq = frame->seq,
            };

            auto result = context->resolvePage(frame->seq);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            pushBoxed<OutcomeBox>(
                state,
                OutcomeBox{.seq = frame->seq, .outcome = *std::move(result)},
                &destroyBox<OutcomeBox>,
                k_outcomeType
            );
            return 1;
        }

        // frame:find(recognizer) -> hit handle, or nil for a completed miss
        // (Tier A). A non-recognizer argument is a Tier B InvalidResource.
        auto findFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* frame = checkBox<FrameBox>(state, 1, k_frameType, "frame");
            auto* recognizer =
                checkBox<annotation::RecognizerId>(state, 2, k_recognizerType, "recognizer");
            auto const call = NativeCallIdentity{
                .verb           = "find",
                .observationSeq = frame->seq,
                .recognizerId   = *recognizer,
            };

            auto result = context->findAction(frame->seq, *recognizer);
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
                HitBox{.seq = frame->seq, .action = *std::move(found)},
                &destroyBox<HitBox>,
                k_hitType
            );
            return 1;
        }

        // outcome:resolved() -> resolved-page handle when the outcome resolved a
        // page, otherwise nil. Unknown/Ambiguous were already traced by the engine,
        // so this never raises.
        auto resolvedFn(lua_State* state) -> int
        {
            auto* outcome = checkBox<OutcomeBox>(state, 1, k_outcomeType, "outcome");
            if (auto const* resolved =
                    std::get_if<annotation::ResolvedPage>(&outcome->outcome))
            {
                pushBoxed<ResolvedPageBox>(
                    state,
                    ResolvedPageBox{.seq = outcome->seq, .page = *resolved},
                    &destroyBox<ResolvedPageBox>,
                    k_resolvedPageType
                );
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }

        // umbra:click(page, hit) -> consumes the shared observation and delivers
        // the click. umbra:click desugars to umbra.click(umbra, page, hit), so the
        // page is argument 2 and the hit argument 3.
        auto clickFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* page = checkBox<ResolvedPageBox>(state, 2, k_resolvedPageType, "page");
            auto* hit  = checkBox<HitBox>(state, 3, k_hitType, "hit");

            // Both sequences are recorded because click's first guard rejects a
            // page and a hit drawn from different captures, and the two numbers
            // are that rejection's entire content.
            auto const call = NativeCallIdentity{
                .verb              = "click",
                .observationSeq    = page->seq,
                .hitObservationSeq = hit->seq,
            };

            auto result = context->click(page->seq, hit->seq, page->page, hit->action);
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
            auto* page = checkBox<ResolvedPageBox>(state, 1, k_resolvedPageType, "page");
            auto* reference =
                checkBox<annotation::PageId>(state, 2, k_pageRefType, "page reference");
            lua_pushboolean(state, page->page.pageId() == *reference ? 1 : 0);
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
        // frame = frame }. It desugars to umbra.wait_for_page(umbra, ref, opts), so
        // the page reference is argument 2 and the optional options table argument
        // 3. A timeout raises a Tier B error; a cancellation takes the Tier C
        // sentinel. The returned page and frame share one observation sequence, so
        // find on the frame and click on the page consume the same wait.
        auto waitForPageFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);
            guardObservationBudget(state, context);

            auto* reference =
                checkBox<annotation::PageId>(state, 2, k_pageRefType, "page reference");
            auto const timeout      = readWaitDuration(state, 3, "timeout_ms");
            auto const pollInterval = readWaitDuration(state, 3, "poll_interval_ms");

            // wait_for_page mints the sequence its own observation is retained
            // under, so like capture it is handed none.
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

            pushBoxed<ResolvedPageBox>(
                state,
                ResolvedPageBox{.seq = wait.seq, .page = std::move(wait.page)},
                &destroyBox<ResolvedPageBox>,
                k_resolvedPageType
            );
            lua_setfield(state, table, "page");

            pushBoxed<FrameBox>(
                state,
                FrameBox{.seq = wait.seq, .context = context},
                &destroyFrameBox,
                k_frameType
            );
            lua_setfield(state, table, "frame");
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
        // VM registry. The resource kinds are always registered; the observation
        // and action kinds, and the error guard, only when a session is bound.
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

            beginMetatable(state, k_hitType);
            finishMetatable(state, k_hitType);

            beginMetatable(state, k_resolvedPageType);
            {
                int const metatable = lua_gettop(state);
                addMethod(state, metatable, "is", &isFn, nullptr);
            }
            finishMetatable(state, k_resolvedPageType);

            beginMetatable(state, k_outcomeType);
            {
                int const metatable = lua_gettop(state);
                addMethod(state, metatable, "resolved", &resolvedFn, nullptr);
            }
            finishMetatable(state, k_outcomeType);

            beginMetatable(state, k_frameType);
            {
                int const metatable = lua_gettop(state);
                addMethod(state, metatable, "resolve_page", &resolvePageFn, context);
                addMethod(state, metatable, "find", &findFn, context);
            }
            finishMetatable(state, k_frameType);
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
        // context it also wires the observation and action verbs to the session.
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
                installUmbraVerb(state, umbra, "capture", &captureFn, "umbra_capture", context);
                installUmbraVerb(state, umbra, "click", &clickFn, "umbra_click", context);
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
