#include "services/login/login_service.hpp"

#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <csignal>
#include <spdlog/spdlog.h>
#include <thread>
#include <type_traits>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Request = LoginRouter::Request;
using Response = LoginRouter::Response;
using Router = LoginRouter;

enum class LoginWsChannel { auth };
struct Connections {
  std::atomic<unsigned> count{};
};

constexpr std::size_t kMessageBodyLimit = 65536;
constexpr std::uint32_t kHttpHeaderLimit = 8192;
constexpr auto kHttpTimeout = std::chrono::seconds(15);

Response error_response(http::status status, const std::string &code) {
  Response response(status, 11);
  response.set(http::field::content_type, "application/json");
  response.set(http::field::cache_control, "no-store");
  response.body() = nlohmann::json{{"error", code}}.dump();
  response.prepare_payload();
  return response;
}

std::string bearer_token(const Request &request) {
  const auto header = request[http::field::authorization];
  if (!header.starts_with("Bearer ") || header.size() > 4103) {
    return {};
  }
  return std::string(header.substr(7));
}

bool requires_auth_work(std::string_view path) {
  return path.starts_with("/emulator/v1/") || path.starts_with("/launch/") ||
         path.starts_with("/api/auth/") || path == "/native/bootstrap";
}

template <class Stream>
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession<Stream>> {
public:
  WebSocketSession(Stream stream, Router &router, asio::thread_pool &workers, std::string token,
                   std::shared_ptr<Connections> connections, std::optional<LoginWsChannel>,
                   std::string peer_address)
      : socket_(std::move(stream)), router_(router), workers_(workers), token_(std::move(token)),
        connections_(std::move(connections)), peer_address_(std::move(peer_address)) {}

  ~WebSocketSession() {
    --connections_->count;
  }

  void run(Request request) {
    beast::get_lowest_layer(socket_).expires_never();
    socket_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    socket_.read_message_max(1024 * 1024);
    request_ = std::move(request);
    socket_.async_accept(request_, [self = this->shared_from_this()](beast::error_code error) {
      if (!error) {
        self->read();
      }
    });
  }

private:
  void read() {
    socket_.async_read(buffer_, [self = this->shared_from_this()](beast::error_code error,
                                                                  std::size_t) {
      if (error) {
        return;
      }
      if (self->socket_.got_text()) {
        self->close();
        return;
      }
      auto message = beast::buffers_to_string(self->buffer_.data());
      self->buffer_.consume(self->buffer_.size());
      auto permit = self->router_.try_auth_work(self->peer_address_);
      if (!permit) {
        self->close();
        return;
      }
      asio::post(self->workers_, [self, message = std::move(message), permit = std::move(permit)] {
        try {
          auto response = self->router_.auth_frame(self->token_, message);
          if (response.size() > 1024 * 1024) {
            throw std::runtime_error("Login WebSocket reply exceeds the frame bound");
          }
          asio::post(self->socket_.get_executor(), [self, response = std::move(response)] {
            self->reply_ = response;
            self->socket_.binary(true);
            self->socket_.async_write(asio::buffer(self->reply_),
                                      [self](beast::error_code write_error, std::size_t) {
                                        if (!write_error) {
                                          self->read();
                                        }
                                      });
          });
        } catch (const std::exception &) {
          asio::post(self->socket_.get_executor(), [self] {
            self->close();
          });
        }
      });
    });
  }

  void close() {
    socket_.async_close(websocket::close_code::policy_error,
                        [self = this->shared_from_this()](beast::error_code) {
                        });
  }

  websocket::stream<Stream> socket_;
  Router &router_;
  asio::thread_pool &workers_;
  std::string token_;
  std::string peer_address_;
  std::shared_ptr<Connections> connections_;
  beast::flat_buffer buffer_;
  Request request_;
  std::string reply_;
};

template <class Stream>
class HttpSession : public std::enable_shared_from_this<HttpSession<Stream>> {
public:
  HttpSession(Stream stream, Router &router, asio::thread_pool &workers,
              asio::thread_pool &auth_workers, std::shared_ptr<Connections> connections,
              bool private_api)
      : stream_(std::move(stream)), router_(router), workers_(workers), auth_workers_(auth_workers),
        connections_(std::move(connections)), private_api_(private_api) {
    parser_.body_limit(private_api_ ? kMaximumAuthWireBytes : kMessageBodyLimit);
    parser_.header_limit(kHttpHeaderLimit);
  }

  ~HttpSession() {
    if (connections_) {
      --connections_->count;
    }
  }

