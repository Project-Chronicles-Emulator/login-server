#include "services/portal/portal_content.hpp"
#include "services/portal_storage/portal_store.hpp"

#include <cstdlib>
#include <gtest/gtest.h>

namespace chronicles {
namespace {

nlohmann::json offer(std::string id = "outfit", bool published = true) {
  return {{"id", id},
          {"published", published},
          {"sort_order", 0},
          {"title", "An outfit"},
          {"summary", "View this collection in the shop."},
          {"category", "COLLECTION"},
          {"art", "citadel"},
          {"price_label", "See shop"},
          {"url", "https://shop.example/items/outfit"}};
}

nlohmann::json community() {
  return {{"id", "forum"},
          {"published", true},
          {"sort_order", 0},
          {"title", "Community forum"},
          {"summary", "Discuss your adventures."},
          {"type", "forum"},
          {"url", "https://forum.example/"}};
}

void exercise_store(PortalStore &content) {
  EXPECT_TRUE(content.list("store").empty());
  content.put("store", offer("draft", false));
  EXPECT_TRUE(content.list("store").empty());
  content.put("store", offer("draft"));
  ASSERT_EQ(content.list("store").size(), 1);
  auto updated = offer("draft");
  updated["title"] = "Updated collection";
  content.put("store", updated);
  EXPECT_EQ(content.list("store").front().at("title"), "Updated collection");
  content.put("community", community());
  EXPECT_EQ(content.list("community").size(), 1);
  EXPECT_TRUE(content.withdraw("store", "draft"));
  EXPECT_TRUE(content.list("store").empty());
  EXPECT_EQ(content.list("store", true).size(), 1);
  EXPECT_FALSE(content.withdraw("store", "missing"));
}

TEST(PortalStoreTest, PublicationEditingAndIndependentSections) {
  PortalStore content;
  exercise_store(content);
}

TEST(PortalContentTest, RejectsDangerousUrlsAndKeepsOnlyRecognizedFields) {
  for (const auto *url :
       {"javascript:alert(1)", "file:///C:/secret", "https://user@shop.example/",
        "http://remote.example/", "https://shop.example\\@evil.example/", "https://x.example/\n"}) {
    auto invalid = offer();
    invalid["url"] = url;
    EXPECT_THROW(validate_portal_entry("store", invalid), std::invalid_argument);
  }
  auto entry = offer();
  entry["unexpected"] = "not forwarded";
  EXPECT_FALSE(validate_portal_entry("store", entry).contains("unexpected"));
  entry["url"] = "http://127.0.0.1:8495/catalog/item";
  EXPECT_NO_THROW(validate_portal_entry("store", entry));
}

TEST(PortalStoreTest, LimitsAndOrdersPublicFeed) {
  PortalStore content;
  for (int index = 0; index < 45; ++index) {
    auto entry = offer("offer-" + std::to_string(index));
    entry["sort_order"] = index;
    content.put("store", entry);
  }
  EXPECT_EQ(content.list("store").size(), 40);
  EXPECT_EQ(content.list("store").front().at("id"), "offer-0");
  auto invalid = offer();
  invalid["sort_order"] = 1001;
  EXPECT_THROW(content.put("store", invalid), std::invalid_argument);
}

TEST(PortalStoreIntegration, PersistsAcrossRepositoryInstances) {
  const auto *connection = std::getenv("CHRONICLES_PORTAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Run tools/portal_smoke_test.py";
  }
  PortalStore content(connection);
  exercise_store(content);
  PortalStore reopened(connection);
  ASSERT_EQ(reopened.list("community").size(), 1);
  EXPECT_EQ(reopened.list("community").front().at("url"), "https://forum.example/");
}

} // namespace
} // namespace chronicles
