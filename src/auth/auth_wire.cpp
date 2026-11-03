#include "auth/auth_wire.hpp"

#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"

#include <algorithm>
#include <memory>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
constexpr auto kMaximumClockSkew = std::chrono::seconds(15);
constexpr auto kReplayRetention = std::chrono::seconds(31);

std::int64_t wall_milliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool valid_operation(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= 'a' && character <= 'z') || character == '_';
         });
}

bool valid_request_id(std::string_view value) {
  return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= 'a' && character <= 'f') || (character >= '0' && character <= '9');
         });
}

std::string authenticated_context(const AuthWireContext &context, std::string_view direction,
                                  unsigned status = 0) {
  return "chronicles-auth-v1\n" + std::string(direction) + "\n" + context.operation + "\n" +
         context.database_identity + "\n" + context.request_id + "\n" +
         std::to_string(context.issued_at_ms) + "\n" + std::to_string(status);
}

std::string message_key(const std::string &master_key_hex, const std::string &context) {
  const auto master = hex_decode(master_key_hex);
  const auto purpose = "chronicles-auth-message-key-v1\n" + context;
  unsigned char derived[EVP_MAX_MD_SIZE]{};
  unsigned int size{};
  if (!HMAC(EVP_sha256(), master.data(), static_cast<int>(master.size()),
            reinterpret_cast<const unsigned char *>(purpose.data()), purpose.size(), derived,
            &size) ||
      size != 32) {
    throw AuthWireError();
  }
  // A fresh request ID and separate request/reply contexts give each message its own
  // encryption key, rather than using one GCM key for the service's entire lifetime.
  return std::string(reinterpret_cast<const char *>(derived), size);
}

Json encrypt(const std::string &key_hex, const std::string &aad, const Json &document) {
  const auto plaintext = document.dump();
  if (!valid_auth_service_key(key_hex) || plaintext.size() > kMaximumAuthPayloadBytes) {
    throw AuthWireError();
  }
  const auto key = message_key(key_hex, aad);
  const auto nonce = hex_decode(random_hex(12));
  Cipher cipher(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!cipher ||
      EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                          nullptr) != 1 ||
      EVP_EncryptInit_ex(cipher.get(), nullptr, nullptr,
                         reinterpret_cast<const unsigned char *>(key.data()),
                         reinterpret_cast<const unsigned char *>(nonce.data())) != 1) {
    throw AuthWireError();
  }
  int written{};
  if (EVP_EncryptUpdate(cipher.get(), nullptr, &written,
                        reinterpret_cast<const unsigned char *>(aad.data()),
                        static_cast<int>(aad.size())) != 1) {
    throw AuthWireError();
  }
  std::string ciphertext(plaintext.size() + 16, '\0');
  if (EVP_EncryptUpdate(cipher.get(), reinterpret_cast<unsigned char *>(ciphertext.data()),
                        &written, reinterpret_cast<const unsigned char *>(plaintext.data()),
                        static_cast<int>(plaintext.size())) != 1) {
    throw AuthWireError();
  }
  int final_bytes{};
  if (EVP_EncryptFinal_ex(cipher.get(),
                          reinterpret_cast<unsigned char *>(ciphertext.data()) + written,
                          &final_bytes) != 1) {
    throw AuthWireError();
  }
  ciphertext.resize(static_cast<std::size_t>(written + final_bytes));
  std::string tag(16, '\0');
  if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
                          tag.data()) != 1) {
    throw AuthWireError();
  }
  return {{"version", 1},
          {"nonce", hex_encode(nonce)},
          {"tag", hex_encode(tag)},
          {"ciphertext", hex_encode(ciphertext)}};
}

