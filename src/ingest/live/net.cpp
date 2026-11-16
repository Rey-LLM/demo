#include "ingest/live/net.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kNoSocket = INVALID_SOCKET;
// select() rather than WSAPoll: WSAPoll does not report a failed non-blocking
// connect on several Windows versions (and Wine), select's except set does.
// Returns > 0 when ready, 0 on timeout, < 0 on error (failed connect).
static int wait_socket(socket_t s, bool forWrite, int ms) {
  fd_set rd, wr, ex;
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET(s, forWrite ? &wr : &rd);
  FD_SET(s, &ex);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  const int n = ::select(0, &rd, &wr, &ex, &tv);
  if (n <= 0) return n;
  return FD_ISSET(s, &ex) ? -1 : 1;
}
static void close_socket(socket_t s) { closesocket(s); }
static bool set_nonblocking(socket_t s, bool on) {
  u_long m = on ? 1 : 0;
  return ioctlsocket(s, FIONBIO, &m) == 0;
}
static bool connect_in_progress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_t = int;
static constexpr socket_t kNoSocket = -1;
static int wait_socket(socket_t s, bool forWrite, int ms) {
  pollfd p{};
  p.fd = s;
  p.events = forWrite ? POLLOUT : POLLIN;
  return ::poll(&p, 1, ms);
}
static void close_socket(socket_t s) { ::close(s); }
static bool set_nonblocking(socket_t s, bool on) {
  const int fl = fcntl(s, F_GETFL, 0);
  return fl >= 0 && fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) == 0;
}
static bool connect_in_progress() { return errno == EINPROGRESS; }
#endif

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if defined(_WIN32)
#include <wincrypt.h>
#else
#include <sys/stat.h>
#endif

namespace ftv::net {

const char* net_error_name(NetError e) noexcept {
  switch (e) {
    case NetError::None: return "ok";
    case NetError::Resolve: return "dns";
    case NetError::Connect: return "connect";
    case NetError::Proxy: return "proxy refused tunnel";
    case NetError::Tls: return "tls";
    case NetError::Handshake: return "websocket handshake";
    case NetError::RateLimited: return "http 429";
    case NetError::HttpStatus: return "http status";
    case NetError::Timeout: return "timeout";
    case NetError::Closed: return "closed";
    case NetError::Protocol: return "protocol";
    case NetError::TooLarge: return "too large";
    case NetError::GeoBlocked: return "region restricted (http 451/403)";
  }
  return "?";
}

bool parse_url(std::string_view url, Url& out) {
  out = Url{};
  std::size_t at = url.find("://");
  if (at == std::string_view::npos) return false;
  const std::string_view scheme = url.substr(0, at);
  if (scheme == "wss" || scheme == "https") {
    out.tls = true;
    out.port = 443;
  } else if (scheme == "ws" || scheme == "http") {
    out.tls = false;
    out.port = 80;
  } else {
    return false;
  }
  std::string_view rest = url.substr(at + 3);
  // Plain loops: GCC 13's LTO flags string_view::find here with a bogus
  // -Wstringop-overread (MinGW builds).
  std::size_t slash = std::string_view::npos;
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '/') {
      slash = i;
      break;
    }
  }
  std::string_view hostport = rest.substr(0, slash);
  out.path = slash == std::string_view::npos ? std::string("/") : std::string(rest.substr(slash));
  std::size_t colon = std::string_view::npos;
  for (std::size_t i = 0; i < hostport.size(); ++i) {
    if (hostport[i] == ':') {
      colon = i;
      break;
    }
  }
  if (colon != std::string_view::npos) {
    unsigned p = 0;
    const auto r = std::from_chars(hostport.data() + colon + 1, hostport.data() + hostport.size(), p);
    if (r.ec != std::errc() || p == 0 || p > 65535) return false;
    out.port = static_cast<std::uint16_t>(p);
    hostport = hostport.substr(0, colon);
  }
  if (hostport.empty()) return false;
  out.host.assign(hostport);
  return true;
}

// ---------------------------------------------------------------------------
// TLS stream
// ---------------------------------------------------------------------------

