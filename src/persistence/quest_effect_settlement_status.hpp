#pragma once

namespace chronicles {

// A settlement never treats an unknown storage outcome as an applied effect. Pending is
// returned by fenced readback; retry and identity_mismatch must not mutate cached state.
enum class QuestEffectSettlementStatus {
  pending,
  applied,
  already_applied,
  retry,
  identity_mismatch,
};

} // namespace chronicles
