// The WebSocket client over StreamSocket, against a real WebSocket server on a
// loopback port: plain and TLS, ping/pong, a close from the peer, a handshake
// that lies, and a port nobody listens on.
//
// The server is a std::thread with blocking sockets; the client runs on a
// phi::runtime loop with a watchdog so a hang fails instead of stalling ctest.

#include <phi/adapter/testing/check.h>

#include "webos_ws_client.h"

#include "phi/runtime/epollloop.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <csignal>
#include <cstdlib>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace phicore::webos::ipc;
using namespace std::chrono_literals;

namespace {

// ------------------------------------------------------------ certificate

struct SelfSigned {
    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;

    bool create()
    {
        key = EVP_RSA_gen(2048);
        cert = X509_new();
        if (!key || !cert)
            return false;
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME *name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"),
                                   -1, -1, 0);
        X509_set_issuer_name(cert, name);
        return X509_sign(cert, key, EVP_sha256()) > 0;
    }

    /// Lower-case hex SHA-256 of the DER certificate.
    std::string fingerprint() const
    {
        unsigned char *der = nullptr;
        const int length = i2d_X509(cert, &der);
        if (length <= 0)
            return {};
        unsigned char digest[SHA256_DIGEST_LENGTH];
        ::SHA256(der, static_cast<std::size_t>(length), digest);
        OPENSSL_free(der);
        std::string hex;
        char pair[3];
        for (unsigned char byte : digest) {
            std::snprintf(pair, sizeof(pair), "%02x", byte);
            hex += pair;
        }
        return hex;
    }

    ~SelfSigned()
    {
        if (cert)
            X509_free(cert);
        if (key)
            EVP_PKEY_free(key);
    }
};

// ----------------------------------------------------------------- server

