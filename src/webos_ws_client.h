#pragma once

// A WebSocket client (RFC 6455) on the phi runtime loop - phi-adapter-test's
// client, with wss:// added for the TV. Text messages, ping/pong, a clean
// close, and the handshake checked rather than assumed.

#include "webos_socket.h"

#include <phi/runtime/loop.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace phicore::webos::ipc {

class WsClient
{
public:
    struct Callbacks {
        std::function<void()> opened;
        std::function<void(std::string_view text)> textMessage;
        /// Empty `message` for a close the peer asked for; otherwise the error.
        std::function<void(const std::string &message)> closed;
        /// Any frame arrived, pongs included - what a liveness check reads.
        std::function<void()> activity;
    };

    explicit WsClient(phi::runtime::Loop &loop);

    WsClient(const WsClient &) = delete;
    WsClient &operator=(const WsClient &) = delete;

    /// `ws://` or `wss://host[:port][/path]`. `false` with `error` set means nothing was
    /// dialled; a failure after that arrives through `closed`.
    bool open(const std::string &url, Callbacks callbacks, std::string *error);
    void sendText(std::string_view text);
    void sendPing();
    /// Drops the connection without a callback.
    void abort();

    bool isOpen() const { return m_state == State::Open; }
    /// See StreamSocket::peerFingerprint().
    const std::string &peerFingerprint() const { return m_socket.peerFingerprint(); }

private:
    enum class State { Idle, Handshake, Open, Closed };

    void onConnected();
    void onBytes(std::string_view bytes);
    bool consumeHandshake();
    void processFrames();
    void sendFrame(std::uint8_t opcode, std::string_view payload);
    void fail(const std::string &message);

    StreamSocket m_socket;
    Callbacks m_callbacks;
    State m_state = State::Idle;
    std::string m_host;
    std::string m_target;
    std::uint16_t m_port = 80;
    bool m_tls = false;
    std::string m_key;
    std::string m_inBuffer;
    std::string m_fragment;
    std::uint8_t m_fragmentOpcode = 0;
    bool m_sentClose = false;
};

} // namespace phicore::webos::ipc
