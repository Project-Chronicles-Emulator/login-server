#include "services/news/news.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

namespace chronicles {
namespace {

nlohmann::json article(std::string id = "launcher-update", bool published = true) {
  return {{"id", id},
          {"category", "Launcher"},
          {"title", "Launcher update"},
          {"summary", "Account sessions are available."},
          {"body", "Sign in once per launcher session.\n\nYour password is never saved."},
          {"art", "citadel"},
          {"published", published}};
}

void exercise_storage(NewsStore &news) {
  EXPECT_TRUE(news.list().empty());
  auto draft = article("draft", false);
  news.put(draft);
  EXPECT_TRUE(news.list().empty());
  ASSERT_EQ(news.list(true).size(), 1);
  draft["published"] = true;
  news.put(draft);
  ASSERT_EQ(news.list().size(), 1);
  const auto published_at = news.list().front().at("published_at");
  EXPECT_GT(published_at.get<std::int64_t>(), 0);
  draft["title"] = "Updated title";
  news.put(draft);
  EXPECT_EQ(news.list().front().at("title"), "Updated title");
  EXPECT_EQ(news.list().front().at("published_at"), published_at);
  EXPECT_TRUE(news.withdraw("draft"));
  EXPECT_TRUE(news.list().empty());
  EXPECT_EQ(news.list(true).front().at("body"), draft.at("body"));
  EXPECT_FALSE(news.withdraw("missing"));
}

TEST(NewsStoreTest, DraftPublicationEditingAndWithdrawal) {
  NewsStore news;
  exercise_storage(news);
}

TEST(NewsStoreTest, RejectsInvalidInputWithoutChangingSavedArticle) {
  NewsStore news;
  news.put(article());
  for (const auto *field : {"title", "summary", "body", "category"}) {
    auto invalid = article();
    invalid[field] = " \n\t";
    EXPECT_THROW(news.put(invalid), std::invalid_argument);
  }
  auto invalid = article("../escape");
  EXPECT_THROW(news.put(invalid), std::invalid_argument);
  invalid = article();
  invalid["art"] = "https://remote.example/image.png";
  EXPECT_THROW(news.put(invalid), std::invalid_argument);
  invalid = article();
  invalid["body"] = std::string(16001, 'x');
  EXPECT_THROW(news.put(invalid), std::invalid_argument);
  EXPECT_EQ(news.list().size(), 1);
}

TEST(NewsStoreTest, BoundsFeedAndUsesStableOrdering) {
  NewsStore news;
  for (int index = 0; index < 45; ++index) {
    news.put(article("item-" + std::to_string(100 + index)));
  }
  const auto first = news.list();
  EXPECT_EQ(first.size(), 40);
  EXPECT_EQ(first, news.list());
  EXPECT_EQ(first.front().at("id"), "item-144");
}

TEST(NewsStoreIntegration, PostgresPersistsAcrossInstances) {
  const auto *connection = std::getenv("CHRONICLES_NEWS_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Set CHRONICLES_NEWS_TEST_DATABASE_URL to an empty, migrated test database";
  }
  NewsStore news(connection);
  exercise_storage(news);
  news.put(article("persistent"));
  NewsStore reopened(connection);
  ASSERT_EQ(reopened.list().size(), 1);
  EXPECT_EQ(reopened.list().front().at("id"), "persistent");
  EXPECT_TRUE(reopened.withdraw("persistent"));
}

} // namespace
} // namespace chronicles
