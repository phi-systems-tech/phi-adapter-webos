#include "webos_socket.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cerrno>
#include <cstring>

namespace phicore::webos::ipc {

namespace {

constexpr std::size_t kReadChunk = 16 * 1024;

std::string errnoText(int code)
{
    return std::strerror(code);
}

/// The last OpenSSL error on this thread, or `fallback` when the queue is
/// empty (a peer that just closed leaves none).
std::string sslErrorText(const char *fallback)
{
    const unsigned long code = ::ERR_get_error();
    ::ERR_clear_error();
    if (code == 0)
        return fallback;
    char buffer[256];
    ::ERR_error_string_n(code, buffer, sizeof(buffer));
    return buffer;
}

bool isNumericHost(const std::string &host)
{
    unsigned char scratch[sizeof(::in6_addr)];
    return ::inet_pton(AF_INET, host.c_str(), scratch) == 1 || ::inet_pton(AF_INET6, host.c_str(), scratch) == 1;
}

std::string fingerprintOf(::SSL *ssl)
{
    ::X509 *cert = ::SSL_get1_peer_certificate(ssl);
    if (cert == nullptr)
        return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    const bool ok = ::X509_digest(cert, ::EVP_sha256(), digest, &length) == 1;
    ::X509_free(cert);
    if (!ok)
        return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0x0F];
    }
    return out;
}

/**
 * @brief The addresses `host` resolves to, in the resolver's order.
 *
 * Numeric first, which costs no lookup: a TV is configured by its address. A
 * name falls back to a resolver call that blocks this instance's loop for its
 * length - the same trade the SDK's HTTP client makes.
 */
template <typename Candidate>
std::vector<Candidate> resolveAll(const std::string &host, std::uint16_t port, std::string *error)
{
    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;

    const std::string service = std::to_string(port);
    ::addrinfo *resolved = nullptr;
    int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0) {
        hints.ai_flags = 0;
        rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    }
    if (rc != 0 || resolved == nullptr) {
        if (error)
            *error = std::string("Cannot resolve host: ") + ::gai_strerror(rc);
        return {};
    }

    std::vector<Candidate> out;
    for (::addrinfo *entry = resolved; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_addrlen > sizeof(::sockaddr_storage))
            continue;
        Candidate candidate;
        candidate.family = entry->ai_family;
        candidate.protocol = entry->ai_protocol;
        std::memcpy(&candidate.addr, entry->ai_addr, entry->ai_addrlen);
        candidate.length = entry->ai_addrlen;
        out.push_back(candidate);
    }
    ::freeaddrinfo(resolved);
    if (out.empty() && error)
        *error = "No address to connect to";
    return out;
}

} // namespace

StreamSocket::StreamSocket(phi::runtime::Loop &loop) : m_loop(loop)
{
}

StreamSocket::~StreamSocket()
{
    *m_alive = false;
    teardown();
}

bool StreamSocket::connectToHost(const std::string &host,
                                 std::uint16_t port,
                                 bool tls,
                                 Callbacks callbacks,
                                 std::string *error)
{
    m_loop.assertOnLoop("StreamSocket::connectToHost");
    if (m_state != State::Idle) {
        if (error)
            *error = "Socket is already in use";
        return false;
    }
    std::vector<Candidate> candidates = resolveAll<Candidate>(host, port, error);
    if (candidates.empty())
        return false;

    m_host = host;
    m_tls = tls;
    m_candidates = std::move(candidates);
    m_nextCandidate = 0;
    m_callbacks = std::move(callbacks);
    if (!dialNext(error)) {
        m_state = State::Idle;
        m_callbacks = Callbacks{};
        m_candidates.clear();
        return false;
    }
    return true;
}

