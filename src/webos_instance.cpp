#include "webos_instance.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>

#include "phi/adapter/sdk/schema_json.h"
#include "phi/runtime/loop.h"
#include "phi/runtime/str.h"

#include "webos_json.h"
#include "webos_model.h"
#include "webos_protocol.h"
#include "webos_settings.h"
#include "webos_ws_client.h"

namespace phicore::webos::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace std::chrono_literals;

namespace {

/// TCP, TLS and the WebSocket upgrade together; a TV in deep standby lets a
/// connect hang far longer than this.
constexpr auto kOpenTimeout = 8s;
/// Pairing waits for someone to reach the remote.
constexpr auto kPairingTimeout = 60s;
constexpr auto kPingInterval = 20s;
/// Three pings without any frame back: the TV is gone, whatever TCP thinks.
constexpr std::int64_t kSilenceLimitMs = 65000;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

class WebosInstance final : public sdk::AdapterInstance
{
public:
    ~WebosInstance() override
    {
        // Runs on the host thread. Everything bound to the loop was released
        // in stop(), on the loop thread.
    }

protected:
    bool start() override
    {
        m_loop = phi::runtime::Loop::current();
        if (m_loop == nullptr) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Lifecycle, "started off a loop; no timers are possible");
            return false;
        }
        m_running = true;
        // Nothing to dial yet: the host arrives with the first config.changed.
        return true;
    }

    void stop() override
    {
        m_running = false;
        dropConnection();
        m_retry.reset();
    }

    void onDisconnected() override
    {
        // phi-core went away; it comes back with the configuration, which is
        // what dials again.
        dropConnection();
        m_retry.reset();
        setConnected(false);
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        if (!m_running)
            return;
        m_info = request.adapter;
        const WebosSettings next = settingsFromAdapter(request.adapter, parseObject(request.adapter.metaJson));

        // Every meta patch this instance sends comes back as a config change;
        // only a change it did not make itself is a reason to dial again.
        const bool endpointChanged = next.host != m_settings.host || next.port != m_settings.port
                                     || next.useTls != m_settings.useTls;
        const bool keyChanged = next.clientKey != m_settings.clientKey;
        const bool pinChanged = next.certFingerprint != m_settings.certFingerprint;
        const bool first = !m_configured;
        m_settings = next;
        m_configured = true;
        m_deviceId = deviceIdFor(m_info, m_settings);
        emitDevice();

        if (!m_settings.hasHost()) {
            log(sdk::LogLevel::Warn, sdk::LogCategory::Config, "no TV host configured");
            dropConnection();
            setConnected(false);
            return;
        }
        if (first || endpointChanged || keyChanged || pinChanged) {
            dropConnection();
            setConnected(false);
            connect();
        }
    }