Json decrypt(const std::string &key_hex, const std::string &aad, const Json &envelope) {
  try {
    if (!valid_auth_service_key(key_hex) || !envelope.is_object() || envelope.at("version") != 1 ||
        (envelope.size() != 4 && envelope.size() != 6) || !envelope.at("nonce").is_string() ||
        !envelope.at("tag").is_string() || !envelope.at("ciphertext").is_string()) {
      throw AuthWireError();
    }
    const auto &nonce_hex = envelope.at("nonce").get_ref<const std::string &>();
    const auto &tag_hex = envelope.at("tag").get_ref<const std::string &>();
    const auto &ciphertext_hex = envelope.at("ciphertext").get_ref<const std::string &>();
    if (nonce_hex.size() != 24 || tag_hex.size() != 32 ||
        ciphertext_hex.size() > 2 * kMaximumAuthPayloadBytes) {
      throw AuthWireError();
    }
    const auto nonce = hex_decode(envelope.at("nonce").get<std::string>());
    auto tag = hex_decode(envelope.at("tag").get<std::string>());
    const auto ciphertext = hex_decode(envelope.at("ciphertext").get<std::string>());
    const auto key = message_key(key_hex, aad);
    if (nonce.size() != 12 || tag.size() != 16 || ciphertext.size() > kMaximumAuthPayloadBytes) {
      throw AuthWireError();
    }
    Cipher cipher(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!cipher ||
        EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) != 1 ||
        EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.data()),
                           reinterpret_cast<const unsigned char *>(nonce.data())) != 1) {
      throw AuthWireError();
    }
    int written{};
    if (EVP_DecryptUpdate(cipher.get(), nullptr, &written,
                          reinterpret_cast<const unsigned char *>(aad.data()),
                          static_cast<int>(aad.size())) != 1) {
      throw AuthWireError();
    }
    std::string plaintext(ciphertext.size() + 16, '\0');
    if (EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char *>(plaintext.data()),
                          &written, reinterpret_cast<const unsigned char *>(ciphertext.data()),
                          static_cast<int>(ciphertext.size())) != 1 ||
        EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                            tag.data()) != 1) {
      throw AuthWireError();
    }
    int final_bytes{};
    if (EVP_DecryptFinal_ex(cipher.get(),
                            reinterpret_cast<unsigned char *>(plaintext.data()) + written,
                            &final_bytes) != 1) {
      throw AuthWireError();
    }
    plaintext.resize(static_cast<std::size_t>(written + final_bytes));
    const auto result = Json::parse(plaintext, nullptr, false);
    if (result.is_discarded() || (!result.is_object() && !result.is_null())) {
      throw AuthWireError();
    }
    return result;
  } catch (const AuthWireError &) {
    throw;
  } catch (const std::exception &) {
    throw AuthWireError();
  }
}

} // namespace

AuthWireRequest seal_auth_request(const std::string &key, std::string_view operation,
                                  const std::string &database_identity, const Json &arguments) {
  if (!valid_operation(operation) || !arguments.is_object() || database_identity.size() > 64) {
    throw std::invalid_argument("Invalid authentication authority request");
  }
  AuthWireContext context{std::string(operation), database_identity, random_hex(16),
                          wall_milliseconds()};
  auto envelope = encrypt(key, authenticated_context(context, "request"), arguments);
  envelope["request_id"] = context.request_id;
  envelope["issued_at_ms"] = context.issued_at_ms;
  return {std::move(context), std::move(envelope)};
}

OpenAuthWireRequest open_auth_request(const std::string &key, std::string_view operation,
                                      const std::string &database_identity, const Json &envelope) {
  try {
    if (!envelope.is_object() || envelope.size() != 6 ||
        !envelope.at("issued_at_ms").is_number_integer()) {
      throw AuthWireError();
    }
    AuthWireContext context{std::string(operation), database_identity,
                            envelope.at("request_id").get<std::string>(),
                            envelope.at("issued_at_ms").get<std::int64_t>()};
    const auto now = wall_milliseconds();
    const auto skew =
        std::chrono::duration_cast<std::chrono::milliseconds>(kMaximumClockSkew).count();
    if (!valid_operation(operation) || !valid_request_id(context.request_id) ||
        context.issued_at_ms < now - skew || context.issued_at_ms > now + skew) {
      throw AuthWireError();
    }
    auto arguments = decrypt(key, authenticated_context(context, "request"), envelope);
    if (!arguments.is_object()) {
      throw AuthWireError();
    }
    return {std::move(context), std::move(arguments)};
  } catch (const AuthWireError &) {
    throw;
  } catch (const std::exception &) {
    throw AuthWireError();
  }
}

Json seal_auth_reply(const std::string &key, const AuthWireContext &context, unsigned status,
                     const Json &reply) {
  return encrypt(key, authenticated_context(context, "response", status), reply);
}

Json open_auth_reply(const std::string &key, const AuthWireContext &context, unsigned status,
                     const Json &envelope) {
  if (!envelope.is_object() || envelope.size() != 4) {
    throw AuthWireError();
  }
  return decrypt(key, authenticated_context(context, "response", status), envelope);
}

AuthReplayGuard::AuthReplayGuard(std::size_t maximum, std::function<Clock::time_point()> now)
    : maximum_(maximum), now_(std::move(now)) {
  if (maximum_ == 0) {
    throw std::invalid_argument("Authentication replay guard requires positive capacity");
  }
}

bool AuthReplayGuard::accept(const std::string &request_id) {
  if (!valid_request_id(request_id)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const auto now = now_();
  while (!order_.empty() && now - order_.front().second >= kReplayRetention) {
    accepted_.erase(order_.front().first);
    order_.pop_front();
  }
  if (accepted_.contains(request_id) || accepted_.size() >= maximum_) {
    return false;
  }
  accepted_.insert(request_id);
  order_.emplace_back(request_id, now);
  return true;
}

} // namespace chronicles