bool StreamSocket::dialNext(std::string *error)
{
    std::string lastError = error && !error->empty() ? *error : std::string();
    while (m_nextCandidate < m_candidates.size()) {
        const Candidate &candidate = m_candidates[m_nextCandidate++];

        m_writeWatch.reset();
        m_readWatch.reset();
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }

        m_fd = ::socket(candidate.family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, candidate.protocol);
        if (m_fd < 0) {
            lastError = "socket(): " + errnoText(errno);
            continue;
        }
        const int one = 1;
        ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        const int rc = ::connect(m_fd, reinterpret_cast<const ::sockaddr *>(&candidate.addr), candidate.length);
        if (rc != 0 && errno != EINPROGRESS) {
            lastError = "connect(): " + errnoText(errno);
            ::close(m_fd);
            m_fd = -1;
            continue;
        }

        // A connect that completed inside the call is reported by the writable
        // watch on the next turn, so no callback runs before connectToHost()
        // returns.
        m_state = State::Connecting;
        m_writeWatch = m_loop.watchFd(m_fd, phi::runtime::Loop::FdEvent::Write, [this]() { onWritable(); });
        return true;
    }

    if (error)
        *error = lastError.empty() ? "No address to connect to" : lastError;
    return false;
}

void StreamSocket::onWritable()
{
    switch (m_state) {
    case State::Connecting: {
        int soError = 0;
        ::socklen_t length = sizeof(soError);
        if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &length) != 0)
            soError = errno;
        if (soError != 0) {
            std::string error = "connect(): " + errnoText(soError);
            if (m_nextCandidate < m_candidates.size() && dialNext(&error))
                return;
            fail(error);
            return;
        }
        m_candidates.clear();
        m_writeWatch.setEnabled(false);
        m_readWatch = m_loop.watchFd(m_fd, phi::runtime::Loop::FdEvent::Read, [this]() { onReadable(); });

        if (m_tls) {
            std::string error;
            if (!startTls(&error)) {
                fail(error);
                return;
            }
            advanceHandshake();
            return;
        }

        m_state = State::Connected;
        auto alive = m_alive;
        if (m_callbacks.connected)
            m_callbacks.connected();
        if (!*alive || m_state != State::Connected)
            return;
        flush();
        return;
    }
    case State::Handshake:
        advanceHandshake();
        return;
    case State::Connected: {
        if (m_readWantsWrite) {
            m_readWantsWrite = false;
            auto alive = m_alive;
            onReadable();
            if (!*alive || m_state != State::Connected)
                return;
        }
        flush();
        return;
    }
    case State::Idle:
    case State::Closed:
        return;
    }
}

void StreamSocket::onReadable()
{
    if (m_state == State::Handshake) {
        advanceHandshake();
        return;
    }
    if (m_state != State::Connected)
        return;
    if (m_tls)
        readTls();
    else
        readPlain();
}

bool StreamSocket::startTls(std::string *error)
{
    ::ERR_clear_error();
    m_ctx = ::SSL_CTX_new(::TLS_client_method());
    if (m_ctx == nullptr) {
        *error = "TLS: " + sslErrorText("cannot create a context");
        return false;
    }
    // Pinned by the caller instead (see the header); a TV's certificate has no
    // chain to verify.
    ::SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);
    ::SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);

    m_ssl = ::SSL_new(m_ctx);
    if (m_ssl == nullptr || ::SSL_set_fd(m_ssl, m_fd) != 1) {
        *error = "TLS: " + sslErrorText("cannot create a session");
        return false;
    }
    ::SSL_set_mode(m_ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (!isNumericHost(m_host))
        ::SSL_set_tlsext_host_name(m_ssl, m_host.c_str());
    ::SSL_set_connect_state(m_ssl);
    m_state = State::Handshake;
    return true;
}

void StreamSocket::advanceHandshake()
{
    ::ERR_clear_error();
    const int rc = ::SSL_connect(m_ssl);
    if (rc == 1) {
        m_peerFingerprint = fingerprintOf(m_ssl);
        m_state = State::Connected;
        m_writeWatch.setEnabled(false);
        m_readWatch.setEnabled(true);
        auto alive = m_alive;
        if (m_callbacks.connected)
            m_callbacks.connected();
        if (!*alive || m_state != State::Connected)
            return;
        flush();
        // Application data may have arrived with the last handshake record.
        if (::SSL_pending(m_ssl) > 0)
            readTls();
        return;
    }
    switch (::SSL_get_error(m_ssl, rc)) {
    case SSL_ERROR_WANT_READ:
        m_writeWatch.setEnabled(false);
        m_readWatch.setEnabled(true);
        return;
    case SSL_ERROR_WANT_WRITE:
        m_writeWatch.setEnabled(true);
        return;
    default:
        fail("TLS handshake failed: " + sslErrorText("connection closed"));
        return;
    }
}