namespace {

// Trust anchors. A statically linked OpenSSL only knows the CA directory of
// the machine it was built on, so the platform's own store is added:
// Windows: every certificate in the system ROOT store (the same anchors the
// browser and WinHTTP use). Linux and macOS: the first CA bundle that exists
// among the distributions' standard locations.
int add_platform_roots(SSL_CTX* ctx) {
  int added = 0;
#if defined(_WIN32)
  X509_STORE* store = SSL_CTX_get_cert_store(ctx);
  HCERTSTORE sys = CertOpenSystemStoreW(0, L"ROOT");
  if (!sys) return 0;
  PCCERT_CONTEXT c = nullptr;
  while ((c = CertEnumCertificatesInStore(sys, c)) != nullptr) {
    const unsigned char* der = c->pbCertEncoded;
    X509* x = d2i_X509(nullptr, &der, static_cast<long>(c->cbCertEncoded));
    if (!x) continue;
    if (X509_STORE_add_cert(store, x) == 1) ++added;
    X509_free(x);
  }
  CertCloseStore(sys, 0);
  ERR_clear_error();  // duplicates report "cert already in hash table"
#else
  static const char* const kBundles[] = {
      "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Alpine, Gentoo
      "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL, CentOS
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // Fedora, RHEL (extracted)
      "/etc/ssl/ca-bundle.pem",                             // openSUSE
      "/etc/ssl/cert.pem",                                  // macOS, Alpine, OpenBSD
  };
  for (const char* b : kBundles) {
    struct stat st {};
    if (::stat(b, &st) == 0 && SSL_CTX_load_verify_locations(ctx, b, nullptr) == 1) {
      added = 1;
      break;
    }
  }
  ERR_clear_error();
#endif
  return added;
}

// Winsock must be initialised before the first getaddrinfo()/socket() call,
// which happens before any TLS context exists.
void net_init() {
#if defined(_WIN32)
  static const bool started = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)started;
#endif
}

struct SslGlobal {
  SSL_CTX* ctx = nullptr;
  SslGlobal() {
    net_init();
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);  // TLS verification is always on
    SSL_CTX_set_default_verify_paths(ctx);              // honours SSL_CERT_FILE / SSL_CERT_DIR
    add_platform_roots(ctx);
    if (const char* f = std::getenv("SSL_CERT_FILE")) SSL_CTX_load_verify_locations(ctx, f, nullptr);
    ERR_clear_error();
  }
};

SSL_CTX* ssl_ctx() {
  static SslGlobal g;  // thread-safe static initialisation
  return g.ctx;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

// Proxy from HTTPS_PROXY / https_proxy unless the host matches NO_PROXY.
bool proxy_for(const std::string& host, Url& proxy) {
  const char* p = std::getenv("HTTPS_PROXY");
  if (!p || !*p) p = std::getenv("https_proxy");
  if (!p || !*p) return false;
  const char* np = std::getenv("NO_PROXY");
  if (!np) np = std::getenv("no_proxy");
  if (np) {
    const std::string h = lower(host);
    std::string_view list(np);
    while (!list.empty()) {
      const std::size_t c = list.find(',');
      std::string_view item = list.substr(0, c);
      while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
      if (!item.empty() && item.front() == '*') item.remove_prefix(1);
      if (!item.empty() && item.front() == '.') item.remove_prefix(1);
      const std::string it = lower(item);
      if (!it.empty() && (h == it || (h.size() > it.size() && h.compare(h.size() - it.size(), it.size(), it) == 0 &&
                                       h[h.size() - it.size() - 1] == '.'))) {
        return false;
      }
      if (c == std::string_view::npos) break;
      list.remove_prefix(c + 1);
    }
  }
  std::string s(p);
  if (s.find("://") == std::string::npos) s = "http://" + s;
  if (!parse_url(s, proxy)) return false;
  if (s.rfind("http://", 0) == 0 && s.find(':', 7) == std::string::npos) proxy.port = 80;
  return true;
}

}  // namespace

struct TlsStream::Impl {
  socket_t fd = kNoSocket;
  SSL* ssl = nullptr;
  bool tls = true;

