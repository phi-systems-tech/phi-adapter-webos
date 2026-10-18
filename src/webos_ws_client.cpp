#include "webos_ws_client.h"

#include "phi/runtime/str.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <vector>

namespace phicore::webos::ipc {

namespace {

namespace str = phi::str;

constexpr std::size_t kMaxHandshakeBytes = 16 * 1024;
constexpr std::size_t kMaxMessageBytes = 16 * 1024 * 1024;

constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpBinary = 0x2;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

std::mt19937_64 &randomEngine()
{
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

std::string randomBytes(std::size_t count)
{
    std::string out(count, '\0');
    auto &engine = randomEngine();
    for (char &c : out)
        c = static_cast<char>(engine() & 0xFF);
    return out;
}

/// The accept value the server has to answer with (RFC 6455 §1.3); the GUID
/// is the protocol's.
std::string acceptKeyFor(const std::string &clientKey)
{
    const std::string material = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    ::SHA1(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
    return str::toBase64(std::string_view(reinterpret_cast<const char *>(digest), sizeof(digest)));
}

bool listContainsToken(std::string_view value, std::string_view token)
{
    for (const std::string &part : str::split(value, ',')) {
        if (str::equalsIgnoreCase(str::trimmed(part), token))
            return true;
    }
    return false;
}

} // namespace

WsClient::WsClient(phi::runtime::Loop &loop) : m_socket(loop)
{
}

bool WsClient::open(const std::string &url, Callbacks callbacks, std::string *error)
{
    const auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    if (m_state != State::Idle)
        return fail("WebSocket client is already in use");

    std::string rest = str::trimmed(url);
    if (str::startsWithIgnoreCase(rest, "wss://")) {
        m_tls = true;
        rest.erase(0, 6);
    } else if (str::startsWithIgnoreCase(rest, "ws://")) {
        m_tls = false;
        rest.erase(0, 5);
    } else {
        return fail("Invalid WebSocket endpoint");
    }

    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    m_target = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (m_target.empty())
        m_target = "/";
    if (authority.empty())
        return fail("Invalid WebSocket endpoint");

    m_port = m_tls ? 443 : 80;
    // An IPv6 literal keeps its brackets in the Host header and loses them for
    // the resolver.
    if (authority.front() == '[') {
        const std::size_t end = authority.find(']');
        if (end == std::string::npos)
            return fail("Invalid WebSocket endpoint");
        m_host = authority.substr(1, end - 1);
        const std::string tail = authority.substr(end + 1);
        if (!tail.empty()) {
            if (tail.front() != ':')
                return fail("Invalid WebSocket endpoint");
            bool ok = false;
            m_port = static_cast<std::uint16_t>(str::toUInt(tail.substr(1), &ok));
            if (!ok || m_port == 0)
                return fail("Invalid WebSocket port");
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            m_host = authority;
        } else {
            m_host = authority.substr(0, colon);
            bool ok = false;
            m_port = static_cast<std::uint16_t>(str::toUInt(authority.substr(colon + 1), &ok));
            if (!ok || m_port == 0)
                return fail("Invalid WebSocket port");
        }
    }
    if (m_host.empty())
        return fail("Invalid WebSocket endpoint");

    m_callbacks = std::move(callbacks);
    m_state = State::Handshake;

    StreamSocket::Callbacks socketCallbacks;
    socketCallbacks.connected = [this]() { onConnected(); };
    socketCallbacks.readyRead = [this](std::string_view bytes) { onBytes(bytes); };
    socketCallbacks.closed = [this](const std::string &message) {
        this->fail(message.empty() ? std::string("WebSocket disconnected") : message);
    };

    std::string socketError;
    if (!m_socket.connectToHost(m_host, m_port, m_tls, std::move(socketCallbacks), &socketError)) {
        m_state = State::Closed;
        m_callbacks = Callbacks{};
        return fail(socketError);
    }
    return true;
}

void WsClient::onConnected()
{
    m_key = str::toBase64(randomBytes(16));

    std::string request;
    request += "GET " + m_target + " HTTP/1.1\r\n";
    request += "Host: " + m_host + ":" + str::number(m_port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + m_key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += "\r\n";
    m_socket.write(request);
}

void WsClient::onBytes(std::string_view bytes)
{
    m_inBuffer.append(bytes);
    if (m_state == State::Handshake) {
        if (!consumeHandshake())
            return;
    }
    if (m_state == State::Open)
        processFrames();
}

bool WsClient::consumeHandshake()
{
    const std::size_t end = m_inBuffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (m_inBuffer.size() > kMaxHandshakeBytes)
            fail("Handshake response too large");
        return false;
    }

    const std::string head = m_inBuffer.substr(0, end);
    m_inBuffer.erase(0, end + 4);

    const std::vector<std::string> lines = str::split(head, "\r\n");
    if (lines.empty()) {
        fail("Malformed handshake response");
        return false;
    }
    const std::vector<std::string> status = str::split(lines.front(), ' ', false);
    if (status.size() < 2 || status[1] != "101") {
        fail("WebSocket upgrade refused: " + str::trimmed(lines.front()));
        return false;
    }

    std::string upgrade;
    std::string connection;
    std::string accept;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = str::toLower(str::trimmed(lines[i].substr(0, colon)));
        const std::string value = str::trimmed(lines[i].substr(colon + 1));
        if (name == "upgrade")
            upgrade = value;
        else if (name == "connection")
            connection = value;
        else if (name == "sec-websocket-accept")
            accept = value;
    }

    if (!str::equalsIgnoreCase(upgrade, "websocket") || !listContainsToken(connection, "upgrade")) {
        fail("Handshake response is not a WebSocket upgrade");
        return false;
    }
    if (accept != acceptKeyFor(m_key)) {
        fail("Handshake response carries a wrong Sec-WebSocket-Accept");
        return false;
    }

    m_state = State::Open;
    if (m_callbacks.opened)
        m_callbacks.opened();
    return m_state == State::Open;
}

void WsClient::processFrames()
{
    while (m_state == State::Open) {
        const std::string &buf = m_inBuffer;
        if (buf.size() < 2)
            return;
        const auto b0 = static_cast<std::uint8_t>(buf[0]);
        const auto b1 = static_cast<std::uint8_t>(buf[1]);
        const bool fin = (b0 & 0x80) != 0;
        const std::uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        std::uint64_t length = b1 & 0x7F;
        std::size_t headerLen = 2;

        if ((b0 & 0x70) != 0)
            return fail("Server frame set reserved bits");
        if (masked)
            return fail("Server frame must not be masked");

        if (length == 126) {
            if (buf.size() < 4)
                return;
            length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[2])) << 8)
                   | static_cast<std::uint8_t>(buf[3]);
            headerLen = 4;
        } else if (length == 127) {
            if (buf.size() < 10)
                return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | static_cast<std::uint8_t>(buf[2 + i]);
            headerLen = 10;
        }
        if (length > kMaxMessageBytes)
            return fail("Server message too big");
        if (buf.size() < headerLen + length)
            return;
        if (m_callbacks.activity)
            m_callbacks.activity();
        if (m_state != State::Open)
            return;

