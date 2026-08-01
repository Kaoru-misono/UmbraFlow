#include <task/cycle-ledger.hpp>
#include <task/native-call-trace.hpp>
#include <task/pixel-probe.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>
#include <task/template-store.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
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
        // The first three are also VM registry keys: a handle kind's metatable is
        // shared by every handle of that kind and is registered under its label.
        // k_errorType is not, because a Tier B error carrier's metatable holds
        // that one error's fields and so exists per instance; the label is still
        // what a script sees, and the label is what the framework compares
        // against, but the carrier's identity to C++ is its userdata tag.
        constexpr auto k_cycleType    = "uf.cycle";
        constexpr auto k_templateType = "uf.template";
        constexpr auto k_deadlineType = "uf.deadline";
        constexpr auto k_errorType    = "uf.error";

        // The two kinds that carry READABLE fields, and so cannot share a
        // registered metatable: the score a match reports and the text a read
        // produces are per instance, and the design restricts __index to a table,
        // so each instance needs its own metatable exactly as a Tier B error
        // carrier does. Their identity to C++ is a userdata tag rather than a
        // registry entry, for the same reason.
        constexpr auto k_matchType   = "uf.match";
        constexpr auto k_readingType = "uf.reading";
        constexpr auto k_probeType   = "uf.probe";

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

        // The tags the two field-carrying handle kinds are minted under, on the
        // same reasoning as the error carrier's: a tag is VM state that a script
        // cannot copy onto a value it built, so it is the whole of how C++
        // recognizes a match handed back to cycle_click.
        constexpr auto k_matchUserdataTag = 2;
        static_assert(k_matchUserdataTag > 0 && k_matchUserdataTag < LUA_UTAG_LIMIT);

        constexpr auto k_readingUserdataTag = 3;
        static_assert(k_readingUserdataTag > 0 && k_readingUserdataTag < LUA_UTAG_LIMIT);

        constexpr auto k_probeUserdataTag = 4;
        static_assert(k_probeUserdataTag > 0 && k_probeUserdataTag < LUA_UTAG_LIMIT);

        // The colour tolerance a probe uses when the caller named a key but no
        // tolerance, matching the v4 authoring line's own `--tolerance` default
        // so a measurement taken through either route reports the same counts.
        //
        // It is the ONE default on this verb, and it is a default about the
        // measurement rather than about the answer: what counts as enough
        // selected pixels is never decided here.
        constexpr auto k_defaultProbeTolerance = uint32{12};

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
        // What a match handle carries: the ordinal of the cycle that produced it
        // plus the position C++ will deliver to. The score and the ceiling ALSO
        // reach the script, through the handle's frozen fields, because judging a
        // score is the trusted framework's job now and it cannot judge what it
        // cannot read.
        //
        // The move-only Observation is never stored in a handle: it lives in the
        // TaskContext's CycleLedger, and the only handle that names it is the
        // ticket. A match therefore carries just the ordinal of the cycle that
        // found it, which is the whole of its staleness check -- with at most one
        // cycle open, an ordinal that is not the open one names a cycle that no
        // longer exists, and there is no second cycle it could have come from.
        //
        // Trivially destructible, which is what lets it be a tagged userdata and
        // therefore recognizable to C++ by tag rather than by metatable.
        struct MatchBox final
        {
            uint64             cycleOrdinal{};
            engine::MatchFound found;
        };
        static_assert(std::is_trivially_destructible_v<MatchBox>);

        // Runs at VM garbage collection to destroy the value placement-constructed
        // into a handle by pushBoxed.
        //
        // No handle's destructor releases a frame. Collecting a ticket or a
        // template frees only the few bytes of the box: the frame behind them is
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

        // The Tier B error carrier at `index` read back, or nullopt when that
        // value is not one.
        //
        // IDENTITY IS THE USERDATA TAG AND NOTHING ELSE, so a script-built
        // look-alike carrying `kind`, `message` and `retryable` is not a Tier B
        // error here however faithfully it copies one. The message is read only
        // AFTER the tag has answered, which is the ordering that keeps that
        // property: by then the value is one raiseTierB below built, and its
        // shape is this file's own.
        //
        // Every lookup is raw and reaches no metamethod, because this runs on a
        // thread that has already been unwound by the error -- there is no
        // protected frame left, so a raise from a metamethod would have nowhere
        // to go. The stack is restored on every path for the same reason: the
        // caller is entitled to find the thread exactly as it left it.
        [[nodiscard]]
        auto tierBError(
            lua_State* state,
            int index
        ) -> std::optional<script::RaisedError>
        {
            auto const carrier = lua_absindex(state, index);
            if (lua_type(state, carrier) != LUA_TUSERDATA)
            {
                return std::nullopt;
            }
            if (lua_userdatatag(state, carrier) != k_errorUserdataTag)
            {
                return std::nullopt;
            }
            // SAFETY: the tag is stamped by the VM at creation and cannot be set
            // from script, and raiseTierB below is the only place that stamps
            // this one, so the block holds exactly one live TierBError.
            auto const* p_error =
                static_cast<TierBError const*>(lua_touserdata(state, carrier));
            auto raised = script::RaisedError{.kind = p_error->kind};

            int const base  = lua_gettop(state);
            auto stackGuard = scopeExit(
                [state, base]() noexcept
                {
                    lua_settop(state, base);
                }
            );

            // The carrier's three script-visible fields live in a frozen table
            // behind its metatable's __index, which raiseTierB puts there and
            // deepFreezeMetatable checks is a table. Walking it by hand is what
            // avoids tostring(), which on a userdata would have to run the
            // __tostring metamethod.
            if (lua_getmetatable(state, carrier) == 0)
            {
                return raised;
            }
            lua_rawgetfield(state, -1, "__index");
            if (lua_type(state, -1) != LUA_TTABLE)
            {
                return raised;
            }
            lua_rawgetfield(state, -1, "message");
            if (lua_type(state, -1) == LUA_TSTRING)
            {
                std::size_t length = 0;
                char const* text   = lua_tolstring(state, -1, &length);
                if (text != nullptr)
                {
                    raised.message.assign(text, length);
                }
            }
            return raised;
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

        // Tier C: latch the terminal kind so the host discards this VM
        // generation, then raise a plain non-table sentinel. It carries no
        // uf.error metatable, so ctx:try refuses to swallow it and re-raises it
        // unchanged.
        //
        // A project pcall CAN catch this value, and that is accepted: control is
        // not what the sentinel protects. The latch is. Every primitive enters
        // through guardFatal, so a script that swallowed the sentinel and kept
        // running is refused at its next primitive call, before any capture or
        // any click.
        [[noreturn]]
        auto raiseCancelled(lua_State* state, TaskContext* context) -> void
        {
            context->markTerminal(AutomationErrorKind::Cancelled);
            lua_pushstring(state, "uf: task cancelled");
            lua_error(state);
        }

        // A framework bug the host caught: latch the generation terminal FIRST,
        // then raise an InternalInvariant carrier. Never returns.
        //
        // The order is the whole point, and it is design section 9's rule 5: a
        // project pcall or ctx:try can catch the value -- it is an ordinary Tier
        // B carrier and there is no way to make a Lua raise uncatchable -- but
        // the latch is already set, so the next primitive refuses at guardFatal
        // before it reaches the engine. Control is protected even though the
        // value is catchable, which is exactly the Tier C position.
        //
        // The kind is a real one rather than a private sentinel because a run
        // that ends on it must SAY so: the host's raised-error classifier reads
        // the carrier's tag, so an uncaught framework bug is reported and traced
        // as internal_invariant rather than as a malformed script.
        [[noreturn]]
        auto raiseInvariant(
            lua_State* state,
            TaskContext* context,
            std::string const& message
        ) -> void
        {
            context->markTerminal(AutomationErrorKind::InternalInvariant);
            raiseTierB(state, AutomationErrorKind::InternalInvariant, message);
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

        // Starts one field-carrying handle: mints a tagged userdata holding
        // `value` and pushes an empty field table above it. The caller fills the
        // fields with scalars and then calls finishFieldHandle.
        //
        // It is split in two because the fields differ per kind while the
        // freezing and the metatable shape do not, and that shape is the part
        // that must not be reinvented: __index a table and never a function, a
        // __metatable label, and frozen before anything wears it.
        template <typename T>
        auto beginFieldHandle(lua_State* state, T value, int tag) -> void
        {
            // SAFETY: lua_newuserdatatagged allocates sizeof(T) VM-owned bytes,
            // aligned as lua_newuserdatadtor's blocks are (see pushBoxed), and T
            // is trivially destructible here, so no destructor has to be
            // registered for the tag and nothing leaks when the VM frees the
            // block. No Lua allocation runs between the allocation and the
            // construction, so a collection can never see the block
            // uninitialised.
            static_assert(std::is_trivially_destructible_v<T>);
            static_assert(alignof(T) <= 16);
            void* storage = lua_newuserdatatagged(state, sizeof(T), tag);
            std::construct_at(static_cast<T*>(storage), std::move(value));

            lua_createtable(state, 0, 8);
        }

        // Freezes the field table on the stack top into a per-instance metatable,
        // attaches it to the carrier beneath, and leaves only the carrier.
        auto finishFieldHandle(
            lua_State* state,
            TaskContext* context,
            char const* label
        ) -> void
        {
            int const fields  = lua_gettop(state);
            int const carrier = fields - 1;

            lua_createtable(state, 0, 4);
            int const metatable = lua_gettop(state);

            lua_pushvalue(state, fields);
            lua_setfield(state, metatable, "__index");

            lua_pushcfunction(state, &denyWrite, "uf_handle_newindex");
            lua_setfield(state, metatable, "__newindex");

            lua_pushstring(state, label);
            lua_pushcclosure(state, &handleToString, "uf_handle_tostring", 1);
            lua_setfield(state, metatable, "__tostring");

            lua_pushstring(state, label);
            lua_setfield(state, metatable, "__metatable");

            if (!script::deepFreezeMetatable(state, metatable))
            {
                // Unreachable while the metatable is the one built directly
                // above: it carries __metatable and a table __index by
                // construction. Handing a script a half-frozen object that
                // answers to a host label would be worse than failing.
                raiseInvariant(
                    state,
                    context,
                    "a host data handle could not be frozen"
                );
            }
            lua_setmetatable(state, carrier);
            lua_settop(state, carrier);
        }

        // Adds one whole-number field to the field table on the stack top.
        auto addNumberField(lua_State* state, char const* name, uint64 value) -> void
        {
            lua_pushnumber(state, static_cast<double>(value));
            lua_setfield(state, -2, name);
        }

        // Refuses the call outright when a prior verb already latched the
        // generation terminal, so a script that swallowed what was raised cannot
        // drive one more engine verb before the host tears the generation down.
        //
        // The question itself is requireLiveGeneration's, shared with the operator
        // front-end, so both consumers of the capability surface refuse a spent
        // generation on the same terms; what stays here is only the raise. It is
        // still asked BEFORE this primitive decodes its arguments, so a spent
        // generation outranks a bad handle -- the caller is told what ended the run
        // rather than what was wrong with the call that came after it.
        auto guardFatal(lua_State* state, TaskContext* context) -> void
        {
            auto const live = requireLiveGeneration(*context);
            if (live)
            {
                return;
            }
            raiseFromError(state, context, live.error());
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

        // Emits the task.native_call trace event for a verb at its exit, and turns
        // a lost line into a Tier B IoFailure that aborts the verb. Both halves --
        // the line's shape and what a sink failure costs -- are
        // native-call-trace.hpp's, shared with the operator front-end; what this
        // adds is the raise.
        auto traceHostCall(
            lua_State* state,
            TaskContext* context,
            NativeCallIdentity const& call,
            trace::NativeCallOutcome outcome
        ) -> void
        {
            auto status = recordNativeCall(*context, call, outcome);
            if (!status)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::IoFailure,
                    std::string{status.error().message()}
                );
            }
        }

        // Records a failed verb and then raises that verb's own error, never the
        // sink's -- see recordNativeCallFailure for why the original cause wins.
        //
        // For a cancellation this keeps the Tier C sentinel on the raise path,
        // which latches fatal before anything else can run. That ordering is
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
            recordNativeCallFailure(*context, call, error);
            raiseFromError(state, context, error);
        }

        // Reads a required string argument at `index` as a view into the VM's own
        // storage. Tier B rather than an invariant failure: every string a
        // primitive takes -- a project file name, a template blob -- comes from a
        // project's own data, so a wrong type is an author error to catch.
        [[nodiscard]]
        auto checkText(
            lua_State* state,
            int index,
            std::string const& what
        ) -> std::string_view
        {
            if (lua_type(state, index) != LUA_TSTRING)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " requires a string"
                );
            }

            // SAFETY: the value was just confirmed to be a string, so
            // lua_tolstring performs no conversion and returns the VM-owned bytes
            // with their length. It stays on the stack for the whole call, so the
            // view is taken from live storage and never outlives it.
            std::size_t length = 0;
            char const* p_text = lua_tolstring(state, index, &length);
            return std::string_view{p_text, length};
        }

        // Reads one whole pixel coordinate or extent at `index`.
        [[nodiscard]]
        auto checkPixelExtent(
            lua_State* state,
            int index,
            std::string const& what
        ) -> uint32
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " requires a number"
                );
            }
            auto const value = checkedIntegralCast<uint32>(lua_tonumber(state, index));
            if (!value)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " must be a whole, non-negative pixel count within range"
                );
            }
            return *value;
        }

        // Reads one 0-255 colour channel at `index`.
        [[nodiscard]]
        auto checkColourChannel(
            lua_State* state,
            int index,
            std::string const& what
        ) -> uint8
        {
            auto const value = checkPixelExtent(state, index, what);
            if (value > 255U)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " must be a channel value between 0 and 255"
                );
            }
            return static_cast<uint8>(value);
        }

        // Reads four numbers starting at `first` as one rectangle.
        //
        // Four scalars rather than one table, because section 5's second
        // invariant admits only host-minted handles and scalars as primitive
        // arguments and a Luau table is neither. The trusted framework wraps this
        // back into whatever shape its own callers want.
        [[nodiscard]]
        auto checkPixelRect(
            lua_State* state,
            int first,
            std::string const& what
        ) -> PixelRect
        {
            auto const x      = checkPixelExtent(state, first, what + " x");
            auto const y      = checkPixelExtent(state, first + 1, what + " y");
            auto const width  = checkPixelExtent(state, first + 2, what + " width");
            auto const height = checkPixelExtent(state, first + 3, what + " height");

            auto const rect = PixelRect::create(x, y, width, height);
            if (!rect)
            {
                raiseTierB(
                    state,
                    kindOf(rect.error()),
                    std::string{rect.error().message()}
                );
            }
            return *rect;
        }

        // The match handle at `index`, or null when that value is not one. The
        // test is the userdata tag and nothing else, exactly as for a Tier B
        // error carrier: a script-built look-alike carrying the same fields is
        // not a match here however faithfully it copies one.
        [[nodiscard]]
        auto matchBoxAt(lua_State* state, int index) -> MatchBox const*
        {
            if (lua_type(state, index) != LUA_TUSERDATA)
            {
                return nullptr;
            }
            if (lua_userdatatag(state, index) != k_matchUserdataTag)
            {
                return nullptr;
            }
            // SAFETY: the tag is stamped by the VM at creation and cannot be set
            // from script, and cycle_match below is the only place that stamps
            // this one, so the block holds exactly one live MatchBox.
            return static_cast<MatchBox const*>(lua_touserdata(state, index));
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
        // It takes no deadline, and never will. The capture IS bounded --
        // EngineSession::observe mints a CaptureBudget from the session's own
        // captureTimeout and hands IFrameSource::capture both that deadline and
        // the run's stop token -- but the bound is the HOST's, not the script's.
        // How long one screenshot may block is a resource boundary the host
        // owns, so a script is given no knob it could widen. The one deadline a
        // script can hold comes from the `deadline` primitive, and it bounds a
        // wait loop rather than any single capture.
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

        // template_load(blob) -> template handle. Decodes one template PNG once
        // and names the result for the rest of this generation.
        //
        // Handle-based rather than decode-per-match, and that is a decision
        // rather than an optimisation: a wait loop matching one template per poll
        // would otherwise pay a PNG decode per poll, and since decoding is
        // deterministic, repeating it can only cost time and never change an
        // answer. Loading the same bytes twice returns the same handle, so a
        // script's load order cannot change what it ends up holding.
        auto templateLoadFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const blob  = checkText(state, 1, "template_load");
            auto const bytes = std::as_bytes(std::span{blob});

            auto result = context->loadTemplate(bytes);
            if (!result)
            {
                auto const failed = NativeCallIdentity{
                    .verb      = "template_load",
                    .byteCount = static_cast<uint64>(bytes.size()),
                };
                traceHostCallFailure(state, context, failed, result.error());
            }

            auto const hashText = result->hash.toString();
            auto const call     = NativeCallIdentity{
                .verb        = "template_load",
                .byteCount   = static_cast<uint64>(bytes.size()),
                .contentHash = hashText,
            };
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            pushBoxed<TemplateTicket>(
                state,
                result->ticket,
                &destroyBox<TemplateTicket>,
                k_templateType
            );
            return 1;
        }

        // cycle_match(ticket, template, x, y, width, height) -> match handle, or
        // nil when the region held no candidate position at all.
        //
        // It reports the distance and the ceiling and judges NEITHER. Whether a
        // score counts as a hit is the trusted framework's, which is the whole
        // content of "scores stay in layer one, judging moves to layer two"; the
        // handle carries `score` and `maximum` so the framework has something to
        // judge with. A budget, deadline or cancel stop RAISES rather than
        // returning nil: a search that stopped looking has established nothing.
        auto cycleMatchFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto* templateTicket =
                checkBox<TemplateTicket>(state, 2, k_templateType, "template");
            auto const searchRoi = checkPixelRect(state, 3, "cycle_match region");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_match",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleMatch(*ticket, *templateTicket, searchRoi);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            auto const found = *result;
            if (!found.has_value())
            {
                traceHostCall(state, context, call, trace::NativeCallOutcome::Empty);
                lua_pushnil(state);
                return 1;
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);

            beginFieldHandle<MatchBox>(
                state,
                MatchBox{
                    .cycleOrdinal = ticket->ordinal,
                    .found        = *found,
                },
                k_matchUserdataTag
            );
            addNumberField(state, "x", found->matchedRect.x());
            addNumberField(state, "y", found->matchedRect.y());
            addNumberField(state, "width", found->matchedRect.width());
            addNumberField(state, "height", found->matchedRect.height());
            addNumberField(state, "click_x", found->clickPixel.x());
            addNumberField(state, "click_y", found->clickPixel.y());
            addNumberField(state, "score", found->sadScore);
            addNumberField(state, "maximum", found->maximumSad);
            finishFieldHandle(state, context, k_matchType);
            return 1;
        }

        // cycle_read(ticket, x, y, width, height) -> reading handle, or nil when
        // the region held no text.
        //
        // It takes no expected text. Whether a reading matches what a page model
        // hoped for -- full width against half width, traditional against
        // simplified, contains against equals -- is policy, and a wrong answer
        // there only picks a different already-authorised target. The host owns
        // no part of that rule, so there is no parameter for it.
        //
        // The handle carries `confidence` beside `text` because reading is the
        // one capability that fails OPEN: a rectangle pointed at the wrong place
        // returns plausible text rather than nothing, and without a confidence a
        // script cannot tell a real line from a guess.
        auto cycleReadFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket   = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const rect = checkPixelRect(state, 2, "cycle_read region");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_read",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleRead(*ticket, rect);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            auto reading = *std::move(result);
            if (!reading.has_value())
            {
                traceHostCall(state, context, call, trace::NativeCallOutcome::Empty);
                lua_pushnil(state);
                return 1;
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);

            // The carrier holds no payload: nothing in C++ ever reads a reading
            // back, so the fields below are the whole of it. A single byte is the
            // smallest block the VM will hand out.
            beginFieldHandle<uint8>(state, uint8{0}, k_readingUserdataTag);
            lua_pushlstring(state, reading->text.data(), reading->text.size());
            lua_setfield(state, -2, "text");
            addNumberField(state, "confidence", reading->confidenceBp);
            addNumberField(state, "x", reading->rect.x());
            addNumberField(state, "y", reading->rect.y());
            addNumberField(state, "width", reading->rect.width());
            addNumberField(state, "height", reading->rect.height());
            finishFieldHandle(state, context, k_readingType);
            return 1;
        }

        // cycle_read_lines(ticket, x, y, width, height) -> a frozen array of
        // reading handles, one per line the frame holds inside the region, in
        // top-to-bottom then left-to-right order. The array is empty when the
        // region held no text at all.
        //
        // IT IS A VERB OF ITS OWN AND NOT A FLAG ON cycle_read. What comes back
        // is different in KIND -- a list rather than a reading -- so a mode
        // parameter would make every caller of the older verb unwrap a value
        // whose shape depended on an argument, and this file already refuses
        // parameters whose only meaning is "not the other kind" (see cycle_read
        // on why it takes no expected text). The two also differ in what they
        // ASSERT: cycle_read's caller says the rectangle holds one line, and
        // this caller says it has no idea what is in the region, which is the
        // whole reason the region is worth reading.
        //
        // WHAT IT COSTS: one read for the detection pass plus one for each line
        // that pass located, out of the same per-cycle pool cycle_read spends.
        // A region holding more lines than the cycle can still pay for RAISES
        // rather than returning the first few, because a partly-read region
        // establishes nothing about the lines nobody looked at -- the same rule
        // a stopped template search obeys. See TaskContext::cycleReadLines.
        //
        // It does not consume the cycle: reading changes nothing on the target,
        // so the same cycle goes on to click one of the lines it found.
        //
        // EVERY LINE COMES BACK IN TARGET PIXELS. The rectangle on a handle is
        // where the FRAME put the text, not where the caller asked it to look,
        // so nothing above this has an origin to add back and forget.
        auto cycleReadLinesFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket    = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const rect = checkPixelRect(state, 2, "cycle_read_lines region");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_read_lines",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleReadLines(*ticket, rect);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            auto const lines = *std::move(result);
            traceHostCall(
                state,
                context,
                call,
                lines.empty()
                    ? trace::NativeCallOutcome::Empty
                    : trace::NativeCallOutcome::Succeeded
            );

            lua_createtable(state, static_cast<int>(lines.size()), 0);
            int const array = lua_gettop(state);
            for (auto index = std::size_t{0}; index < lines.size(); ++index)
            {
                auto const& line = lines[index];

                // The same carrier shape a single reading gets, because a line
                // IS a reading: one region, one string, one confidence. A second
                // shape for the same facts would be a second thing for layer two
                // to learn.
                beginFieldHandle<uint8>(state, uint8{0}, k_readingUserdataTag);
                lua_pushlstring(state, line.text.data(), line.text.size());
                lua_setfield(state, -2, "text");
                addNumberField(state, "confidence", line.confidenceBp);
                addNumberField(state, "x", line.rect.x());
                addNumberField(state, "y", line.rect.y());
                addNumberField(state, "width", line.rect.width());
                addNumberField(state, "height", line.rect.height());
                finishFieldHandle(state, context, k_readingType);

                lua_rawseti(state, array, static_cast<int>(index) + 1);
            }

            // Frozen like every other host-minted value. The entries already are;
            // the array around them is frozen so a framework bug cannot append a
            // line the frame never located to a list the layer above treats as
            // evidence.
            if (!script::deepFreeze(state, array))
            {
                raiseInvariant(
                    state,
                    context,
                    "the array of read lines could not be frozen"
                );
            }
            lua_settop(state, array);
            return 1;
        }

        // project_read(name) -> blob. Reads one file from the generation's own
        // project directory.
        //
        // It is NOT cycle-scoped, and deliberately so: a page model is loaded
        // before any observation exists, and requiring an open cycle would make a
        // captured frame a precondition for reading a file. Confinement to the
        // project directory is ProjectFileStore's, and it is the whole of what
        // keeps `name` from reaching the rest of the disk.
        auto projectReadFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const name = checkText(state, 1, "project_read");

            auto result = context->projectRead(name);
            if (!result)
            {
                auto const failed = NativeCallIdentity{
                    .verb         = "project_read",
                    .resourceName = name,
                };
                traceHostCallFailure(state, context, failed, result.error());
            }

            auto const hash = sha256(*result);
            if (!hash)
            {
                raiseInvariant(
                    state,
                    context,
                    "the bytes read from a project file could not be hashed"
                );
            }
            auto const hashText = hash->toString();
            auto const call     = NativeCallIdentity{
                .verb         = "project_read",
                .resourceName = name,
                .byteCount    = static_cast<uint64>(result->size()),
                .contentHash  = hashText,
            };
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);

            // SAFETY: reinterpreting the byte buffer as characters for one
            // lua_pushlstring call. Lua copies the bytes into VM-owned string
            // storage before returning, so no view survives this statement.
            auto const* p_chars = reinterpret_cast<char const*>(result->data());
            lua_pushlstring(state, p_chars, result->size());
            return 1;
        }

        // project_write(name, blob) -> (). Writes one file into the generation's
        // own project directory, replacing it if it is already there.
        //
        // Replacing rather than refusing an existing file is the difference from
        // the input agent's output confinement, which shares this shape: an agent
        // capture must be a file that does not exist yet, while a page model is
        // rewritten every time it changes.
        auto projectWriteFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const name  = checkText(state, 1, "project_write");
            auto const blob  = checkText(state, 2, "project_write contents");
            auto const bytes = std::as_bytes(std::span{blob});

            auto const hash = sha256(bytes);
            if (!hash)
            {
                raiseInvariant(
                    state,
                    context,
                    "the bytes handed to a project write could not be hashed"
                );
            }
            auto const hashText = hash->toString();
            auto const call     = NativeCallIdentity{
                .verb         = "project_write",
                .resourceName = name,
                .byteCount    = static_cast<uint64>(bytes.size()),
                .contentHash  = hashText,
            };

            auto const status = context->projectWrite(name, bytes);
            if (!status)
            {
                traceHostCallFailure(state, context, call, status.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // cycle_click_point(ticket, x, y) -> (). Consumes the cycle and delivers
        // a click at a bare coordinate.
        //
        // THIS IS THE PRIVILEGE THE FRAMEWORK HOLDS AND A PROJECT NEVER SEES. It
        // is a separate primitive rather than an argument shape on cycle_click
        // because the trusted framework gates it: the business environment is
        // handed wrappers that click an annotated element, and this name is not
        // on anything a project can reach. What C++ still enforces is the rest of
        // the fence -- this ticket's frame, the observation's lease, the project
        // fingerprint, and one delivery per cycle.
        auto cycleClickPointFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const x = checkPixelExtent(state, 2, "cycle_click_point x");
            auto const y = checkPixelExtent(state, 3, "cycle_click_point y");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_click_point",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleClickPoint(
                *ticket,
                std::nullopt,
                PixelPoint{x, y}
            );
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // cycle_crop(ticket, x, y, width, height) -> (blob, hash). The agent's
        // eye: the pixels of one rectangle, encoded as a PNG, plus the lowercase
        // hex SHA-256 of exactly those bytes.
        //
        // ONLY THE EXPLORATION SURFACE HAS IT. Handing raw pixels to a business
        // script would give it a way to decide things no evidence can falsify,
        // which is the whole content of the pixel row in the two-trust-mode table
        // (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2). A run
        // VM's private surface does not carry this key, so a framework module
        // that reached for it finds nil.
        //
        // TWO RETURNS, AND THE SECOND IS NOT A CONVENIENCE. A template asset is
        // named by the content hash of its own bytes, and the sandbox leaves Luau
        // no hash function at all -- so a caller that could not be told the hash
        // could not write the file. The host already computed it for the trace
        // line, and handing over the same value is what keeps the file name and
        // the evidence from being two truths that could disagree.
        //
        // It does not consume the cycle: see TaskContext::cycleCrop.
        auto cycleCropFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket    = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const rect = checkPixelRect(state, 2, "cycle_crop region");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_crop",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleCrop(*ticket, rect);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            // The trace line carries the prefixed spelling every other
            // content-hash line uses, and the script gets the BARE hex.
            //
            // They are two renderings of one value rather than two values, and
            // the difference is not cosmetic: a template asset lives at
            // assets/templates/<64 hex>.png, and a name carrying the "sha256:"
            // prefix would be a path no loader reads. The script layer is the
            // one that has to spell the file name, so it gets the spelling that
            // IS the file name.
            auto const traceHash = result->hash.toString();
            auto const scriptHash = result->hash.hex();
            auto const done       = NativeCallIdentity{
                .verb         = "cycle_crop",
                .cycleOrdinal = ticket->ordinal,
                .byteCount    = static_cast<uint64>(result->png.size()),
                .contentHash  = traceHash,
            };
            traceHostCall(state, context, done, trace::NativeCallOutcome::Succeeded);

            // SAFETY: reinterpreting the byte buffer as characters for one
            // lua_pushlstring call. Lua copies the bytes into VM-owned string
            // storage before returning, so no view survives this statement.
            auto const* p_chars = reinterpret_cast<char const*>(result->png.data());
            lua_pushlstring(state, p_chars, result->png.size());
            lua_pushlstring(state, scriptHash.data(), scriptHash.size());
            return 2;
        }

        // probe(blob, x, y, width, height[, key_r, key_g, key_b, tolerance])
        // -> probe handle. Colour statistics over one rectangle of one PNG.
        //
        // PURE, AND SO IT TAKES NO TICKET. Every other verb on this surface is
        // about the live target and is fenced by the cycle that observed it; this
        // one is arithmetic over bytes the caller already holds, so requiring an
        // open cycle would be ceremony that made a measurement depend on a frame
        // it never reads. It is privileged nonetheless, for the same reason
        // cycle_crop is: the only way to hold pixels here is to have cropped
        // them.
        //
        // THE KEY IS OPTIONAL AND ITS ABSENCE IS VISIBLE. With no key the handle
        // carries the census fields alone and no selection fields at all, so a
        // caller cannot read "no key was passed" as "the key selected nothing".
        // The census is what an agent probes FIRST -- the plan's loop is crop,
        // then probe to fix the key and the threshold -- and a verb that demanded
        // a key would be useless for choosing one.
        //
        // NOTHING HERE IS A THRESHOLD. The handle reports counts; whether a count
        // is good enough is the caller's, and the caller writes what it decided
        // into the project file where it can be argued with.
        auto probeFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const blob = checkText(state, 1, "probe blob");
            auto const rect = checkPixelRect(state, 2, "probe region");

            auto key = std::optional<ProbeColourKey>{};
            if (!lua_isnoneornil(state, 6))
            {
                key = ProbeColourKey{
                    .red   = checkColourChannel(state, 6, "probe key red"),
                    .green = checkColourChannel(state, 7, "probe key green"),
                    .blue  = checkColourChannel(state, 8, "probe key blue"),
                    .tolerance = lua_isnoneornil(state, 9)
                        ? k_defaultProbeTolerance
                        : checkPixelExtent(state, 9, "probe tolerance"),
                };
            }

            auto const call = NativeCallIdentity{
                .verb      = "probe",
                .byteCount = static_cast<uint64>(blob.size()),
            };

            auto result = probePngRegion(
                std::as_bytes(std::span{blob}),
                rect,
                key
            );
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);

            // The carrier holds no payload: nothing in C++ ever reads a probe
            // back, so the fields below are the whole of it -- the same shape a
            // reading handle takes, for the same reason.
            beginFieldHandle<uint8>(state, uint8{0}, k_probeUserdataTag);
            addNumberField(state, "image_width", result->imageWidth);
            addNumberField(state, "image_height", result->imageHeight);
            addNumberField(state, "rect_pixels", result->rectPixels);
            addNumberField(state, "distinct_colours", result->distinctColours);
            addNumberField(state, "dominant_red", result->dominantRed);
            addNumberField(state, "dominant_green", result->dominantGreen);
            addNumberField(state, "dominant_blue", result->dominantBlue);
            addNumberField(state, "dominant_pixels", result->dominantPixels);
            if (result->fullySelectedPixels.has_value())
            {
                addNumberField(
                    state,
                    "fully_selected_pixels",
                    *result->fullySelectedPixels
                );
                addNumberField(
                    state,
                    "ramp_selected_pixels",
                    *result->rampSelectedPixels
                );
                addNumberField(state, "selected_weight", *result->selectedWeight);
            }
            finishFieldHandle(state, context, k_probeType);
            return 1;
        }

        // cycle_click(ticket, match) -> (). Consumes the cycle and delivers the
        // click at the position the match reports.
        //
        // IT TAKES A MATCH AND NOTHING ELSE. There is no catalog hit any more:
        // an element is a layer-two object built out of the project file, so the
        // only evidence C++ can check is that this template matched on THIS
        // frame. The other requisites are unchanged -- the same-frame ordinal, the
        // observation's lease, the project fingerprint, one delivery per cycle --
        // and "this page authorises this element" is enforced above, in
        // modules/task/runtime/observe.luau, which is the only place that knows
        // what a page is.
        //
        // Both ordinals reach the wire: they agree on every delivered click, and
        // differ exactly when a match from a spent cycle was refused.
        auto cycleClickFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");

            auto const* p_match = matchBoxAt(state, 2);
            if (p_match == nullptr)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "expected a match handle"
                );
            }

            auto const call = NativeCallIdentity{
                .verb            = "cycle_click",
                .cycleOrdinal    = ticket->ordinal,
                .hitCycleOrdinal = p_match->cycleOrdinal,
            };
            auto matched = context->cycleClickPoint(
                *ticket,
                p_match->cycleOrdinal,
                p_match->found.clickPixel
            );
            if (!matched)
            {
                traceHostCallFailure(state, context, call, matched.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // key(ticket, name) -> (). Consumes the cycle and delivers one
        // press-and-release of the key `name` prints.
        //
        // Its authorization contract is deliberately NOT a click's, and the full
        // reasoning is at TaskContext::cycleKey and EngineSession::pressKey. In one
        // sentence: it requires an open cycle and nothing else, because a keystroke
        // names no screen position -- so there is no detection to be same-frame with
        // and no coordinate whose shelf life a lease could bound -- while it still
        // consumes the cycle, because a delivered keystroke changes the screen
        // exactly as a click does.
        //
        // The name is a string rather than a handle because it is a scalar the
        // target itself publishes ("E ends the turn"), not an identity the catalog
        // mints. domain::KeyName is the single definition of which names exist, so a
        // name outside the set is a Tier B ActionRejected an author can catch and
        // correct, refused BEFORE the cycle is spent -- a typo must not cost a
        // frame.
        auto keyFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            if (lua_type(state, 2) != LUA_TSTRING)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:key needs the key's printed name as a string"
                );
            }

            // SAFETY: the value was just confirmed to be a string, so lua_tolstring
            // performs no conversion and returns the VM-owned bytes with their
            // length. It stays on the stack for the whole call, so the view is taken
            // from live storage.
            std::size_t nameLength = 0;
            char const* p_nameText = lua_tolstring(state, 2, &nameLength);
            auto const  keyName    = KeyName::create(
                std::string_view{p_nameText, nameLength}
            );
            if (!keyName)
            {
                raiseTierB(
                    state,
                    kindOf(keyName.error()),
                    std::string{keyName.error().message()}
                );
            }

            auto const call = NativeCallIdentity{
                .verb         = "key",
                .cycleOrdinal = ticket->ordinal,
                .key          = *keyName,
            };

            auto result = context->cycleKey(*ticket, *keyName);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // cycle_scroll(ticket, notches) -> (). Consumes the cycle and delivers one
        // wheel scroll of `notches` detents, positive away from the operator and
        // negative toward them.
        //
        // Its authorization contract is `key`'s rather than a click's, and the full
        // reasoning is at TaskContext::cycleScroll and EngineSession::scroll. In one
        // sentence: it requires an open cycle and nothing else, because the verb
        // names no screen position -- so there is no hit to be same-frame with and
        // no coordinate whose shelf life a lease could bound -- while it still
        // consumes the cycle, because a delivered scroll moves the screen exactly as
        // a keystroke does.
        //
        // IT IS ON BOTH SURFACES. Scrolling a list too long to fit is ordinary
        // business work -- the fighter list of teaching step 4 is the instance the
        // verb roster cites -- and nothing about it hands a script pixels or a bare
        // coordinate, which is what the two exploration privileges are.
        //
        // ANCHORING ONE TO AN ANNOTATED REGION IS DELIBERATELY NOT HERE. That is
        // open question 5 of docs/plans/2026-08-01-three-layers-and-agent-operator
        // .md; the raw verb ships with no anchoring rule rather than with an
        // invented one, and when the question is settled the region arrives as an
        // argument rather than as a reinterpretation of this one.
        //
        // The count is checked here only for the shape a Luau number can be wrong
        // in -- not a number, not whole, or beyond what the port carries. What a
        // deliverable count IS belongs to the delivery layer: the bound comes from
        // the word the platform's wheel message encodes it in, and zero is refused
        // there too. Restating either here would be a second rule with nothing
        // holding it equal to the first, and the cost of leaving it there is one
        // spent frame on a mistyped scroll.
        auto cycleScrollFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            if (lua_type(state, 2) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:cycle_scroll needs the wheel notch count as a number"
                );
            }

            auto const notches = checkedIntegralCast<int32>(
                lua_tonumber(state, 2)
            );
            if (!notches)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "ctx:cycle_scroll needs a whole number of wheel notches "
                    "within range"
                );
            }

            auto const call = NativeCallIdentity{
                .verb         = "cycle_scroll",
                .cycleOrdinal = ticket->ordinal,
                .wheelNotches = *notches,
            };

            auto result = context->cycleScroll(*ticket, *notches);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
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

        // The automation kind whose domain wire spelling is `wireName`, or
        // nullopt when no kind carries it. The scan is over the reflected
        // entries and compares against the same domain function that produces
        // uf.errors, a Tier B carrier's `kind` field, and the trace, so a
        // spelling the framework passes here is checked against the one truth
        // rather than against a second copy of it.
        [[nodiscard]]
        auto kindOfWireName(std::string_view wireName) noexcept
            -> std::optional<AutomationErrorKind>
        {
            for (auto const& entry : enumEntries<AutomationErrorKind>())
            {
                if (automationErrorWireName(entry.value) == wireName)
                {
                    return entry.value;
                }
            }
            return std::nullopt;
        }

        // raise(kind, message) -> never. Mints a Tier B error carrier of the
        // named kind and raises it.
        //
        // It is admitted under the design's primitive rule -- an indivisible
        // effect or a safety primitive -- as the second of those. A Tier B
        // carrier is tagged userdata that only the host can produce, so a
        // framework policy that fails on its own terms (a wait whose deadline
        // expired) has no other way to fail as a real automation error: an
        // error() from Luau is a plain string, which reaches the host as a
        // malformed script and which neither ctx:try nor ctx:retry will treat
        // as an automation failure. Nothing about page selection, looping,
        // retry or game decision-making is here; the caller decided all of it.
        //
        // Two kinds are refused. `cancelled` is the host's own terminal verdict
        // and arrives with the fatal latch already set, so a Tier B carrying it
        // would claim a stop nobody requested and would be catchable besides.
        // `internal_invariant` must latch the generation terminal BEFORE it is
        // raised, or a project pcall could swallow a framework bug; this
        // primitive deliberately does not latch, so that kind keeps its own door
        // -- raiseInvariant, reached only when the host itself caught the bug --
        // rather than a catchable impostor here.
        auto raiseFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            if (lua_type(state, 1) != LUA_TSTRING
                || lua_type(state, 2) != LUA_TSTRING)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "raise takes an error kind and a message, both strings"
                );
            }

            // SAFETY: both values were just confirmed to be strings, so
            // lua_tolstring performs no conversion and returns the VM-owned
            // bytes with their length. Both stay on the stack for the whole
            // call, so the view and the copy below are taken from live storage.
            std::size_t kindLength = 0;
            char const* p_kindText = lua_tolstring(state, 1, &kindLength);
            auto const  wireName   = std::string_view{p_kindText, kindLength};

            std::size_t messageLength = 0;
            char const* p_messageText = lua_tolstring(state, 2, &messageLength);
            auto const  message       = std::string{p_messageText, messageLength};

            auto const kind = kindOfWireName(wireName);
            if (!kind
                || *kind == AutomationErrorKind::Cancelled
                || *kind == AutomationErrorKind::InternalInvariant)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "raise cannot mint the error kind '" + std::string{wireName}
                        + "'"
                );
            }
            raiseTierB(state, *kind, message);
        }

        // Raises the Tier C sentinel when the run's cancel source has already
        // requested a stop. The three time primitives below reach no engine
        // verb, so nothing downstream would fail closed for them: this is where
        // they do it, and it runs both before a pause and after one, so a stop
        // that lands mid-sleep ends the primitive on the terminal path rather
        // than as a normal return. Every later primitive is then refused by
        // guardFatal, before any capture.
        // The question itself is requireNotCancelled's, shared with the operator
        // front-end so a stop refuses a pause on the same terms whichever front-end
        // asked; what stays here is the raise, which reaches the Tier C sentinel
        // through raiseFromError's Cancelled branch.
        auto guardCancelled(lua_State* state, TaskContext* context) -> void
        {
            auto const live = requireNotCancelled(*context);
            if (live)
            {
                return;
            }
            raiseFromError(state, context, live.error());
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

        // One spelling the framework may pass to `emit`, and the stream event it
        // names. The framework writes the bare verb ("step_started") and the wire
        // name carries the layer ("framework.step_started"), so the layer prefix
        // is spelled once, in the trace schema, and cannot drift from what a
        // reader sees.
        struct SemanticEventName final
        {
            std::string_view      verb;
            trace::TraceEventKind kind;
        };

        // The whole semantic vocabulary the framework may request. Anything else
        // is a framework bug rather than a bad argument: this table and the Luau
        // call sites are the same binary, so a name that is not here was never
        // written by a framework this host shipped.
        //
        // framework.subtask_entered / subtask_exited are absent because cross-file
        // reuse is P1: there is no ctx:call to emit them.
        constexpr auto k_semanticEventNames = std::array{
            SemanticEventName{
                .verb = "step_started",
                .kind = trace::TraceEventKind::FrameworkStepStarted,
            },
            SemanticEventName{
                .verb = "step_finished",
                .kind = trace::TraceEventKind::FrameworkStepFinished,
            },
            SemanticEventName{
                .verb = "retry_attempt",
                .kind = trace::TraceEventKind::FrameworkRetryAttempt,
            },
            SemanticEventName{
                .verb = "retry_backoff",
                .kind = trace::TraceEventKind::FrameworkRetryBackoff,
            },
            SemanticEventName{
                .verb = "interrupt_matched",
                .kind = trace::TraceEventKind::FrameworkInterruptMatched,
            },
            SemanticEventName{
                .verb = "interrupt_handled",
                .kind = trace::TraceEventKind::FrameworkInterruptHandled,
            },
            SemanticEventName{
                .verb = "interrupt_exhausted",
                .kind = trace::TraceEventKind::FrameworkInterruptExhausted,
            },
            SemanticEventName{
                .verb = "settled",
                .kind = trace::TraceEventKind::FrameworkSettled,
            },
        };

        // Reads the scope label at `index`. A non-string is a framework bug: every
        // call site has already checked its own argument -- ctx:step refuses a
        // non-string name and task.interrupt refuses a non-string id -- so a
        // non-string arriving here means the framework, not the project, is wrong.
        // Length and character set are the stream validator's, because they are
        // the same rules for every label whatever asked for one.
        [[nodiscard]]
        auto checkScopeLabel(
            lua_State* state,
            TaskContext* context,
            int index
        ) -> std::string
        {
            if (lua_type(state, index) != LUA_TSTRING)
            {
                raiseInvariant(state, context, "emit needs a string scope label");
            }

            // SAFETY: the value was just confirmed to be a string, so
            // lua_tolstring performs no conversion and returns the VM-owned bytes
            // with their length. It stays on the stack for the whole call, so the
            // copy is taken from live storage.
            std::size_t length = 0;
            char const* p_text = lua_tolstring(state, index, &length);
            return std::string{p_text, length};
        }

        // Reads a whole non-negative count at `index`.
        //
        // Tier B rather than an invariant failure, because every count `emit`
        // takes originates in a project's own policy table -- retry attempts and
        // backoff milliseconds -- so a value out of range is a project error the
        // author can catch and correct.
        [[nodiscard]]
        auto checkCount(lua_State* state, int index, std::string const& what) -> uint64
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " requires a number"
                );
            }
            auto const value = checkedIntegralCast<uint64>(lua_tonumber(state, index));
            if (!value)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    what + " must be a whole, non-negative number within range"
                );
            }
            return *value;
        }

        // Builds the framework payload the named event carries.
        [[nodiscard]]
        auto readSemanticPayload(
            lua_State* state,
            TaskContext* context,
            trace::TraceEventKind kind
        ) -> trace::TraceEvent::Framework
        {
            if (kind == trace::TraceEventKind::FrameworkRetryAttempt)
            {
                return trace::TraceEvent::Framework{
                    .attempt  = checkCount(state, 2, "a retry attempt number"),
                    .attempts = checkCount(state, 3, "a retry policy's attempts"),
                };
            }
            if (
                kind == trace::TraceEventKind::FrameworkRetryBackoff
                || kind == trace::TraceEventKind::FrameworkSettled
            )
            {
                return trace::TraceEvent::Framework{
                    .durationMillis = checkCount(state, 2, "a declared pause"),
                };
            }
            return trace::TraceEvent::Framework{
                .label = checkScopeLabel(state, context, 2),
            };
        }

        // emit(name, ...) -> (). Requests one framework semantic event.
        //
        // It is admitted under the design's primitive rule as a safety primitive,
        // for the same shape of reason `raise` is: the framework's own structure
        // -- which step is open, which attempt this is, which interrupt matched --
        // is not observable from anywhere else, and a trace that records only what
        // the host did cannot explain a run the framework shaped. Nothing about
        // page selection, looping or game decisions crosses here; the caller
        // decided all of it and is only saying what it decided.
        //
        // The design writes this primitive as emit(event) taking a table. It takes
        // a name and scalars instead, because section 5's second invariant admits
        // only host-minted handles and scalars as arguments, and a Luau table is
        // neither. Nothing is lost: the vocabulary is closed (see above), so the
        // positional shape is decided by the name.
        //
        // It is deliberately NOT a passthrough. The host validates the request
        // against the stream state machine and records nothing that fails, which
        // is what keeps this from being a hole through which a buggy framework
        // writes a plausible history of a run that did not happen that way.
        //
        // It writes no task.native_call of its own. The event IS the record, and a
        // second line saying a trace call happened would double every framework
        // event for no evidence.
        auto emitFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const verb = checkScopeLabel(state, context, 1);
            auto const named = std::ranges::find(
                k_semanticEventNames,
                verb,
                &SemanticEventName::verb
            );
            if (named == k_semanticEventNames.end())
            {
                raiseInvariant(state, context, "emit does not know the event '" + verb + "'");
            }

            auto const kind = named->kind;
            auto const status = context->emitTrace(
                trace::TraceEvent{
                    .kind      = kind,
                    .framework = readSemanticPayload(state, context, kind),
                }
            );
            if (!status)
            {
                // The validator's own classification decides the tier: a request
                // it declines is the project's to fix and stays catchable, while
                // a protocol breach is a framework bug and spends the generation.
                auto const failureKind = kindOf(status.error());
                auto const message     = std::string{status.error().message()};
                if (failureKind == AutomationErrorKind::InternalInvariant)
                {
                    raiseInvariant(state, context, message);
                }
                raiseTierB(state, failureKind, message);
            }
            return 0;
        }

        // terminal() -> bool. Whether this generation is already spent.
        //
        // It is the one primitive that does NOT enter through guardFatal, and
        // that is its entire purpose: the framework's cleanup paths have to ask
        // whether the generation is still live before they emit a closing event,
        // and a question that raised when the answer is "no" could never be
        // asked. Without it, a step whose body was cancelled would raise a second
        // time on the way out and bury the cause under its own consequence.
        //
        // It confers nothing: the answer is already observable to a script as
        // "the last primitive refused".
        auto terminalFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            lua_pushboolean(state, context->fatal() ? 1 : 0);
            return 1;
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

        // Registers the metatables only a bound session can ever mint: the cycle
        // ticket, the template and the deadline. They belong to the private
        // capability installer because every one of them is produced by a
        // primitive, so a VM with no session has nothing that could wear them.
        //
        // The Tier B error carrier is absent on purpose. Its metatable is per
        // instance -- it holds that error's own kind, message and retryable
        // behind a table __index -- so there is nothing shared to register here,
        // and its identity is the userdata tag rather than a registry entry. The
        // match and reading handles are absent for exactly that reason too: both
        // carry per-instance fields, so both wear a metatable of their own.
        //
        // None of the three carries a method. They are pure names the host hands
        // back to itself, so every operation on a cycle is a primitive taking the
        // ticket. The resolved page's `is` was the one method here, and it went
        // with the page model.
        [[nodiscard]]
        auto installSessionMetatables(lua_State* state) -> Status
        {
            beginMetatable(state, k_cycleType);
            UF_TRY(finishMetatable(state, k_cycleType));

            beginMetatable(state, k_templateType);
            UF_TRY(finishMetatable(state, k_templateType));

            beginMetatable(state, k_deadlineType);
            return finishMetatable(state, k_deadlineType);
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

        // Assembles the frozen global uf table: the error kinds, and today
        // nothing else. Every entry is data. Nothing here can observe or act,
        // which is why it is safe as a project global.
        //
        // Its `elements` and `pages` name tables went with the C++ page model.
        // The root survives them because uf.errors is what a project compares an
        // error kind against, and because the pre-VM pass still resolves
        // uf.elements.<name> literals -- against the project file now, see
        // task/script-validator.hpp.
        [[nodiscard]]
        auto buildUfData(lua_State* state) -> Status
        {
            lua_newtable(state);
            int const root = lua_gettop(state);

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
        // WHERE THE TWO ENVIRONMENTS DIFFER, AND THE ONLY PLACE THEY DO.
        //
        // `mode` decides whether two keys exist on the table this builds. It is
        // not a permission check and there is no refusal anywhere below: a Run
        // surface simply never binds cycle_crop or probe, so a framework module
        // that named one of them reads nil. That is what section 2 rule 2 of
        // docs/plans/2026-08-01-three-layers-and-agent-operator.md asks for: the
        // privileged verbs do not exist rather than being refused one call at a
        // time.
        //
        // The third privileged verb, cycle_click_point, is bound on both -- see
        // its installation below for why, and for where its own confinement
        // actually lives.
        //
        // Everything else on the surface is identical between the two, because
        // the exploration environment is the run surface PLUS these three and
        // never a different set (docs/plans/2026-08-01-agent-front-end-and-
        // exploration.md 1). An agent must meet exactly the guarantees a task
        // meets on every verb they share, or what it measures would be a
        // statement about a system the product does not ship.
        [[nodiscard]]
        auto buildPrivateSurface(
            lua_State* state,
            TaskContext* context,
            ScriptTrustMode mode
        ) -> Status
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
                "cycle_match",
                &cycleMatchFn,
                "uf_cycle_match",
                context
            );
            installPrimitive(
                state,
                surface,
                "cycle_read",
                &cycleReadFn,
                "uf_cycle_read",
                context
            );
            // Bound outside the Exploration block, like cycle_read and unlike
            // cycle_crop. It hands back text and rectangles, which is an ANSWER
            // about the frame rather than the frame's pixels or a bare
            // coordinate -- and those two are what the privileged verbs are. A
            // business task reading a scrolling list it cannot draw rectangles
            // inside is doing ordinary work, and an agent authoring that list
            // needs the same verb under the same guarantees.
            installPrimitive(
                state,
                surface,
                "cycle_read_lines",
                &cycleReadLinesFn,
                "uf_cycle_read_lines",
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
            // Bound on BOTH surfaces, and this is not an oversight. The trusted
            // framework needs it in run mode: an element verified by its expected
            // text carries no match handle, so observe.click delivers at the
            // rectangle the page model gave it, and that is the locked decision 4
            // of docs/plans/2026-08-01-three-layers-and-agent-operator.md 5. What
            // makes it privileged is that no BUSINESS environment can name it --
            // it is a closure upvalue of the framework and nothing published into
            // a project environment forwards it, which is that document's section
            // 2 rule stated exactly. The exploration environment publishes a
            // forward for it; the run environment publishes none.
            installPrimitive(
                state,
                surface,
                "cycle_click_point",
                &cycleClickPointFn,
                "uf_cycle_click_point",
                context
            );
            if (mode == ScriptTrustMode::Exploration)
            {
                installPrimitive(
                    state,
                    surface,
                    "cycle_crop",
                    &cycleCropFn,
                    "uf_cycle_crop",
                    context
                );
                installPrimitive(
                    state,
                    surface,
                    "probe",
                    &probeFn,
                    "uf_probe",
                    context
                );
            }
            installPrimitive(
                state,
                surface,
                "template_load",
                &templateLoadFn,
                "uf_template_load",
                context
            );
            installPrimitive(
                state,
                surface,
                "project_read",
                &projectReadFn,
                "uf_project_read",
                context
            );
            installPrimitive(
                state,
                surface,
                "project_write",
                &projectWriteFn,
                "uf_project_write",
                context
            );
            installPrimitive(state, surface, "key", &keyFn, "uf_key", context);
            // Bound outside the Exploration block, like `key` and unlike
            // cycle_crop: a business task that scrolls a list is doing ordinary
            // work, and the two privileges are pixels and bare coordinates rather
            // than input in general.
            installPrimitive(
                state,
                surface,
                "cycle_scroll",
                &cycleScrollFn,
                "uf_cycle_scroll",
                context
            );
            installPrimitive(state, surface, "raise", &raiseFn, "uf_raise", context);
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
            installPrimitive(state, surface, "emit", &emitFn, "uf_emit", context);
            installPrimitive(
                state,
                surface,
                "terminal",
                &terminalFn,
                "uf_terminal",
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

    auto scriptProjectGlobals() -> std::vector<std::string>
    {
        return std::vector<std::string>{std::string{k_ufRoot}};
    }

    auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier
    {
        return [](lua_State* state, int index) -> std::optional<script::RaisedError>
        {
            return tierBError(state, index);
        };
    }

    auto scriptHostTableInstaller() -> script::HostTableInstaller
    {
        return [](lua_State* state) -> Status
        {
            return buildUfData(state);
        };
    }

    auto scriptPrivateCapabilities(TaskContext& context, ScriptTrustMode mode)
        -> script::PrivateCapabilityInstaller
    {
        // Lifetime: the closure stores this address, which the caller guarantees
        // outlives the VM this installer configures (see the header contract). The
        // TaskContext is non-movable, so the pointer stays valid for the VM's life;
        // it is a non-owning observation, never an owner.
        TaskContext* const contextPtr = &context;
        return [contextPtr, mode](lua_State* state) -> Status
        {
            return buildPrivateSurface(state, contextPtr, mode);
        };
    }
}
