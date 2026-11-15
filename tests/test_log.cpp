#include <gtest/gtest.h>

#include <string>

#include "cabin/bytes.hpp"
#include "cabin/log.hpp"
#include "cabin/result.hpp"
#include "cabin/trace.hpp"

using namespace cabin;

TEST(Result, HoldsValueOrError) {
  Result<int> good(42);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);
  EXPECT_EQ(good.code(), ErrorCode::kOk);

  Result<int> bad(make_error(ErrorCode::kOutOfRange, "too hot"));
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.code(), ErrorCode::kOutOfRange);
  EXPECT_EQ(bad.error().message, "too hot");
}

TEST(Bytes, RoundTripsLittleEndianScalars) {
  Bytes bytes;
  bytes.append_u8(0x12);
  bytes.append_u16(0xBEEF);
  bytes.append_u32(0xDEADBEEF);
  bytes.append_i32(-7);
  bytes.append_f32(21.5F);
  bytes.append_string("cabin");
  bytes.append_f64(3.25);

  auto u8 = bytes.read_u8();
  ASSERT_TRUE(u8.ok());
  EXPECT_EQ(u8.value(), 0x12);
  auto u16 = bytes.read_u16();
  ASSERT_TRUE(u16.ok());
  EXPECT_EQ(u16.value(), 0xBEEF);
  auto u32 = bytes.read_u32();
  ASSERT_TRUE(u32.ok());
  EXPECT_EQ(u32.value(), 0xDEADBEEFu);
  auto i32 = bytes.read_i32();
  ASSERT_TRUE(i32.ok());
  EXPECT_EQ(i32.value(), -7);
  auto f32 = bytes.read_f32();
  ASSERT_TRUE(f32.ok());
  EXPECT_FLOAT_EQ(f32.value(), 21.5F);
  auto text = bytes.read_string();
  ASSERT_TRUE(text.ok());
  EXPECT_EQ(text.value(), "cabin");
  auto f64 = bytes.read_f64();
  ASSERT_TRUE(f64.ok());
  EXPECT_DOUBLE_EQ(f64.value(), 3.25);
  EXPECT_EQ(bytes.remaining(), 0u);
}

TEST(Bytes, TruncatedReadsReturnErrors) {
  Bytes bytes;
  bytes.append_u8(1);
  auto value = bytes.read_u32();
  EXPECT_FALSE(value.ok());
  EXPECT_EQ(value.code(), ErrorCode::kTruncated);

  auto text = bytes.read_string();
  EXPECT_FALSE(text.ok());
  EXPECT_EQ(text.code(), ErrorCode::kTruncated);
}

TEST(Logger, EmitsOneJsonObjectPerLineWithRequiredKeys) {
  std::string sink;
  Logger::instance().reset_for_testing();
  Logger::instance().set_sink_for_testing(&sink);
  Logger::instance().set_level(Level::kInfo);

  log_info("unit", "profile_applied",
           {Field("driver_id", "driver-a"), Field("latency_us", 120),
            Field("note", "quote \" and \\ slash\nnewline")});

  Logger::instance().reset_for_testing();
  ASSERT_FALSE(sink.empty());
  EXPECT_EQ(sink.back(), '\n');
  EXPECT_EQ(sink.find('\n'), sink.size() - 1);
  for (const char* key :
       {"\"ts_us\":", "\"level\":\"INFO\"", "\"service\":\"unit\"",
        "\"trace_id\":", "\"span\":", "\"event\":\"profile_applied\"",
        "\"fields\":", "\"driver_id\":\"driver-a\"", "\"latency_us\":120"}) {
    EXPECT_NE(sink.find(key), std::string::npos) << key;
  }
  EXPECT_NE(sink.find("\\\" and \\\\ slash\\nnewline"), std::string::npos);
}

TEST(Logger, LevelFilterSuppressesLowerLevels) {
  std::string sink;
  Logger::instance().reset_for_testing();
  Logger::instance().set_sink_for_testing(&sink);
  Logger::instance().set_level(Level::kWarn);

  log_info("unit", "hidden");
  log_error("unit", "visible");

  Logger::instance().reset_for_testing();
  EXPECT_EQ(sink.find("hidden"), std::string::npos);
  EXPECT_NE(sink.find("visible"), std::string::npos);
  EXPECT_NE(sink.find("\"level\":\"ERROR\""), std::string::npos);
}

TEST(Logger, LevelNamesAreStable) {
  EXPECT_STREQ(level_name(Level::kTrace), "TRACE");
  EXPECT_STREQ(level_name(Level::kError), "ERROR");
  Level parsed{};
  ASSERT_TRUE(parse_level("warn", &parsed));
  EXPECT_EQ(parsed, Level::kWarn);
  EXPECT_FALSE(parse_level("verbose", &parsed));
}
