#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace chronicles {

enum class MoneyType : int32_t {
    Gold = 0,
    HersanderToken = 1,
};

constexpr int64_t kMoneyLimit = 9000000000000000LL;

enum class MoneyErrorCode {
    Ok = 0,
    MoneyNotEnough = 1201,
    InternalFailure = -1,
};

struct MoneyChange {
    MoneyType type = MoneyType::Gold;
    int64_t change = 0;
    int64_t result = 0;
    bool omit_type_on_wire = false;
};

struct CurrencyBalances {
    int64_t gold = 0;
    int64_t hersander_token = 0;
    int64_t blue_salt = 0;
};

struct ApplyResult {
    bool ok = false;
    MoneyErrorCode error = MoneyErrorCode::Ok;
    MoneyChange change{};
};

// Apply a signed delta. Blue Salt mutation is not established by this money contract.
ApplyResult apply_money_change(CurrencyBalances& balances, MoneyType type,
                               int64_t delta);

// The caller determines whether opcode 126 includes Hersander, even when zero.
std::map<std::string, int64_t> build_money_values_map(const CurrencyBalances& balances,
                                                      bool include_hersander);

} // namespace chronicles
