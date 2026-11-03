#pragma once

#include <string>
#include <vector>

namespace chronicles {

class ClientCatalog;

// A missing optional table disables only the caller's feature. Parsing an available table
// remains the loader's responsibility and retains its existing fatal-error policy.
bool has_catalog_tables(ClientCatalog &catalog, const std::vector<std::string> &tables,
                        const char *feature);

} // namespace chronicles
