#include "protocol/protocol.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>

namespace chronicles {
namespace {

constexpr std::size_t kFrameLengthSize       = 2;
constexpr std::size_t kApplicationHeaderSize = 4;
constexpr std::size_t kClientHeaderSize      = 8;
constexpr std::size_t kBuildHashSize         = 40;
constexpr std::size_t kLogHeaderSize         = 57;
constexpr std::size_t kRecordTimestampSize   = 4;
constexpr std::size_t kMinimumRecordSize     = kRecordTimestampSize + kApplicationHeaderSize;
constexpr unsigned    kMaximumOpcode         = 65535;
constexpr auto        kMaximumInputBytes     = 1024LL * 1024 * 1024;

// Native log fields and application frame headers use little-endian byte order.
std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] |
                                    (static_cast<unsigned>(bytes[offset + 1]) << 8));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(read_u16(bytes, offset)) |
         (static_cast<std::uint32_t>(read_u16(bytes, offset + 2)) << 16);
}

bool has_unknown_fields(const google::protobuf::Message &message) {
  auto *reflection = message.GetReflection();
  if (reflection->GetUnknownFields(message).field_count()) {
    return true;
  }

  std::vector<const google::protobuf::FieldDescriptor *> fields;
  reflection->ListFields(message, &fields);

  for (auto *field : fields) {
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }

    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
        if (has_unknown_fields(reflection->GetRepeatedMessage(message, field, i))) {
          return true;
        }
      }
    } else if (has_unknown_fields(reflection->GetMessage(message, field))) {
      return true;
    }
  }

  return false;
}

void build_descriptor_pool(const google::protobuf::FileDescriptorSet &descriptors,
                           google::protobuf::DescriptorPool &         pool) {
  std::vector<const google::protobuf::FileDescriptorProto *> pending_files;
  for (const auto &file : descriptors.file()) {
    pending_files.push_back(&file);
  }

  // A file can be built only after all of its dependencies are in the pool.
  while (!pending_files.empty()) {
    const auto pending_before = pending_files.size();

    std::erase_if(pending_files, [&](auto *file) {
      for (const auto &dependency : file->dependency()) {
        if (!pool.FindFileByName(dependency)) {
          return false;
        }
      }

      if (!pool.BuildFile(*file)) {
        throw std::runtime_error("Descriptor build failed: " + file->name());
      }

      return true;
    });

    if (pending_before == pending_files.size()) {
      throw std::runtime_error("Unresolved descriptor dependencies");
    }
  }
}

std::string resolve_schema_name(const google::protobuf::FileDescriptorSet &descriptors,
                                const nlohmann::json &opcode_entry) {
  std::vector<std::string> directional_candidates;
  std::vector<std::string> shared_candidates;
  const auto direction_prefix = opcode_entry.at("family") == "PC2S" ? "p_c2s" : "p_s2c";

  for (const auto &file : descriptors.file()) {
    for (const auto &message : file.message_type()) {
      if (opcode_entry.at("message_basename") != message.name()) {
        continue;
      }

      const auto schema_name = file.package() + "." + message.name();
      if (file.name().starts_with(direction_prefix)) {
        directional_candidates.push_back(schema_name);
      }

      if (schema_name.starts_with("x5.pb.")) {
        shared_candidates.push_back(schema_name);
      }
    }
  }

  // Prefer direction-specific matches; use shared schemas only when none exist.
  // An ambiguous match stays unresolved rather than choosing an arbitrary schema.
  const auto &candidates =
      directional_candidates.empty() ? shared_candidates : directional_candidates;
  return candidates.size() == 1 ? candidates.front() : "";
}

void verify_message_round_trip(const google::protobuf::Message &message) {
  std::unique_ptr<google::protobuf::Message> round_trip(message.New());
  if (!round_trip->ParseFromString(message.SerializeAsString()) ||
      !google::protobuf::util::MessageDifferencer::Equals(message, *round_trip)) {
    throw std::runtime_error("Protobuf round-trip failed");
  }
}

} // namespace

