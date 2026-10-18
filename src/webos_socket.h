#pragma once

// A TCP stream on the phi runtime loop, optionally inside TLS - the socket
// under the SSAP WebSocket. Non-blocking connect, bytes as they arrive, a
// buffered write that never blocks the loop, one closed callback for every way
// a connection ends. The TCP part is phi-adapter-test's LoopSocket.
//
// TLS does not verify the TV against a CA: a webOS set presents a certificate
// of its own that no CA vouches for. What it does is hand the certificate's
// SHA-256 to the caller, which pins it on first use and refuses a different
// one later.
//
// Loop affinity is the runtime's: the socket belongs to the thread that built
// it, and must be destroyed there too.

#include <phi/runtime/loop.h>

#include <sys/socket.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ssl_st;
struct ssl_ctx_st;

namespace phicore::webos::ipc {

class StreamSocket
{
public:
    struct Callbacks {
        /// TCP is up and, with TLS, the handshake is done.
        std::function<void()> connected;
        std::function<void(std::string_view bytes)> readyRead;
        /// The connection ended; the descriptor is already gone. Empty
        /// `message` when the peer closed cleanly.
        std::function<void(const std::string &message)> closed;
    };

    explicit StreamSocket(phi::runtime::Loop &loop);
    ~StreamSocket();

    StreamSocket(const StreamSocket &) = delete;
    StreamSocket &operator=(const StreamSocket &) = delete;

    /// Resolves and dials. `false` with `error` set means the attempt never
    /// started; a failure after that arrives through `closed`.
    bool connectToHost(const std::string &host,
                       std::uint16_t port,
                       bool tls,
                       Callbacks callbacks,
                       std::string *error);

    /// Queues bytes; whatever the kernel will not take now goes out when the
    /// descriptor is writable again.
    void write(std::string_view bytes);

    /// Drops the connection without a callback. Safe to call twice, and from
    /// inside a callback.
    void close();

    bool isConnected() const { return m_state == State::Connected; }

    /// Lower-case hex SHA-256 of the peer certificate (DER); empty without TLS
    /// or before the handshake.
    const std::string &peerFingerprint() const { return m_peerFingerprint; }

private:
    enum class State { Idle, Connecting, Handshake, Connected, Closed };

    struct Candidate {
        int family = 0;
        int protocol = 0;
        ::sockaddr_storage addr{};
        ::socklen_t length = 0;
    };

    bool dialNext(std::string *error);
    void onWritable();
    void onReadable();
    bool startTls(std::string *error);
    void advanceHandshake();
    void readPlain();
    void readTls();
    void flush();
    void fail(const std::string &message);
    void teardown();

    phi::runtime::Loop &m_loop;
    Callbacks m_callbacks;
    State m_state = State::Idle;
    int m_fd = -1;
    bool m_tls = false;
    std::string m_host;
    std::vector<Candidate> m_candidates;
    std::size_t m_nextCandidate = 0;
    std::string m_outBuffer;
    phi::runtime::FdWatch m_readWatch;
    phi::runtime::FdWatch m_writeWatch;

    ssl_ctx_st *m_ctx = nullptr;
    ssl_st *m_ssl = nullptr;
    // TLS can turn a read into a wait for writable and a write into a wait for
    // readable; these remember which side to retry.
    bool m_readWantsWrite = false;
    bool m_writeWantsRead = false;
    std::string m_peerFingerprint;

    // Read by every callback that runs user code: a `closed` handler may
    // delete the object that owns this socket.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace phicore::webos::ipc
