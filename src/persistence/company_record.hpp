#pragma once

#include <cstdint>

namespace chronicles {

// Values equal x5.cms.EX5CompanyType; None means no company.
inline constexpr int kCompanyNone = 0;
inline constexpr int kMaximumCompanyType = 5;

// Durable per-character company state. company_type kCompanyNone means not in a company.
struct CompanyRecord {
  std::int64_t char_id{};
  int company_type{kCompanyNone};
  std::int64_t contribution{};
  std::int64_t join_epoch_ticks{};
  std::int64_t activity_epoch_ticks{};
  std::int64_t permit_expiry_epoch_ticks{};
  // Highest position level unlocked by CompanyLevelUnlock rewards or GM commands.
  int unlocked_position_level{};

  bool operator==(const CompanyRecord &) const = default;
};

} // namespace chronicles
