#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"
#include "auth/auth_wire.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = asio::ip::tcp;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

constexpr std::string_view kTestServiceKey =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class ReplyExchange : public std::enable_shared_from_this<ReplyExchange> {
public:
  ReplyExchange(Tcp::socket socket, Response response, std::promise<Request> &received)
      : socket_(std::move(socket)), response_(std::move(response)), received_(received) {}

  void start() {
    http::async_read(
        socket_, buffer_, request_,
        [self = shared_from_this()](boost::system::error_code error, std::size_t) {
          if (error) {
            return;
          }
          self->received_.set_value(self->request_);
          const auto reply = nlohmann::json::parse(self->response_.body(), nullptr, false);
          if (!reply.is_discarded()) {
            const auto opened = open_auth_request(
                std::string(kTestServiceKey), std::string(self->request_.target()).substr(18),
                std::string(self->request_["X-Chronicles-Database"]),
                nlohmann::json::parse(self->request_.body()));
            self->response_.body() = seal_auth_reply(std::string(kTestServiceKey), opened.context,
                                                     self->response_.result_int(), reply)
                                         .dump();
            self->response_.prepare_payload();
          }

          http::async_write(self->socket_, self->response_,
                            [self](boost::system::error_code, std::size_t) {
                              boost::system::error_code ignored;
                              self->socket_.close(ignored);
                            });
        });
  }

private:
  Tcp::socket socket_;
  beast::flat_buffer buffer_;
  Request request_;
  Response response_;
  std::promise<Request> &received_;
};

// A real loopback peer verifies the serialized request and transport failure paths.
class OneReplyAuthority {
public:
  explicit OneReplyAuthority(http::status status, std::string body)
      : acceptor_(context_, Tcp::endpoint(asio::ip::address_v4::loopback(), 0)) {
    Response response(status, 11);
    response.set(http::field::content_type, "application/json");
    response.body() = std::move(body);
    response.prepare_payload();
    acceptor_.async_accept([this, response = std::move(response)](boost::system::error_code error,
                                                                  Tcp::socket socket) mutable {
      if (!error) {
        std::make_shared<ReplyExchange>(std::move(socket), std::move(response), received_)->start();
      }
    });
    thread_ = std::jthread([this] {
      context_.run();
    });
  }

  ~OneReplyAuthority() {
    context_.stop();
    thread_.join();
  }

  std::string origin() const {
    return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
  }

  Request request() {
    return received_.get_future().get();
  }

private:
  asio::io_context context_;
  Tcp::acceptor acceptor_;
  std::promise<Request> received_;
  std::jthread thread_;
};

TEST(AuthAuthority, SendsPrivateCredentialSeparatelyFromPlayerArguments) {
  OneReplyAuthority peer(http::status::ok, R"({"accepted":true})");
  const auto client = make_local_auth_authority(peer.origin(), std::string(kTestServiceKey));
  EXPECT_TRUE(client->call("session", {{"token", "synthetic-player-token"}}).at("accepted"));

  const auto request = peer.request();
  EXPECT_EQ(request.target(), "/internal/auth/v1/session");
  EXPECT_EQ(request[http::field::authorization], std::string(kAuthWireAuthorization));
  EXPECT_EQ(request.body().find("synthetic-player-token"), std::string::npos);
  const auto opened = open_auth_request(std::string(kTestServiceKey), "session", "",
                                        nlohmann::json::parse(request.body()));
  EXPECT_EQ(opened.arguments.at("token"), "synthetic-player-token");
  EXPECT_FALSE(request.keep_alive());
}

TEST(AuthAuthority, RejectsRemoteOriginsAndInvalidServiceKeysBeforeConnecting) {
  EXPECT_THROW(make_local_auth_authority("https://example.com", std::string(kTestServiceKey)),
               std::invalid_argument);
  EXPECT_THROW(make_local_auth_authority("https://127.0.0.1", std::string(kTestServiceKey)),
               std::invalid_argument);
  EXPECT_THROW(make_local_auth_authority("http://127.0.0.1:8080", "short"), std::invalid_argument);
}

TEST(AuthAuthority, RejectsOperationPathInjectionBeforeConnecting) {
  const auto client = make_local_auth_authority("http://127.0.0.1:1", std::string(kTestServiceKey));
  EXPECT_THROW(client->call("../admin", nlohmann::json::object()), std::invalid_argument);
  EXPECT_THROW(client->call("session", nlohmann::json::array()), std::invalid_argument);
}

TEST(AuthAuthority, DoesNotAcceptDeniedMalformedOrOversizedReplies) {
  {
    OneReplyAuthority peer(http::status::unauthorized, R"({"error":"denied"})");
    const auto client = make_local_auth_authority(peer.origin(), std::string(kTestServiceKey));
    EXPECT_THROW(client->call("session", nlohmann::json::object()), Unauthorized);
  }
  {
    OneReplyAuthority peer(http::status::ok, "invalid JSON");
    const auto client = make_local_auth_authority(peer.origin(), std::string(kTestServiceKey));
    EXPECT_THROW(client->call("session", nlohmann::json::object()), std::runtime_error);
  }
  {
    OneReplyAuthority peer(http::status::ok, std::string(1024 * 1024 + 1, 'x'));
    const auto client = make_local_auth_authority(peer.origin(), std::string(kTestServiceKey));
    EXPECT_THROW(client->call("session", nlohmann::json::object()), std::runtime_error);
  }
}

TEST(AuthAuthority, AuthenticatedMessagesRejectTamperingAndCrossRequestReplies) {
  const std::string key(kTestServiceKey);
  const auto request = seal_auth_request(key, "session", "database", {{"token", "private"}});
  EXPECT_EQ(open_auth_request(key, "session", "database", request.envelope).arguments.at("token"),
            "private");
  EXPECT_THROW(open_auth_request(key, "login", "database", request.envelope), AuthWireError);
  EXPECT_THROW(open_auth_request(key, "session", "other", request.envelope), AuthWireError);
  EXPECT_THROW(open_auth_request(std::string(64, 'a'), "session", "database", request.envelope),
               AuthWireError);
  auto changed = request.envelope;
  changed["tag"] = std::string(32, '0');
  EXPECT_THROW(open_auth_request(key, "session", "database", changed), AuthWireError);
  changed = request.envelope;
  changed["issued_at_ms"] = 0;
  EXPECT_THROW(open_auth_request(key, "session", "database", changed), AuthWireError);

  const auto reply = seal_auth_reply(key, request.context, 200, {{"accepted", true}});
  EXPECT_TRUE(open_auth_reply(key, request.context, 200, reply).at("accepted"));
  EXPECT_THROW(open_auth_reply(key, request.context, 401, reply), AuthWireError);
  const auto other = seal_auth_request(key, "session", "database", {{"token", "private"}});
  EXPECT_THROW(open_auth_reply(key, other.context, 200, reply), AuthWireError);
  EXPECT_THROW(open_auth_reply(key, request.context, 200, request.envelope), AuthWireError);

  AuthReplayGuard guard(1);
  EXPECT_TRUE(guard.accept(request.context.request_id));
  EXPECT_FALSE(guard.accept(request.context.request_id));
  EXPECT_FALSE(guard.accept(other.context.request_id));
}

} // namespace
} // namespace chronicles
