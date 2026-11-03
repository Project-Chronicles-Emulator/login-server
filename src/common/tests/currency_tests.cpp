#include "common/currency.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace chronicles {
namespace {

TEST(Currency, GoldChangeOmitsTypeButHersanderDoesNot) {
    CurrencyBalances balances;
    const auto gold = apply_money_change(balances, MoneyType::Gold, 100);
    const auto hersander = apply_money_change(balances, MoneyType::HersanderToken, 20);

    ASSERT_TRUE(gold.ok);
    ASSERT_TRUE(hersander.ok);
    EXPECT_EQ(gold.change.result, 100);
    EXPECT_TRUE(gold.change.omit_type_on_wire);
    EXPECT_EQ(hersander.change.result, 20);
    EXPECT_FALSE(hersander.change.omit_type_on_wire);
}

TEST(Currency, UnderflowAndOverflowLeaveBalancesUnchanged) {
    CurrencyBalances balances{kMoneyLimit, 0, 5};
    const auto underflow = apply_money_change(balances, MoneyType::HersanderToken, -1);
    const auto overflow = apply_money_change(balances, MoneyType::Gold, 1);

    EXPECT_FALSE(underflow.ok);
    EXPECT_EQ(underflow.error, MoneyErrorCode::MoneyNotEnough);
    EXPECT_FALSE(overflow.ok);
    EXPECT_EQ(overflow.error, MoneyErrorCode::InternalFailure);
    EXPECT_EQ(balances.gold, kMoneyLimit);
    EXPECT_EQ(balances.hersander_token, 0);
    EXPECT_EQ(balances.blue_salt, 5);
}

TEST(Currency, ExtremeNegativeDeltaDoesNotOverflowSignedArithmetic) {
    CurrencyBalances balances{1, 0, 0};
    const auto result = apply_money_change(
        balances, MoneyType::Gold, std::numeric_limits<int64_t>::min());

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, MoneyErrorCode::MoneyNotEnough);
    EXPECT_EQ(balances.gold, 1);
}

TEST(Currency, CallerControlsPresenceOfZeroHersanderBalance) {
    CurrencyBalances balances{100, 0, 5};
    const auto included = build_money_values_map(balances, true);
    const auto omitted = build_money_values_map(balances, false);

    EXPECT_EQ(included.at("0"), 100);
    EXPECT_EQ(included.at("1"), 0);
    EXPECT_EQ(omitted.at("0"), 100);
    EXPECT_FALSE(omitted.contains("1"));
}

} // namespace
} // namespace chronicles