        std::string payload = m_inBuffer.substr(headerLen, static_cast<std::size_t>(length));
        m_inBuffer.erase(0, headerLen + static_cast<std::size_t>(length));

        if (opcode >= kOpClose) {
            if (!fin || length > 125)
                return fail("Malformed control frame");
            if (opcode == kOpPing) {
                sendFrame(kOpPong, payload);
                continue;
            }
            if (opcode == kOpPong)
                continue;
            // Close: echo the code once, then let the peer's FIN end it.
            if (!m_sentClose) {
                m_sentClose = true;
                sendFrame(kOpClose, payload.substr(0, std::min<std::size_t>(payload.size(), 2)));
            }
            return fail("WebSocket closed by peer");
        }

        if (opcode == kOpBinary)
            return fail("Binary frames are not supported");
        if (opcode == kOpText) {
            if (m_fragmentOpcode != 0)
                return fail("New message inside a fragmented one");
            if (fin) {
                if (m_callbacks.textMessage)
                    m_callbacks.textMessage(payload);
                continue;
            }
            m_fragmentOpcode = kOpText;
            m_fragment = std::move(payload);
            continue;
        }
        if (opcode == kOpContinuation) {
            if (m_fragmentOpcode == 0)
                return fail("Continuation without a start");
            if (m_fragment.size() + payload.size() > kMaxMessageBytes)
                return fail("Server message too big");
            m_fragment += payload;
            if (fin) {
                std::string message = std::move(m_fragment);
                m_fragment.clear();
                m_fragmentOpcode = 0;
                if (m_callbacks.textMessage)
                    m_callbacks.textMessage(message);
            }
            continue;
        }
        return fail("Unknown opcode");
    }
}

void WsClient::sendText(std::string_view text)
{
    if (m_state != State::Open)
        return;
    sendFrame(kOpText, text);
}

void WsClient::sendPing()
{
    if (m_state != State::Open)
        return;
    sendFrame(kOpPing, {});
}

void WsClient::sendFrame(std::uint8_t opcode, std::string_view payload)
{
    if (m_state == State::Closed)
        return;

    std::string frame;
    frame.reserve(payload.size() + 14);
    frame += static_cast<char>(0x80 | opcode);
    // Every client frame is masked (RFC 6455 §5.3); a server closes the
    // connection on one that is not.
    if (payload.size() < 126) {
        frame += static_cast<char>(0x80 | payload.size());
    } else if (payload.size() <= 0xFFFF) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>((payload.size() >> 8) & 0xFF);
        frame += static_cast<char>(payload.size() & 0xFF);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame += static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF);
    }

    const std::string mask = randomBytes(4);
    frame += mask;
    const std::size_t offset = frame.size();
    frame += payload;
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame[offset + i] = static_cast<char>(frame[offset + i] ^ mask[i & 3]);

    m_socket.write(frame);
}

void WsClient::fail(const std::string &message)
{
    if (m_state == State::Closed)
        return;
    m_state = State::Closed;
    m_socket.close();
    m_inBuffer.clear();
    m_fragment.clear();
    if (m_callbacks.closed)
        m_callbacks.closed(message);
}

void WsClient::abort()
{
    // Kept, like the socket's: abort() is called from inside a message
    // handler, and clearing the callbacks there would destroy the closure
    // running above this frame. Closed state is what silences them.
    m_state = State::Closed;
    m_socket.close();
    m_inBuffer.clear();
    m_fragment.clear();
}

} // namespace phicore::webos::ipc