  void reset() {
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
      ssl = nullptr;
    }
    if (fd != kNoSocket) {
      close_socket(fd);
      fd = kNoSocket;
    }
  }

  bool wait(bool forWrite, int ms) {
    return wait_socket(fd, forWrite, ms) > 0;
  }

  // Plain (pre-TLS) I/O, used for the proxy CONNECT exchange.
  NetError raw_write(const char* d, std::size_t n, int ms) {
    while (n > 0) {
      if (!wait(true, ms)) return NetError::Timeout;
      const auto w = ::send(fd, d, static_cast<int>(n), 0);
      if (w <= 0) return NetError::Closed;
      d += w;
      n -= static_cast<std::size_t>(w);
    }
    return NetError::None;
  }
};

TlsStream::TlsStream() : impl_(std::make_unique<Impl>()) {}
TlsStream::~TlsStream() { close(); }
void TlsStream::close() noexcept { impl_->reset(); }
bool TlsStream::is_open() const noexcept { return impl_->fd != kNoSocket; }

NetError TlsStream::connect(const Url& url, int timeoutMs, std::string& detail) {
  net_init();
  close();
  Impl& m = *impl_;
  m.tls = url.tls;
  Url proxy;
  const bool viaProxy = proxy_for(url.host, proxy);
  const std::string& host = viaProxy ? proxy.host : url.host;
  const std::uint16_t port = viaProxy ? proxy.port : url.port;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
    detail = "cannot resolve " + host;
    return NetError::Resolve;
  }
  NetError err = NetError::Connect;
  for (addrinfo* a = res; a; a = a->ai_next) {
    socket_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kNoSocket) continue;
    set_nonblocking(s, true);
    const int rc = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
    bool ok = rc == 0;
    if (!ok && connect_in_progress()) {
      if (wait_socket(s, true, timeoutMs) > 0) {
        int so = 0;
        socklen_t sl = sizeof(so);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl);
        ok = so == 0;
      }
    }
    if (ok) {
      m.fd = s;
      err = NetError::None;
      break;
    }
    close_socket(s);
  }
  freeaddrinfo(res);
  if (err != NetError::None) {
    detail = "tcp connect to " + host + ":" + portStr + " failed";
    return err;
  }
  int one = 1;
  setsockopt(m.fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

  if (viaProxy) {
    const std::string req = "CONNECT " + url.host + ":" + std::to_string(url.port) + " HTTP/1.1\r\nHost: " + url.host +
                            ":" + std::to_string(url.port) + "\r\n\r\n";
    if (m.raw_write(req.data(), req.size(), timeoutMs) != NetError::None) {
      detail = "proxy write failed";
      close();
      return NetError::Proxy;
    }
    std::string resp;
    char c = 0;
    const std::int64_t deadline = now_ms() + timeoutMs;
    while (resp.size() < 8192 && resp.find("\r\n\r\n") == std::string::npos) {
      const int left = static_cast<int>(deadline - now_ms());
      if (left <= 0 || !m.wait(false, left) || ::recv(m.fd, &c, 1, 0) != 1) break;
      resp.push_back(c);
    }
    const std::size_t sp = resp.find(' ');
    const int code = sp != std::string::npos ? std::atoi(resp.c_str() + sp + 1) : 0;
    if (code != 200) {
      detail = "proxy CONNECT " + url.host + " answered " + (code ? std::to_string(code) : std::string("nothing"));
      close();
      return NetError::Proxy;
    }
  }

  if (!url.tls) {
    set_nonblocking(m.fd, true);
    return NetError::None;
  }
  SSL_CTX* ctx = ssl_ctx();
  if (!ctx) {
    detail = "no TLS context";
    close();
    return NetError::Tls;
  }
  m.ssl = SSL_new(ctx);
  SSL_set_fd(m.ssl, static_cast<int>(m.fd));
  SSL_set_tlsext_host_name(m.ssl, url.host.c_str());  // SNI
  SSL_set1_host(m.ssl, url.host.c_str());             // certificate must match the host name
  const std::int64_t deadline = now_ms() + timeoutMs;
  for (;;) {
    const int rc = SSL_connect(m.ssl);
    if (rc == 1) break;
    const int e = SSL_get_error(m.ssl, rc);
    const int left = static_cast<int>(deadline - now_ms());
    if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && left > 0 && m.wait(e == SSL_ERROR_WANT_WRITE, left)) {
      continue;
    }
    const long vr = SSL_get_verify_result(m.ssl);
    detail = vr != X509_V_OK ? std::string("certificate verification failed: ") + X509_verify_cert_error_string(vr)
                             : std::string("tls handshake failed");
    close();
    return NetError::Tls;
  }
  return NetError::None;
}

