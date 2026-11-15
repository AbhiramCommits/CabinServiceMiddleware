#include <gtest/gtest.h>

#include <string>

#include "cabin/log.hpp"
#include "cabin/trace.hpp"

using namespace cabin;

TEST(Trace, GeneratesDistinctNonZeroIds) {
  const TraceId a = new_trace_id();
  const TraceId b = new_trace_id();
  EXPECT_NE(a, 0u);
  EXPECT_NE(b, 0u);
  EXPECT_NE(a, b);
  EXPECT_EQ(a >> 63, 1u) << "local ids are tagged in the high bit";
}

TEST(Trace, HexRoundTrip) {
  const TraceId id = 0x8000000000002A7Fu;
  const std::string text = trace_id_hex(id);
  EXPECT_EQ(text, "8000000000002a7f");
  TraceId parsed = 0;
  ASSERT_TRUE(parse_trace_id(text, &parsed));
  EXPECT_EQ(parsed, id);
  EXPECT_FALSE(parse_trace_id("not-hex", &parsed));
}

TEST(Trace, ThreadLocalTracePropagates) {
  clear_current_trace_id();
  EXPECT_EQ(current_trace_id(), 0u);
  const TraceId id = new_trace_id();
  set_current_trace_id(id);
  EXPECT_EQ(current_trace_id(), id);

  ScopedSpan span("unit", "propagate");
  EXPECT_EQ(span.trace_id(), id);
  clear_current_trace_id();
  EXPECT_EQ(current_trace_id(), 0u);
}

TEST(Trace, ScopedSpanEmitsBeginAndEndWithDuration) {
  std::string sink;
  Logger::instance().reset_for_testing();
  Logger::instance().set_sink_for_testing(&sink);
  Logger::instance().set_level(Level::kTrace);

  {
    ScopedSpan span("hvac", "ApplyProfile");
    EXPECT_FALSE(span.id().empty());
  }

  Logger::instance().reset_for_testing();
  const auto begin = sink.find("span_begin");
  const auto end = sink.find("span_end");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  EXPECT_LT(begin, end);
  EXPECT_NE(sink.find("\"duration_us\":"), std::string::npos);
  EXPECT_NE(sink.find("\"service\":\"hvac\""), std::string::npos);
}

TEST(Trace, NestedSpansRestoreParent) {
  std::string sink;
  Logger::instance().reset_for_testing();
  Logger::instance().set_sink_for_testing(&sink);
  Logger::instance().set_level(Level::kTrace);

  {
    ScopedSpan outer("unit", "outer");
    const std::string outer_id(outer.id());
    {
      ScopedSpan inner("unit", "inner");
      EXPECT_NE(inner.id(), outer_id);
    }
    EXPECT_EQ(std::string(current_span_id()), outer_id);
  }
  EXPECT_TRUE(current_span_id().empty());

  Logger::instance().reset_for_testing();
}