std::string base64(const unsigned char *data, std::size_t length)
{
    std::string out(4 * ((length + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), data, static_cast<int>(length));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

/// One WebSocket server connection, blocking.
class Server
{
public:
    enum class Manner {
        Echo,         ///< echoes text as "echo:<text>"; "ping" -> pings back; "close" -> closes
        BadAccept,    ///< answers the upgrade with a wrong Sec-WebSocket-Accept
        CloseAtOnce   ///< completes the handshake, then sends a close frame
    };

    Server(Manner manner, const SelfSigned *tls = nullptr) : m_manner(manner)
    {
        if (tls) {
            m_ctx = SSL_CTX_new(TLS_server_method());
            if (!m_ctx || SSL_CTX_use_certificate(m_ctx, tls->cert) != 1
                || SSL_CTX_use_PrivateKey(m_ctx, tls->key) != 1)
                return;
        }
        m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(m_listen, 4) < 0)
            return;
        socklen_t len = sizeof(addr);
        ::getsockname(m_listen, reinterpret_cast<sockaddr *>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { serve(); });
    }

    ~Server()
    {
        m_done = true;
        if (m_listen >= 0) {
            ::shutdown(m_listen, SHUT_RDWR);
            ::close(m_listen);
        }
        const int fd = m_clientFd.load();
        if (fd >= 0)
            ::shutdown(fd, SHUT_RDWR);
        if (m_thread.joinable())
            m_thread.join();
        if (m_ctx)
            SSL_CTX_free(m_ctx);
    }

    bool ok() const { return m_port != 0; }
    std::uint16_t port() const { return m_port; }
    bool sawPong() const { return m_sawPong.load(); }
    bool sawClientClose() const { return m_sawClose.load(); }

private:
    int rawRead(char *buf, int cap)
    {
        return m_ssl ? SSL_read(m_ssl, buf, cap) : static_cast<int>(::recv(m_fd, buf, static_cast<std::size_t>(cap), 0));
    }

    bool readExact(std::string &out, std::size_t count)
    {
        while (m_in.size() < count) {
            char buf[4096];
            const int n = rawRead(buf, sizeof(buf));
            if (n <= 0)
                return false;
            m_in.append(buf, static_cast<std::size_t>(n));
        }
        out = m_in.substr(0, count);
        m_in.erase(0, count);
        return true;
    }

    void writeAll(const std::string &data)
    {
        if (m_ssl)
            SSL_write(m_ssl, data.data(), static_cast<int>(data.size()));
        else
            ::send(m_fd, data.data(), data.size(), MSG_NOSIGNAL);
    }

    void sendFrame(std::uint8_t opcode, const std::string &payload)
    {
        std::string frame;
        frame += static_cast<char>(0x80 | opcode);
        frame += static_cast<char>(payload.size()); // tests keep payloads < 126
        frame += payload;
        writeAll(frame);
    }

    struct Frame {
        std::uint8_t opcode = 0;
        std::string payload;
    };

    bool readFrame(Frame &frame)
    {
        std::string head;
        if (!readExact(head, 2))
            return false;
        frame.opcode = static_cast<std::uint8_t>(head[0]) & 0x0F;
        std::uint64_t length = static_cast<std::uint8_t>(head[1]) & 0x7F;
        std::string ext;
        if (length == 126) {
            if (!readExact(ext, 2))
                return false;
            length = (static_cast<std::uint8_t>(ext[0]) << 8) | static_cast<std::uint8_t>(ext[1]);
        }
        std::string mask;
        if (!readExact(mask, 4))
            return false;
        std::string body;
        if (length > 0 && !readExact(body, static_cast<std::size_t>(length)))
            return false;
        for (std::size_t i = 0; i < body.size(); ++i)
            body[i] = static_cast<char>(body[i] ^ mask[i & 3]);
        frame.payload = std::move(body);
        return true;
    }

    bool handshake()
    {
        std::string request;
        for (;;) {
            const std::size_t end = m_in.find("\r\n\r\n");
            if (end != std::string::npos) {
                request = m_in.substr(0, end);
                m_in.erase(0, end + 4);
                break;
            }
            char buf[2048];
            const int n = rawRead(buf, sizeof(buf));
            if (n <= 0)
                return false;
            m_in.append(buf, static_cast<std::size_t>(n));
        }
        const std::string tag = "Sec-WebSocket-Key:";
        const std::size_t at = request.find(tag);
        if (at == std::string::npos)
            return false;
        std::size_t from = at + tag.size();
        while (from < request.size() && request[from] == ' ')
            ++from;
        const std::string key = request.substr(from, request.find("\r\n", from) - from);
        const std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char digest[SHA_DIGEST_LENGTH];
        ::SHA1(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
        std::string accept = base64(digest, sizeof(digest));
        if (m_manner == Manner::BadAccept)
            accept = "AAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        writeAll("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
        return true;
    }

    void serve()
    {
        m_fd = ::accept(m_listen, nullptr, nullptr);
        if (m_fd < 0)
            return;
        m_clientFd = m_fd;
        if (m_ctx) {
            m_ssl = SSL_new(m_ctx);
            SSL_set_fd(m_ssl, m_fd);
            if (SSL_accept(m_ssl) != 1)
                return cleanup();
        }
        if (!handshake())
            return cleanup();
        if (m_manner == Manner::BadAccept) {
            // Hold the line open so the client's rejection, not our hang-up, ends it.
            Frame ignored;
            while (!m_done && readFrame(ignored)) {}
            return cleanup();
        }
        if (m_manner == Manner::CloseAtOnce) {
            sendFrame(0x8, std::string("\x03\xE8", 2));
            Frame f;
            while (!m_done && readFrame(f)) {
                if (f.opcode == 0x8)
                    m_sawClose = true;
            }
            return cleanup();
        }
        sendFrame(0x1, "hello");
        Frame f;
        while (!m_done && readFrame(f)) {
            if (f.opcode == 0x1) {
                if (f.payload == "ping") {
                    sendFrame(0x9, "pp");
                } else if (f.payload == "close") {
                    sendFrame(0x8, std::string("\x03\xE8", 2));
                } else {
                    sendFrame(0x1, "echo:" + f.payload);
                }
            } else if (f.opcode == 0xA) {
                if (f.payload == "pp") {
                    m_sawPong = true;
                    sendFrame(0x1, "pong-seen");
                }
            } else if (f.opcode == 0x8) {
                m_sawClose = true;
                break;
            }
        }
        cleanup();
    }

    void cleanup()
    {
        if (m_ssl) {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd);
            m_clientFd = -1;
            m_fd = -1;
        }
    }

    Manner m_manner;
    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;
    int m_listen = -1;
    int m_fd = -1;
    std::atomic<int> m_clientFd{-1};
    std::uint16_t m_port = 0;
    std::thread m_thread;
    std::string m_in;
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_sawPong{false};
    std::atomic<bool> m_sawClose{false};
};

// ----------------------------------------------------------------- client

struct Session {
    phi::runtime::EpollLoop loop;
    WsClient client{loop};
    bool opened = false;
    bool closed = false;
    std::string closeMessage;
    std::vector<std::string> texts;
    int activity = 0;
    std::string fingerprintAtOpen;
    /// Called after each event; return true to stop the loop.
    std::function<bool(Session &)> done;

    WsClient::Callbacks callbacks()
    {
        WsClient::Callbacks cb;
        cb.opened = [this]() {
            opened = true;
            fingerprintAtOpen = client.peerFingerprint();
            check();
        };
        cb.textMessage = [this](std::string_view text) {
            texts.emplace_back(text);
            check();
        };
        cb.closed = [this](const std::string &message) {
            closed = true;
            closeMessage = message;
            check();
        };
        cb.activity = [this]() { ++activity; };
        return cb;
    }

    void check()
    {
        if (done && done(*this))
            loop.stop();
    }

    bool hasText(const std::string &text) const
    {
        for (const std::string &t : texts)
            if (t == text)
                return true;
        return false;
    }

    /// Runs until `done` says stop or the budget is spent.
    void run(std::chrono::milliseconds budget = 8000ms)
    {
        phi::runtime::Timer watchdog = loop.timerAfter(budget, [this]() { loop.stop(); });
        loop.run();
    }
};

void testPlainEchoPingAndServerClose()
{
    // ws:// end to end: the upgrade is accepted, text goes both ways, a ping
    // from the server is answered with a pong carrying its payload, and a
    // close frame from the server ends the connection through `closed`.
    Server server(Server::Manner::Echo);
    PHI_CHECK(server.ok());

    Session s;
    int stage = 0;
    s.done = [&](Session &self) {
        if (stage == 0 && self.hasText("hello")) {
            stage = 1;
            self.client.sendText("abc");
        }
        if (stage == 1 && self.hasText("echo:abc")) {
            stage = 2;
            self.client.sendText("ping");
        }
        if (stage == 2 && self.hasText("pong-seen")) {
            stage = 3;
            self.client.sendText("close");
        }
        return self.closed;
    };
    std::string error;
    PHI_CHECK(s.client.open("ws://127.0.0.1:" + std::to_string(server.port()) + "/", s.callbacks(), &error));
    s.run();

    PHI_CHECK(s.opened);
    PHI_CHECK_MSG(stage == 3, "stage %d", stage);
    PHI_CHECK(server.sawPong());
    PHI_CHECK(s.closed);
    PHI_CHECK_MSG(s.closeMessage == "WebSocket closed by peer", "message '%s'", s.closeMessage.c_str());
    PHI_CHECK(!s.client.isOpen());
    PHI_CHECK(s.activity >= 3);
    // No TLS, no fingerprint.
    PHI_CHECK(s.fingerprintAtOpen.empty());
}

void testIsOpenAndSendPing()
{
    // sendPing goes out as a ping frame; the server of this test only answers
    // text, so what this pins is that the client stays open and sends no error.
    Server server(Server::Manner::Echo);
    PHI_CHECK(server.ok());
    Session s;
    s.done = [&](Session &self) {
        if (self.hasText("hello")) {
            PHI_CHECK(self.client.isOpen());
            self.client.sendPing();
            self.client.sendText("after");
        }
        return self.hasText("echo:after");
    };
    std::string error;
    PHI_CHECK(s.client.open("ws://127.0.0.1:" + std::to_string(server.port()) + "/x", s.callbacks(), &error));
    s.run();
    PHI_CHECK(s.hasText("echo:after"));
    PHI_CHECK(!s.closed);
    s.client.abort();
    PHI_CHECK(!s.client.isOpen());
}

void testTlsFingerprintAndRoundTrip()
{
    // wss:// against a self-signed certificate: no CA check, but the
    // certificate's SHA-256 (DER, lower-case hex) is handed over for pinning
    // and is known by the time `opened` fires.
    SelfSigned cert;
    PHI_CHECK(cert.create());
    Server server(Server::Manner::Echo, &cert);
    PHI_CHECK(server.ok());

    Session s;
    bool sent = false;
    s.done = [&](Session &self) {
        if (!sent && self.hasText("hello")) {
            sent = true;
            self.client.sendText("secure");
        }
        return self.hasText("echo:secure");
    };
    std::string error;
    PHI_CHECK(s.client.open("wss://127.0.0.1:" + std::to_string(server.port()) + "/", s.callbacks(), &error));
    s.run();

    PHI_CHECK(s.opened);
    PHI_CHECK(s.hasText("echo:secure"));
    const std::string expected = cert.fingerprint();
    PHI_CHECK(expected.size() == 64);
    PHI_CHECK_MSG(s.client.peerFingerprint() == expected, "got '%s' want '%s'", s.client.peerFingerprint().c_str(),
                  expected.c_str());
    PHI_CHECK(s.fingerprintAtOpen == expected);
    s.client.abort();
}

void testBadAcceptIsRejected()
{
    // A server that does not prove it read our key is not a WebSocket server.
    Server server(Server::Manner::BadAccept);
    PHI_CHECK(server.ok());
    Session s;
    s.done = [](Session &self) { return self.closed; };
    std::string error;
    PHI_CHECK(s.client.open("ws://127.0.0.1:" + std::to_string(server.port()) + "/", s.callbacks(), &error));
    s.run();
    PHI_CHECK(s.closed);
    PHI_CHECK(!s.opened);
    PHI_CHECK_MSG(s.closeMessage.find("Sec-WebSocket-Accept") != std::string::npos, "message '%s'",
                  s.closeMessage.c_str());
}

void testCloseRightAfterHandshake()
{
    // The peer's close frame arrives in the same read as, or right after, the
    // upgrade answer; the client still reports `opened` and then `closed`.
    Server server(Server::Manner::CloseAtOnce);
    PHI_CHECK(server.ok());
    Session s;
    s.done = [](Session &self) { return self.closed; };
    std::string error;
    PHI_CHECK(s.client.open("ws://127.0.0.1:" + std::to_string(server.port()) + "/", s.callbacks(), &error));
    s.run();
    PHI_CHECK(s.opened);
    PHI_CHECK(s.closed);
    PHI_CHECK(s.closeMessage == "WebSocket closed by peer");
}

void testConnectToClosedPort()
{
    // Nothing listens: the dial starts, then `closed` says why - with text.
    std::uint16_t port = 0;
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
        port = ntohs(addr.sin_port);
        ::close(fd);
    }
    Session s;
    s.done = [](Session &self) { return self.closed; };
    std::string error;
    const bool started = s.client.open("ws://127.0.0.1:" + std::to_string(port) + "/", s.callbacks(), &error);
    if (started) {
        s.run();
        PHI_CHECK(s.closed);
        PHI_CHECK(!s.closeMessage.empty());
    } else {
        // Refusing at once is also an answer, but it must carry a reason.
        PHI_CHECK(!error.empty());
    }
    PHI_CHECK(!s.opened);
}

void testBadUrlsAreRefusedUpFront()
{
    // open() returns false with an error and dials nothing.
    const char *bad[] = {"http://127.0.0.1/", "ws://", "ws://host:0/", "ws://host:notaport/", "ws://[::1/", ""};
    for (const char *url : bad) {
        Session s;
        std::string error;
        PHI_CHECK_MSG(!s.client.open(url, s.callbacks(), &error), "accepted '%s'", url);
        PHI_CHECK_MSG(!error.empty(), "no error for '%s'", url);
    }
}

} // namespace

int main()
{
    // My server writes to TLS sockets the client may already have closed.
    if (!std::getenv("KEEP_SIGPIPE")) ::signal(SIGPIPE, SIG_IGN);
    testPlainEchoPingAndServerClose();
    testIsOpenAndSendPing();
    testTlsFingerprintAndRoundTrip();
    testBadAcceptIsRejected();
    testCloseRightAfterHandshake();
    testConnectToClosedPort();
    testBadUrlsAreRefusedUpFront();
    return phi::testing::report("webos_ws_client_tests");
}