void StreamSocket::readPlain()
{
    auto alive = m_alive;
    std::vector<char> chunk(kReadChunk);
    while (true) {
        const ssize_t got = ::read(m_fd, chunk.data(), chunk.size());
        if (got > 0) {
            if (m_callbacks.readyRead)
                m_callbacks.readyRead(std::string_view(chunk.data(), static_cast<std::size_t>(got)));
            if (!*alive || m_state != State::Connected)
                return;
            continue;
        }
        if (got == 0) {
            fail({});
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        fail("read(): " + errnoText(errno));
        return;
    }
}

void StreamSocket::readTls()
{
    auto alive = m_alive;
    std::vector<char> chunk(kReadChunk);
    while (true) {
        ::ERR_clear_error();
        const int got = ::SSL_read(m_ssl, chunk.data(), static_cast<int>(chunk.size()));
        if (got > 0) {
            if (m_callbacks.readyRead)
                m_callbacks.readyRead(std::string_view(chunk.data(), static_cast<std::size_t>(got)));
            if (!*alive || m_state != State::Connected)
                return;
            continue;
        }
        switch (::SSL_get_error(m_ssl, got)) {
        case SSL_ERROR_WANT_READ:
            if (m_writeWantsRead) {
                m_writeWantsRead = false;
                flush();
            }
            return;
        case SSL_ERROR_WANT_WRITE:
            m_readWantsWrite = true;
            m_writeWatch.setEnabled(true);
            return;
        case SSL_ERROR_ZERO_RETURN:
            fail({});
            return;
        case SSL_ERROR_SYSCALL:
            if (errno == 0 || errno == ECONNRESET) {
                fail({});
                return;
            }
            fail("read(): " + errnoText(errno));
            return;
        default:
            fail("TLS: " + sslErrorText("read failed"));
            return;
        }
    }
}

void StreamSocket::write(std::string_view bytes)
{
    if (m_state == State::Closed || m_fd < 0)
        return;
    m_outBuffer.append(bytes);
    if (m_state == State::Connected)
        flush();
}

void StreamSocket::flush()
{
    while (!m_outBuffer.empty()) {
        if (m_tls) {
            ::ERR_clear_error();
            const int sent = ::SSL_write(m_ssl, m_outBuffer.data(), static_cast<int>(m_outBuffer.size()));
            if (sent > 0) {
                m_outBuffer.erase(0, static_cast<std::size_t>(sent));
                continue;
            }
            switch (::SSL_get_error(m_ssl, sent)) {
            case SSL_ERROR_WANT_WRITE:
                m_writeWatch.setEnabled(true);
                return;
            case SSL_ERROR_WANT_READ:
                m_writeWantsRead = true;
                return;
            default:
                fail("TLS: " + sslErrorText("write failed"));
                return;
            }
        }
        const ssize_t sent = ::send(m_fd, m_outBuffer.data(), m_outBuffer.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            m_outBuffer.erase(0, static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            m_writeWatch.setEnabled(true);
            return;
        }
        fail("send(): " + errnoText(errno));
        return;
    }
    if (!m_readWantsWrite)
        m_writeWatch.setEnabled(false);
}

void StreamSocket::fail(const std::string &message)
{
    if (m_state == State::Closed)
        return;
    teardown();
    // The handlers are kept: a `closed` handler that calls close() from inside
    // its own frame would otherwise destroy the closure it is running in.
    if (m_callbacks.closed)
        m_callbacks.closed(message);
}

void StreamSocket::close()
{
    teardown();
}

void StreamSocket::teardown()
{
    m_state = State::Closed;
    m_readWatch.reset();
    m_writeWatch.reset();
    if (m_ssl != nullptr) {
        // No close_notify: the descriptor goes either way, and a TV that is
        // already gone would only make this wait.
        ::SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_ctx != nullptr) {
        ::SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_outBuffer.clear();
    m_readWantsWrite = false;
    m_writeWantsRead = false;
}

} // namespace phicore::webos::ipc