  void run() {
    beast::get_lowest_layer(stream_).expires_after(kHttpTimeout);
    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
      read_request();
    } else {
      stream_.async_handshake(asio::ssl::stream_base::server,
                              [self = this->shared_from_this()](beast::error_code error) {
                                if (!error) {
                                  self->read_request();
                                }
                              });
    }
  }

private:
  void read_request() {
    beast::get_lowest_layer(stream_).expires_after(kHttpTimeout);
    http::async_read(stream_, buffer_, parser_,
                     [self = this->shared_from_this()](beast::error_code error, std::size_t) {
                       self->on_read(error);
                     });
  }

  void on_read(beast::error_code error) {
    if (error) {
      return;
    }

    auto request = parser_.release();
    const auto target = request.target();
    auto path = std::string_view(target.data(), target.size());
    path = path.substr(0, path.find('?'));
    beast::error_code endpoint_error;
    const auto peer = beast::get_lowest_layer(stream_).socket().remote_endpoint(endpoint_error);
    const bool peer_loopback = !endpoint_error && peer.address().is_loopback();
    const auto peer_address = endpoint_error ? std::string("unknown") : peer.address().to_string();
    const bool private_path = path.starts_with("/internal/auth/v1/");
    if (private_api_ != private_path) {
      write(error_response(http::status::not_found, "route_not_found"));
      return;
    }
    const bool native_auth = !private_api_ && path == "/ws/auth";
    if (websocket::is_upgrade(request) && native_auth) {
      std::optional<LoginWsChannel> native_channel;
      if (native_auth) {
        native_channel = LoginWsChannel::auth;
      }

      upgrade(std::move(request), native_channel, peer_address);
      return;
    }

    const bool expensive_auth = private_api_ || requires_auth_work(path);
    auto permit = expensive_auth ? router_.try_auth_work(peer_address, private_api_) : nullptr;
    if (expensive_auth && !permit) {
      auto response = error_response(http::status::service_unavailable, "authentication_busy");
      response.set(http::field::retry_after, "1");
      write(std::move(response));
      return;
    }
    auto &pool = private_api_ ? workers_ : (expensive_auth ? auth_workers_ : workers_);
    asio::post(pool, [self = this->shared_from_this(), request = std::move(request), peer_loopback,
                      peer_address, permit = std::move(permit)] {
      if (auto patch = self->router_.patch_blob(request)) {
        asio::post(self->stream_.get_executor(), [self, response = std::move(*patch)]() mutable {
          self->write(std::move(response));
        });
        return;
      }
      auto response = self->router_.handle(request, peer_loopback, peer_address);
      asio::post(self->stream_.get_executor(), [self, response = std::move(response)]() mutable {
        self->write(std::move(response));
      });
    });
  }

  void upgrade(Request request, std::optional<LoginWsChannel> native_channel,
               const std::string &peer_address) {
    auto permit = router_.try_auth_work(peer_address, private_api_);
    if (!permit) {
      auto response = error_response(http::status::service_unavailable, "authentication_busy");
      response.set(http::field::retry_after, "1");
      write(std::move(response));
      return;
    }
    auto token = bearer_token(request);
    // Local session handoff can verify a password; keep that work off the network executor.
    asio::post(auth_workers_, [self = this->shared_from_this(), request = std::move(request),
                               token = std::move(token), native_channel, peer_address,
                               permit = std::move(permit)]() mutable {
      bool authenticated{};
      try {
        authenticated = self->router_.authenticated(token, native_channel.has_value());
      } catch (const std::exception &) {
        spdlog::error("WebSocket session authentication failed");
      }

      asio::post(self->stream_.get_executor(), [self, request = std::move(request),
                                                token = std::move(token), native_channel,
                                                authenticated, peer_address]() mutable {
        if (!authenticated) {
          self->write(error_response(http::status::unauthorized, "authentication_required"));
          return;
        }

        // Transfer the connection count to the upgraded session.
        std::make_shared<WebSocketSession<Stream>>(
            std::move(self->stream_), self->router_, self->auth_workers_, std::move(token),
            std::move(self->connections_), native_channel, peer_address)
            ->run(std::move(request));
      });
    });
  }

  template <class Body> void write(http::response<Body> response) {
    response.keep_alive(false);
    auto outgoing = std::make_shared<http::response<Body>>(std::move(response));
    auto serializer = std::make_shared<http::response_serializer<Body>>(*outgoing);
    write_chunk(std::move(outgoing), std::move(serializer));
  }

  template <class Body>
  void write_chunk(std::shared_ptr<http::response<Body>> outgoing,
                   std::shared_ptr<http::response_serializer<Body>> serializer) {
    // Reset the deadline after progress, so large patch blobs do not need an unbounded timeout.
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(60));
    http::async_write_some(
        stream_, *serializer,
        [self = this->shared_from_this(), outgoing, serializer](beast::error_code error,
                                                                std::size_t) {
          if (error) {
            return;
          }
          if (!serializer->is_done()) {
            self->write_chunk(outgoing, serializer);
            return;
          }
          if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
            beast::error_code shutdown_error;
            beast::get_lowest_layer(self->stream_)
                .socket()
                .shutdown(tcp::socket::shutdown_send, shutdown_error);
          } else {
            beast::get_lowest_layer(self->stream_).expires_after(std::chrono::seconds(5));
            self->stream_.async_shutdown([self](beast::error_code) {
            });
          }
        });
  }

  Stream stream_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  Router &router_;
  asio::thread_pool &workers_;
  asio::thread_pool &auth_workers_;
  std::shared_ptr<Connections> connections_;
  bool private_api_{};
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context &io, tcp::endpoint endpoint, Router &router, asio::thread_pool &workers,
           asio::thread_pool &auth_workers, std::shared_ptr<asio::ssl::context> tls_context,
           unsigned max_connections, bool private_api)
      : acceptor_(io), router_(router), workers_(workers), auth_workers_(auth_workers),
        connections_(std::make_shared<Connections>()), tls_context_(std::move(tls_context)),
        max_connections_(max_connections), private_api_(private_api) {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
  }

  unsigned short port() const {
    return acceptor_.local_endpoint().port();
  }

  void run() {
    acceptor_.async_accept(
        asio::make_strand(acceptor_.get_executor()),
        [self = this->shared_from_this()](beast::error_code error, tcp::socket socket) {
          if (!error) {
            if (self->connections_->count.fetch_add(1) < self->max_connections_) {
              if (self->tls_context_) {
                using SecureStream = beast::ssl_stream<beast::tcp_stream>;
                auto stream = SecureStream(std::move(socket), *self->tls_context_);
                std::make_shared<HttpSession<SecureStream>>(std::move(stream), self->router_,
                                                            self->workers_, self->auth_workers_,
                                                            self->connections_, self->private_api_)
                    ->run();
              } else {
                std::make_shared<HttpSession<beast::tcp_stream>>(
                    beast::tcp_stream(std::move(socket)), self->router_, self->workers_,
                    self->auth_workers_, self->connections_, self->private_api_)
                    ->run();
              }
            } else {
              --self->connections_->count;
            }
          }
          if (self->acceptor_.is_open()) {
            self->run();
          }
        });
  }

