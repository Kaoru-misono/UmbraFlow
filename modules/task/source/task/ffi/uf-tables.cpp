#include <task/cycle-ledger.hpp>
#include <task/runtime-model-file.hpp>
#include <task/pixel-probe.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <script/engine.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
        constexpr auto k_cycleType = "uf.annotation-cycle";
        constexpr auto k_errorType = "uf.error";
        constexpr auto k_errorTag  = 1;
        constexpr auto k_defaultProbeTolerance = uint32{12};

        // The action kinds a Runtime Receipt payload may name, as a table and
        // an enumerator rather than a chain of string comparisons: a kind the
        // schema gains has to be given a row here, and the total switch that
        // reads the result then stops compiling until it is handled.
        enum class ReceiptActionKind : uint8
        {
            Click,
            Key,
        };

        constexpr auto k_receiptActionKinds = std::array{
            std::pair{std::string_view{"click"}, ReceiptActionKind::Click},
            std::pair{std::string_view{"key"}, ReceiptActionKind::Key},
        };

        struct TierBError final
        {
            AutomationErrorKind kind{};
        };

        auto denyWrite(lua_State* state) -> int
        {
            luaL_error(state, "Host handles are read-only");
        }

        auto fixedToString(lua_State* state) -> int
        {
            lua_pushvalue(state, lua_upvalueindex(1));
            return 1;
        }

        [[nodiscard]]
        auto retryable(AutomationErrorKind kind) noexcept -> bool
        {
            return failureResponse(kind) == FailureResponse::Retry;
        }

        [[noreturn]]
        auto raiseTierB(
            lua_State* state,
            AutomationErrorKind kind,
            std::string const& message
        ) -> void
        {
            static_assert(std::is_trivially_destructible_v<TierBError>);
            void* const storage = lua_newuserdatatagged(
                state,
                sizeof(TierBError),
                k_errorTag
            );
            std::construct_at(
                static_cast<TierBError*>(storage),
                TierBError{.kind = kind}
            );
            int const carrier = lua_gettop(state);

            lua_createtable(state, 0, 3);
            int const fields = lua_gettop(state);
            auto const wire = automationErrorWireName(kind);
            lua_pushlstring(state, wire.data(), wire.size());
            lua_setfield(state, fields, "kind");
            lua_pushlstring(state, message.data(), message.size());
            lua_setfield(state, fields, "message");
            lua_pushboolean(state, retryable(kind) ? 1 : 0);
            lua_setfield(state, fields, "retryable");

            lua_createtable(state, 0, 4);
            int const metatable = lua_gettop(state);
            lua_pushvalue(state, fields);
            lua_setfield(state, metatable, "__index");
            lua_pushcfunction(state, &denyWrite, "uf_error_newindex");
            lua_setfield(state, metatable, "__newindex");
            lua_pushstring(state, k_errorType);
            lua_pushcclosure(state, &fixedToString, "uf_error_tostring", 1);
            lua_setfield(state, metatable, "__tostring");
            lua_pushstring(state, k_errorType);
            lua_setfield(state, metatable, "__metatable");
            auto const frozen = script::deepFreezeMetatable(state, metatable);
            if (!frozen)
            {
                lua_pushstring(state, "cannot freeze Host error carrier");
                lua_error(state);
            }
            lua_setmetatable(state, carrier);
            lua_settop(state, carrier);
            lua_error(state);
        }

        [[nodiscard]]
        auto receiptActionKind(lua_State* state, std::string_view name)
            -> ReceiptActionKind
        {
            auto const found = std::ranges::find(
                k_receiptActionKinds,
                name,
                &std::pair<std::string_view, ReceiptActionKind>::first
            );
            if (found == k_receiptActionKinds.end())
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "action_kind has an unsupported value"
                );
            }
            return found->second;
        }

        [[nodiscard]]
        auto decodeTierB(lua_State* state, int index)
            -> std::optional<script::RaisedError>
        {
            auto const carrier = lua_absindex(state, index);
            if (
                lua_type(state, carrier) != LUA_TUSERDATA
                || lua_userdatatag(state, carrier) != k_errorTag
            )
            {
                return std::nullopt;
            }
            auto const* p_error = static_cast<TierBError const*>(
                lua_touserdata(state, carrier)
            );
            auto result = script::RaisedError{.kind = p_error->kind};
            int const base = lua_gettop(state);
            auto guard = scopeExit([state, base]() noexcept { lua_settop(state, base); });
            if (lua_getmetatable(state, carrier) != 0)
            {
                lua_rawgetfield(state, -1, "__index");
                lua_rawgetfield(state, -1, "message");
                if (lua_type(state, -1) == LUA_TSTRING)
                {
                    std::size_t size{};
                    char const* const text = lua_tolstring(state, -1, &size);
                    result.message.assign(text, size);
                }
            }
            return result;
        }

        [[noreturn]]
        auto raiseFromError(lua_State* state, TaskContext* context, Error const& error)
            -> void
        {
            auto const kind = automationErrorKind(error)
                .value_or(AutomationErrorKind::InternalInvariant);
            if (kind == AutomationErrorKind::Cancelled)
            {
                if (context != nullptr)
                {
                    context->markTerminal(kind);
                }
                lua_pushstring(state, "uf: annotation cancelled");
                lua_error(state);
            }
            raiseTierB(state, kind, std::string{error.message()});
        }

        [[nodiscard]] auto boundContext(lua_State* state) -> TaskContext*
        {
            return static_cast<TaskContext*>(
                lua_tolightuserdata(state, lua_upvalueindex(1))
            );
        }

        auto guardLive(lua_State* state, TaskContext* context) -> void
        {
            if (auto const terminal = context->terminalKind(); terminal.has_value())
            {
                raiseTierB(state, *terminal, "annotation generation is spent");
            }
            if (context->cancellationRequested())
            {
                context->markTerminal(AutomationErrorKind::Cancelled);
                lua_pushstring(state, "uf: annotation cancelled");
                lua_error(state);
            }
        }

        template <typename T>
        auto destroyBox(void* storage) -> void
        {
            std::destroy_at(static_cast<T*>(storage));
        }

        template <typename T>
        auto pushBox(lua_State* state, T value, char const* metatable) -> void
        {
            static_assert(alignof(T) <= 16);
            void* const storage = lua_newuserdatadtor(
                state,
                sizeof(T),
                &destroyBox<T>
            );
            std::construct_at(static_cast<T*>(storage), std::move(value));
            luaL_getmetatable(state, metatable);
            lua_setmetatable(state, -2);
        }

        template <typename T>
        [[nodiscard]]
        auto boxAt(
            lua_State* state,
            int index,
            char const* metatable,
            std::string_view expected
        ) -> T*
        {
            if (
                lua_type(state, index) != LUA_TUSERDATA
                || lua_getmetatable(state, index) == 0
            )
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{"expected "} + std::string{expected}
                );
            }
            luaL_getmetatable(state, metatable);
            auto const same = lua_rawequal(state, -1, -2) != 0;
            lua_pop(state, 2);
            if (!same)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{"expected "} + std::string{expected}
                );
            }
            return static_cast<T*>(lua_touserdata(state, index));
        }

        auto beginOpaqueMetatable(
            lua_State* state,
            char const* name,
            char const* label
        ) -> Status
        {
            lua_newtable(state);
            int const metatable = lua_gettop(state);
            lua_newtable(state);
            lua_setfield(state, metatable, "__index");
            lua_pushcfunction(state, &denyWrite, "uf_opaque_newindex");
            lua_setfield(state, metatable, "__newindex");
            lua_pushstring(state, label);
            lua_pushcclosure(state, &fixedToString, "uf_opaque_tostring", 1);
            lua_setfield(state, metatable, "__tostring");
            lua_pushstring(state, label);
            lua_setfield(state, metatable, "__metatable");
            UF_TRY(script::deepFreezeMetatable(state, metatable));
            lua_setfield(state, LUA_REGISTRYINDEX, name);
            return ok();
        }

        [[nodiscard]]
        auto cycleAt(lua_State* state, int index) -> CycleTicket*
        {
            if (lua_type(state, index) != LUA_TUSERDATA || lua_getmetatable(state, index) == 0)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "expected an Annotation cycle handle"
                );
            }
            luaL_getmetatable(state, k_cycleType);
            auto const same = lua_rawequal(state, -1, -2) != 0;
            lua_pop(state, 2);
            if (!same)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "expected an Annotation cycle handle"
                );
            }
            return static_cast<CycleTicket*>(lua_touserdata(state, index));
        }

        [[nodiscard]]
        auto unsignedInteger(lua_State* state, int index, std::string_view name) -> uint32
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must be a number"
                );
            }
            auto const value = checkedIntegralCast<uint32>(lua_tonumber(state, index));
            if (!value)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must be a non-negative whole number"
                );
            }
            return *value;
        }

        [[nodiscard]]
        auto rectangle(lua_State* state, int first, std::string_view name) -> PixelRect
        {
            auto const built = PixelRect::create(
                unsignedInteger(state, first, name),
                unsignedInteger(state, first + 1, name),
                unsignedInteger(state, first + 2, name),
                unsignedInteger(state, first + 3, name)
            );
            if (!built)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{built.error().message()}
                );
            }
            return *built;
        }

        [[nodiscard]]
        auto colourKey(lua_State* state, int first)
            -> std::optional<ProbeColourKey>
        {
            if (lua_isnoneornil(state, first))
            {
                return std::nullopt;
            }
            auto const red   = unsignedInteger(state, first, "colour red");
            auto const green = unsignedInteger(state, first + 1, "colour green");
            auto const blue  = unsignedInteger(state, first + 2, "colour blue");
            if (red > 255U || green > 255U || blue > 255U)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "colour channels must be between 0 and 255"
                );
            }
            if (lua_type(state, first + 4) != LUA_TBOOLEAN)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "colour direction must be a boolean"
                );
            }
            return ProbeColourKey{
                .red   = static_cast<uint8>(red),
                .green = static_cast<uint8>(green),
                .blue  = static_cast<uint8>(blue),
                .tolerance = lua_isnoneornil(state, first + 3)
                    ? k_defaultProbeTolerance
                    : unsignedInteger(state, first + 3, "colour tolerance"),
                .removes = lua_toboolean(state, first + 4) != 0,
            };
        }

        auto addNumber(lua_State* state, char const* name, uint64 value) -> void
        {
            lua_pushnumber(state, static_cast<double>(value));
            lua_setfield(state, -2, name);
        }

        auto freezeData(lua_State* state, TaskContext* context) -> void
        {
            auto const frozen = script::deepFreeze(state, -1);
            if (!frozen)
            {
                context->markTerminal(AutomationErrorKind::InternalInvariant);
                raiseTierB(
                    state,
                    AutomationErrorKind::InternalInvariant,
                    "cannot freeze Annotation result"
                );
            }
        }

        auto cycleOpen(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            guardLive(state, context);
            auto result = context->openCycle();
            if (!result)
            {
                raiseFromError(state, context, result.error());
            }
            pushBox(state, *result, k_cycleType);
            return 1;
        }

        auto cycleClose(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            guardLive(state, context);
            lua_pushboolean(state, context->closeCycle(*cycleAt(state, 1)) ? 1 : 0);
            return 1;
        }

        auto cycleCrop(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            guardLive(state, context);
            auto result = context->cycleCrop(
                *cycleAt(state, 1),
                rectangle(state, 2, "crop rectangle"),
                colourKey(state, 6)
            );
            if (!result)
            {
                raiseFromError(state, context, result.error());
            }

            auto bytes = std::string{};
            bytes.reserve(result->png.size());
            for (auto const value : result->png)
            {
                bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
            }
            lua_pushlstring(state, bytes.data(), bytes.size());
            auto const hash = result->hash.hex();
            lua_pushlstring(state, hash.data(), hash.size());
            if (!result->mask.has_value())
            {
                lua_pushnil(state);
                return 3;
            }
            lua_createtable(state, 0, 10);
            addNumber(state, "key_red", result->mask->key.red);
            addNumber(state, "key_green", result->mask->key.green);
            addNumber(state, "key_blue", result->mask->key.blue);
            addNumber(state, "tolerance", result->mask->key.tolerance);
            lua_pushboolean(state, result->mask->key.removes ? 1 : 0);
            lua_setfield(state, -2, "key_removes");
            addNumber(state, "rect_pixels", result->mask->rectPixels);
            addNumber(state, "selected_pixels", result->mask->selectedPixels);
            addNumber(state, "ramp_selected_pixels", result->mask->rampSelectedPixels);
            lua_pushlstring(
                state,
                result->mask->warning.data(),
                result->mask->warning.size()
            );
            lua_setfield(state, -2, "warning");
            freezeData(state, context);
            return 3;
        }

        auto probe(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            guardLive(state, context);
            if (lua_type(state, 1) != LUA_TSTRING)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "probe bytes must be a string"
                );
            }
            std::size_t size{};
            char const* const text = lua_tolstring(state, 1, &size);
            auto const view = std::string_view{text, size};
            auto const bytes = std::as_bytes(std::span{view});
            auto result = probePngRegion(
                bytes,
                rectangle(state, 2, "probe rectangle"),
                colourKey(state, 6)
            );
            if (!result)
            {
                raiseFromError(state, context, result.error());
            }
            lua_createtable(state, 0, 12);
            addNumber(state, "image_width", result->imageWidth);
            addNumber(state, "image_height", result->imageHeight);
            addNumber(state, "rect_pixels", result->rectPixels);
            addNumber(state, "distinct_colours", result->distinctColours);
            addNumber(state, "dominant_red", result->dominantRed);
            addNumber(state, "dominant_green", result->dominantGreen);
            addNumber(state, "dominant_blue", result->dominantBlue);
            addNumber(state, "dominant_pixels", result->dominantPixels);
            // PixelProbeReport engages the three selection counts together or
            // not at all, so this asks for exactly what it then reads rather
            // than reading two of them on the strength of the first.
            if (
                result->fullySelectedPixels.has_value()
                && result->rampSelectedPixels.has_value()
                && result->selectedWeight.has_value()
            )
            {
                addNumber(state, "fully_selected_pixels", *result->fullySelectedPixels);
                addNumber(state, "ramp_selected_pixels", *result->rampSelectedPixels);
                addNumber(state, "selected_weight", *result->selectedWeight);
            }
            freezeData(state, context);
            return 1;
        }

        [[nodiscard]]
        auto stringAt(lua_State* state, int index, std::string_view name)
            -> std::string_view
        {
            if (lua_type(state, index) != LUA_TSTRING)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must be a string"
                );
            }
            std::size_t size{};
            char const* const text = lua_tolstring(state, index, &size);
            return std::string_view{text, size};
        }

        auto projectRead(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            guardLive(state, context);
            auto result = context->projectRead(stringAt(state, 1, "project path"));
            if (!result)
            {
                raiseFromError(state, context, result.error());
            }
            auto bytes = std::string{};
            bytes.reserve(result->size());
            for (auto const value : *result)
            {
                bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
            }
            lua_pushlstring(state, bytes.data(), bytes.size());
            return 1;
        }

        auto projectWrite(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            guardLive(state, context);
            auto const path = stringAt(state, 1, "project path");
            auto const text = stringAt(state, 2, "project content");
            auto const status = context->projectWrite(
                path,
                std::as_bytes(std::span{text})
            );
            if (!status)
            {
                raiseFromError(state, context, status.error());
            }
            return 0;
        }

        auto terminal(lua_State* state) -> int
        {
            auto* const context = boundContext(state);
            lua_pushboolean(state, context->fatal() ? 1 : 0);
            return 1;
        }

        auto beginCycleMetatable(lua_State* state) -> Status
        {
            lua_newtable(state);
            int const metatable = lua_gettop(state);
            lua_newtable(state);
            lua_setfield(state, metatable, "__index");
            lua_pushcfunction(state, &denyWrite, "uf_cycle_newindex");
            lua_setfield(state, metatable, "__newindex");
            lua_pushstring(state, k_cycleType);
            lua_pushcclosure(state, &fixedToString, "uf_cycle_tostring", 1);
            lua_setfield(state, metatable, "__tostring");
            lua_pushstring(state, k_cycleType);
            lua_setfield(state, metatable, "__metatable");
            UF_TRY(script::deepFreezeMetatable(state, metatable));
            lua_setfield(state, LUA_REGISTRYINDEX, k_cycleType);
            return ok();
        }

        auto install(
            lua_State* state,
            int surface,
            char const* name,
            lua_CFunction function,
            TaskContext* context
        ) -> void
        {
            lua_pushlightuserdata(state, context);
            lua_pushcclosure(state, function, name, 1);
            lua_setfield(state, surface, name);
        }

        auto buildAnnotationSurface(lua_State* state, TaskContext* context) -> Status
        {
            UF_TRY(beginCycleMetatable(state));
            lua_createtable(state, 0, 8);
            int const surface = lua_gettop(state);
            install(state, surface, "explore_cycle_open", &cycleOpen, context);
            install(state, surface, "explore_cycle_close", &cycleClose, context);
            install(state, surface, "explore_crop", &cycleCrop, context);
            install(state, surface, "explore_probe", &probe, context);
            install(state, surface, "explore_project_read", &projectRead, context);
            install(state, surface, "explore_project_write", &projectWrite, context);
            install(state, surface, "explore_terminal", &terminal, context);
            lua_pushstring(state, k_errorType);
            lua_setfield(state, surface, "error_tag");
            UF_TRY(script::deepFreeze(state, surface));
            return ok();
        }
    }

    class TaskHost::RuntimeNativeState final
    {
        static constexpr auto k_bindingType = "uf.runtime-binding";
        static constexpr auto k_assetType   = "uf.runtime-asset";
        static constexpr auto k_cycleType   = "uf.runtime-cycle";
        static constexpr auto k_proofType   = "uf.runtime-proof";
        static constexpr auto k_receiptType = "uf.runtime-receipt";

        struct BindingToken final
        {
            GenerationId generation;
        };

        struct AssetToken final
        {
            GenerationId   generation;
            TemplateTicket ticket;
        };

        struct ProofToken final
        {
            GenerationId   generation;
            CycleTicket    cycle;
            TemplateTicket asset;
            PixelRect      searchRect;
            PixelRect      matchedRect;
        };

        TaskHost*    m_pHost;
        GenerationId m_generation;

    public:
        // Public only so that std::construct_at can reach it. The class name is
        // private to TaskHost, so nothing outside the Host can name the type in
        // order to call this.
        RuntimeNativeState(TaskHost& host, GenerationId generation) noexcept
            : m_pHost{&host}
            , m_generation{generation}
        {
        }

        RuntimeNativeState(RuntimeNativeState const&) = delete;
        RuntimeNativeState(RuntimeNativeState&&) = delete;
        auto operator=(RuntimeNativeState const&) -> RuntimeNativeState& = delete;
        auto operator=(RuntimeNativeState&&) -> RuntimeNativeState& = delete;
        ~RuntimeNativeState() = default;

    private:

        [[nodiscard]] static auto bound(lua_State* state) -> RuntimeNativeState&
        {
            auto* const p_state = static_cast<RuntimeNativeState*>(
                lua_touserdata(state, lua_upvalueindex(1))
            );
            UF_CHECK(p_state != nullptr);
            return *p_state;
        }

        [[nodiscard]] auto context(lua_State* state) -> TaskContext&
        {
            auto found = m_pHost->activeRuntimeContext(m_generation);
            if (!found)
            {
                raiseFromError(state, nullptr, found.error());
            }
            auto* const p_context = *found;
            guardLive(state, p_context);
            return *p_context;
        }

        auto requireArity(
            lua_State* state,
            int expected,
            std::string_view operation
        ) const -> void
        {
            if (lua_gettop(state) != expected)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{operation} + " received the wrong number of arguments"
                );
            }
        }

        [[nodiscard]]
        static auto stringArray(lua_State* state, int index, std::string_view name)
            -> std::vector<std::string>
        {
            if (lua_type(state, index) != LUA_TTABLE)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must be a dense string array"
                );
            }
            auto const absolute = lua_absindex(state, index);
            // lua_objlen answers a length as int and never a negative one; the
            // cast is what lets it meet the std::size_t counts below without a
            // signed/unsigned comparison.
            auto const size     = static_cast<std::size_t>(lua_objlen(state, absolute));
            auto result         = std::vector<std::string>{};
            result.reserve(size);
            for (auto child = std::size_t{1}; child <= size; ++child)
            {
                lua_rawgeti(state, absolute, static_cast<int>(child));
                result.emplace_back(stringAt(state, -1, name));
                lua_pop(state, 1);
            }

            auto count = std::size_t{0};
            lua_pushnil(state);
            while (lua_next(state, absolute) != 0)
            {
                ++count;
                lua_pop(state, 1);
            }
            if (count != size)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must be a dense string array"
                );
            }
            return result;
        }

        // One element of a dense two-integer array standing on the stack.
        // base_resolution and base_dpi are both that shape in the model, so
        // they cross as the two pairs the model declares rather than as four
        // loose numbers, and the native arity stays equal to the field count
        // the trusted parser reads.
        [[nodiscard]]
        static auto pairElement(
            lua_State* state,
            int index,
            int element,
            std::string_view name
        ) -> uint32
        {
            if (
                lua_type(state, index) != LUA_TTABLE
                || lua_objlen(state, index) != 2U
            )
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must contain exactly two integers"
                );
            }
            auto const absolute = lua_absindex(state, index);
            lua_rawgeti(state, absolute, element);
            auto const value = unsignedInteger(state, -1, name);
            lua_pop(state, 1);
            return value;
        }

        [[nodiscard]]
        static auto tableRect(
            lua_State* state,
            int table,
            char const* field,
            std::string_view name
        ) -> PixelRect
        {
            auto const absolute = lua_absindex(state, table);
            lua_rawgetfield(state, absolute, field);
            if (lua_type(state, -1) != LUA_TTABLE || lua_objlen(state, -1) != 4U)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must contain four integers"
                );
            }
            auto const values = lua_absindex(state, -1);
            for (auto index = 1; index <= 4; ++index)
            {
                lua_rawgeti(state, values, index);
            }
            auto const result = rectangle(state, -4, name);
            lua_pop(state, 5);
            return result;
        }

        [[nodiscard]]
        static auto tablePoint(
            lua_State* state,
            int table,
            char const* field,
            std::string_view name
        ) -> PixelPoint
        {
            auto const absolute = lua_absindex(state, table);
            lua_rawgetfield(state, absolute, field);
            if (lua_type(state, -1) != LUA_TTABLE || lua_objlen(state, -1) != 2U)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must contain two integers"
                );
            }
            auto const values = lua_absindex(state, -1);
            lua_rawgeti(state, values, 1);
            lua_rawgeti(state, values, 2);
            auto const result = PixelPoint{
                unsignedInteger(state, -2, name),
                unsignedInteger(state, -1, name),
            };
            lua_pop(state, 3);
            return result;
        }

        static auto requireStringField(
            lua_State* state,
            int table,
            char const* field,
            std::optional<std::string_view> expected = std::nullopt
        ) -> std::string
        {
            auto const absolute = lua_absindex(state, table);
            lua_rawgetfield(state, absolute, field);
            auto const value = std::string{stringAt(state, -1, field)};
            if (expected.has_value() && value != *expected)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{field} + " has an unsupported value"
                );
            }
            lua_pop(state, 1);
            return value;
        }

        // Refuses a field the payload must not carry at all.
        //
        // Unfalsifiable through any model, and kept for the reason every other
        // re-check on this boundary is kept: the trusted compiler already
        // forbids a key action's action_point and a click action's key, so the
        // only writer that reaches here cannot produce either. What this would
        // notice is a SECOND writer of the payload table -- which is the whole
        // job of the checks around it, `placement.kind == "fixed"` included.
        // No mutation of this tree turns it red; that is a fact about the
        // estate, not evidence that the check is idle.
        static auto requireAbsentField(
            lua_State* state,
            int table,
            char const* field,
            std::string_view name
        ) -> void
        {
            auto const absolute = lua_absindex(state, table);
            lua_rawgetfield(state, absolute, field);
            auto const present = lua_type(state, -1) != LUA_TNIL;
            lua_pop(state, 1);
            if (present)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    std::string{name} + " must not carry " + field
                );
            }
        }

        static auto pushUnknown(lua_State* state, std::string_view reason) -> int
        {
            lua_createtable(state, 0, 2);
            lua_pushstring(state, "unknown");
            lua_setfield(state, -2, "kind");
            lua_pushlstring(state, reason.data(), reason.size());
            lua_setfield(state, -2, "reason");
            auto const frozen = script::deepFreeze(state, -1);
            if (!frozen)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InternalInvariant,
                    "cannot freeze Runtime Unknown result"
                );
            }
            return 1;
        }

        static auto pushAbsent(lua_State* state) -> int
        {
            lua_createtable(state, 0, 1);
            lua_pushstring(state, "absent");
            lua_setfield(state, -2, "kind");
            auto const frozen = script::deepFreeze(state, -1);
            if (!frozen)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InternalInvariant,
                    "cannot freeze Runtime Absent result"
                );
            }
            return 1;
        }

        [[nodiscard]]
        static auto unknownReason(Error const& error) noexcept -> std::string_view
        {
            switch (
                automationErrorKind(error)
                    .value_or(AutomationErrorKind::InternalInvariant)
            )
            {
            case AutomationErrorKind::RecognitionIncomplete:
                return "locator_failed";
            case AutomationErrorKind::StaleObservation:
                return "stale_cycle";
            case AutomationErrorKind::CaptureUnavailable:
                return "host_unavailable";

            // Everything a script is told nothing about. Listed rather than
            // defaulted for the reason AutomationErrorKind itself states: a new
            // kind must not acquire a script-visible reason by falling through.
            case AutomationErrorKind::Cancelled:
            case AutomationErrorKind::Timeout:
            case AutomationErrorKind::InvalidResource:
            case AutomationErrorKind::UnsupportedCapability:
            case AutomationErrorKind::TargetCompatibilityUnverified:
            case AutomationErrorKind::TargetUnavailable:
            case AutomationErrorKind::CaptureStalled:
            case AutomationErrorKind::PageUnresolved:
            case AutomationErrorKind::ActionRejected:
            case AutomationErrorKind::ControllerDisconnected:
            case AutomationErrorKind::InternalInvariant:
            case AutomationErrorKind::IoFailure:
            case AutomationErrorKind::ExternalFailure:
                return "internal_error";
            }

            UF_UNREACHABLE_MSG("Unknown AutomationErrorKind value");
        }

        static auto modelBytes(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 0, "runtime_model_bytes");
            auto bytes = self.m_pHost->runtimeModelBytes(self.m_generation);
            if (!bytes)
            {
                raiseFromError(state, nullptr, bytes.error());
            }
            auto text = std::string{};
            text.reserve(bytes->size());
            for (auto const value : *bytes)
            {
                text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
            }
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        static auto semanticHash(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 1, "runtime_semantic_hash");
            auto const bytes = stringAt(state, 1, "canonical RuntimeModel bytes");
            auto hashed = sha256(
                std::as_bytes(std::span{bytes.data(), bytes.size()})
            );
            if (!hashed)
            {
                raiseFromError(state, nullptr, hashed.error());
            }
            auto const encoded = hashed->hex();
            lua_pushlstring(state, encoded.data(), encoded.size());
            return 1;
        }

        static auto finalizeModel(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 9, "runtime_model_finalize");
            auto encoded = std::string{"sha256:"};
            encoded += stringAt(state, 1, "runtime model schema hash");
            auto schemaHash = ContentHash::parse(encoded);
            if (!schemaHash)
            {
                raiseFromError(state, nullptr, schemaHash.error());
            }
            encoded = "sha256:";
            encoded += stringAt(state, 2, "canonical RuntimeModel semantic hash");
            auto semantic = ContentHash::parse(encoded);
            if (!semantic)
            {
                raiseFromError(state, nullptr, semantic.error());
            }
            auto assets = stringArray(state, 3, "RuntimeModel.asset_paths");
            // The three vocabularies arrive as separate arrays rather than one
            // table because that is the only shape stringArray reads, and each
            // must reach the Host as its own list: they are compared
            // independently and a merged list would answer the wrong question.
            auto declaredUi = DeclaredRuntimeUi{
                .surfaces = stringArray(
                    state,
                    4,
                    "RuntimeModel.declared_surface_ids"
                ),
                .uiTargets = stringArray(
                    state,
                    5,
                    "RuntimeModel.declared_ui_target_ids"
                ),
                .actions = stringArray(
                    state,
                    6,
                    "RuntimeModel.declared_action_ids"
                ),
            };
            // The geometry the model states, carried rather than interpreted:
            // ProjectFingerprint is four numbers a capture is measured against,
            // and nothing here reads what any of them mean. Its create refuses
            // a zero in any of the four, which is the last place a model that
            // named an empty extent can be stopped.
            auto geometry = ProjectFingerprint::create(
                pairElement(state, 7, 1, "RuntimeModel.base_resolution"),
                pairElement(state, 7, 2, "RuntimeModel.base_resolution"),
                pairElement(state, 8, 1, "RuntimeModel.base_dpi"),
                pairElement(state, 8, 2, "RuntimeModel.base_dpi")
            );
            if (!geometry)
            {
                raiseFromError(state, nullptr, geometry.error());
            }
            // Every key the model would ever press, validated and then dropped.
            // KeyName is the single definition of which key names exist, and
            // applying it here is what turns "this deployment can never press
            // its key" from a refusal at the moment an action is taken into a
            // refusal at the boundary where a model is checked. Nothing is
            // stored: the name that reaches delivery comes from the Receipt
            // payload and is re-created there, so a list kept here would be a
            // second copy nothing compares against.
            auto const declaredKeys = stringArray(
                state,
                9,
                "RuntimeModel.declared_key_names"
            );
            for (auto const& name : declaredKeys)
            {
                auto const key = KeyName::create(name);
                if (!key)
                {
                    raiseFromError(state, nullptr, key.error());
                }
            }
            auto finalized = self.m_pHost->finalizeRuntimeModel(
                self.m_generation,
                TrustedRuntimeFinalize{
                    .parserSchemaHash = *schemaHash,
                    .semanticHash     = *semantic,
                    .assetReferences  = std::move(assets),
                    .declaredUi       = std::move(declaredUi),
                    .fingerprint      = *geometry,
                }
            );
            if (!finalized)
            {
                raiseFromError(state, nullptr, finalized.error());
            }
            pushBox(
                state,
                BindingToken{.generation = self.m_generation},
                k_bindingType
            );
            return 1;
        }

        static auto asset(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 2, "runtime_asset");
            auto* const p_binding = boxAt<BindingToken>(
                state,
                1,
                k_bindingType,
                "a RuntimeModel binding"
            );
            if (p_binding->generation != self.m_generation)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "RuntimeModel binding belongs to another generation"
                );
            }
            auto& runtimeContext = self.context(state);
            auto bytes = self.m_pHost->runtimeAssetBytes(
                self.m_generation,
                stringAt(state, 2, "runtime asset path")
            );
            if (!bytes)
            {
                raiseFromError(state, &runtimeContext, bytes.error());
            }
            auto loaded = runtimeContext.loadTemplate(*bytes);
            if (!loaded)
            {
                raiseFromError(state, &runtimeContext, loaded.error());
            }
            pushBox(
                state,
                AssetToken{
                    .generation = self.m_generation,
                    .ticket     = loaded->ticket,
                },
                k_assetType
            );
            return 1;
        }

        static auto openCycle(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 1, "runtime_cycle_open");
            auto* const p_binding = boxAt<BindingToken>(
                state,
                1,
                k_bindingType,
                "a RuntimeModel binding"
            );
            if (p_binding->generation != self.m_generation)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "RuntimeModel binding belongs to another generation"
                );
            }
            auto& runtimeContext = self.context(state);
            auto cycle = runtimeContext.openCycle();
            if (!cycle)
            {
                raiseFromError(state, &runtimeContext, cycle.error());
            }
            pushBox(state, *cycle, k_cycleType);
            return 1;
        }

        static auto closeCycle(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 1, "runtime_cycle_close");
            auto& runtimeContext = self.context(state);
            auto* const p_cycle = boxAt<CycleTicket>(
                state,
                1,
                k_cycleType,
                "a Runtime observation cycle"
            );
            lua_pushboolean(state, runtimeContext.closeCycle(*p_cycle) ? 1 : 0);
            return 1;
        }

        static auto match(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 6, "runtime_match");
            auto& runtimeContext = self.context(state);
            auto* const p_cycle = boxAt<CycleTicket>(
                state,
                1,
                k_cycleType,
                "a Runtime observation cycle"
            );
            auto* const p_asset = boxAt<AssetToken>(
                state,
                2,
                k_assetType,
                "a Runtime asset"
            );
            if (p_asset->generation != self.m_generation)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "Runtime asset belongs to another generation"
                );
            }
            auto const searchRect = rectangle(state, 3, "Runtime match rectangle");
            auto found = runtimeContext.cycleMatch(
                *p_cycle,
                p_asset->ticket,
                searchRect
            );
            if (!found)
            {
                if (automationErrorKind(found.error()) == AutomationErrorKind::Cancelled)
                {
                    raiseFromError(state, &runtimeContext, found.error());
                }
                return pushUnknown(state, unknownReason(found.error()));
            }
            if (!found->has_value())
            {
                return pushAbsent(state);
            }

            auto const& value = **found;
            auto confidence = 1.0;
            if (value.maximumSad != 0U)
            {
                auto const bounded = std::min(value.sadScore, value.maximumSad);
                confidence = 1.0
                    - static_cast<double>(bounded)
                        / static_cast<double>(value.maximumSad);
            }
            lua_createtable(state, 0, 3);
            lua_pushstring(state, "present");
            lua_setfield(state, -2, "kind");
            lua_pushnumber(state, confidence);
            lua_setfield(state, -2, "confidence");
            lua_createtable(state, 4, 0);
            lua_pushnumber(state, value.matchedRect.x());
            lua_rawseti(state, -2, 1);
            lua_pushnumber(state, value.matchedRect.y());
            lua_rawseti(state, -2, 2);
            lua_pushnumber(state, value.matchedRect.width());
            lua_rawseti(state, -2, 3);
            lua_pushnumber(state, value.matchedRect.height());
            lua_rawseti(state, -2, 4);
            lua_setfield(state, -2, "rect");
            freezeData(state, &runtimeContext);

            pushBox(
                state,
                ProofToken{
                    .generation = self.m_generation,
                    .cycle      = *p_cycle,
                    .asset       = p_asset->ticket,
                    .searchRect  = searchRect,
                    .matchedRect = value.matchedRect,
                },
                k_proofType
            );
            return 2;
        }

        static auto read(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 5, "runtime_read");
            auto& runtimeContext = self.context(state);
            auto* const p_cycle = boxAt<CycleTicket>(
                state,
                1,
                k_cycleType,
                "a Runtime observation cycle"
            );
            auto reading = runtimeContext.cycleRead(
                *p_cycle,
                rectangle(state, 2, "Runtime read rectangle")
            );
            if (!reading)
            {
                if (automationErrorKind(reading.error()) == AutomationErrorKind::Cancelled)
                {
                    raiseFromError(state, &runtimeContext, reading.error());
                }
                return pushUnknown(state, unknownReason(reading.error()));
            }
            if (!reading->has_value())
            {
                return pushAbsent(state);
            }

            lua_createtable(state, 0, 3);
            lua_pushstring(state, "read");
            lua_setfield(state, -2, "kind");
            lua_pushlstring(
                state,
                (*reading)->text.data(),
                (*reading)->text.size()
            );
            lua_setfield(state, -2, "text");
            lua_pushnumber(
                state,
                static_cast<double>((*reading)->confidenceBp) / 10'000.0
            );
            lua_setfield(state, -2, "confidence");
            freezeData(state, &runtimeContext);
            return 1;
        }

        static auto receipt(lua_State* state) -> int
        {
            auto& self = bound(state);
            self.requireArity(state, 3, "runtime_receipt");
            auto& runtimeContext = self.context(state);
            auto* const p_cycle = boxAt<CycleTicket>(
                state,
                1,
                k_cycleType,
                "a Runtime observation cycle"
            );
            auto* const p_proof = boxAt<ProofToken>(
                state,
                2,
                k_proofType,
                "Host-owned Runtime evidence"
            );
            if (
                p_proof->generation != self.m_generation
                || p_proof->cycle.generation != p_cycle->generation
                || p_proof->cycle.ordinal != p_cycle->ordinal
            )
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "Runtime evidence does not belong to this cycle"
                );
            }
            if (lua_type(state, 3) != LUA_TTABLE)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "Runtime Receipt payload must be a table"
                );
            }
            auto const stateIdentity = requireStringField(state, 3, "state_identity");
            auto const surface = requireStringField(state, 3, "surface");
            auto const uiTarget = requireStringField(state, 3, "ui_target");
            auto const binding = requireStringField(state, 3, "binding");
            auto const variant = requireStringField(state, 3, "variant");
            auto const action = requireStringField(state, 3, "action");
            auto const proofLocator = requireStringField(state, 3, "proof_locator");
            auto const actionKind = receiptActionKind(
                state,
                requireStringField(state, 3, "action_kind")
            );

            lua_rawgetfield(state, 3, "placement");
            if (lua_type(state, -1) != LUA_TTABLE)
            {
                raiseTierB(
                    state,
                    AutomationErrorKind::InvalidResource,
                    "Runtime Receipt placement must be a table"
                );
            }
            auto const placement = lua_absindex(state, -1);
            requireStringField(state, placement, "kind", "fixed");
            auto const placementRect = tableRect(
                state,
                placement,
                "rect",
                "Runtime Receipt placement rect"
            );

            // The rectangle is checked for BOTH kinds and for one reason: it is
            // the rectangle the proof template was searched in, so a payload
            // naming another one is not the Binding this cycle measured. A
            // keystroke aims at nothing inside it and is still tied to it.
            auto const measuredPlacement = placementRect == p_proof->searchRect;
            auto input = [&]() -> TrustedReceiptInput
            {
                switch (actionKind)
                {
                case ReceiptActionKind::Click:
                {
                    requireAbsentField(state, 3, "key", "Runtime Receipt click");
                    auto const point = tablePoint(
                        state,
                        placement,
                        "action_point",
                        "Runtime Receipt action point"
                    );
                    if (
                        !measuredPlacement
                        || point.x() < placementRect.x()
                        || point.y() < placementRect.y()
                        || point.x() >= placementRect.right()
                        || point.y() >= placementRect.bottom()
                    )
                    {
                        raiseTierB(
                            state,
                            AutomationErrorKind::InvalidResource,
                            "Runtime Receipt placement is not the measured Binding placement"
                        );
                    }
                    return TrustedReceiptInput{point};
                }
                case ReceiptActionKind::Key:
                {
                    // A keystroke names no coordinate, so a placement carrying
                    // one here would be a point nothing authorized and nothing
                    // would ever notice it was ignored.
                    requireAbsentField(
                        state,
                        placement,
                        "action_point",
                        "Runtime Receipt key placement"
                    );
                    if (!measuredPlacement)
                    {
                        raiseTierB(
                            state,
                            AutomationErrorKind::InvalidResource,
                            "Runtime Receipt placement is not the measured Binding placement"
                        );
                    }
                    // The one definition of which key names exist. A model
                    // whose key is outside the set was already refused when the
                    // artifact was bound; this is the same call on the value
                    // that actually reached the Host.
                    auto key = KeyName::create(requireStringField(state, 3, "key"));
                    if (!key)
                    {
                        raiseFromError(state, &runtimeContext, key.error());
                    }
                    return TrustedReceiptInput{*key};
                }
                }

                UF_UNREACHABLE_MSG("Unknown ReceiptActionKind value");
            }();
            lua_pop(state, 1);

            auto minted = self.m_pHost->mintReceipt(
                self.m_generation,
                runtimeContext,
                *p_cycle,
                p_cycle->ordinal,
                TrustedReceiptIntent{
                    .stateIdentity = stateIdentity,
                    .surface       = surface,
                    .uiTarget      = uiTarget,
                    .binding       = binding,
                    .variant       = variant,
                    .action        = action,
                    .proofLocator  = proofLocator,
                    .input         = std::move(input),
                }
            );
            if (!minted)
            {
                raiseFromError(state, &runtimeContext, minted.error());
            }
            pushBox(state, *minted, k_receiptType);
            return 1;
        }

        static auto installFunction(
            lua_State* state,
            int surface,
            int capability,
            char const* name,
            lua_CFunction function
        ) -> void
        {
            lua_pushvalue(state, capability);
            lua_pushcclosure(state, function, name, 1);
            lua_setfield(state, surface, name);
        }

    public:
        [[nodiscard]]
        static auto install(
            lua_State* state,
            TaskHost& host,
            GenerationId generation
        ) -> Status
        {
            UF_TRY(beginOpaqueMetatable(state, k_bindingType, "RuntimeModelBinding"));
            UF_TRY(beginOpaqueMetatable(state, k_assetType, "RuntimeAsset"));
            UF_TRY(beginOpaqueMetatable(state, k_cycleType, "RuntimeCycle"));
            UF_TRY(beginOpaqueMetatable(state, k_proofType, "RuntimeEvidence"));
            UF_TRY(beginOpaqueMetatable(state, k_receiptType, "HostReceipt"));

            void* const storage = lua_newuserdatadtor(
                state,
                sizeof(RuntimeNativeState),
                &destroyBox<RuntimeNativeState>
            );
            std::construct_at(
                static_cast<RuntimeNativeState*>(storage),
                host,
                generation
            );
            int const capability = lua_gettop(state);

            lua_createtable(state, 0, 9);
            int const surface = lua_gettop(state);
            installFunction(
                state,
                surface,
                capability,
                "runtime_model_bytes",
                &modelBytes
            );
            installFunction(
                state,
                surface,
                capability,
                "runtime_semantic_hash",
                &semanticHash
            );
            installFunction(
                state,
                surface,
                capability,
                "runtime_model_finalize",
                &finalizeModel
            );
            installFunction(state, surface, capability, "runtime_asset", &asset);
            installFunction(
                state,
                surface,
                capability,
                "runtime_cycle_open",
                &openCycle
            );
            installFunction(state, surface, capability, "runtime_match", &match);
            installFunction(state, surface, capability, "runtime_read", &read);
            installFunction(state, surface, capability, "runtime_receipt", &receipt);
            installFunction(
                state,
                surface,
                capability,
                "runtime_cycle_close",
                &closeCycle
            );
            lua_remove(state, capability);
            UF_TRY(script::deepFreeze(state, -1));
            return ok();
        }
    };

    auto TaskHost::runtimePrivateCapabilities(
        GenerationId generation
    ) -> script::PrivateCapabilityInstaller
    {
        TaskHost* const p_host = this;
        return [p_host, generation](lua_State* state) -> Status
        {
            // Engine::create invokes this installer synchronously and the Lua
            // userdata it creates is destroyed before its owning Host generation.
            return RuntimeNativeState::install(state, *p_host, generation);
        };
    }

    auto scriptProjectGlobals() -> std::vector<std::string>
    {
        return {};
    }

    auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier
    {
        return [](lua_State* state, int index) -> std::optional<script::RaisedError>
        {
            return decodeTierB(state, index);
        };
    }

    auto scriptHostTableInstaller() -> script::HostTableInstaller
    {
        return [](lua_State* /*state*/) -> Status { return ok(); };
    }

    auto annotationPrivateCapabilities(TaskContext& context)
        -> script::PrivateCapabilityInstaller
    {
        TaskContext* const p_context = &context;
        return [p_context](lua_State* state) -> Status
        {
            return buildAnnotationSurface(state, p_context);
        };
    }
}