Bytes read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::runtime_error("Cannot open " + path.string());
  }

  const auto file_size = input.tellg();
  if (file_size < 0 || file_size > kMaximumInputBytes) {
    throw std::runtime_error("Invalid or oversized input");
  }

  Bytes bytes(file_size);
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()), file_size);
  if (!input && !bytes.empty()) {
    throw std::runtime_error("Input read failed");
  }

  return bytes;
}

std::uint16_t Frame::opcode() const {
  const Bytes &wire = shared ? *shared : bytes;
  if (wire.size() < kApplicationHeaderSize) {
    throw std::runtime_error("Short application header");
  }

  return read_u16(wire, kFrameLengthSize);
}

void FrameStream::feed(std::span<const std::uint8_t>     bytes,
                       const std::function<void(Frame)> &consume) {
  // Append at most one frame at a time so an untrusted chunk cannot grow buffering without bound.
  for (auto byte : bytes) {
    buffer_.push_back(byte);
    if (buffer_.size() < kFrameLengthSize) {
      continue;
    }

    const auto frame_length = read_u16(buffer_, 0);
    if (frame_length < kApplicationHeaderSize) {
      throw std::runtime_error("Application frame length is less than header");
    }

    if (buffer_.size() == frame_length) {
      Frame frame{std::move(buffer_)};
      buffer_.clear();
      consume(std::move(frame));
    }
  }
}

LogInfo read_log(std::span<const std::uint8_t>              bytes,
                 const std::function<void(const Record &)> &consume) {
  if (bytes.size() < kLogHeaderSize || bytes[kBuildHashSize] != 0 ||
      !std::all_of(bytes.begin(), bytes.begin() + kBuildHashSize, [](auto character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
      })) {
    throw std::runtime_error("Unexpected native log header");
  }

  LogInfo     info{std::string(reinterpret_cast<const char *>(bytes.data()), kBuildHashSize)};
  std::size_t position = kLogHeaderSize;

  while (bytes.size() - position >= kMinimumRecordSize) {
    const auto frame_length = read_u16(bytes, position + kRecordTimestampSize);
    if (frame_length < kApplicationHeaderSize) {
      throw std::runtime_error("Invalid native record length");
    }

    const auto record_end = position + kRecordTimestampSize + frame_length;
    if (record_end > bytes.size()) {
      break;
    }

    consume({position, read_u32(bytes, position),
             Frame{Bytes(bytes.begin() + position + kRecordTimestampSize,
                         bytes.begin() + record_end)}});
    ++info.records;
    position = record_end;
  }

  info.complete_end = position;
  info.tail_bytes   = bytes.size() - position;
  return info;
}

Protocol::Protocol(const std::filesystem::path &directory) {
  const auto descriptor_bytes = read_file(directory / "descriptors.normalized.pb");
  google::protobuf::FileDescriptorSet descriptors;
  if (!descriptors.ParseFromArray(descriptor_bytes.data(),
                                  static_cast<int>(descriptor_bytes.size()))) {
    throw std::runtime_error("Invalid descriptors");
  }

  build_descriptor_pool(descriptors, pool_);

  std::ifstream  input(directory / "opcodes.json");
  nlohmann::json opcode_map;
  input >> opcode_map;

  for (auto opcode_entry : opcode_map.at("entries")) {
    const auto opcode = opcode_entry.at("opcode").get<unsigned>();
    if (opcode > kMaximumOpcode || opcodes_.contains(static_cast<std::uint16_t>(opcode))) {
      throw std::runtime_error("Opcode collision/range");
    }

    opcode_entry["resolved_schema"] = resolve_schema_name(descriptors, opcode_entry);
    opcodes_.emplace(static_cast<std::uint16_t>(opcode), std::move(opcode_entry));
  }
}

