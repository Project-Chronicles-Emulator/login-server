#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>

namespace chronicles {

// Local-only preparation metadata. It never changes the protocol payload format; the
// recipient binds and evaluates it on its I/O executor immediately before enqueueing.
enum class LocalPacketDecision { keep, replace, drop };

class LocalPacketPlan {
public:
  virtual ~LocalPacketPlan() = default;
  virtual int expected_map() const noexcept = 0;
  virtual std::uint64_t expected_channel() const noexcept = 0;
  virtual std::size_t retained_bytes() const noexcept = 0;
  // Holds the source publication ordering domain through actual I/O queue insertion.
  virtual std::shared_ptr<void> begin_ordered_delivery() const = 0;
  virtual std::optional<std::uint32_t> initial_sector() const noexcept {
    return std::nullopt;
  }
  virtual std::optional<std::uint32_t> source_sector() const noexcept {
    return std::nullopt;
  }
  virtual LocalPacketDecision prepare(nlohmann::json &message) const = 0;
};

// One server packet produced by a gameplay service, encoded later by the session's codec.
struct GameplayPacket {
  std::uint16_t opcode{};
  nlohmann::json message;
  std::shared_ptr<const LocalPacketPlan> local_plan;
  std::shared_ptr<void> local_construction;
  // A server-only map-load barrier; it is never encoded as a client packet.
  bool local_full_map_snapshot{};
  // When a handler starts travel before frame encoding, retain its pre-travel view epoch.
  std::optional<std::uint64_t> local_view_epoch;
};

} // namespace chronicles
