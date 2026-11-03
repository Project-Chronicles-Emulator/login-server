#include "services/discord/discord_oauth.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

#include <array>
#include <memory>
#endif

namespace chronicles {

nlohmann::json request_discord_https(const std::string &method, const std::string &path,
                                     const std::string &form, const std::string &bearer) {
#ifdef _WIN32
  const auto deadline = Clock::now() + std::chrono::seconds(15);
  if ((method != "GET" && method != "POST") ||
      (path != "/api/oauth2/token" && path != "/api/v10/oauth2/@me") || form.size() > 8192 ||
      bearer.size() > 2048 || bearer.find_first_of("\r\n") != std::string::npos) {
    throw std::invalid_argument("Invalid Discord HTTPS request");
  }
  using Handle = std::unique_ptr<std::remove_pointer_t<HINTERNET>, decltype(&WinHttpCloseHandle)>;
  Handle session(WinHttpOpen(L"ChroniclesDiscordLogin/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                 WinHttpCloseHandle);
  if (!session || !WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 3000)) {
    throw std::runtime_error("Discord HTTPS initialization failed");
  }
  Handle connection(WinHttpConnect(session.get(), L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0),
                    WinHttpCloseHandle);
  const std::wstring target(path.begin(), path.end());
  Handle request(connection
                     ? WinHttpOpenRequest(connection.get(), method == "POST" ? L"POST" : L"GET",
                                          target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                     : nullptr,
                 WinHttpCloseHandle);
  if (!request) {
    throw std::runtime_error("Discord HTTPS connection failed");
  }
  DWORD disabled =
      WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                        sizeof(disabled))) {
    throw std::runtime_error("Discord HTTPS request policy failed");
  }
  std::wstring headers = L"Accept: application/json\r\n";
  if (method == "POST") {
    headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
  }
  if (!bearer.empty()) {
    headers += L"Authorization: Bearer " + std::wstring(bearer.begin(), bearer.end()) + L"\r\n";
  }
  if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                          form.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(form.data()),
                          static_cast<DWORD>(form.size()), static_cast<DWORD>(form.size()), 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    throw std::runtime_error("Discord HTTPS request failed");
  }
  DWORD status{};
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    throw std::runtime_error("Discord authorization was not accepted");
  }
  constexpr std::size_t kMaximumResponseBytes = 32768;
  std::array<char, 4096> buffer{};
  std::string body;
  for (;;) {
    if (Clock::now() >= deadline) {
      throw std::runtime_error("Discord HTTPS response timed out");
    }
    DWORD received{};
    if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                         &received)) {
      throw std::runtime_error("Discord HTTPS response failed");
    }
    if (received == 0) {
      break;
    }
    if (received > kMaximumResponseBytes - body.size()) {
      throw std::runtime_error("Discord HTTPS response exceeded its limit");
    }
    body.append(buffer.data(), received);
  }
  return nlohmann::json::parse(body);
#else
  throw std::runtime_error("Discord HTTPS transport is available on the Windows server build");
#endif
}

} // namespace chronicles