NetError TlsStream::write_all(const void* data, std::size_t len, int timeoutMs) {
  Impl& m = *impl_;
  if (m.fd == kNoSocket) return NetError::Closed;
  const char* p = static_cast<const char*>(data);
  const std::int64_t deadline = now_ms() + timeoutMs;
  while (len > 0) {
    int w;
    if (m.ssl) {
      w = SSL_write(m.ssl, p, static_cast<int>(std::min<std::size_t>(len, 1 << 20)));
      if (w <= 0) {
        const int e = SSL_get_error(m.ssl, w);
        const int left = static_cast<int>(deadline - now_ms());
        if ((e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) && left > 0 &&
            m.wait(e == SSL_ERROR_WANT_WRITE, left)) {
          continue;
        }
        return NetError::Closed;
      }
    } else {
      const int left = static_cast<int>(deadline - now_ms());
      if (left <= 0 || !m.wait(true, left)) return NetError::Timeout;
      w = static_cast<int>(::send(m.fd, p, static_cast<int>(len), 0));
      if (w <= 0) return NetError::Closed;
    }
    p += w;
    len -= static_cast<std::size_t>(w);
  }
  return NetError::None;
}

NetError TlsStream::read_some(void* data, std::size_t len, std::size_t& n, int timeoutMs) {
  Impl& m = *impl_;
  n = 0;
  if (m.fd == kNoSocket) return NetError::Closed;
  if (m.ssl) {
    const std::int64_t deadline = now_ms() + timeoutMs;
    for (;;) {
      const int r = SSL_read(m.ssl, data, static_cast<int>(std::min<std::size_t>(len, 1 << 20)));
      if (r > 0) {
        n = static_cast<std::size_t>(r);
        return NetError::None;
      }
      const int e = SSL_get_error(m.ssl, r);
      if (e == SSL_ERROR_ZERO_RETURN) return NetError::Closed;
      if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return NetError::Closed;
      const int left = static_cast<int>(deadline - now_ms());
      if (left <= 0 || !m.wait(e == SSL_ERROR_WANT_WRITE, left)) return NetError::Timeout;
    }
  }
  if (!m.wait(false, timeoutMs)) return NetError::Timeout;
  const auto r = ::recv(m.fd, static_cast<char*>(data), static_cast<int>(len), 0);
  if (r <= 0) return NetError::Closed;
  n = static_cast<std::size_t>(r);
  return NetError::None;
}

// ---------------------------------------------------------------------------
// Helpers: base64 and SHA-1 for the WebSocket handshake
// ---------------------------------------------------------------------------

namespace {

std::string base64(const unsigned char* d, std::size_t n) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < n; i += 3) {
    const unsigned v = (unsigned(d[i]) << 16) | (i + 1 < n ? unsigned(d[i + 1]) << 8 : 0u) | (i + 2 < n ? d[i + 2] : 0u);
    out.push_back(k[(v >> 18) & 63]);
    out.push_back(k[(v >> 12) & 63]);
    out.push_back(i + 1 < n ? k[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < n ? k[v & 63] : '=');
  }
  return out;
}

std::string ws_accept(const std::string& key) {
  const std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int mdLen = 0;
  EVP_Digest(s.data(), s.size(), md, &mdLen, EVP_sha1(), nullptr);
  return base64(md, mdLen);
}

// Reads an HTTP response head (status line + headers) byte by byte; any bytes
// after the head stay in the stream.
NetError read_head(TlsStream& s, std::string& head, int timeoutMs) {
  head.clear();
  const std::int64_t deadline = now_ms() + timeoutMs;
  char c;
  while (head.size() < 16384) {
    std::size_t n = 0;
    const int left = static_cast<int>(deadline - now_ms());
    if (left <= 0) return NetError::Timeout;
    const NetError e = s.read_some(&c, 1, n, left);
    if (e != NetError::None) return e;
    head.push_back(c);
    if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) return NetError::None;
  }
  return NetError::Protocol;
}

int status_of(const std::string& head) {
  const std::size_t sp = head.find(' ');
  return sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
}

