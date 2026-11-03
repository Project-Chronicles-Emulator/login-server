#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {
class LocalPacketPlan;

using Bytes = std::vector<std::uint8_t>;
Bytes read_file(const std::filesystem::path &path);

// Shared identity used only for bounded local metadata accounting.
struct LocalFrameAllocation {
  std::size_t bytes{};
  std::shared_ptr<void> construction;
};

struct Frame {
  Bytes bytes;
  // Optional shared payload; when set, `shared` is the wire bytes and
  // `bytes` stays empty. Lets broadcast paths encode once per (opcode,
  // message) instead of once per recipient. opcode() reads through it.
  std::shared_ptr<const Bytes> shared;
  // Local-only preparation, run on the recipient's I/O executor before queue insertion.
  // Return false to discard a stale frame; throwing closes the connection.
  std::function<bool(Frame &)> prepare_for_enqueue;
  // A prepared local-only marker inserts this bounded batch instead of a wire frame.
  std::shared_ptr<std::vector<Frame>> prepared_batch;
  // Temporary unbound metadata; native binds it to one recipient before posting the frame.
  std::shared_ptr<const LocalPacketPlan> local_plan;
  std::shared_ptr<const nlohmann::json> local_message;
  // Estimated bytes retained by local metadata before its enqueue callback runs.
  std::size_t local_retained_bytes{};
  std::shared_ptr<const LocalFrameAllocation> local_allocation;
  // Transient ordering guard and optional readiness marker, released after queue insertion.
  std::shared_ptr<void> enqueue_scope;
  std::function<void()> after_enqueue;
  std::uint16_t opcode() const;
};

// Application frames only. TLS, native socket envelopes and ciphers are outside this parser.
class FrameStream {
public:
  void feed(std::span<const std::uint8_t> bytes, const std::function<void(Frame)> &consume);

  std::size_t pending() const {
    return buffer_.size();
  }

private:
  Bytes buffer_;
};

struct Record {
  std::size_t offset;
  std::uint32_t elapsed_ms;
  Frame frame;
};

struct LogInfo {
  std::string build;
  std::size_t records{};
  std::size_t complete_end{57};
  std::size_t tail_bytes{};
};

LogInfo read_log(std::span<const std::uint8_t> data,
                 const std::function<void(const Record &)> &consume);

class Protocol {
public:
  explicit Protocol(const std::filesystem::path &directory);

  nlohmann::json decode(const Frame &frame);

  Frame encode(std::uint16_t opcode, const nlohmann::json &message,
               std::optional<std::uint32_t> sequence = std::nullopt);

  std::size_t opcode_count() const {
    return opcodes_.size();
  }

private:
  google::protobuf::DescriptorPool pool_;
  google::protobuf::DynamicMessageFactory factory_{&pool_};
  std::unordered_map<std::uint16_t, nlohmann::json> opcodes_;
};
} // namespace chronicles
