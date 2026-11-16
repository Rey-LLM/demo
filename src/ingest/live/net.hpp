#pragma once
// Minimal network layer for the feed adapters: a TLS stream (OpenSSL, peer
// and host-name verification always on), an RFC 6455 WebSocket client and an
// HTTP/1.1 JSON POST, all with timeouts. HTTP CONNECT proxies from
// HTTPS_PROXY / https_proxy are honoured (NO_PROXY suffixes are bypassed).
//
// Everything here runs on feed threads only. Buffers are allocated when a
// connection opens, never per message.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ftv::net {

enum class NetError : std::uint8_t {
  None = 0,
  Resolve,        // DNS failure
  Connect,        // TCP connect failed or timed out
  Proxy,          // the HTTP proxy refused the CONNECT tunnel
  Tls,            // handshake or certificate verification failed
  Handshake,      // WebSocket upgrade refused
  RateLimited,    // HTTP 429
  HttpStatus,     // any other non-2xx status
  Timeout,        // no data within the timeout (not fatal for reads)
  Closed,         // peer closed the connection
  Protocol,       // malformed frame or response
  TooLarge,       // frame or body over the 8 MB limit
  GeoBlocked,     // HTTP 451 (or 403 from the venue itself): not available in this region
};
const char* net_error_name(NetError e) noexcept;

struct Url {
  std::string host;
  std::string path = "/";
  std::uint16_t port = 443;
  bool tls = true;
};
// Parses wss://, ws://, https://, http:// URLs. Returns false on error.
bool parse_url(std::string_view url, Url& out);

class TlsStream {
 public:
  TlsStream();
  ~TlsStream();
  TlsStream(const TlsStream&) = delete;
  TlsStream& operator=(const TlsStream&) = delete;

  NetError connect(const Url& url, int timeoutMs, std::string& detail);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  // Writes all bytes. Returns None or an error.
  NetError write_all(const void* data, std::size_t len, int timeoutMs);
  // Reads up to len bytes; n = 0 with Timeout when nothing arrived in time.
  NetError read_some(void* data, std::size_t len, std::size_t& n, int timeoutMs);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

class WebSocket {
 public:
  static constexpr std::size_t kMaxFrame = std::size_t{8} << 20;  // 8 MB max message

  explicit WebSocket(std::size_t maxMessage = kMaxFrame);
  NetError open(const Url& url, int timeoutMs, std::string& detail);
  void close() noexcept;
  NetError send_text(std::string_view text, int timeoutMs);
  // Receives one complete text or binary message into `out` (padded buffer
  // of `capacity` bytes). Control frames (ping/pong/close) are handled here.
  // Returns Timeout with len = 0 when no message is complete yet.
  NetError recv(char* out, std::size_t capacity, std::size_t& len, int timeoutMs);
  [[nodiscard]] bool is_open() const noexcept { return stream_.is_open(); }

 private:
  NetError fill(std::size_t need, int timeoutMs);
  NetError send_frame(std::uint8_t opcode, const void* data, std::size_t len, int timeoutMs);

  TlsStream stream_;
  std::size_t max_message_;
  std::unique_ptr<char[]> rx_;  // receive buffer (header + payload staging)
  std::size_t rx_cap_ = 0, rx_len_ = 0, rx_pos_ = 0;
  std::size_t msg_len_ = 0;     // bytes of a fragmented message assembled so far
  std::uint64_t mask_state_ = 0x2545F4914F6CDD1Dull;
};

// HTTP/1.1 requests (a JSON POST or a GET) on a kept-alive TLS connection.
// Redirects are not followed: a 3xx answer is NetError::HttpStatus.
class HttpsClient {
 public:
  explicit HttpsClient(std::size_t maxBody = std::size_t{8} << 20);
  // Sends `body` to url.path and reads the response body into `out`
  // (`capacity` bytes). `status` receives the HTTP status code.
  NetError post_json(const Url& url, std::string_view body, char* out, std::size_t capacity, std::size_t& len,
                     int& status, int timeoutMs, std::string& detail);
  // GET url.path; the response body goes to `out` like post_json.
  NetError get(const Url& url, char* out, std::size_t capacity, std::size_t& len, int& status, int timeoutMs,
               std::string& detail);
  void close() noexcept { stream_.close(); }

 private:
  NetError request(bool post, const Url& url, std::string_view body, char* out, std::size_t capacity,
                   std::size_t& len, int& status, int timeoutMs, std::string& detail);
  TlsStream stream_;
  std::size_t max_body_;
  std::string host_;
};

}  // namespace ftv::net