std::string header_value(const std::string& head, std::string_view name) {
  const std::string h = lower(head);
  const std::string n = "\r\n" + lower(name) + ":";
  const std::size_t at = h.find(n);
  if (at == std::string::npos) return {};
  std::size_t b = at + n.size();
  while (b < head.size() && head[b] == ' ') ++b;
  const std::size_t e = head.find("\r\n", b);
  return head.substr(b, e - b);
}

}  // namespace

// ---------------------------------------------------------------------------
// WebSocket (RFC 6455), client side
// ---------------------------------------------------------------------------

WebSocket::WebSocket(std::size_t maxMessage) : max_message_(maxMessage) {}

NetError WebSocket::open(const Url& url, int timeoutMs, std::string& detail) {
  NetError e = stream_.connect(url, timeoutMs, detail);
  if (e != NetError::None) return e;
  unsigned char nonce[16];
  std::random_device rd;
  for (auto& b : nonce) b = static_cast<unsigned char>(rd());
  mask_state_ ^= (static_cast<std::uint64_t>(rd()) << 32) | rd();
  const std::string key = base64(nonce, 16);
  const std::string hostHdr = url.host + ((url.port == 443 || url.port == 80) ? "" : ":" + std::to_string(url.port));
  const std::string req = "GET " + url.path + " HTTP/1.1\r\nHost: " + hostHdr +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ftviz\r\n\r\n";
  if ((e = stream_.write_all(req.data(), req.size(), timeoutMs)) != NetError::None) {
    detail = "handshake write failed";
    return e;
  }
  std::string head;
  if ((e = read_head(stream_, head, timeoutMs)) != NetError::None) {
    detail = "no handshake response";
    stream_.close();
    return e;
  }
  const int status = status_of(head);
  if (status == 429) {
    detail = "http 429 on upgrade";
    stream_.close();
    return NetError::RateLimited;
  }
  if (status == 451 || status == 403) {
    // 451 Unavailable For Legal Reasons is how Binance refuses restricted
    // regions; a 403 on the upgrade comes from the venue (a proxy refusal
    // fails earlier, at CONNECT). The adapter decides after trying every URL.
    detail = "http " + std::to_string(status) + " on upgrade";
    stream_.close();
    return NetError::GeoBlocked;
  }
  if (status != 101 || header_value(head, "Sec-WebSocket-Accept") != ws_accept(key)) {
    detail = "upgrade refused (http " + std::to_string(status) + ")";
    stream_.close();
    return NetError::Handshake;
  }
  rx_cap_ = max_message_ + 16;
  if (!rx_) rx_ = std::make_unique<char[]>(rx_cap_);
  rx_len_ = rx_pos_ = msg_len_ = 0;
  return NetError::None;
}

void WebSocket::close() noexcept {
  if (stream_.is_open()) {
    const unsigned char code[2] = {0x03, 0xE8};  // 1000 normal closure
    (void)send_frame(0x8, code, 2, 200);
  }
  stream_.close();
}

NetError WebSocket::send_frame(std::uint8_t opcode, const void* data, std::size_t len, int timeoutMs) {
  unsigned char hdr[14];
  std::size_t h = 0;
  hdr[h++] = static_cast<unsigned char>(0x80 | opcode);
  if (len < 126) {
    hdr[h++] = static_cast<unsigned char>(0x80 | len);
  } else if (len < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = static_cast<unsigned char>(len >> 8);
    hdr[h++] = static_cast<unsigned char>(len);
  } else {
    hdr[h++] = 0x80 | 127;
    for (int i = 7; i >= 0; --i) hdr[h++] = static_cast<unsigned char>(static_cast<std::uint64_t>(len) >> (8 * i));
  }
  // Client frames are masked (xorshift: masking is not a security feature).
  mask_state_ ^= mask_state_ << 13;
  mask_state_ ^= mask_state_ >> 7;
  mask_state_ ^= mask_state_ << 17;
  unsigned char mask[4];
  for (int i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(mask_state_ >> (8 * i));
  std::memcpy(hdr + h, mask, 4);
  h += 4;
  std::string frame(reinterpret_cast<const char*>(hdr), h);
  frame.resize(h + len);
  const auto* src = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < len; ++i) frame[h + i] = static_cast<char>(src[i] ^ mask[i & 3]);
  return stream_.write_all(frame.data(), frame.size(), timeoutMs);
}

