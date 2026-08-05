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

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/stream-validator.hpp>

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
        // k_cycleType, k_templateType and k_deadlineType are also the registry
        // keys their shared metatable is stored under; k_errorType is not,
        // because a Tier B carrier's metatable is per instance.
        constexpr auto k_cycleType    = "uf.cycle";
        constexpr auto k_templateType = "uf.template";
        constexpr auto k_deadlineType = "uf.deadline";
        constexpr auto k_errorType    = "uf.error";

        // Kinds that carry readable per-instance fields, so each instance wears
        // a metatable of its own -- the design restricts __index to a table,
        // leaving nowhere else for the fields -- and is recognized by userdata
        // tag rather than by a registry entry.
        constexpr auto k_matchType   = "uf.match";
        constexpr auto k_readingType = "uf.reading";
        constexpr auto k_probeType   = "uf.probe";
        constexpr auto k_maskType    = "uf.mask";

        // The one global the DATA installer registers and the project
        // environment whitelists; spelled once so the two cannot drift. The
        // private surface has none: it is registered under no name at all.
        constexpr auto k_ufRoot = "uf";

        // Carries k_errorType to the framework over the private surface -- the
        // only table the framework is handed and no project script can name --
        // so ctx:try never spells the label in Luau and the constant above stays
        // its single source.
        constexpr auto k_errorTagField = "error_tag";

        // Carries trace::k_maxScopeLabelBytes over the same surface. A page name
        // becomes the label of the framework.page_resolved line the moment a
        // signature holds, and the stream REFUSES a label past that ceiling --
        // out of observe.resolve_page, whose contract is (Receipt?, string?) and
        // whose callers do not pcall. model.Page.new refuses the name instead,
        // and reads the number from here so the two cannot drift apart.
        constexpr auto k_labelCeilingField = "max_label_bytes";

        // The tag every Tier B error carrier is minted under, and the whole of
        // how C++ recognizes one: the VM stores a tag on the object itself, and
        // no script can put one on a value it built -- setmetatable and
        // table.clone take tables, and newproxy is removed from both
        // environments.
        //
        // Any value below LUA_UTAG_LIMIT would do; zero is what plain
        // lua_newuserdata stamps, and lua_newuserdatadtor stamps UTAG_IDTOR
        // (== LUA_UTAG_LIMIT), so the handle kinds never collide.
        constexpr auto k_errorUserdataTag = 1;
        static_assert(k_errorUserdataTag > 0 && k_errorUserdataTag < LUA_UTAG_LIMIT);

        // The tags the field-carrying handle kinds are minted under, on the
        // error carrier's reasoning: a tag is the whole of how C++ recognizes a
        // match handed back to cycle_click.
        constexpr auto k_matchUserdataTag = 2;
        static_assert(k_matchUserdataTag > 0 && k_matchUserdataTag < LUA_UTAG_LIMIT);

        constexpr auto k_readingUserdataTag = 3;
        static_assert(k_readingUserdataTag > 0 && k_readingUserdataTag < LUA_UTAG_LIMIT);

        constexpr auto k_probeUserdataTag = 4;
        static_assert(k_probeUserdataTag > 0 && k_probeUserdataTag < LUA_UTAG_LIMIT);

        // A crop's mask reports what the key the crop was cut under took out of
        // that crop's own pixels.
        constexpr auto k_maskUserdataTag = 5;
        static_assert(k_maskUserdataTag > 0 && k_maskUserdataTag < LUA_UTAG_LIMIT);

        // The tolerance a probe uses when the caller named a key but no
        // tolerance; it matches the v4 authoring line's `--tolerance` default so
        // either route reports the same counts. It is a default about the
        // measurement, never about what counts as enough selected pixels.
        constexpr auto k_defaultProbeTolerance = uint32{12};

        // Pushes an error kind's wire spelling as a Lua string, with its length
        // rather than as a C string: the domain returns a view.
        auto pushWireName(lua_State* state, AutomationErrorKind kind) -> void
        {
            auto const name = automationErrorWireName(kind);
            lua_pushlstring(state, name.data(), name.size());
        }

        // `retryable` reuses the domain's own unwind axis rather than a parallel
        // table: FailureResponse::Retry is retryable and nothing else is.
        [[nodiscard]]
        auto retryableOf(AutomationErrorKind kind) noexcept -> bool
        {
            return failureResponse(kind) == FailureResponse::Retry;
        }

        // No handle ever stores the move-only Observation -- it lives in the
        // CycleLedger and only the ticket names it -- so the ordinal is the
        // whole of the staleness check: with at most one cycle open, an ordinal
        // that is not the open one names a cycle that no longer exists.
        struct MatchBox final
        {
            uint64             cycleOrdinal{};
            engine::MatchFound found;
        };
        static_assert(std::is_trivially_destructible_v<MatchBox>);

        // Destroys at GC the value pushBoxed placement-constructed.
        //
        // No handle's destructor releases a frame: collecting a ticket frees the
        // box alone, and the frame goes back at cycle_close, at the click that
        // consumes the cycle, or with the ledger. Frame release timing is the
        // cycle protocol's, never the Lua collector's.
        template <typename T>
        auto destroyBox(void* storage) -> void
        {
            // SAFETY: `storage` is the userdata block lua_newuserdatadtor handed
            // pushBoxed, where exactly one T was placement-constructed via
            // std::construct_at. Destroy it once here; the VM frees the block
            // afterwards.
            std::destroy_at(static_cast<T*>(storage));
        }

        // __newindex on every handle and Tier B carrier metatable. A userdata
        // with no __newindex already refuses a write; this is here for the
        // message, so an author sees why rather than "attempt to index".
        auto denyWrite(lua_State* state) -> int
        {
            // luaL_error is l_noret: it longjmps out of this frame and never
            // returns, so this lua_CFunction never falls off its int-returning end.
            luaL_error(state, "task handles are read-only");
        }

        // __tostring on every handle metatable: the fixed kind label in upvalue
        // 1, so tostring(handle) leaks no address and stays deterministic.
        auto handleToString(lua_State* state) -> int
        {
            lua_pushvalue(state, lua_upvalueindex(1));
            return 1;
        }

        // Creates one opaque host-owned userdata carrying `value`, wearing the
        // shared protected metatable registered under `metatableType`.
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

        // True when the value at `index` is a full userdata wearing the metatable
        // registered under `metatableType`. Identity against the registry entry
        // holds because a script can neither mint a userdata carrying a chosen
        // metatable nor, past __metatable, reset one.
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

        // What a Tier B carrier stores in its own userdata block. The kind is
        // read back from here rather than off the script-visible `kind` string,
        // which is what keeps the decode structural. Trivially destructible, so
        // the carrier needs no destructor.
        struct TierBError final
        {
            AutomationErrorKind kind{};
        };

        // The Tier B carrier at `index` read back, or nullopt when that value is
        // not one. Identity is the userdata tag alone, so a script-built
        // look-alike is not one however faithfully it copies the fields; the
        // message is read only after the tag has answered.
        //
        // Every lookup is raw and the stack is restored on every path: this runs
        // on a thread the error already unwound, so a raise from a metamethod
        // would have no protected frame to land in.
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

            // The script-visible fields live in a frozen table behind the
            // metatable's __index. Walking it by hand avoids tostring(), which
            // on a userdata would run the __tostring metamethod.
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
        // The carrier's kind, message and retryable live in a frozen table
        // behind a per-instance metatable whose __index deepFreezeMetatable
        // requires to be a table and never a function.
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

            // A fixed string rather than a formatter reading the carrier, so
            // tostring stays deterministic, leaks no address, and still carries
            // the cause of an uncaught error into the run report.
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
                // Unreachable for the metatable built directly above. A plain
                // string rather than a half-frozen carrier, so no mutable object
                // ever answers to the Tier B label.
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

        // The payload of the handle at `index`. A wrong type is a Tier B
        // InvalidResource, keeping the script-facing failure model one shape.
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

        // Tier C: latch the terminal kind FIRST, then raise a plain non-table
        // sentinel carrying no uf.error metatable, so ctx:try re-raises it
        // unchanged. A project pcall can still catch it, and that is accepted:
        // the latch is what the tier protects, and guardFatal refuses the next
        // primitive before any capture or click.
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
        // The order is load-bearing (design section 9 rule 5): the carrier is an
        // ordinary catchable Tier B value, so the latch set before it is the only
        // thing that stops the next primitive at guardFatal. The kind is a real
        // one rather than a private sentinel, so the raised-error classifier
        // reports an uncaught framework bug as internal_invariant.
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

        // Starts one field-carrying handle: a tagged userdata holding `value`
        // with an empty field table above it. The caller fills the fields with
        // scalars and calls finishFieldHandle, which owns the metatable shape --
        // __index a table and never a function, a __metatable label, frozen
        // before anything wears it.
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
                // Unreachable for the metatable built directly above. Failing
                // beats handing a script a half-frozen object that answers to a
                // host label.
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

        auto addBooleanField(lua_State* state, char const* name, bool value)
            -> void
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, name);
        }

        // Adds one string field to the field table on the stack top, with its
        // length rather than as a C string, so any bytes reach the script whole.
        auto addTextField(
            lua_State* state,
            char const* name,
            std::string_view value
        ) -> void
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, name);
        }

        // Refuses the call when a prior verb latched the generation terminal, so
        // a script that swallowed what was raised drives no further engine verb.
        // The question is requireLiveGeneration's, shared with the operator
        // front-end; only the raise is here. It is asked before the primitive
        // decodes its arguments, so a spent generation outranks a bad handle.
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

        // Mirrors raiseFromError's mapping, so the HostCall trace records the
        // kind the Tier ladder will raise.
        [[nodiscard]]
        auto kindOf(Error const& error) noexcept -> AutomationErrorKind
        {
            return automationErrorKind(error)
                .value_or(AutomationErrorKind::InternalInvariant);
        }

        // Emits the task.native_call line at a verb's exit and turns a lost line
        // into a Tier B IoFailure. The line's shape and the cost of a sink
        // failure are native-call-trace.hpp's; only the raise is here.
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

        // Records a failed verb and raises that verb's own error, never the
        // sink's -- see recordNativeCallFailure. A cancellation keeps the Tier C
        // sentinel on this path, so the latch is set before anything else runs:
        // ctx:try is pure Luau and consults nothing. Never returns.
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

        // A required string argument at `index`, as a view into VM storage. Tier
        // B rather than an invariant: every string a primitive takes comes from a
        // project's own data, so a wrong type is an author error.
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

        // The optional colour key at `first` and the three positions after it:
        // red, green, blue, tolerance.
        //
        // Presence is decided by the first position alone, so half a key is told
        // which channel is missing rather than silently keying on black.
        [[nodiscard]]
        auto checkColourKey(
            lua_State* state,
            int first,
            std::string const& what
        ) -> std::optional<ProbeColourKey>
        {
            if (lua_isnoneornil(state, first))
            {
                return std::nullopt;
            }
            return ProbeColourKey{
                .red   = checkColourChannel(state, first, what + " red"),
                .green = checkColourChannel(state, first + 1, what + " green"),
                .blue  = checkColourChannel(state, first + 2, what + " blue"),
                .tolerance = lua_isnoneornil(state, first + 3)
                    ? k_defaultProbeTolerance
                    : checkPixelExtent(state, first + 3, what + " tolerance"),
                // Absent means "keep what this colour names", the reading every
                // key had before this flag existed, so a caller that spells no
                // fifth scalar gets exactly the old behaviour.
                .removes = lua_toboolean(state, first + 4) != 0,
            };
        }

        // Four numbers starting at `first` as one rectangle. Scalars rather than
        // a table because section 5's second invariant admits only host-minted
        // handles and scalars as primitive arguments.
        [[nodiscard]]
        auto checkPixelRect(
            lua_State* state,
            TaskContext* context,
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
                // PixelRect::create reports an empty rectangle and an extent
                // overflow alike as InternalInvariant, which must latch the
                // generation terminal BEFORE it is raised (design section 9 rule
                // 5): raiseTierB alone would mint a carrier ctx:try swallows,
                // and the run would drive on to capture and click.
                auto const kind    = kindOf(rect.error());
                auto const message = std::string{rect.error().message()};
                if (kind == AutomationErrorKind::InternalInvariant)
                {
                    raiseInvariant(state, context, message);
                }
                raiseTierB(state, kind, message);
            }
            return *rect;
        }

        // The match handle at `index`, or null. The test is the userdata tag
        // alone, as for a Tier B carrier: a script-built look-alike is not one.
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
        // table, called as native.<verb>(...), so its arguments start at stack
        // index 1.
        //
        // cycle_open() -> ticket. Observes one frame and opens the generation's
        // single observation cycle over it. A capture failure maps through the
        // Tier ladder; opening while a cycle is already open is a framework bug
        // and fails InternalInvariant rather than a Tier B a script could retry.
        //
        // It takes no deadline, and never will: EngineSession::observe bounds the
        // capture from the session's own captureTimeout, and how long one
        // screenshot may block is a host resource boundary no script may widen.
        // The `deadline` primitive bounds a wait loop, never a capture.
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
        // and names the result for the rest of this generation, so a wait loop
        // pays no decode per poll. Loading the same bytes twice returns the same
        // handle, so load order cannot change what a script holds.
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
        // nil when the region held no candidate position.
        //
        // It reports `score` and `maximum` and judges neither: whether a score
        // counts as a hit is the trusted framework's. A budget, deadline or
        // cancel stop raises rather than returning nil, because a search that
        // stopped looking has established nothing.
        auto cycleMatchFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto* templateTicket =
                checkBox<TemplateTicket>(state, 2, k_templateType, "template");
            auto const searchRoi = checkPixelRect(
                state,
                context,
                3,
                "cycle_match region"
            );

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
        // this frame read no text inside the region. Nil does not say the region
        // holds nothing: a region still being drawn reads the same way, and one
        // frame carries no evidence separating the two. Which of them the caller
        // meant is declared a layer up (`observe.empty_is_absence` /
        // `observe.empty_is_unknown`), and this verb takes no part in it.
        //
        // It takes no expected text: whether a reading matches what a page model
        // hoped for is policy, and the host owns no part of that rule.
        //
        // The handle carries `confidence` beside `text` because reading fails
        // open -- a rectangle pointed at the wrong place returns plausible text
        // rather than nothing.
        auto cycleReadFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket   = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const rect = checkPixelRect(state, context, 2, "cycle_read region");

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

            // Nothing in C++ ever reads a reading back, so the fields below are
            // the whole of it; a single byte is the smallest block the VM hands.
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
        // top-to-bottom then left-to-right order. Empty when this frame read no
        // line, which is not the claim that the region holds nothing -- see
        // cycle_read.
        //
        // Why it is a verb of its own rather than a flag on cycle_read:
        // docs/plans/2026-08-01-three-layers-and-agent-operator.md.
        //
        // It costs one read for the detection pass plus one per line that pass
        // located, out of the pool cycle_read spends; a region holding more lines
        // than the cycle can pay for raises rather than returning the first few,
        // on the rule a stopped template search obeys (see
        // TaskContext::cycleReadLines). It does not consume the cycle. Every
        // rectangle comes back in target pixels -- where the frame put the text,
        // not where the caller looked -- so nothing above adds an origin back.
        auto cycleReadLinesFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket    = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const rect = checkPixelRect(
                state,
                context,
                2,
                "cycle_read_lines region"
            );

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

            // The entries are already frozen; the array is too, so no framework
            // bug can append a line the frame never located to what the layer
            // above treats as evidence.
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
        // project directory. Deliberately not cycle-scoped: a page model is
        // loaded before any observation exists. Confinement of `name` to that
        // directory is ProjectFileStore's alone.
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
        // own project directory, replacing it. Replacing rather than refusing is
        // the one difference from the input agent's output confinement: a page
        // model is rewritten every time it changes.
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
        // A separate primitive rather than an argument shape on cycle_click,
        // because the trusted framework gates it and no project environment can
        // name it. C++ still enforces the rest of the fence: this ticket's frame,
        // the observation's lease, the project fingerprint, and one delivery per
        // cycle.
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

        // Free room under the memory ceiling a crop insists on before minting
        // its PNG string, as a multiple of that string's bytes. See its use.
        constexpr auto k_cropHeadroomFactor = uint64{4};

        // cycle_crop(ticket, x, y, width, height[, key_r, key_g, key_b,
        // tolerance]) -> (blob, hash[, mask]). The agent's eye: the pixels of one
        // rectangle as a PNG, plus the lowercase hex SHA-256 of exactly those
        // bytes. It does not consume the cycle (see TaskContext::cycleCrop).
        //
        // The key sits where `probe` puts it and carries the same default
        // tolerance, because it is meant to be the same key: an agent probes a
        // rectangle to choose one and then cuts under the one it chose. With a
        // key the PNG carries an alpha plane -- opaque where the key took the
        // pixel -- which is what makes the template masked down to the matcher.
        // The mask return is then present, and absent exactly when no key was
        // given: a zeroed handle would read as a key that selected nothing, and
        // such a key is refused rather than reported.
        //
        // The hash is not a convenience. A template asset is named by the content
        // hash of its own bytes and the sandbox leaves Luau no hash function, so
        // a caller not told the hash could not write the file.
        //
        // Only the exploration surface binds it: handing raw pixels to a business
        // script would let it decide things no evidence can falsify
        // (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
        auto cycleCropFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket    = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const rect = checkPixelRect(state, context, 2, "cycle_crop region");
            auto const key  = checkColourKey(state, 6, "cycle_crop key");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_crop",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleCrop(*ticket, rect, key);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }

            // Two renderings of one value: the trace line takes the prefixed
            // spelling every content-hash line uses and the script takes the bare
            // hex, because a template asset lives at assets/templates/<64
            // hex>.png and "sha256:" would be a path no loader reads.
            auto const traceHash = result->hash.toString();
            auto const scriptHash = result->hash.hex();
            auto const done       = NativeCallIdentity{
                .verb         = "cycle_crop",
                .cycleOrdinal = ticket->ordinal,
                .byteCount    = static_cast<uint64>(result->png.size()),
                .contentHash  = traceHash,
            };
            traceHostCall(state, context, done, trace::NativeCallOutcome::Succeeded);

            // Reclaim before minting, when the ledger is near its ceiling. Luau
            // throws LUA_ERRMEM the moment the allocator refuses and never
            // collects and retries, so an agent loop dropping each
            // multi-megabyte blob would die on a ceiling that is almost all
            // garbage. Collecting here is sound where collecting inside the
            // allocator would not be: no allocation is in flight and every value
            // this frame holds is a stack root.
            //
            // The headroom is four times the payload: one for the string
            // lua_pushlstring must fit, one for what the push costs besides the
            // bytes (interning, a string-table rehash, the hash string), and two
            // of hysteresis -- the string just minted is live and unreclaimable,
            // so collecting at exactly the payload would sweep on every crop.
            if (
                script::heapUsage(state).headroomBytes()
                < k_cropHeadroomFactor * static_cast<uint64>(result->png.size())
            )
            {
                script::collectGarbage(state);
            }

            // SAFETY: reinterpreting the byte buffer as characters for one
            // lua_pushlstring call. Lua copies the bytes into VM-owned string
            // storage before returning, so no view survives this statement.
            auto const* p_chars = reinterpret_cast<char const*>(result->png.data());
            lua_pushlstring(state, p_chars, result->png.size());
            lua_pushlstring(state, scriptHash.data(), scriptHash.size());
            if (!result->mask.has_value())
            {
                return 2;
            }

            auto const& mask = *result->mask;
            beginFieldHandle<uint8>(state, uint8{0}, k_maskUserdataTag);
            addNumberField(state, "key_red", mask.key.red);
            addNumberField(state, "key_green", mask.key.green);
            addNumberField(state, "key_blue", mask.key.blue);
            addNumberField(state, "tolerance", mask.key.tolerance);
            // Always present, unlike `warning`: the layer that records a key in
            // the project file records the one that cut these pixels, and a
            // missing field there would be read as "kept", which is the opposite
            // of what this mask did.
            addBooleanField(state, "key_removes", mask.key.removes);
            addNumberField(state, "rect_pixels", mask.rectPixels);
            addNumberField(state, "selected_pixels", mask.selectedPixels);
            addNumberField(
                state,
                "ramp_selected_pixels",
                mask.rampSelectedPixels
            );
            if (!mask.warning.empty())
            {
                addTextField(state, "warning", mask.warning);
            }
            finishFieldHandle(state, context, k_maskType);
            return 3;
        }

        // probe(blob, x, y, width, height[, key_r, key_g, key_b, tolerance])
        // -> probe handle. Colour statistics over one rectangle of one PNG.
        //
        // It takes no ticket because it is arithmetic over bytes the caller
        // already holds, not a question about the live target. It is privileged
        // nonetheless, for cycle_crop's reason: the only way to hold pixels here
        // is to have cropped them.
        //
        // With no key the handle carries the census fields and no selection
        // fields at all, so "no key was passed" cannot be read as "the key
        // selected nothing". The census is what an agent probes first, to choose
        // a key -- a verb that demanded one would be useless for that.
        //
        // Nothing here is a threshold: the handle reports counts, and whether a
        // count is good enough is the caller's to write into the project file.
        auto probeFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto const blob = checkText(state, 1, "probe blob");
            auto const rect = checkPixelRect(state, context, 2, "probe region");

            auto const key = checkColourKey(state, 6, "probe key");

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

            // No payload, for a reading handle's reason: nothing in C++ ever
            // reads a probe back, so the fields below are the whole of it.
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
        // It takes a match and nothing else: an element is a layer-two object
        // built out of the project file, so the only evidence C++ can check is
        // that this template matched on this frame, under the same-frame ordinal,
        // the observation's lease, the project fingerprint and one delivery per
        // cycle. "This page authorises this element" is enforced in
        // modules/task/runtime/observe.luau, the only place that knows what a
        // page is. Both ordinals reach the wire, and differ exactly when a match
        // from a spent cycle was refused.
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
        // Its authorization contract is not a click's: a keystroke names no
        // screen position, so it requires an open cycle and nothing else -- no
        // same-frame detection, no lease over a coordinate -- while still
        // consuming the cycle, because it changes the screen as a click does. See
        // TaskContext::cycleKey and EngineSession::pressKey.
        //
        // The name is a scalar the target publishes ("E ends the turn"), not an
        // identity the catalog mints. domain::KeyName defines which names exist,
        // so one outside the set is a Tier B ActionRejected refused before the
        // cycle is spent: a typo must not cost a frame.
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

        // cycle_scroll(ticket, notches) -> (). Consumes the cycle and delivers
        // one wheel scroll of `notches` detents, positive away from the operator
        // and negative toward them.
        //
        // Its authorization contract is `key`'s rather than a click's -- an open
        // cycle and nothing else, since the verb names no screen position, while
        // still consuming the cycle. See TaskContext::cycleScroll and
        // EngineSession::scroll. It is on both surfaces because scrolling a list
        // too long to fit is ordinary business work, and the two exploration
        // privileges are pixels and bare coordinates.
        //
        // Anchoring a scroll to an annotated region is open question 5 of
        // docs/plans/2026-08-01-three-layers-and-agent-operator.md.
        //
        // The count is checked only for the shape a Luau number can be wrong in;
        // the delivery layer bounds what a deliverable count is and refuses zero.
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

        // cycle_move_pointer(ticket, x, y) -> (). Consumes the cycle and moves the
        // pointer to the coordinate, pressing nothing.
        //
        // Its authorization contract is cycle_click_point's and not `key`'s: the
        // point was measured off this frame, so the fingerprint check, the lease
        // and the same-frame rule all apply, and it consumes the cycle because a
        // pointer message changes what the target believes is hovered. See
        // TaskContext::cycleMovePointer and EngineSession::movePointer.
        //
        // A move activates nothing, so it is published like cycle_scroll -- on
        // both surfaces and forwarded on `ctx` -- and it is the input a scroll
        // needs before it (docs/pitfalls/capture-and-target-selection.md).
        auto cycleMovePointerFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const x = checkPixelExtent(state, 2, "cycle_move_pointer x");
            auto const y = checkPixelExtent(state, 3, "cycle_move_pointer y");

            auto const call = NativeCallIdentity{
                .verb         = "cycle_move_pointer",
                .cycleOrdinal = ticket->ordinal,
            };

            auto result = context->cycleMovePointer(*ticket, PixelPoint{x, y});
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // A script's millisecond count as the monotonic Duration the host times
        // with; `what` names the script-facing spelling the author wrote.
        //
        // A raw duration_cast would overflow the signed tick rep -- undefined
        // behaviour -- far below the millisecond count's own int64 limit and
        // before any downstream monotonic-overflow guard could run, so the
        // Duration is built through the checked helpers and a NaN, negative,
        // non-finite or out-of-range count is a Tier B InvalidResource instead.
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

        // A required millisecond argument at `index`; a missing or non-numeric
        // one is a Tier B InvalidResource like every other argument rejection.
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

        // cycle_long_press(ticket, x, y, hold_ms) -> (). Consumes the cycle and
        // delivers one long press at the point: the button goes down, stays down
        // for `hold_ms`, and comes back up before this returns. It sits here
        // rather than beside the other cycle verbs only because it needs
        // checkMillisDuration above.
        //
        // Its authorization contract is cycle_click_point's, not `key`'s: it
        // names a coordinate the caller measured off this frame, so the
        // fingerprint check, the lease and the same-frame rule all apply, and it
        // consumes the cycle because a delivered press changes the screen. See
        // TaskContext::cycleLongPress and EngineSession::longPress.
        //
        // A bare press would span frames and nothing yet says who guarantees the
        // release; see engine::IActionSink::longPress.
        //
        // It is privileged exactly as cycle_click_point is, and bound on both
        // surfaces for the same reason: no business environment can name it, and
        // only `observe.long_press` and `explore.long_press` reach it.
        //
        // The hold has no default: a duration the caller cannot see is a decision
        // the caller did not make.
        auto cycleLongPressFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            guardFatal(state, context);

            auto* ticket = checkBox<CycleTicket>(state, 1, k_cycleType, "cycle");
            auto const x = checkPixelExtent(state, 2, "cycle_long_press x");
            auto const y = checkPixelExtent(state, 3, "cycle_long_press y");
            auto const hold =
                checkMillisDuration(state, 4, "cycle_long_press hold_ms");
            if (hold > k_maxLongPressHold)
            {
                // Refused before the cycle is spent, so a mistyped hold costs
                // no frame and leaves no button down.
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "cycle_long_press exceeds the host's long-press ceiling; a "
                    "hold that long leaves the target mid-press"
                );
            }

            // The count was whole and non-negative and the ceiling above caps
            // it far below any tick limit, so the round trip is exact.
            auto const call = NativeCallIdentity{
                .verb           = "cycle_long_press",
                .cycleOrdinal   = ticket->ordinal,
                .durationMillis = static_cast<uint64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(hold)
                        .count()
                ),
            };

            auto result = context->cycleLongPress(*ticket, PixelPoint{x, y}, hold);
            if (!result)
            {
                traceHostCallFailure(state, context, call, result.error());
            }
            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // The automation kind whose domain wire spelling is `wireName`. It
        // compares against the same domain function that produces uf.errors, a
        // carrier's `kind` field and the trace, so there is no second copy of the
        // spellings to drift from.
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
        // error() from Luau reaches the host as a plain string that neither
        // ctx:try nor ctx:retry treats as an automation failure, so only a
        // host-minted Tier B carrier can fail as one.
        //
        // `cancelled` and `internal_invariant` are refused. Both must arrive with
        // the fatal latch already set and this primitive deliberately does not
        // latch, so each keeps its own door -- raiseCancelled and raiseInvariant,
        // reached only when the host itself decided -- rather than a catchable
        // impostor here.
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

        // Raises the Tier C sentinel when a stop has already been requested. The
        // time primitives below reach no engine verb, so nothing downstream would
        // fail closed for them; this runs before a pause and after one, so a stop
        // landing mid-sleep ends the primitive on the terminal path. The question
        // is requireNotCancelled's, shared with the operator front-end; only the
        // raise is here.
        auto guardCancelled(lua_State* state, TaskContext* context) -> void
        {
            auto const live = requireNotCancelled(*context);
            if (live)
            {
                return;
            }
            raiseFromError(state, context, live.error());
        }

        // deadline(ms) -> deadline handle. The absolute instant `ms` from now,
        // opaque for a cycle ticket's reason: a wait budget a script could name
        // is one it could renew. No clock reads back into Lua, so the only thing
        // to do with a deadline is hand it to wait.
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
        // reports whether the deadline still has budget; false ends the
        // framework's wait loop.
        //
        // It polls nothing itself -- what is re-observed between two calls is the
        // framework's business, which keeps this an indivisible effect rather
        // than a policy loop in C++ (design section 18). The pause is
        // min(interval, time to deadline) and is skipped once the deadline has
        // passed; the interval is clamped up to k_minWaitPollInterval so a
        // framework bug asking for zero cannot busy-wait.
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

        // settle(ms) -> (). The one declarative bounded pause. It reaches no
        // engine verb, so its whole content is the duration -- which design
        // section 10 makes part of the replay input, hence the trace line.
        //
        // A request beyond k_maxSettleDuration is Tier B rather than an invariant
        // failure, because section 9 reserves the invariant kind for failures a
        // project cannot cause. Refused before the pause and untraced.
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

            // The duration came from a whole millisecond count and is capped
            // far below any tick ceiling, so the round trip is exact.
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
                // Record the abandoned settle first, so the trace shows the
                // pause that was cut short rather than a verb that said nothing.
                auto const cancelled = fail(
                    AutomationErrorKind::Cancelled,
                    "cancelled while settling"
                );
                traceHostCallFailure(state, context, call, cancelled.error());
            }

            traceHostCall(state, context, call, trace::NativeCallOutcome::Succeeded);
            return 0;
        }

        // One spelling the framework may pass to `emit`, and the event it names.
        // The framework writes the bare verb ("step_started"); the layer prefix
        // lives once, in the trace schema.
        struct SemanticEventName final
        {
            std::string_view      verb;
            trace::TraceEventKind kind;
        };

        // The whole vocabulary the framework may request. Anything else is a
        // framework bug rather than a bad argument: this table and the Luau call
        // sites ship in the same binary. subtask_entered / subtask_exited are
        // absent because cross-file reuse is P1 -- there is no ctx:call yet.
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
            SemanticEventName{
                .verb = "page_resolved",
                .kind = trace::TraceEventKind::FrameworkPageResolved,
            },
        };

        // The scope label at `index`. A non-string is a framework bug rather than
        // a bad argument: every call site has already checked its own. Length and
        // character set are the stream validator's.
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

        // A whole non-negative count at `index`. Tier B rather than an invariant
        // failure, because every count `emit` takes comes from a project's own
        // policy table.
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
        // A safety primitive; the admission argument is in
        // docs/plans/2026-07-29-three-layer-task-system.md.
        //
        // The design writes it as emit(event) taking a table; it takes a name and
        // scalars because section 5's second invariant admits only host-minted
        // handles and scalars. Nothing is lost: the vocabulary is closed, so the
        // name decides the positional shape.
        //
        // It is not a passthrough -- the host validates against the stream state
        // machine and records nothing that fails, so a buggy framework cannot
        // write a plausible history of a run that did not happen. It writes no
        // task.native_call of its own: the event IS the record.
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
        // The one primitive that does not enter through guardFatal, which is its
        // whole purpose: a framework cleanup path must be able to ask whether the
        // generation is live before emitting a closing event, and a question that
        // raised when the answer is "no" could never be asked. It confers nothing
        // -- the answer is already observable as "the last primitive refused".
        auto terminalFn(lua_State* state) -> int
        {
            auto* context = boundContext(state);
            lua_pushboolean(state, context->fatal() ? 1 : 0);
            return 1;
        }

        // The largest magnitude a random bound may take: beyond 2^53 a Lua
        // number cannot represent every integer, so the bound the script passed
        // would already have been rounded.
        constexpr auto k_maxExactInteger = int64{9007199254740992};

        // A random bound at `index`. Out of range is a Tier B InvalidResource,
        // and the messages name ctx:random because that is the spelling the
        // author wrote.
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

        // random([m [, n]]) -> number. math.random's shape over the host's
        // deterministic seeded RNG: no argument gives a double in [0, 1), one
        // gives an integer in [1, m], two give one in [m, n]. An empty interval is
        // a Tier B InvalidResource, as math.random rejects one.
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

        // The caller adds any methods, then finishMetatable freezes and
        // registers it.
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
        // deepFreezeMetatable rather than deepFreeze: this metatable is
        // registered here and worn later, so the plain walk would only ever see
        // an ordinary table and would never check the design's metatable rules --
        // a __metatable field, a table __index, frozen before anything wears it.
        [[nodiscard]]
        auto finishMetatable(lua_State* state, char const* registryType) -> Status
        {
            UF_TRY(script::deepFreezeMetatable(state, -1));
            lua_setfield(state, LUA_REGISTRYINDEX, registryType);
            return ok();
        }

        // Registers the metatables only a bound session can mint. They belong to
        // the private capability installer because a primitive produces each one,
        // so a VM with no session has nothing that could wear them.
        //
        // The Tier B carrier and the field-carrying handle kinds are absent on
        // purpose: their metatables are per instance, so there is nothing shared
        // to register.
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

        // Populates `uf.errors` with one constant per AutomationErrorKind, key
        // and value both that kind's domain wire spelling, so `err.kind ==
        // uf.errors.timeout` compares the exact string the carrier and the trace
        // line carry.
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

        // Binds one primitive into the private surface table at `surface`, as a
        // C closure carrying the TaskContext as lightuserdata upvalue 1.
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

        // Assembles the frozen global uf table: the error kinds and nothing
        // else. Every entry is data that can neither observe nor act, which is
        // what makes it safe as a project global. Its `elements` and `pages`
        // tables went with the C++ page model; the pre-VM pass still resolves
        // uf.elements.<name> against the project file (task/script-validator.hpp).
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
        // as script::PrivateCapabilityInstaller's contract states: the boot hands
        // this table to the framework bundle as a chunk argument and drops it, so
        // no name in either environment refers to it. It is frozen for the uf
        // table's reason: rebinding a primitive would silently redefine what
        // "click" means.
        //
        // `mode` is the only place the two environments differ, and it is not a
        // permission check -- a Run surface simply never binds cycle_crop or
        // probe, so a framework module naming one reads nil. Section 2 rule 2 of
        // docs/plans/2026-08-01-three-layers-and-agent-operator.md asks for
        // exactly that: the privileged verbs do not exist rather than being
        // refused one call at a time. cycle_click_point and cycle_long_press are
        // privileged too but bound on both; their installations below say where
        // their confinement lives.
        //
        // Everything else is identical between the two, because the exploration
        // environment is the run surface plus those mode-gated keys and never a
        // different set (docs/plans/2026-08-01-agent-front-end-and-exploration.md
        // 1). An agent must meet exactly the guarantees a task meets on every
        // verb they share.
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
            // Unprivileged like cycle_read: it hands back text and rectangles,
            // which is an answer about the frame rather than the frame's pixels
            // or a bare coordinate.
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
            // Bound on both surfaces, not an oversight: an element verified by
            // its expected text carries no match handle, so observe.click
            // delivers at the rectangle the page model gave it (locked decision 4
            // of docs/plans/2026-08-01-three-layers-and-agent-operator.md 5). Its
            // confinement is that no business environment can name it -- it is a
            // framework closure upvalue, and only the exploration environment
            // publishes a forward.
            installPrimitive(
                state,
                surface,
                "cycle_click_point",
                &cycleClickPointFn,
                "uf_cycle_click_point",
                context
            );
            // Bound on both surfaces for the reason immediately above: a long
            // press names a bare coordinate, so it is the same privilege under
            // the same confinement.
            installPrimitive(
                state,
                surface,
                "cycle_long_press",
                &cycleLongPressFn,
                "uf_cycle_long_press",
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
            // Unprivileged like `key`: the two privileges are pixels and bare
            // coordinates, not input in general.
            installPrimitive(
                state,
                surface,
                "cycle_scroll",
                &cycleScrollFn,
                "uf_cycle_scroll",
                context
            );
            // Unprivileged too, which sharpens the coordinate rule above rather
            // than bending it: what a bare coordinate buys a script is ACTIVATING
            // an element the page did not authorise, and a move activates nothing.
            // See cycleMovePointerFn.
            installPrimitive(
                state,
                surface,
                "cycle_move_pointer",
                &cycleMovePointerFn,
                "uf_cycle_move_pointer",
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

            // The two non-primitive entries. The Tier B label, so ctx:try can
            // ask whether a caught value wears it; and the trace's own scope
            // label ceiling, so a page name too long to emit is refused where it
            // is authored. See k_errorTagField and k_labelCeilingField.
            lua_pushstring(state, k_errorType);
            lua_setfield(state, surface, k_errorTagField);

            lua_pushnumber(state, static_cast<double>(trace::k_maxScopeLabelBytes));
            lua_setfield(state, surface, k_labelCeilingField);

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
        // outlives the VM this installer configures (see the header contract).
        // The TaskContext is non-movable, so it is a non-owning observation.
        TaskContext* const contextPtr = &context;
        return [contextPtr, mode](lua_State* state) -> Status
        {
            return buildPrivateSurface(state, contextPtr, mode);
        };
    }
}
