// Encode-once fan-out proof: a shared payload must be byte-identical to a
// fresh per-recipient encode for representative broadcast opcodes, and the
// shared path must encode once regardless of recipient count.
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace chronicles {
namespace {

// Representative broadcast payloads: PS2C_MoveNoti (177) and PS2C_UnitAddNoti
// (10). PS2C frames carry no sequence, matching the server encode path.
nlohmann::json move_message() {
  // Wire shape mirrors PS2C_MoveNoti produced by presence.move: 64-bit ids and
  // ticks serialize as strings, enums by name, WorldPos in millimetres.
  return {
      {"unit_id", "500001"},
      {"tick", "1234567"},
      {"mode", "MoveMode_Normal"},
      {"detail", "MoveDetail_Run"},
      {"pos",
       {{"millimeter_x", 120000}, {"millimeter_y", 340000}, {"millimeter_z", 0}}},
      {"body_yaw", 90.0},
      {"pred_pos",
       {{"millimeter_x", 120050}, {"millimeter_y", 340050}, {"millimeter_z", 0}}},
  };
}

TEST(EncodeOnce, SharedPayloadMatchesFreshEncodeMoveNoti) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto message = move_message();
  const auto baseline = protocol.encode(177, message);
  const auto shared = std::make_shared<const Bytes>(baseline.bytes);
  for (int recipient = 0; recipient < 250; ++recipient) {
    const auto fresh = protocol.encode(177, message);
    EXPECT_EQ(*shared, fresh.bytes) << "recipient " << recipient;
  }
}

TEST(EncodeOnce, SharedFrameOpcodeMatchesBufferOpcode) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto encoded = protocol.encode(177, move_message());
  Frame frame;
  frame.shared = std::make_shared<const Bytes>(encoded.bytes);
  EXPECT_EQ(frame.opcode(), 177);
  EXPECT_EQ(frame.opcode(), encoded.opcode());
}

// Gated microbenchmark (set CHRONICLES_BENCH=1). Reports encode calls and a rough
// allocation proxy for fan-outs of 10/50/250 recipients; the shared path
// encodes once no matter the audience size.
TEST(EncodeOnce, FanoutEncodesOnceForAnyAudience) {
  if (!std::getenv("CHRONICLES_BENCH")) {
    GTEST_SKIP() << "set CHRONICLES_BENCH=1 to run the fan-out microbenchmark";
  }
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto message = move_message();
  for (const std::size_t recipients : {10u, 50u, 250u}) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<Bytes> naive;
    naive.reserve(recipients);
    for (std::size_t i = 0; i < recipients; ++i) {
      naive.push_back(protocol.encode(177, message).bytes);
    }
    const auto naive_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
    const auto start_shared = std::chrono::steady_clock::now();
    auto frame = protocol.encode(177, message);
    const auto shared = std::make_shared<const Bytes>(std::move(frame.bytes));
    std::vector<Frame> deliveries;
    deliveries.reserve(recipients);
    for (std::size_t i = 0; i < recipients; ++i) {
      Frame out;
      out.shared = shared;
      deliveries.push_back(std::move(out));
    }
    const auto shared_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start_shared)
                               .count();
    std::printf("fanout=%zu naive: %zu encodes %.3f ms | shared: 1 encode %.3f ms\n",
                recipients, recipients, naive_ms, shared_ms);
    // Byte identity across every shared delivery is the wire contract.
    for (const auto &out : deliveries) {
      EXPECT_EQ(*out.shared, naive[0]);
    }
  }
}

} // namespace

