#include "common/currency.hpp"

#include <stdexcept>

namespace chronicles {
namespace {

int64_t& balance_for(CurrencyBalances& balances, MoneyType type) {
    switch (type) {
        case MoneyType::Gold:
            return balances.gold;
        case MoneyType::HersanderToken:
            return balances.hersander_token;
    }

    throw std::invalid_argument("Unknown money type");
}

} // namespace

ApplyResult apply_money_change(CurrencyBalances& balances, MoneyType type,
                               int64_t delta) {
    ApplyResult result;
    int64_t& balance = balance_for(balances, type);
    if (balance < 0 || balance > kMoneyLimit) {
        result.error = MoneyErrorCode::InternalFailure;
        return result;
    }
    if (delta < -balance) {
        result.error = MoneyErrorCode::MoneyNotEnough;
        return result;
    }
    if (delta > kMoneyLimit - balance) {
        result.error = MoneyErrorCode::InternalFailure;
        return result;
    }

    balance += delta;
    result.ok = true;
    result.change.type = type;
    result.change.change = delta;
    result.change.result = balance;
    result.change.omit_type_on_wire = (type == MoneyType::Gold);
    return result;
}

std::map<std::string, int64_t> build_money_values_map(const CurrencyBalances& balances,
                                                      bool include_hersander) {
    std::map<std::string, int64_t> values;
    values["0"] = balances.gold;
    if (include_hersander) {
        values["1"] = balances.hersander_token;
    }

    return values;
}

} // namespace chronicles
