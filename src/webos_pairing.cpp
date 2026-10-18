#include "webos_pairing.h"

#include "webos_protocol.h"

#include "phi/runtime/str.h"

namespace phicore::webos::ipc {

namespace str = phi::str;

Pairing::Pairing(phi::runtime::Loop &loop) : m_loop(loop)
{
}

Pairing::~Pairing()
{
    m_done = {};
    m_deadline.reset();
    if (m_ws) {
        m_ws->abort();
        delete m_ws;
        m_ws = nullptr;
    }
}

bool Pairing::start(const WebosSettings &settings, std::chrono::milliseconds timeout, Done done,
                    std::string *error)
{
    if (busy()) {
        *error = "A pairing is already running";
        return false;
    }
    if (!settings.hasHost()) {
        *error = "The TV host is required";
        return false;
    }
    m_settings = settings;
    m_fingerprint.clear();
    m_ws = new WsClient(m_loop);

    WsClient::Callbacks callbacks;
    callbacks.opened = [this]() { onOpened(); };
    callbacks.textMessage = [this](std::string_view text) { onText(text); };
    callbacks.closed = [this](const std::string &message) {
        Result result;
        result.error = "TV at " + m_settings.host + " not reachable: "
                       + (message.empty() ? std::string("connection closed") : message);
        finish(std::move(result));
    };
    if (!m_ws->open(m_settings.url(), std::move(callbacks), error)) {
        delete m_ws;
        m_ws = nullptr;
        return false;
    }
    m_done = std::move(done);
    m_deadline = m_loop.timerAfter(timeout, [this]() {
        Result result;
        result.error = m_fingerprint.empty() ? "The TV did not answer" : "The prompt on the TV was not accepted in time";
        finish(std::move(result));
    });
    return true;
}

void Pairing::onOpened()
{
    if (m_settings.useTls) {
        m_fingerprint = m_ws->peerFingerprint();
        if (m_fingerprint.empty()) {
            finish({false, false, "The TV presented no certificate", {}, {}});
            return;
        }
        if (!m_settings.certFingerprint.empty() && m_settings.certFingerprint != m_fingerprint) {
            finish({false, false,
                    "The TV presents a different certificate than the one pinned; clear the pinned one if that "
                    "is expected",
                    {}, {}});
            return;
        }
    }
    // Pairing always starts without a key: the point is to get a new one.
    m_ws->sendText(registerMessage({}));
}

void Pairing::onText(std::string_view text)
{
    const SsapMessage message = parseMessage(text);
    if (!message.valid || message.id != kIdRegister)
        return;
    if (isPairingPrompt(message))
        return;
    if (message.type == "error") {
        Result result;
        result.refused = true;
        result.error = "The TV refused the connection: " + message.error;
        finish(std::move(result));
        return;
    }
    if (message.type != "registered")
        return;
    Result result;
    result.clientKey = str::trimmed(jsonString(message.payload, "client-key"));
    result.certFingerprint = m_fingerprint;
    result.ok = !result.clientKey.empty();
    if (!result.ok)
        result.error = "The TV registered without handing out a key";
    finish(std::move(result));
}

void Pairing::finish(Result result)
{
    if (!m_done)
        return;
    Done done = std::move(m_done);
    m_done = {};
    m_deadline.reset();
    if (m_ws) {
        m_ws->abort();
        phi::runtime::Loop::deleteDeferred(m_ws);
        m_ws = nullptr;
    }
    done(result);
}

void Pairing::cancel()
{
    m_done = {};
    m_deadline.reset();
    if (m_ws) {
        m_ws->abort();
        phi::runtime::Loop::deleteDeferred(m_ws);
        m_ws = nullptr;
    }
}

} // namespace phicore::webos::ipc