NetError WebSocket::send_text(std::string_view text, int timeoutMs) {
  return send_frame(0x1, text.data(), text.size(), timeoutMs);
}

NetError WebSocket::fill(std::size_t need, int timeoutMs) {
  // Ensures rx_[rx_pos_, rx_pos_ + need) is buffered.
  if (rx_pos_ > 0 && rx_len_ - rx_pos_ + need > rx_cap_ - rx_pos_) {
    std::memmove(rx_.get(), rx_.get() + rx_pos_, rx_len_ - rx_pos_);
    rx_len_ -= rx_pos_;
    rx_pos_ = 0;
  }
  if (rx_pos_ + need > rx_cap_) return NetError::TooLarge;
  const std::int64_t deadline = now_ms() + timeoutMs;
  while (rx_len_ - rx_pos_ < need) {
    std::size_t n = 0;
    const int left = std::max(0, static_cast<int>(deadline - now_ms()));
    const NetError e = stream_.read_some(rx_.get() + rx_len_, rx_cap_ - rx_len_, n, left);
    if (e != NetError::None) return e;
    rx_len_ += n;
  }
  return NetError::None;
}

NetError WebSocket::recv(char* out, std::size_t capacity, std::size_t& len, int timeoutMs) {
  len = 0;
  const std::int64_t deadline = now_ms() + timeoutMs;
  for (;;) {
    const int left = std::max(0, static_cast<int>(deadline - now_ms()));
    // Frame header: 2 bytes, then extended length and (server frames: no) mask.
    NetError e = fill(2, left);
    if (e != NetError::None) return e;
    const auto* b = reinterpret_cast<const unsigned char*>(rx_.get() + rx_pos_);
    const bool fin = (b[0] & 0x80) != 0;
    const std::uint8_t op = b[0] & 0x0F;
    const bool masked = (b[1] & 0x80) != 0;
    std::uint64_t plen = b[1] & 0x7F;
    std::size_t hlen = 2;
    if (plen == 126) hlen += 2;
    if (plen == 127) hlen += 8;
    if (masked) hlen += 4;
    if ((e = fill(hlen, std::max(0, static_cast<int>(deadline - now_ms())) + 2000)) != NetError::None) {
      return e == NetError::Timeout ? NetError::Protocol : e;
    }
    b = reinterpret_cast<const unsigned char*>(rx_.get() + rx_pos_);
    if (plen == 126) {
      plen = (std::uint64_t{b[2]} << 8) | b[3];
    } else if (plen == 127) {
      plen = 0;
      for (int i = 0; i < 8; ++i) plen = (plen << 8) | b[2 + i];
    }
    if (plen > max_message_ || msg_len_ + plen > capacity) return NetError::TooLarge;
    // The payload of a frame that has started arriving is read to the end
    // (with a grace period), so the stream never desynchronises.
    if ((e = fill(hlen + plen, std::max(0, static_cast<int>(deadline - now_ms())) + 5000)) != NetError::None) {
      return e == NetError::Timeout ? NetError::Protocol : e;
    }
    b = reinterpret_cast<const unsigned char*>(rx_.get() + rx_pos_);
    const unsigned char* payload = b + hlen;
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) std::memcpy(mask, b + hlen - 4, 4);
    rx_pos_ += hlen + plen;
    if (op == 0x9) {  // ping -> pong with the same payload
      std::string pl(reinterpret_cast<const char*>(payload), plen);
      if (masked) for (std::size_t i = 0; i < pl.size(); ++i) pl[i] = static_cast<char>(pl[i] ^ mask[i & 3]);
      (void)send_frame(0xA, pl.data(), pl.size(), 1000);
      continue;
    }
    if (op == 0xA) continue;  // pong
    if (op == 0x8) {
      stream_.close();
      return NetError::Closed;
    }
    if (op != 0x0 && op != 0x1 && op != 0x2) return NetError::Protocol;
    if (op != 0x0) msg_len_ = 0;  // a new data message starts
    for (std::size_t i = 0; i < plen; ++i) {
      out[msg_len_ + i] = static_cast<char>(masked ? payload[i] ^ mask[i & 3] : payload[i]);
    }
    msg_len_ += plen;
    if (fin) {
      len = msg_len_;
      msg_len_ = 0;
      return NetError::None;
    }
  }
}

