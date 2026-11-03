#include "apps/postgres_server_lease.hpp"

#include <libpq-fe.h>
#include <openssl/evp.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <sys/select.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace chronicles {
namespace {

// Stable two-part advisory key, scoped by the connected PostgreSQL database.
constexpr const char *kAcquireSql = "SELECT pg_try_advisory_lock(1129465680, 1398096722)";
constexpr const char *kAcquireLoginSql = "SELECT pg_try_advisory_lock(1129465680, 1280264009)";
constexpr auto kProbeInterval = std::chrono::milliseconds(500);
constexpr auto kProbeTimeout = std::chrono::seconds(5);
constexpr auto kSocketWait = std::chrono::milliseconds(100);
constexpr int kLeaseLostExitCode = 70;

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

[[noreturn]] void fail_stopped_server() noexcept {
  // Never print libpq's error text: it can contain connection details.
  std::fputs("Fatal: PostgreSQL server lease was lost; terminating the server.\n", stderr);
  std::fflush(stderr);
  std::_Exit(kLeaseLostExitCode);
}

bool wait_socket(int socket, bool writable, std::chrono::milliseconds wait) {
  if (socket < 0) {
    return false;
  }
#ifndef _WIN32
  if (socket >= FD_SETSIZE) {
    return false;
  }
#endif
  fd_set descriptors;
  FD_ZERO(&descriptors);
#ifdef _WIN32
  FD_SET(static_cast<SOCKET>(socket), &descriptors);
#else
  FD_SET(socket, &descriptors);
#endif
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(wait.count() / 1000);
  timeout.tv_usec = static_cast<long>((wait.count() % 1000) * 1000);
  const int result = select(socket + 1, writable ? nullptr : &descriptors,
                            writable ? &descriptors : nullptr, nullptr, &timeout);
  if (result >= 0) {
    return true;
  }
#ifdef _WIN32
  return WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

} // namespace

class HostServerLease::Impl {
public:
  explicit Impl(const std::string &database_identity, ServerLeaseRole role) {
#ifdef _WIN32
    if (database_identity.size() != 64) {
      throw std::invalid_argument("Invalid server lease database identity");
    }
    const std::wstring prefix = role == ServerLeaseRole::login
                                    ? L"Global\\ChroniclesLoginExclusiveOwner-"
                                    : L"Global\\ChroniclesServerExclusiveOwner-";
    const std::wstring name = prefix +
                              std::wstring(database_identity.begin(), database_identity.end());
    SetLastError(ERROR_SUCCESS);
    handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
    const auto result = GetLastError();
    if (!handle_ || result == ERROR_ALREADY_EXISTS) {
      if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
      }
      throw std::runtime_error("Another local server owns the host lease");
    }
#endif
#ifndef _WIN32
    static_cast<void>(database_identity);
    static_cast<void>(role);
#endif
  }

  ~Impl() {
#ifdef _WIN32
    if (handle_) {
      CloseHandle(handle_);
    }
#endif
  }

private:
#ifdef _WIN32
  HANDLE handle_{};
#endif
};

HostServerLease::HostServerLease(const std::string &database_identity, ServerLeaseRole role)
    : impl_(std::make_unique<Impl>(database_identity, role)) {
}

HostServerLease::~HostServerLease() = default;

