#include "services/portal_storage/portal_store.hpp"
#include "services/portal/portal_content.hpp"

#include <algorithm>
#include <chrono>
#include <pqxx/pqxx>
#include <tuple>
#include <vector>

namespace chronicles {

PortalStore::PortalStore(std::string connection) : connection_(std::move(connection)) {}

nlohmann::json PortalStore::list(const std::string &kind, bool include_drafts) const {
  validate_portal_kind(kind);
  auto result = nlohmann::json::array();
  if (!connection_.empty()) {
    pqxx::connection connection(connection_);
    pqxx::read_transaction transaction(connection);
    const auto rows =
        transaction.exec("SELECT (document || jsonb_build_object('id',id,'published',published,"
                         "'sort_order',sort_order,'updated_at',updated_at))::text AS entry "
                         "FROM emulator_launcher_content WHERE kind=$1 AND ($2 OR published) "
                         "ORDER BY sort_order ASC,updated_at DESC,id ASC LIMIT 40",
                         pqxx::params{kind, include_drafts});
    for (const auto &row : rows) {
      result.push_back(nlohmann::json::parse(row["entry"].as<std::string>()));
    }
    return result;
  }

  std::scoped_lock lock(mutex_);
  std::vector<nlohmann::json> sorted;
  const auto found = records_.find(kind);
  if (found == records_.end()) {
    return result;
  }
  for (const auto &entry : *found) {
    if (include_drafts || entry.at("published").get<bool>()) {
      sorted.push_back(entry);
    }
  }
  std::ranges::sort(sorted, [](const auto &left, const auto &right) {
    return std::tuple(left.at("sort_order").template get<int>(),
                      -left.at("updated_at").template get<std::int64_t>(),
                      left.at("id").template get<std::string>()) <
           std::tuple(right.at("sort_order").template get<int>(),
                      -right.at("updated_at").template get<std::int64_t>(),
                      right.at("id").template get<std::string>());
  });
  if (sorted.size() > 40) {
    sorted.resize(40);
  }
  return sorted;
}

void PortalStore::put(const std::string &kind, const nlohmann::json &entry) {
  auto stored = validate_portal_entry(kind, entry);
  const auto id = stored.at("id").get<std::string>();
  const auto published = stored.at("published").get<bool>();
  const auto order = stored.at("sort_order").get<int>();
  if (!connection_.empty()) {
    stored.erase("id");
    stored.erase("published");
    stored.erase("sort_order");
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    transaction.exec(
        "INSERT INTO emulator_launcher_content(kind,id,document,published,sort_order,updated_at) "
        "VALUES($1,$2,$3::jsonb,$4,$5,extract(epoch FROM now())::bigint) "
        "ON CONFLICT(kind,id) DO UPDATE SET "
        "document=EXCLUDED.document,published=EXCLUDED.published,"
        "sort_order=EXCLUDED.sort_order,updated_at=EXCLUDED.updated_at",
        pqxx::params{kind, id, stored.dump(), published, order});
    transaction.commit();
    return;
  }

  stored["updated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  std::scoped_lock lock(mutex_);
  records_[kind][id] = std::move(stored);
}

bool PortalStore::withdraw(const std::string &kind, const std::string &id) {
  validate_portal_kind(kind);
  validate_portal_id(id);
  if (!connection_.empty()) {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    const auto rows = transaction.exec(
        "UPDATE emulator_launcher_content SET published=false,"
        "updated_at=extract(epoch FROM now())::bigint WHERE kind=$1 AND id=$2 RETURNING id",
        pqxx::params{kind, id});
    transaction.commit();
    return !rows.empty();
  }

  std::scoped_lock lock(mutex_);
  auto section = records_.find(kind);
  if (section == records_.end() || !section->contains(id)) {
    return false;
  }
  (*section)[id]["published"] = false;
  return true;
}

} // namespace chronicles
