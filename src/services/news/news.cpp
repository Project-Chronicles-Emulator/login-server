#include "services/news/news.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <pqxx/pqxx>

namespace chronicles {
namespace {

using json = nlohmann::json;
constexpr std::size_t kFeedLimit = 40;

void validate_id(const std::string &id) {
  if (id.empty() || id.size() > 80 || !std::ranges::all_of(id, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
      })) {
    throw std::invalid_argument("News id must contain 1-80 lowercase letters, digits or hyphens");
  }
}

void validate_text(const json &article, const char *field, std::size_t maximum) {
  const auto &value = article.at(field).get_ref<const std::string &>();
  const bool has_text =
      std::ranges::any_of(value, [](unsigned char character) { return character > 32; });
  if (!has_text || value.size() > maximum ||
      std::ranges::any_of(value, [](unsigned char character) {
        return character < 32 && character != '\n' && character != '\t' && character != '\r';
      })) {
    throw std::invalid_argument(std::string("News ") + field +
                                " is empty, too long or contains control characters");
  }
}

json row_article(const pqxx::row &row) {
  return {{"id", row["id"].as<std::string>()},
          {"category", row["category"].as<std::string>()},
          {"title", row["title"].as<std::string>()},
          {"summary", row["summary"].as<std::string>()},
          {"body", row["body"].as<std::string>()},
          {"art", row["art"].as<std::string>()},
          {"published", !row["published_at"].is_null()},
          {"published_at", row["published_at"].as<std::int64_t>(0)},
          {"updated_at", row["updated_at"].as<std::int64_t>()}};
}

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

NewsStore::NewsStore(std::string connection) : connection_(std::move(connection)) {}

void NewsStore::validate(const json &article) {
  validate_id(article.at("id").get<std::string>());
  validate_text(article, "category", 40);
  validate_text(article, "title", 160);
  validate_text(article, "summary", 400);
  validate_text(article, "body", 16000);
  const auto art = article.at("art").get<std::string>();
  if (art != "citadel" && art != "voyage" && art != "company" && art != "discover") {
    throw std::invalid_argument("News art must be citadel, voyage, company or discover");
  }
  if (!article.at("published").is_boolean()) {
    throw std::invalid_argument("News published must be true or false");
  }
}

json NewsStore::list(bool include_drafts) const {
  if (!connection_.empty()) {
    pqxx::connection connection(connection_);
    pqxx::read_transaction transaction(connection);
    const auto rows =
        transaction.exec("SELECT id,category,title,summary,body,art,published_at,updated_at "
                         "FROM emulator_news WHERE ($1 OR published_at IS NOT NULL) "
                         "ORDER BY published_at DESC NULLS LAST,updated_at DESC,id DESC LIMIT 40",
                         pqxx::params{include_drafts});
    auto result = json::array();
    for (const auto &row : rows) {
      result.push_back(row_article(row));
    }
    return result;
  }

  std::scoped_lock lock(mutex_);
  std::vector<json> sorted;
  for (const auto &article : articles_) {
    if (include_drafts || article.at("published").get<bool>()) {
      sorted.push_back(article);
    }
  }
  std::ranges::sort(sorted, [](const json &left, const json &right) {
    return std::tuple(left.at("published_at").get<std::int64_t>(),
                      left.at("updated_at").get<std::int64_t>(), left.at("id").get<std::string>()) >
           std::tuple(right.at("published_at").get<std::int64_t>(),
                      right.at("updated_at").get<std::int64_t>(),
                      right.at("id").get<std::string>());
  });
  if (sorted.size() > kFeedLimit) {
    sorted.resize(kFeedLimit);
  }
  return sorted;
}

void NewsStore::put(const json &article) {
  validate(article);
  const auto id = article.at("id").get<std::string>();
  const bool published = article.at("published").get<bool>();
  if (!connection_.empty()) {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    transaction.exec(
        "INSERT INTO emulator_news(id,category,title,summary,body,art,published_at,updated_at) "
        "VALUES($1,$2,$3,$4,$5,$6,CASE WHEN $7 THEN extract(epoch FROM now())::bigint END,"
        "extract(epoch FROM now())::bigint) ON CONFLICT(id) DO UPDATE SET "
        "category=EXCLUDED.category,title=EXCLUDED.title,summary=EXCLUDED.summary,"
        "body=EXCLUDED.body,art=EXCLUDED.art,updated_at=EXCLUDED.updated_at,"
        "published_at=CASE WHEN $7 THEN COALESCE(emulator_news.published_at,EXCLUDED.published_at) "
        "END",
        pqxx::params{
            id, article.at("category").get<std::string>(), article.at("title").get<std::string>(),
            article.at("summary").get<std::string>(), article.at("body").get<std::string>(),
            article.at("art").get<std::string>(), published});
    transaction.commit();
    return;
  }

  std::scoped_lock lock(mutex_);
  const auto existing = articles_.find(id);
  const auto previous_date =
      existing == articles_.end() ? 0 : existing->at("published_at").get<std::int64_t>();
  json stored;
  for (const auto *field : {"id", "category", "title", "summary", "body", "art", "published"}) {
    stored[field] = article.at(field);
  }
  auto publication_date = previous_date;
  if (published && publication_date == 0) {
    publication_date = now_seconds();
  }
  stored["updated_at"] = now_seconds();
  stored["published_at"] = published ? publication_date : 0;
  articles_[id] = std::move(stored);
}

bool NewsStore::withdraw(const std::string &id) {
  validate_id(id);
  if (!connection_.empty()) {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    const auto rows = transaction.exec(
        "UPDATE emulator_news SET published_at=NULL,updated_at=extract(epoch FROM now())::bigint "
        "WHERE id=$1 RETURNING id",
        pqxx::params{id});
    transaction.commit();
    return !rows.empty();
  }

  std::scoped_lock lock(mutex_);
  auto existing = articles_.find(id);
  if (existing == articles_.end()) {
    return false;
  }
  (*existing)["published"] = false;
  (*existing)["published_at"] = 0;
  (*existing)["updated_at"] = now_seconds();
  return true;
}

} // namespace chronicles