private:
    // --- the connection ----------------------------------------------------

    void connect()
    {
        m_retry.reset();
        dropConnection();
        if (!m_running || !m_settings.hasHost())
            return;

        m_ws.emplace(*m_loop);
        WsClient::Callbacks callbacks;
        callbacks.opened = [this]() { onOpened(); };
        callbacks.textMessage = [this](std::string_view text) { onText(text); };
        callbacks.closed = [this](const std::string &message) { onClosed(message); };
        callbacks.activity = [this]() { m_lastActivityMs = nowMs(); };

        std::string error;
        if (!m_ws->open(m_settings.url(), std::move(callbacks), &error)) {
            noteUnreachable(error);
            scheduleRetry();
            return;
        }
        m_deadline = m_loop->timerAfter(kOpenTimeout, [this]() {
            noteUnreachable("no answer within the connect timeout");
            reconnectLater();
        });
    }

    void onOpened()
    {
        m_deadline.reset();
        if (m_settings.useTls && !acceptCertificate())
            return;
        if (m_wasUnreachable) {
            log(sdk::LogLevel::Info, sdk::LogCategory::Network, "TV at %1 answers again",
                {v1::Utf8String(m_settings.url())});
            m_wasUnreachable = false;
        }
        m_lastActivityMs = nowMs();
        m_ping = m_loop->timerEvery(kPingInterval, [this]() {
            if (nowMs() - m_lastActivityMs > kSilenceLimitMs) {
                log(sdk::LogLevel::Info, sdk::LogCategory::Network, "TV went silent; reconnecting");
                reconnectLater();
                return;
            }
            if (m_ws)
                m_ws->sendPing();
        });
        m_ws->sendText(registerMessage(m_settings.clientKey));
        // Without a key the TV puts up a prompt; with one it answers at once.
        m_deadline = m_loop->timerAfter(m_settings.clientKey.empty() ? kPairingTimeout : kOpenTimeout, [this]() {
            log(sdk::LogLevel::Warn, sdk::LogCategory::Security, "the TV did not accept the connection in time");
            reconnectLater();
        });
    }

    /// Pins the TV's certificate on first contact and refuses another one
    /// after that. False when the connection was dropped.
    bool acceptCertificate()
    {
        const std::string seen = m_ws->peerFingerprint();
        if (seen.empty()) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Security, "the TV presented no certificate");
            reconnectLater();
            return false;
        }
        if (m_settings.certFingerprint.empty()) {
            m_settings.certFingerprint = seen;
            sendMeta({{"certFingerprint", seen}}, "certFingerprint");
            log(sdk::LogLevel::Info, sdk::LogCategory::Security, "pinned the TV's certificate %1",
                {v1::Utf8String(seen)});
            return true;
        }
        if (seen != m_settings.certFingerprint) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Security,
                "the TV presents a different certificate (%1, pinned %2); clear the pinned one if that is expected",
                {v1::Utf8String(seen), v1::Utf8String(m_settings.certFingerprint)});
            reconnectLater();
            return false;
        }
        return true;
    }

    void onClosed(const std::string &message)
    {
        const bool wasRegistered = m_registered;
        noteUnreachable(message.empty() ? std::string("connection closed") : message, wasRegistered);
        reconnectLater();
    }

    /// Drops the connection now and dials again after the retry interval.
    void reconnectLater()
    {
        dropConnection();
        setConnected(false);
        scheduleRetry();
    }

    void scheduleRetry()
    {
        if (!m_running)
            return;
        m_retry = m_loop->timerAfter(std::chrono::milliseconds(m_settings.retryIntervalMs), [this]() { connect(); });
    }

    void dropConnection()
    {
        m_deadline.reset();
        m_ping.reset();
        if (m_ws)
            m_ws->abort();
        // Not destroyed here: this runs inside the client's own callbacks. The
        // next connect() replaces it, stop() lets it go.
        if (!m_running)
            m_ws.reset();
        m_registered = false;
    }

    /// A TV that is switched off is the normal case: said once at Info, then
    /// only at Debug until it answers again.
    void noteUnreachable(const std::string &why, bool loud = false)
    {
        if (!m_wasUnreachable || loud) {
            log(sdk::LogLevel::Info, sdk::LogCategory::Network, "TV at %1 not reachable: %2",
                {v1::Utf8String(m_settings.url()), v1::Utf8String(why)});
        } else {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Network, "TV at %1 not reachable: %2",
                {v1::Utf8String(m_settings.url()), v1::Utf8String(why)});
        }
        m_wasUnreachable = true;
    }

    // --- SSAP --------------------------------------------------------------

    void onText(std::string_view text)
    {
        m_lastActivityMs = nowMs();
        const SsapMessage message = parseMessage(text);
        if (!message.valid) {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "unreadable message from the TV");
            return;
        }
        if (message.id == kIdRegister) {
            onRegisterAnswer(message);
            return;
        }
        if (!m_registered)
            return;
        if (message.id == kIdSystemInfo)
            onSystemInfo(message);
        else if (message.id == kIdMedia)
            onMedia(message);
        else if (message.id == kIdApp)
            onApp(message);
        else if (message.id == kIdLaunchPoints)
            onLaunchPoints(message);
    }

    void onRegisterAnswer(const SsapMessage &message)
    {
        if (isPairingPrompt(message)) {
            log(sdk::LogLevel::Info, sdk::LogCategory::Security, "confirm the connection on the TV");
            return;
        }
        if (message.type == "error") {
            log(sdk::LogLevel::Warn, sdk::LogCategory::Security, "the TV refused the connection: %1",
                {v1::Utf8String(message.error)});
            reconnectLater();
            return;
        }
        if (message.type != "registered")
            return;

        m_deadline.reset();
        const std::string key = str::trimmed(jsonString(message.payload, "client-key"));
        if (!key.empty() && key != m_settings.clientKey) {
            m_settings.clientKey = key;
            sendMeta({{"clientKey", key}}, "clientKey");
            log(sdk::LogLevel::Info, sdk::LogCategory::Security, "paired with the TV");
        }
        m_registered = true;
        m_mediaUnsupported = false;
        setConnected(true);

        m_ws->sendText(requestMessage(kIdSystemInfo, kUriSystemInfo));
        m_ws->sendText(subscribeMessage(kIdMedia, kUriMediaForeground));
        m_ws->sendText(subscribeMessage(kIdApp, kUriAppForeground));
        m_ws->sendText(subscribeMessage(kIdLaunchPoints, kUriLaunchPoints));
    }

    void onSystemInfo(const SsapMessage &message)
    {
        const std::string model = str::trimmed(jsonString(message.payload, "modelName"));
        if (model.empty() || model == m_model)
            return;
        m_model = model;
        emitDevice(true);
    }

    void onMedia(const SsapMessage &message)
    {
        if (message.type == "error") {
            if (!m_mediaUnsupported)
                log(sdk::LogLevel::Info, sdk::LogCategory::Device, "this TV does not report playback: %1",
                    {v1::Utf8String(message.error)});
            m_mediaUnsupported = true;
            setPlayState(PlayState::Unknown);
            return;
        }
        const PlayState state = playStateFrom(message.payload);
        if (state == PlayState::Unknown && !m_mediaUnsupported) {
            log(sdk::LogLevel::Info, sdk::LogCategory::Device, "the TV answers playback without a state");
            m_mediaUnsupported = true;
        }
        setPlayState(state);
    }

    void onApp(const SsapMessage &message)
    {
        if (message.type == "error")
            return;
        m_appId = foregroundAppId(message.payload);
        emitApp();
    }

    void onLaunchPoints(const SsapMessage &message)
    {
        if (message.type == "error")
            return;
        mergeLaunchPoints(message.payload, m_titles);
        emitApp();
    }

    // --- talking to phi-core -----------------------------------------------

    void emitDevice(bool force = false)
    {
        if (m_deviceId.empty() || (m_deviceSent && !force))
            return;
        const std::string name = str::trimmed(m_info.name).empty() ? std::string("LG TV") : str::trimmed(m_info.name);
        v1::Utf8String error;
        if (!sendDeviceUpdated(buildDevice(m_deviceId, name, m_model), buildChannels(), &error)) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the device: %1", {error});
            return;
        }
        if (!m_deviceSent) {
            m_deviceSent = true;
            emitChannel(kChannelConnectivity, connectivityValue());
            emitChannel(kChannelPlayState, v1::Utf8String(playStateName(m_playState)));
            emitChannel(kChannelApp, v1::Utf8String(m_appShown));
        }
    }

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;
        v1::Utf8String error;
        if (!sendConnectionStateChanged(m_connected, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send connectionStateChanged: %1",
                {error});
        emitChannel(kChannelConnectivity, connectivityValue());
        if (!connected) {
            // A TV that is off plays nothing and shows no app.
            setPlayState(PlayState::Idle);
            m_appId.clear();
            emitApp();
        }
    }

    v1::ScalarValue connectivityValue() const
    {
        return static_cast<std::int64_t>(m_connected ? v1::ConnectivityStatus::Connected
                                                     : v1::ConnectivityStatus::Disconnected);
    }

    void setPlayState(PlayState state)
    {
        if (state == m_playState)
            return;
        m_playState = state;
        emitChannel(kChannelPlayState, v1::Utf8String(playStateName(state)));
    }

    void emitApp()
    {
        const auto title = m_titles.find(m_appId);
        const std::string shown = title == m_titles.end() ? m_appId : title->second;
        if (shown == m_appShown)
            return;
        m_appShown = shown;
        emitChannel(kChannelApp, v1::Utf8String(shown));
    }

    void emitChannel(const char *channelId, const v1::ScalarValue &value)
    {
        if (m_deviceId.empty() || !m_deviceSent)
            return;
        v1::Utf8String error;
        if (!sendChannelStateUpdated(m_deviceId, channelId, value, nowMs(), &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the state of %1: %2",
                {v1::Utf8String(channelId), error});
    }

    void sendMeta(const Json &patch, const char *context)
    {
        v1::Utf8String error;
        if (!sendAdapterMetaUpdated(sdk::formValuesFromJson(dump(patch)), &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send adapterMetaUpdated(%1): %2",
                {v1::Utf8String(context), error});
    }

    phi::runtime::Loop *m_loop = nullptr;
    bool m_running = false;
    bool m_configured = false;
    v1::Adapter m_info;
    WebosSettings m_settings;
    std::string m_deviceId;
    std::string m_model;
    bool m_deviceSent = false;

    std::optional<WsClient> m_ws;
    phi::runtime::Timer m_deadline;
    phi::runtime::Timer m_ping;
    phi::runtime::Timer m_retry;
    std::int64_t m_lastActivityMs = 0;
    bool m_registered = false;
    bool m_connected = false;
    bool m_wasUnreachable = false;

    PlayState m_playState = PlayState::Unknown;
    bool m_mediaUnsupported = false;
    std::string m_appId;
    std::string m_appShown;
    std::map<std::string, std::string> m_titles;
};

} // namespace

std::unique_ptr<sdk::AdapterInstance> makeInstance()
{
    return std::make_unique<WebosInstance>();
}

} // namespace phicore::webos::ipc