nlohmann::json Protocol::decode(const Frame &frame) {
  if (frame.bytes.size() < kApplicationHeaderSize ||
      read_u16(frame.bytes, 0) != frame.bytes.size()) {
    throw std::runtime_error("Frame length mismatch");
  }

  const auto     opcode = frame.opcode();
  nlohmann::json result{{"opcode", opcode}, {"packet_bytes", frame.bytes.size()}};
  const auto     opcode_iterator = opcodes_.find(opcode);
  if (opcode_iterator == opcodes_.end()) {
    result["status"] = "unknown_opcode";
    return result;
  }

  const auto &opcode_entry = opcode_iterator->second;
  result["packet"]         = opcode_entry.at("packet");
  result["family"]         = opcode_entry.at("family");

  std::size_t payload_offset = kApplicationHeaderSize;
  if (opcode_entry.at("family") == "PC2S") {
    if (frame.bytes.size() < kClientHeaderSize) {
      throw std::runtime_error("C2S frame lacks sequence");
    }

    result["sequence"] = read_u32(frame.bytes, kApplicationHeaderSize);
    payload_offset     = kClientHeaderSize;
  }

  result["protobuf_bytes"] = frame.bytes.size() - payload_offset;
  const auto schema_name   = opcode_entry.at("resolved_schema").get<std::string>();
  if (schema_name.empty()) {
    result["status"] = "unresolved_schema";
    return result;
  }

  auto *descriptor = pool_.FindMessageTypeByName(schema_name);
  result["schema"] = schema_name;
  std::unique_ptr<google::protobuf::Message> message(factory_.GetPrototype(descriptor)->New());
  if (!message->ParseFromArray(frame.bytes.data() + payload_offset,
                               static_cast<int>(frame.bytes.size() - payload_offset))) {
    result["status"] = "decode_failed";
    return result;
  }

  result["status"] = has_unknown_fields(*message) ? "unknown_fields" : "decoded";
  verify_message_round_trip(*message);

  std::string message_json;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  const auto status = google::protobuf::util::MessageToJsonString(*message, &message_json, options);
  if (!status.ok()) {
    throw std::runtime_error(status.ToString());
  }

  result["message"] = nlohmann::json::parse(message_json);
  return result;
}

Frame Protocol::encode(std::uint16_t                opcode, const nlohmann::json &json,
                       std::optional<std::uint32_t> sequence) {
  const auto entry = opcodes_.find(opcode);
  if (entry == opcodes_.end()) {
    throw std::invalid_argument("Cannot encode unknown opcode");
  }
  const bool client = entry->second.at("family") == "PC2S";
  if (client != sequence.has_value()) {
    throw std::invalid_argument("C2S requires sequence; S2C has no sequence");
  }
  const auto name = entry->second.at("resolved_schema").get<std::string>();
  if (name.empty()) {
    throw std::invalid_argument("Cannot encode unresolved schema");
  }
  auto *descriptor = pool_.FindMessageTypeByName(name);
  std::unique_ptr<google::protobuf::Message> message(factory_.GetPrototype(descriptor)->New());
  const auto status = google::protobuf::util::JsonStringToMessage(json.dump(), message.get());
  if (!status.ok()) {
    throw std::invalid_argument("Invalid protobuf JSON for opcode");
  }
  const auto body   = message->SerializeAsString();
  const auto length = (client ? kClientHeaderSize : kApplicationHeaderSize) + body.size();
  if (length > kMaximumOpcode) {
    throw std::invalid_argument("Encoded application frame exceeds uint16 length");
  }
  Frame result;
  // Header (2 length + 2 opcode [+ 4 sequence]) then the serialized body in
  // one go; reserve avoids the growth reallocations of per-byte appends.
  result.bytes.reserve(length);
  auto append = [&](std::uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
      result.bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    }
  };
  append(static_cast<std::uint32_t>(length), 2);
  append(opcode, 2);
  if (client) {
    append(*sequence, 4);
  }
  result.bytes.insert(result.bytes.end(), body.begin(), body.end());
  return result;
}

} // namespace chronicles