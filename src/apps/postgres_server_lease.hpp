#pragma once

#include <memory>
#include <string>

namespace chronicles {

enum class ServerLeaseRole { world, login };

// Windows holds a mutex scoped to the database identity until all server work stops.
class HostServerLease final {
public:
  explicit HostServerLease(const std::string &database_identity,
                            ServerLeaseRole role = ServerLeaseRole::world);
  ~HostServerLease();

  HostServerLease(const HostServerLease &) = delete;
  HostServerLease &operator=(const HostServerLease &) = delete;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Holds the realm's PostgreSQL session lock until the server and its workers have stopped.
// A lost lease terminates this process; the outbox has an additional database epoch fence.
class PostgresServerLease final {
public:
  explicit PostgresServerLease(const std::string &connection_string,
                                ServerLeaseRole role = ServerLeaseRole::world);
  ~PostgresServerLease();

  PostgresServerLease(const PostgresServerLease &) = delete;
  PostgresServerLease &operator=(const PostgresServerLease &) = delete;

  // A non-secret digest of PostgreSQL cluster identity and database OID.
  std::string database_identity() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace chronicles
