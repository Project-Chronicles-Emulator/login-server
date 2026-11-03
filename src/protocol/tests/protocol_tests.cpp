// Protocol framing, codec, generated-schema and protobuf-JSON tests (Framing, Protocol,
// GeneratedSchema, ProtoJson suites). Moved verbatim from the former aggregated
// src/testing/tests.cpp (phase 1b-2 Task 6); auth/auth.hpp provides parse_json and
// message_json plus the generated message headers these suites use.
#include "auth/auth.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

using namespace chronicles;

namespace {

constexpr std::size_t kLogHeaderSize = 57;
constexpr std::size_t kBuildHashSize = 40;

Bytes make_log_header() {
  Bytes log_bytes(kLogHeaderSize);
  std::fill_n(log_bytes.begin(), kBuildHashSize, 'a');
  return log_bytes;
}

Frame make_client_frame(unsigned opcode = 20001) {
  // PC2S header: total length, opcode, and sequence number 123, all little-endian.
  return {{8, 0, static_cast<std::uint8_t>(opcode & 255), static_cast<std::uint8_t>(opcode >> 8),
           123, 0, 0, 0}};
}

TEST(Framing, FragmentedAndCoalesced) {
  auto frame = make_client_frame();
  FrameStream stream;
  std::vector<Frame> frames;
  Bytes combined = frame.bytes;
  combined.insert(combined.end(), frame.bytes.begin(), frame.bytes.end());

  for (auto byte : combined) {
    stream.feed(std::span(&byte, 1),
                [&](Frame received_frame) { frames.push_back(std::move(received_frame)); });
  }

  ASSERT_EQ(frames.size(), 2);
  EXPECT_EQ(frames[0].bytes, frame.bytes);
  EXPECT_EQ(stream.pending(), 0);

  FrameStream coalesced_stream;
  coalesced_stream.feed(combined,
                        [&](Frame received_frame) { frames.push_back(std::move(received_frame)); });
  EXPECT_EQ(frames.size(), 4);
}

TEST(Framing, RejectsMalformedAndRetainsPartial) {
  FrameStream stream;
  Bytes invalid{3, 0};
  EXPECT_THROW(stream.feed(invalid, [](Frame) {}), std::runtime_error);

  FrameStream partial_stream;
  Bytes bytes{8, 0, 1, 2, 3};
  partial_stream.feed(bytes, [](Frame) { FAIL(); });
  EXPECT_EQ(partial_stream.pending(), 5);

  auto log_bytes = make_log_header();
  log_bytes.insert(log_bytes.end(), {1, 0, 0, 0, 8, 0, 33});
  auto log = read_log(log_bytes, [](auto &) { FAIL(); });
  EXPECT_EQ(log.tail_bytes, 7);

  log_bytes = make_log_header();
  log_bytes.insert(log_bytes.end(), {1, 0, 0, 0, 3, 0, 0, 0});
  EXPECT_THROW(read_log(log_bytes, [](auto &) {}), std::runtime_error);

  log_bytes = make_log_header();
  log_bytes[40] = 'x';
  EXPECT_THROW(read_log(log_bytes, [](auto &) {}), std::runtime_error);
}

TEST(Protocol, SequenceAndUnknownFields) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  auto decoded = protocol.decode(make_client_frame());
  EXPECT_EQ(decoded.at("sequence"), 123);
  EXPECT_EQ(decoded.at("status"), "decoded");
  EXPECT_EQ(decoded.at("protobuf_bytes"), 0);
  EXPECT_EQ(protocol.decode(Frame{{4, 0, 255, 255}}).at("status"), "unknown_opcode");

  auto unknown_fields_frame = make_client_frame();
  unknown_fields_frame.bytes.insert(unknown_fields_frame.bytes.end(), {0xf8, 0x7f, 1});
  unknown_fields_frame.bytes[0] = 11;
  EXPECT_EQ(protocol.decode(unknown_fields_frame).at("status"), "unknown_fields");

  auto malformed_frame = make_client_frame();
  malformed_frame.bytes.push_back(0x80);
  malformed_frame.bytes[0] = 9;
  EXPECT_EQ(protocol.decode(malformed_frame).at("status"), "decode_failed");
  EXPECT_THROW(protocol.decode(Frame{{4, 0, 33, 78}}), std::runtime_error);
}

TEST(Protocol, EncodesDirectionalHeadersAndRejectsUnknownInputs) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  auto client = protocol.encode(20001, nlohmann::json::object(), 123);
  EXPECT_EQ(client.bytes, make_client_frame().bytes);

  auto server = protocol.encode(1, nlohmann::json::object());
  EXPECT_EQ(server.bytes, (Bytes{4, 0, 1, 0}));
  EXPECT_EQ(protocol.decode(server).at("status"), "decoded");

  EXPECT_THROW(protocol.encode(20001, nlohmann::json::object()), std::invalid_argument);
  EXPECT_THROW(protocol.encode(1, nlohmann::json::object(), 1), std::invalid_argument);
  EXPECT_THROW(protocol.encode(65535, nlohmann::json::object()), std::invalid_argument);
  EXPECT_THROW(protocol.encode(1, {{"unknown_field", 1}}), std::invalid_argument);
}

TEST(ProtoJson, RejectUnknownFieldsAndRoundTrip) {

  x5::pb::ws::front::CharSelectReq request;
  parse_json({{"char_id", "9223372036854775806"}}, request);
  EXPECT_EQ(request.char_id(), 9223372036854775806LL);
  EXPECT_EQ(message_json(request).at("char_id"), "9223372036854775806");
  EXPECT_THROW(parse_json({{"unknown", 1}}, request), std::invalid_argument);
}

TEST(GeneratedSchema, PreservesCollidingEnumValueAndDescriptorName) {
  EXPECT_EQ(static_cast<int>(x5::cms::EX5RecipeSortType_Name), 2);
  EXPECT_EQ(x5::cms::EX5RecipeSortType_ValueName(x5::cms::EX5RecipeSortType_Name),
            "EX5RecipeSortType_Name");
}

} // namespace
