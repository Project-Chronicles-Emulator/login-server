#pragma once

#include <google/protobuf/message.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {
namespace native_ws {

// Each service owns a separate ID space. These constants come from native
// wrapper getters, recorded in protocol/native-ws-opcodes.json.
constexpr std::uint16_t kWorldEnterRequest = 20001;
constexpr std::uint16_t kWorldEnterResponse = 1;
constexpr std::uint16_t kCharacterListRequest = 20001;
constexpr std::uint16_t kCharacterListResponse = 6;
constexpr std::uint16_t kCharacterCreateRequest = 20002;
constexpr std::uint16_t kCharacterCreateResponse = 7;
constexpr std::uint16_t kCharacterDeletePreCheckRequest = 20003;
constexpr std::uint16_t kCharacterDeletePreCheckResponse = 8;
constexpr std::uint16_t kCharacterDeleteRequest = 20004;
constexpr std::uint16_t kCharacterDeleteResponse = 9;
constexpr std::uint16_t kCharacterDeleteCancelRequest = 20005;
constexpr std::uint16_t kCharacterDeleteCancelResponse = 10;
constexpr std::uint16_t kCharacterDeleteImmediateRequest = 20006;
constexpr std::uint16_t kCharacterDeleteImmediateResponse = 11;
constexpr std::uint16_t kCharacterSelectRequest = 20007;
constexpr std::uint16_t kCharacterSelectResponse = 12;
constexpr std::uint16_t kFriendListRequest = 20024;
constexpr std::uint16_t kFriendListResponse = 28;
constexpr std::uint16_t kChatRequest = 20008;
constexpr std::uint16_t kChatNotification = 13;
constexpr std::uint16_t kDirectMessageRequest = 20009;
constexpr std::uint16_t kDirectMessageResponse = 14;
constexpr std::uint16_t kDirectMessageNotification = 15;
constexpr std::uint16_t kLookupCharIdByNameRequest = 20010;
constexpr std::uint16_t kLookupCharIdByNameResponse = 16;

constexpr std::size_t kHeaderBytes = 4;
constexpr std::size_t kMaximumMessageBytes = 1024 * 1024;
constexpr std::size_t kMaximumPacketsPerMessage = 256;

struct FrameView {
  std::uint16_t packet_id;
  std::string_view payload;
};

// Views refer to the caller's buffer. A WebSocket message contains complete,
// concatenated native packets; Beast assembles WebSocket fragments first.
std::vector<FrameView> decode_frames(std::string_view message);

std::string encode_frame(std::uint16_t packet_id, const google::protobuf::Message &message);

// Parses one packet payload as `Request`; throws std::invalid_argument naming the message
// type when the bytes are not a valid protobuf encoding.
template <typename Request> Request parse_payload(std::string_view payload) {
  Request request;
  if (!request.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    throw std::invalid_argument("Malformed native WebSocket protobuf payload: " +
                                request.GetTypeName());
  }

  return request;
}

} // namespace native_ws
} // namespace chronicles