class PostgresServerLease::Impl {
public:
  explicit Impl(const std::string &connection_string, ServerLeaseRole role)
      : connection_(nullptr, &PQfinish) {
    if (connection_string.empty()) {
      throw std::invalid_argument("PostgreSQL server lease needs a database configuration");
    }
    const char *keywords[] = {"dbname", "application_name", "connect_timeout", nullptr};
    const char *application = role == ServerLeaseRole::login
                                  ? "chronicles-login-exclusive-lease"
                                  : "chronicles-server-exclusive-lease";
    const char *values[] = {connection_string.c_str(), application, "5", nullptr};
    connection_.reset(PQconnectdbParams(keywords, values, 1));
    if (!connection_ || PQstatus(connection_.get()) != CONNECTION_OK) {
      throw std::runtime_error("Cannot connect to PostgreSQL for the exclusive server lease");
    }

    Result acquired(PQexec(connection_.get(),
                            role == ServerLeaseRole::login ? kAcquireLoginSql : kAcquireSql),
                      &PQclear);
    if (!acquired || PQresultStatus(acquired.get()) != PGRES_TUPLES_OK ||
        PQntuples(acquired.get()) != 1 || PQnfields(acquired.get()) != 1 ||
        std::string_view(PQgetvalue(acquired.get(), 0, 0)) != "t") {
      throw std::runtime_error("PostgreSQL exclusive server lease is unavailable");
    }

    Result identity(PQexec(connection_.get(),
                           "SELECT system_identifier::text, "
                           "(SELECT oid FROM pg_database WHERE datname=current_database())::text "
                           "FROM pg_control_system()"),
                    &PQclear);
    if (!identity || PQresultStatus(identity.get()) != PGRES_TUPLES_OK ||
        PQntuples(identity.get()) != 1 || PQnfields(identity.get()) != 2 ||
        PQgetisnull(identity.get(), 0, 0) || PQgetisnull(identity.get(), 0, 1)) {
      throw std::runtime_error("Cannot obtain the PostgreSQL database identity");
    }
    const std::string source = std::string(PQgetvalue(identity.get(), 0, 0)) + ":" +
                               PQgetvalue(identity.get(), 0, 1);
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_size{};
    if (EVP_Digest(source.data(), source.size(), digest, &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
      throw std::runtime_error("Cannot hash the PostgreSQL database identity");
    }
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned int index = 0; index < digest_size; ++index) {
      database_identity_.push_back(hex[digest[index] >> 4]);
      database_identity_.push_back(hex[digest[index] & 15]);
    }
    if (PQsetnonblocking(connection_.get(), 1) != 0) {
      throw std::runtime_error("Cannot configure PostgreSQL lease monitoring");
    }
    watcher_ = std::thread([this] {
      watch();
    });
  }

  ~Impl() {
    stopping_.store(true);
    wake_.notify_all();
    if (watcher_.joinable()) {
      watcher_.join();
    }
  }

  const std::string &database_identity() const noexcept {
    return database_identity_;
  }

private:
  bool probe() {
    if (PQstatus(connection_.get()) != CONNECTION_OK ||
        PQsendQuery(connection_.get(), "SELECT 1") != 1) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + kProbeTimeout;
    while (!stopping_.load() && std::chrono::steady_clock::now() < deadline) {
      const int flush = PQflush(connection_.get());
      if (flush < 0) {
        return false;
      }
      if (flush == 0) {
        break;
      }
      if (!wait_socket(PQsocket(connection_.get()), true, kSocketWait)) {
        return false;
      }
    }
    if (stopping_.load()) {
      return true;
    }
    while (std::chrono::steady_clock::now() < deadline && !stopping_.load()) {
      if (PQconsumeInput(connection_.get()) != 1) {
        return false;
      }
      if (!PQisBusy(connection_.get())) {
        Result result(PQgetResult(connection_.get()), &PQclear);
        if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
            PQntuples(result.get()) != 1 || std::string_view(PQgetvalue(result.get(), 0, 0)) != "1") {
          return false;
        }
        // Drain the complete result sequence before the next query.
        Result extra(PQgetResult(connection_.get()), &PQclear);
        return !extra && PQstatus(connection_.get()) == CONNECTION_OK;
      }
      if (!wait_socket(PQsocket(connection_.get()), false, kSocketWait)) {
        return false;
      }
    }
    return stopping_.load();
  }

  void watch() {
    while (!stopping_.load()) {
      if (!probe() && !stopping_.load()) {
        fail_stopped_server();
      }
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, kProbeInterval, [this] {
        return stopping_.load();
      });
    }
  }

  Connection connection_;
  std::string database_identity_;
  std::atomic<bool> stopping_{};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread watcher_;
};

PostgresServerLease::PostgresServerLease(const std::string &connection_string,
                                         ServerLeaseRole role)
    : impl_(std::make_unique<Impl>(connection_string, role)) {
}

PostgresServerLease::~PostgresServerLease() = default;

std::string PostgresServerLease::database_identity() const {
  return impl_->database_identity();
}

} // namespace chronicles
