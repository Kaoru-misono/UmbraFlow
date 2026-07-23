#include <engine/trace.hpp>

#include <annotation/catalog.hpp>

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
            .m_kind             = TraceEventKind::ActionFound,
            .m_frameId          = FrameId{uint64{42}},
            .m_sessionId        = SessionId{uint64{7}},
            .m_targetGeneration = TargetGeneration::fromValue(3),
            .m_pageId           = annotation::PageId{
                resourceId("11111111-2222-3333-4444-555555555555")
            },
            .m_recognizerId = annotation::RecognizerId{
                resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
            },
            .m_sadScore    = uint64{1234},
            .m_maximumSad  = uint64{5000},
            .m_matchedRect = pixelRect(10, 20, 30, 40),
            .m_stopReason  = SadSearchStopReason::TimedOut,
            .m_errorKind   = AutomationErrorKind::RecognitionFailed,
            .m_message     = std::string{"hello"},
            .m_clickClient = Point<ClientSpace>{128.0F, 64.0F},
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
        auto const event = TraceEvent{.m_kind = TraceEventKind::SessionStarted};

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
            .m_kind    = TraceEventKind::Failure,
            .m_message = message,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"engine-trace/v1\",\"kind\":\"Failure\""
            ",\"message\":\"a\\\"b\\\\c\\n\\u0001\"}"
        };

        CHECK(serializeTraceEvent(event) == expected);
    }
}
