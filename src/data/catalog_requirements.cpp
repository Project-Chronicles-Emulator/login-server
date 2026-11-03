#include "data/catalog_requirements.hpp"

#include "data/client_catalog.hpp"

#include <spdlog/spdlog.h>

namespace chronicles {

bool has_catalog_tables(ClientCatalog &catalog, const std::vector<std::string> &tables,
                        const char *feature) {
  for (const auto &table : tables) {
    if (!catalog.has_table(table)) {
      spdlog::warn("{} disabled: client catalog table {} is absent", feature, table);
      return false;
    }
  }
  return true;
}

} // namespace chronicles