private:
  tcp::acceptor acceptor_;
  Router &router_;
  asio::thread_pool &workers_;
  asio::thread_pool &auth_workers_;
  std::shared_ptr<Connections> connections_;
  std::shared_ptr<asio::ssl::context> tls_context_;
  unsigned max_connections_;
  bool private_api_{};
};

} // namespace

void run_login_listeners(const LoginListenerOptions &options, LoginRouter &router) {
  asio::io_context context;
  asio::thread_pool workers(16);
  asio::thread_pool auth_workers(4);
  std::shared_ptr<asio::ssl::context> tls;
  if (!options.certificate_file.empty()) {
    tls = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    tls->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                     asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                     asio::ssl::context::no_tlsv1_1);
    tls->use_certificate_chain_file(options.certificate_file);
    tls->use_private_key_file(options.private_key_file, asio::ssl::context::pem);
  }
  auto public_listener = std::make_shared<Listener>(
      context, tcp::endpoint(asio::ip::make_address(options.bind_address), options.port), router,
      workers, auth_workers, tls, options.maximum_connections, false);
  auto private_listener = std::make_shared<Listener>(
      context, tcp::endpoint(asio::ip::address_v4::loopback(), options.authority_port), router,
      workers, auth_workers, nullptr, 256, true);
  public_listener->run();
  private_listener->run();
  spdlog::info("Login service listening on port {}; private authority on loopback port {}",
               public_listener->port(), private_listener->port());

  asio::signal_set signals(context, SIGINT, SIGTERM);
  signals.async_wait([&context](beast::error_code, int) {
    context.stop();
  });
  std::jthread second_io([&context] {
    context.run();
  });
  context.run();
  second_io.join();
  auth_workers.join();
  workers.join();
}

} // namespace chronicles