// ---------------------------------------------------------------------------
// HTTPS JSON POST
// ---------------------------------------------------------------------------

HttpsClient::HttpsClient(std::size_t maxBody) : max_body_(maxBody) {}

NetError HttpsClient::post_json(const Url& url, std::string_view body, char* out, std::size_t capacity,
                                std::size_t& len, int& status, int timeoutMs, std::string& detail) {
  return request(true, url, body, out, capacity, len, status, timeoutMs, detail);
}

NetError HttpsClient::get(const Url& url, char* out, std::size_t capacity, std::size_t& len, int& status,
                          int timeoutMs, std::string& detail) {
  return request(false, url, std::string_view(), out, capacity, len, status, timeoutMs, detail);
}

NetError HttpsClient::request(bool post, const Url& url, std::string_view body, char* out, std::size_t capacity,
                              std::size_t& len, int& status, int timeoutMs, std::string& detail) {
  len = 0;
  status = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!stream_.is_open() || host_ != url.host) {
      host_ = url.host;
      const NetError e = stream_.connect(url, timeoutMs, detail);
      if (e != NetError::None) return e;
    }
    const std::string req =
        post ? "POST " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                   "\r\nContent-Type: application/json\r\nAccept: application/json\r\nUser-Agent: ftviz"
                   "\r\nConnection: keep-alive\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\n\r\n" + std::string(body)
             : "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                   "\r\nAccept: application/json\r\nUser-Agent: ftviz\r\nConnection: keep-alive\r\n\r\n";
    if (stream_.write_all(req.data(), req.size(), timeoutMs) != NetError::None) {
      stream_.close();
      continue;  // stale keep-alive connection: reconnect once
    }
    std::string head;
    NetError e = read_head(stream_, head, timeoutMs);
    if (e != NetError::None) {
      stream_.close();
      if (attempt == 0 && e == NetError::Closed) continue;
      detail = "no response";
      return e;
    }
    status = status_of(head);
    const std::string te = lower(header_value(head, "Transfer-Encoding"));
    const std::string cl = header_value(head, "Content-Length");
    const std::size_t limit = std::min(capacity, max_body_);
    auto read_exact = [&](char* dst, std::size_t n) -> NetError {
      while (n > 0) {
        std::size_t got = 0;
        const NetError r = stream_.read_some(dst, n, got, timeoutMs);
        if (r != NetError::None) return r;
        dst += got;
        n -= got;
      }
      return NetError::None;
    };
    if (te.find("chunked") != std::string::npos) {
      for (;;) {
        std::string line;
        char c;
        while (line.size() < 32) {
          std::size_t got = 0;
          if ((e = stream_.read_some(&c, 1, got, timeoutMs)) != NetError::None) return e;
          line.push_back(c);
          if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) break;
        }
        const std::size_t n = std::strtoul(line.c_str(), nullptr, 16);
        if (n == 0) {
          char crlf[2];
          read_exact(crlf, 2);
          break;
        }
        if (len + n > limit) {
          stream_.close();
          return NetError::TooLarge;
        }
        if ((e = read_exact(out + len, n)) != NetError::None) return e;
        len += n;
        char crlf[2];
        if ((e = read_exact(crlf, 2)) != NetError::None) return e;
      }
    } else if (!cl.empty()) {
      const std::size_t n = std::strtoul(cl.c_str(), nullptr, 10);
      if (n > limit) {
        stream_.close();
        return NetError::TooLarge;
      }
      if ((e = read_exact(out, n)) != NetError::None) return e;
      len = n;
    } else {
      stream_.close();
      return NetError::Protocol;
    }
    if (lower(header_value(head, "Connection")) == "close") stream_.close();
    if (status == 429) return NetError::RateLimited;
    if (status == 451) {
      detail = "http 451";
      return NetError::GeoBlocked;
    }
    if (status < 200 || status >= 300) {
      detail = "http " + std::to_string(status);
      return NetError::HttpStatus;
    }
    return NetError::None;
  }
  detail = "request failed";
  return NetError::Closed;
}

}  // namespace ftv::net
