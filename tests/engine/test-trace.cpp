#include <engine/trace.hpp>

#include <annotation/resource.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace uf::engine
{
    namespace
    {
        [[nodiscard]]
        auto resourceId(std::string_view value) -> annotation::ResourceId
        {
            auto const parsed = annotation::ResourceId::parse(value);
            REQUIRE(parsed.has_value());
            return *parsed;
        }

        [[nodiscard]]
        auto pixelRect(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto const rect = PixelRect::create(x, y, width, height);
            REQUIRE(rect.has_value());
            return *rect;
        }
    }

    TEST_CASE("serializeTraceEvent emits every populated field in schema order")
    {
        auto event = TraceEvent{
            .kind             = TraceEventKind::ActionFound,
            .frameId          = FrameId{uint64{42}},
            .sessionId        = CaptureSessionId{uint64{7}},
            .targetGeneration = TargetGeneration::fromValue(3),
            .pageId           = annotation::PageId{
                resourceId("11111111-2222-3333-4444-555555555555")
            },
            .recognizerId = annotation::RecognizerId{
                resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
            },
            .sadScore    = uint64{1234},
            .maximumSad  = uint64{5000},
            .matchedRect = pixelRect(10, 20, 30, 40),
            .stopReason  = SadSearchStopReason::TimedOut,
            .errorKind   = AutomationErrorKind::RecognitionFailed,
            .message     = std::string{"hello"},
            .clickClient = Point<ClientSpace>{128.0F, 64.0F},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"engine-trace/v1\",\"kind\":\"ActionFound\""
            ",\"frameId\":42,\"sessionId\":7,\"targetGeneration\":3"
            ",\"pageId\":\"11111111-2222-3333-4444-555555555555\""
            ",\"recognizerId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\""
            ",\"sadScore\":1234,\"maximumSad\":5000"
            ",\"matchedRect\":{\"x\":10,\"y\":20,\"width\":30,\"height\":40}"
            ",\"stopReason\":\"TimedOut\",\"errorKind\":\"RecognitionFailed\""
            ",\"message\":\"hello\",\"clickClientX\":128,\"clickClientY\":64}"
        };

        CHECK(serializeTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTraceEvent emits only schema and kind for a minimal event")
    {
        auto const event = TraceEvent{.kind = TraceEventKind::SessionStarted};

        auto constexpr expected = std::string_view{
            "{\"schema\":\"engine-trace/v1\",\"kind\":\"SessionStarted\"}"
        };

        CHECK(serializeTraceEvent(event) == expected);
    }

    TEST_CASE("serializeTraceEvent escapes quotes, backslashes, and control bytes")
    {
        auto message = std::string{"a\"b\\c\n"};
        message.push_back(static_cast<char>(0x01));

        auto const event = TraceEvent{
            .kind    = TraceEventKind::Failure,
            .message = message,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"engine-trace/v1\",\"kind\":\"Failure\""
            ",\"message\":\"a\\\"b\\\\c\\n\\u0001\"}"
        };

        CHECK(serializeTraceEvent(event) == expected);
    }
}
