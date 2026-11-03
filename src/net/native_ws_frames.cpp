#include "net/native_ws_frames.hpp"

#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

std::uint16_t read_u16(std::string_view bytes, std::size_t offset) {
  const auto low = static_cast<unsigned char>(bytes[offset]);
  const auto high = static_cast<unsigned char>(bytes[offset + 1]);
  return static_cast<std::uint16_t>(low | (high << 8));
}

void append_u16(std::string &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value & 0xff));
  bytes.push_back(static_cast<char>(value >> 8));
}

} // namespace

std::vector<native_ws::FrameView> native_ws::decode_frames(std::string_view message) {
  if (message.empty() || message.size() > kMaximumMessageBytes) {
    throw std::invalid_argument("Native WebSocket message size is outside the accepted range");
  }

  std::vector<FrameView> frames;
  std::size_t offset = 0;
  while (offset < message.size()) {
    if (frames.size() == kMaximumPacketsPerMessage) {
      throw std::invalid_argument("Native WebSocket message contains too many packets");
    }

    const auto remaining_bytes = message.size() - offset;
    if (remaining_bytes < kHeaderBytes) {
      throw std::invalid_argument("Truncated native WebSocket packet header");
    }

    const auto frame_bytes = read_u16(message, offset);
    if (frame_bytes < kHeaderBytes) {
      throw std::invalid_argument("Native WebSocket packet length is smaller than its header");
    }
    if (frame_bytes > remaining_bytes) {
      throw std::invalid_argument("Truncated native WebSocket packet body");
    }

    const auto packet_id = read_u16(message, offset + sizeof(std::uint16_t));
    const auto payload = message.substr(offset + kHeaderBytes, frame_bytes - kHeaderBytes);
    frames.push_back(FrameView{packet_id, payload});
    offset += frame_bytes;
  }

  return frames;
}

std::string native_ws::encode_frame(std::uint16_t packet_id,
                                    const google::protobuf::Message &message) {
  const auto payload_bytes = message.ByteSizeLong();
  constexpr auto maximum_frame_bytes = std::numeric_limits<std::uint16_t>::max();
  if (payload_bytes > maximum_frame_bytes - kHeaderBytes) {
    throw std::length_error("Native WebSocket protobuf payload exceeds the 16-bit frame length");
  }

  std::string payload;
  if (!message.SerializeToString(&payload)) {
    throw std::runtime_error("Cannot serialize native WebSocket protobuf payload");
  }

  const auto frame_bytes = static_cast<std::uint16_t>(kHeaderBytes + payload.size());
  std::string frame;
  frame.reserve(frame_bytes);
  append_u16(frame, frame_bytes);
  append_u16(frame, packet_id);
  frame.append(payload);
  return frame;
}

} // namespace chronicles