namespace {

// JSON body exactly as WorldPresence::move emits it for one mover (subject-
// derived only: same value reaches every observer).
nlohmann::json presence_move_body() {
  return {{"unit_id", "4500001"},
          {"tick", "7654321"},
          {"mode", "MoveMode_Normal"},
          {"detail", "MoveDetail_Run"},
          {"pos",
           {{"millimeter_x", 5000}, {"millimeter_y", -8000}, {"millimeter_z", 0}}},
          {"body_yaw", 12.5},
          {"pred_pos",
           {{"millimeter_x", 5100}, {"millimeter_y", -7900}, {"millimeter_z", 0}}}};
}

// UnitAdd body after subject-side decoration (party/guild/faction fields are
// resolved from the SUBJECT, identical for every viewer).
nlohmann::json presence_add_body() {
  return {{"unit_id", "4500002"},
          {"nub",
           {{"pos",
             {{"millimeter_x", 100}, {"millimeter_y", 200}, {"millimeter_z", 0}}},
            {"yaw", 0.0},
            {"player",
             {{"char_id", "4500002"},
              {"char_class", "EX5CharacterClass_Fight"},
              {"name", "Runner"},
              {"level", 12},
              {"state", "EX5UnitStateType_Idle"},
              {"party_id", "7"},
              {"guild_id", "3"},
              {"guild_name", "Project Chronicles"},
              {"faction_type", "EX5Faction_Player"}}}}},
          {"by_sight", true}};
}

nlohmann::json presence_remove_body() {
  return {{"unit_ids", nlohmann::json::array({"4500001"})}, {"by_sight", true}};
}

// Mirrors deliver_presence_events: a map keyed on the full JSON value reuses
// one EncodedEvent (one encode) per identical message across recipients.
struct EncodedEvent {
  std::shared_ptr<const Bytes> wire;
};

void queue_shared(Frame &out, const std::shared_ptr<const Bytes> &wire) {
  out.shared = wire;
}

TEST(EncodeOnce, PresenceFanoutSharesOneEncodePerIdenticalMessage) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const std::size_t recipients = 100;
  for (const auto &[opcode, body] : std::vector<std::pair<std::uint16_t, nlohmann::json>>{
           {177, presence_move_body()},
           {10, presence_add_body()},
           {11, presence_remove_body()},
       }) {
    std::map<nlohmann::json, EncodedEvent> encoded;
    const auto baseline = protocol.encode(opcode, body);
    std::vector<Frame> deliveries;
    for (std::size_t i = 0; i < recipients; ++i) {
      // Same event.message content arrives per recipient.
      const nlohmann::json event_message = body;
      auto &slot = encoded[event_message];
      if (!slot.wire) {
        auto frame = protocol.encode(opcode, event_message);
        slot.wire = std::make_shared<const Bytes>(std::move(frame.bytes));
      }
      Frame out;
      queue_shared(out, slot.wire);
      deliveries.push_back(std::move(out));
    }
    ASSERT_EQ(encoded.size(), 1u) << "opcode " << opcode;
    ASSERT_EQ(deliveries.size(), recipients);
    for (const auto &frame : deliveries) {
      ASSERT_TRUE(frame.shared);
      EXPECT_EQ(*frame.shared, baseline.bytes) << "opcode " << opcode;
      EXPECT_EQ(frame.opcode(), opcode);
      // All recipients share the exact same immutable payload object.
      EXPECT_EQ(frame.shared, deliveries.front().shared);
    }
  }
}

TEST(EncodeOnce, PresenceFanoutPreservesPerRecipientOrder) {
  // Deliveries must keep the input event order per recipient: this simulates
  // one mover's mixed event batch (add -> move -> remove) arriving ordered.
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const std::vector<std::pair<std::uint16_t, nlohmann::json>> events = {
      {10, presence_add_body()}, {177, presence_move_body()}, {11, presence_remove_body()}};
  std::map<nlohmann::json, EncodedEvent> encoded;
  std::vector<Frame> deliveries;
  for (const auto &[opcode, body] : events) {
    auto &slot = encoded[body];
    if (!slot.wire) {
      auto frame = protocol.encode(opcode, body);
      slot.wire = std::make_shared<const Bytes>(std::move(frame.bytes));
    }
    Frame out;
    queue_shared(out, slot.wire);
    deliveries.push_back(std::move(out));
  }
  ASSERT_EQ(deliveries.size(), events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(deliveries[i].opcode(), events[i].first);
    const auto fresh = protocol.encode(events[i].first, events[i].second);
    EXPECT_EQ(*deliveries[i].shared, fresh.bytes);
  }
}

TEST(EncodeOnce, DistinctSubjectsNeverSharePayload) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  auto first = presence_move_body();
  auto second = presence_move_body();
  second["unit_id"] = "4500009";
  std::map<nlohmann::json, EncodedEvent> encoded;
  for (const auto &body : {first, second}) {
    auto &slot = encoded[body];
    if (!slot.wire) {
      auto frame = protocol.encode(177, body);
      slot.wire = std::make_shared<const Bytes>(std::move(frame.bytes));
    }
  }
  ASSERT_EQ(encoded.size(), 2u);
  EXPECT_NE(encoded[first].wire, encoded[second].wire);
  EXPECT_NE(*encoded[first].wire, *encoded[second].wire);
}

} // namespace
} // namespace chronicles
