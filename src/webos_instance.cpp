#include "webos_instance.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "phi/adapter/sdk/schema_json.h"
#include "phi/runtime/loop.h"
#include "phi/runtime/str.h"

#include "webos_json.h"
#include "webos_model.h"
#include "webos_protocol.h"
#include "webos_settings.h"
#include "webos_wol.h"
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
/// After Wake-on-LAN the TV takes a while to open its socket; dial sooner
/// than the retry interval would.
constexpr auto kDialAfterWake = 4s;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<double> scalarToDouble(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<double>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*v);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? 1.0 : 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        bool ok = false;
        const double parsed = str::toDouble(str::trimmed(*v), &ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

std::optional<bool> scalarToBool(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<bool>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v != 0;
    if (const auto *v = std::get_if<double>(&value))
        return *v != 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        const std::string text = str::toLower(str::trimmed(*v));
        if (text == "true" || text == "1" || text == "on")
            return true;
        if (text == "false" || text == "0" || text == "off")
            return false;
    }
    return std::nullopt;
}

std::string scalarToString(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<v1::Utf8String>(&value))
        return str::trimmed(*v);
    return {};
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
        m_info = request.adapter();
        const WebosSettings next = settingsFromAdapter(request.adapter(), parseObject(request.adapter().metaJson()));

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

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        if (request.deviceExternalId() != m_deviceId) {
            answer(request.cmdId(), v1::CmdStatus::NotSupported, "Unknown device");
            return;
        }
        const std::string &channel = request.channelExternalId();
        if (channel == kChannelPower) {
            const std::optional<bool> on = scalarToBool(request.value());
            if (!on) {
                answer(request.cmdId(), v1::CmdStatus::InvalidArgument, "Power takes on or off");
                return;
            }
            setPower(request.cmdId(), *on);
            return;
        }
        if (!m_registered) {
            answer(request.cmdId(), v1::CmdStatus::TemporarilyOffline, "The TV is off or not reachable");
            return;
        }
        if (channel == kChannelVolume) {
            const std::optional<double> volume = scalarToDouble(request.value());
            if (!volume) {
                answer(request.cmdId(), v1::CmdStatus::InvalidArgument, "Volume takes 0 to 100");
                return;
            }
            const int level = std::clamp(static_cast<int>(std::lround(*volume)), 0, 100);
            sendCommand(request.cmdId(), kUriSetVolume, {{"volume", level}});
        } else if (channel == kChannelMute) {
            const std::optional<bool> muted = scalarToBool(request.value());
            if (!muted) {
                answer(request.cmdId(), v1::CmdStatus::InvalidArgument, "Mute takes on or off");
                return;
            }
            sendCommand(request.cmdId(), kUriSetMute, {{"mute", *muted}});
        } else if (channel == kChannelInput) {
            const std::string input = scalarToString(request.value());
            if (input.empty()) {
                answer(request.cmdId(), v1::CmdStatus::InvalidArgument, "No input given");
                return;
            }
            sendCommand(request.cmdId(), kUriSwitchInput, {{"inputId", input}});
        } else if (channel == kChannelApp) {
            const std::string app = scalarToString(request.value());
            if (app.empty()) {
                answer(request.cmdId(), v1::CmdStatus::InvalidArgument, "No app given");
                return;
            }
            sendCommand(request.cmdId(), kUriLaunch, {{"id", app}});
        } else {
            answer(request.cmdId(), v1::CmdStatus::NotSupported, "This channel is read-only");
        }
    }

    void onMediaPlayerInvoke(const sdk::MediaPlayerInvokeRequest &request) override
    {
        if (request.deviceExternalId() != m_deviceId) {
            answer(request.cmdId(), v1::CmdStatus::NotSupported, "Unknown device");
            return;
        }
        const char *uri = nullptr;
        switch (request.action()) {
        case v1::MediaPlayerAction::Play:
            uri = kUriPlay;
            break;
        case v1::MediaPlayerAction::Pause:
            uri = kUriPause;
            break;
        case v1::MediaPlayerAction::Stop:
            uri = kUriStop;
            break;
        default:
            answer(request.cmdId(), v1::CmdStatus::NotSupported, "Media player action not supported");
            return;
        }
        if (!m_registered) {
            answer(request.cmdId(), v1::CmdStatus::TemporarilyOffline, "The TV is off or not reachable");
            return;
        }
        sendCommand(request.cmdId(), uri, Json::object());
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

    void scheduleRetry(std::optional<std::chrono::milliseconds> delay = std::nullopt)
    {
        if (!m_running)
            return;
        m_retry = m_loop->timerAfter(delay.value_or(std::chrono::milliseconds(m_settings.retryIntervalMs)),
                                     [this]() { connect(); });
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
        failPendingCommands();
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
        if (message.id.starts_with(kCommandIdPrefix))
            onCommandAnswer(message);
        else if (message.id == kIdSystemInfo)
            onSystemInfo(message);
        else if (message.id == kIdMedia)
            onMedia(message);
        else if (message.id == kIdApp)
            onApp(message);
        else if (message.id == kIdLaunchPoints)
            onLaunchPoints(message);
        else if (message.id == kIdPower)
            onPower(message);
        else if (message.id == kIdVolume)
            onVolume(message);
        else if (message.id == kIdInputs)
            onInputs(message);
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
        m_powerUnsupported = false;
        m_powerOn.reset();
        setConnected(true);
        learnMacAddress();

        m_ws->sendText(requestMessage(kIdSystemInfo, kUriSystemInfo));
        m_ws->sendText(subscribeMessage(kIdPower, kUriPowerState));
        m_ws->sendText(subscribeMessage(kIdVolume, kUriVolume));
        m_ws->sendText(requestMessage(kIdInputs, kUriInputs));
        m_ws->sendText(subscribeMessage(kIdMedia, kUriMediaForeground));
        m_ws->sendText(subscribeMessage(kIdApp, kUriAppForeground));
        m_ws->sendText(subscribeMessage(kIdLaunchPoints, kUriLaunchPoints));
    }

    /// Wake-on-LAN needs the MAC, and the only time to learn it is while the
    /// TV answers: the neighbour table has it now.
    void learnMacAddress()
    {
        const std::string mac = macFromArpTable(m_settings.host);
        if (mac.empty() || mac == m_settings.macAddress)
            return;
        m_settings.macAddress = mac;
        sendMeta({{"macAddress", mac}}, "macAddress");
        log(sdk::LogLevel::Info, sdk::LogCategory::Device, "learnt the TV's MAC address %1", {v1::Utf8String(mac)});
    }

    void onSystemInfo(const SsapMessage &message)
    {
        const std::string model = str::trimmed(jsonString(message.payload, "modelName"));
        if (model.empty() || model == m_model)
            return;
        m_model = model;
        emitDevice(true);
    }

    void onPower(const SsapMessage &message)
    {
        if (message.type == "error") {
            if (!m_powerUnsupported)
                log(sdk::LogLevel::Info, sdk::LogCategory::Device,
                    "this TV does not report its power state (%1); power follows the connection",
                    {v1::Utf8String(message.error)});
            m_powerUnsupported = true;
            return;
        }
        m_powerState = jsonString(message.payload, "state");
        if (const std::optional<bool> on = powerOnFrom(message.payload))
            m_powerOn = on;
        emitPower();
    }

    void onVolume(const SsapMessage &message)
    {
        if (message.type == "error") {
            log(sdk::LogLevel::Info, sdk::LogCategory::Device, "this TV does not report its volume: %1",
                {v1::Utf8String(message.error)});
            return;
        }
        const VolumeState state = volumeFrom(message.payload);
        if (state.volume && state.volume != m_volumeShown) {
            m_volumeShown = state.volume;
            emitChannel(kChannelVolume, static_cast<std::int64_t>(*state.volume));
        }
        if (state.muted && state.muted != m_muteShown) {
            m_muteShown = state.muted;
            emitChannel(kChannelMute, *state.muted);
        }
    }

    void onInputs(const SsapMessage &message)
    {
        if (message.type == "error") {
            log(sdk::LogLevel::Info, sdk::LogCategory::Device, "this TV does not list its inputs: %1",
                {v1::Utf8String(message.error)});
            return;
        }
        std::vector<ExternalInput> inputs = inputsFrom(message.payload);
        const bool changed = inputs.size() != m_inputs.size()
                             || !std::equal(inputs.begin(), inputs.end(), m_inputs.begin(),
                                            [](const ExternalInput &a, const ExternalInput &b) {
                                                return a.id == b.id && a.label == b.label && a.appId == b.appId;
                                            });
        if (!changed)
            return;
        m_inputs = std::move(inputs);
        publishChannel(buildInputChannel(inputChoices()));
        emitInput();
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
        emitInput();
    }

    void onLaunchPoints(const SsapMessage &message)
    {
        if (message.type == "error")
            return;
        const std::map<std::string, std::string> before = m_titles;
        mergeLaunchPoints(message.payload, m_titles);
        if (m_titles != before)
            publishChannel(buildAppChannel(appChoices()));
    }

    // --- commands ----------------------------------------------------------

    void setPower(v1::CmdId cmdId, bool on)
    {
        if (!on) {
            if (!m_registered) {
                // Not answering is what off looks like from here.
                answer(cmdId, v1::CmdStatus::Success, {});
                return;
            }
            sendCommand(cmdId, kUriTurnOff, Json::object());
            return;
        }
        if (m_registered) {
            // A dark panel playing sound is on to the TV, not to whoever asks.
            if (m_powerState == "Screen Off" || m_powerState == "Screen Saver")
                sendCommand(cmdId, kUriScreenOn, Json::object());
            else
                answer(cmdId, v1::CmdStatus::Success, {});
            return;
        }
        std::string mac = m_settings.macAddress;
        if (mac.empty())
            mac = macFromArpTable(m_settings.host);
        if (mac.empty()) {
            answer(cmdId, v1::CmdStatus::Failure,
                   "The TV's MAC address is not known yet; switch it on once with the remote");
            return;
        }
        std::string error;
        if (!sendWakeOnLan(mac, m_settings.host, &error)) {
            answer(cmdId, v1::CmdStatus::Failure, error);
            return;
        }
        log(sdk::LogLevel::Info, sdk::LogCategory::Device, "sent Wake-on-LAN to %1", {v1::Utf8String(mac)});
        // The packet is all there is to send; whether the TV woke shows when
        // it answers.
        answer(cmdId, v1::CmdStatus::Success, {});
        scheduleRetry(kDialAfterWake);
    }

    void sendCommand(v1::CmdId cmdId, const char *uri, const Json &payload)
    {
        if (!m_registered || !m_ws) {
            answer(cmdId, v1::CmdStatus::TemporarilyOffline, "The TV is off or not reachable");
            return;
        }
        const std::string id = std::string(kCommandIdPrefix) + std::to_string(++m_nextCommand);
        m_commands[id] = cmdId;
        m_ws->sendText(requestMessage(id, uri, payload));
    }

    void onCommandAnswer(const SsapMessage &message)
    {
        const auto pending = m_commands.find(message.id);
        if (pending == m_commands.end())
            return;
        const v1::CmdId cmdId = pending->second;
        m_commands.erase(pending);
        if (isSuccess(message)) {
            answer(cmdId, v1::CmdStatus::Success, {});
            return;
        }
        std::string error = message.error;
        if (error.empty())
            error = jsonString(message.payload, "errorText");
        answer(cmdId, v1::CmdStatus::Failure, error.empty() ? std::string("The TV refused the command") : error);
    }

    void failPendingCommands()
    {
        std::map<std::string, v1::CmdId> pending;
        pending.swap(m_commands);
        for (const auto &[id, cmdId] : pending)
            answer(cmdId, v1::CmdStatus::TemporarilyOffline, "The connection to the TV ended");
    }

    void answer(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::CmdResponse response = v1::CmdResponse().setId(cmdId).setStatus(status).setError(error).setTsMs(nowMs());
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send a command result: %1",
                {sendError});
    }

    // --- talking to phi-core -----------------------------------------------

    v1::AdapterConfigOptionList inputChoices() const
    {
        v1::AdapterConfigOptionList out;
        for (const ExternalInput &input : m_inputs)
            out.push_back({input.id, input.label});
        return out;
    }

    v1::AdapterConfigOptionList appChoices() const
    {
        v1::AdapterConfigOptionList out;
        for (const auto &[id, title] : m_titles)
            out.push_back({id, title});
        std::sort(out.begin(), out.end(),
                  [](const v1::AdapterConfigOption &a, const v1::AdapterConfigOption &b) { return a.label() < b.label(); });
        return out;
    }

    void emitDevice(bool force = false)
    {
        if (m_deviceId.empty() || (m_deviceSent && !force))
            return;
        const std::string name = str::trimmed(m_info.name()).empty() ? std::string("LG TV") : str::trimmed(m_info.name());
        v1::Utf8String error;
        if (!sendDeviceUpdated(buildDevice(m_deviceId, name, m_model), buildChannels(inputChoices(), appChoices()),
                               &error)) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the device: %1", {error});
            return;
        }
        if (!m_deviceSent) {
            m_deviceSent = true;
            emitChannel(kChannelConnectivity, connectivityValue());
            emitChannel(kChannelPower, powerValue());
            emitChannel(kChannelPlayState, v1::Utf8String(playStateName(m_playState)));
            emitChannel(kChannelApp, v1::Utf8String(m_appShown));
            emitChannel(kChannelInput, v1::Utf8String(m_inputShown));
        }
    }

    void publishChannel(const v1::Channel &channel)
    {
        if (m_deviceId.empty() || !m_deviceSent)
            return;
        v1::Utf8String error;
        if (!sendChannelUpdated(m_deviceId, channel, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send channelUpdated(%1): %2",
                {v1::Utf8String(channel.externalId()), error});
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
            m_powerState.clear();
            m_powerOn.reset();
            setPlayState(PlayState::Idle);
            m_appId.clear();
            emitApp();
            emitInput();
        }
        emitPower();
    }

    v1::ScalarValue connectivityValue() const
    {
        return static_cast<std::int64_t>(m_connected ? v1::ConnectivityStatus::Connected
                                                     : v1::ConnectivityStatus::Disconnected);
    }

    /// On while the TV answers, unless it says it is in standby.
    bool powerValue() const { return m_connected && m_powerOn.value_or(true); }

    void emitPower()
    {
        const bool on = powerValue();
        if (m_powerShown == on)
            return;
        m_powerShown = on;
        emitChannel(kChannelPower, on);
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
        if (m_appId == m_appShown)
            return;
        m_appShown = m_appId;
        emitChannel(kChannelApp, v1::Utf8String(m_appShown));
    }

    /// The input in front is the one whose app is in front; anything else -
    /// Netflix, live TV - is no input.
    void emitInput()
    {
        std::string current;
        for (const ExternalInput &input : m_inputs) {
            if (!m_appId.empty() && input.appId == m_appId)
                current = input.id;
        }
        if (current == m_inputShown)
            return;
        m_inputShown = current;
        emitChannel(kChannelInput, v1::Utf8String(current));
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

    std::map<std::string, v1::CmdId> m_commands;
    std::uint64_t m_nextCommand = 0;

    std::string m_powerState;
    std::optional<bool> m_powerOn;
    std::optional<bool> m_powerShown;
    bool m_powerUnsupported = false;
    std::optional<int> m_volumeShown;
    std::optional<bool> m_muteShown;
    std::vector<ExternalInput> m_inputs;
    std::string m_inputShown;

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
