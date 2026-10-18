// SSAP messages, settings and the device model, without a TV or a socket.

#include <phi/adapter/testing/check.h>

#include "webos_model.h"
#include "webos_protocol.h"
#include "webos_settings.h"
#include "webos_wol.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>

using namespace phicore::webos::ipc;
namespace v1 = phicore::adapter::v1;

namespace {

bool contains(const Json &array, const char *value)
{
    return array.is_array() && std::find(array.begin(), array.end(), Json(value)) != array.end();
}

void testRegisterMessageWithoutKey()
{
    // No stored key: the TV has to prompt, so "client-key" must be absent.
    const Json m = parseObject(registerMessage(""));
    PHI_CHECK(m["type"] == "register");
    PHI_CHECK(m["id"] == "register");
    PHI_CHECK(!m.contains("uri"));
    PHI_CHECK(m["payload"].is_object());
    PHI_CHECK(!m["payload"].contains("client-key"));
    PHI_CHECK(m["payload"]["pairingType"] == "PROMPT");
    PHI_CHECK(contains(m["payload"]["manifest"]["permissions"], "READ_RUNNING_APPS"));
    PHI_CHECK(contains(m["payload"]["manifest"]["permissions"], "READ_APP_STATUS"));
}

void testRegisterMessageWithKey()
{
    // A paired TV is told its key so it answers `registered` without a prompt.
    const Json m = parseObject(registerMessage("abc123"));
    PHI_CHECK(m["payload"]["client-key"] == "abc123");
    PHI_CHECK(m["payload"]["pairingType"] == "PROMPT");
    PHI_CHECK(m["type"] == "register");
}

void testRequestAndSubscribeShapes()
{
    const Json r = parseObject(requestMessage("sys", kUriSystemInfo));
    PHI_CHECK(r["type"] == "request");
    PHI_CHECK(r["id"] == "sys");
    PHI_CHECK(r["uri"] == kUriSystemInfo);
    PHI_CHECK(r["payload"].is_object() && r["payload"].empty());

    const Json s = parseObject(subscribeMessage(kIdMedia, kUriMediaForeground, Json{{"subscribe", true}}));
    PHI_CHECK(s["type"] == "subscribe");
    PHI_CHECK(s["id"] == "media");
    PHI_CHECK(s["uri"] == kUriMediaForeground);
    PHI_CHECK(s["payload"]["subscribe"] == true);

    // A non-object payload is replaced by an empty object, never sent as is.
    const Json odd = parseObject(requestMessage("x", "ssap://y", Json::array()));
    PHI_CHECK(odd["payload"].is_object());
}

void testParseMessage()
{
    auto m = parseMessage(R"({"type":"response","id":"media","payload":{"a":1}})");
    PHI_CHECK(m.valid);
    PHI_CHECK(m.type == "response");
    PHI_CHECK(m.id == "media");
    PHI_CHECK(m.payload["a"] == 1);

    // The TV may answer with a numeric id.
    m = parseMessage(R"({"type":"response","id":42,"payload":{}})");
    PHI_CHECK(m.valid);
    PHI_CHECK(m.id == "42");

    // No type: not an SSAP message.
    PHI_CHECK(!parseMessage(R"({"id":"x","payload":{}})").valid);
    PHI_CHECK(!parseMessage("garbage").valid);
    PHI_CHECK(!parseMessage("").valid);
    PHI_CHECK(!parseMessage("[1,2]").valid);

    m = parseMessage(R"({"type":"error","id":"x","error":"401 insufficient permissions","payload":{}})");
    PHI_CHECK(m.valid);
    PHI_CHECK(m.type == "error");
    PHI_CHECK(m.error == "401 insufficient permissions");

    // A payload that is not an object reads as an empty object.
    m = parseMessage(R"({"type":"response","id":"x","payload":"nope"})");
    PHI_CHECK(m.valid);
    PHI_CHECK(m.payload.is_object() && m.payload.empty());
    m = parseMessage(R"({"type":"response","id":"x"})");
    PHI_CHECK(m.valid);
    PHI_CHECK(m.payload.is_object() && m.payload.empty());
}

void testIsPairingPrompt()
{
    PHI_CHECK(isPairingPrompt(parseMessage(R"({"type":"response","id":"register","payload":{"pairingType":"PROMPT","returnValue":true}})")));
    // `registered` is the end of pairing, not the prompt.
    PHI_CHECK(!isPairingPrompt(parseMessage(R"({"type":"registered","id":"register","payload":{"client-key":"k"}})")));
    PHI_CHECK(!isPairingPrompt(parseMessage(R"({"type":"response","id":"register","payload":{"pairingType":"PIN"}})")));
    PHI_CHECK(!isPairingPrompt(parseMessage(R"({"type":"response","id":"register","payload":{}})")));
}

void testPlayStateFrom()
{
    const auto state = [](const char *text) { return playStateFrom(parseObject(text)); };
    PHI_CHECK(state(R"({"foregroundAppInfo":[{"playState":"playing"}]})") == PlayState::Playing);
    PHI_CHECK(state(R"({"foregroundAppInfo":[{"playState":"paused"}]})") == PlayState::Paused);
    PHI_CHECK(state(R"({"foregroundAppInfo":[{"playState":"unloaded"}]})") == PlayState::Idle);
    PHI_CHECK(state(R"({"foregroundAppInfo":[{"playState":""}]})") == PlayState::Idle);
    // Nothing loaded at all.
    PHI_CHECK(state(R"({"foregroundAppInfo":[]})") == PlayState::Idle);
    // webOS 4.5 answers without the key: we do not know.
    PHI_CHECK(state("{}") == PlayState::Unknown);
    PHI_CHECK(state(R"({"foregroundAppInfo":"x"})") == PlayState::Unknown);
    // The first entry carrying a state wins.
    PHI_CHECK(state(R"({"foregroundAppInfo":[{"playState":"unloaded"},{"playState":"paused"},{"playState":"playing"}]})")
              == PlayState::Paused);
    PHI_CHECK(state(R"({"foregroundAppInfo":[{"playState":"playing"},{"playState":"paused"}]})")
              == PlayState::Playing);
}

void testPlayStateName()
{
    PHI_CHECK(std::string(playStateName(PlayState::Playing)) == "playing");
    PHI_CHECK(std::string(playStateName(PlayState::Paused)) == "paused");
    PHI_CHECK(std::string(playStateName(PlayState::Idle)) == "idle");
    PHI_CHECK(std::string(playStateName(PlayState::Unknown)) == "unknown");
}

void testForegroundAppId()
{
    PHI_CHECK(foregroundAppId(parseObject(R"({"appId":"netflix","returnValue":true})")) == "netflix");
    PHI_CHECK(foregroundAppId(parseObject(R"({"appId":""})")).empty());
    PHI_CHECK(foregroundAppId(parseObject("{}")).empty());
    PHI_CHECK(foregroundAppId(parseObject(R"({"appId":5})")).empty());
}

void testMergeLaunchPoints()
{
    std::map<std::string, std::string> titles;
    mergeLaunchPoints(parseObject(R"({"launchPoints":[{"id":"netflix","title":"Netflix"},{"id":"youtube.leanback.v4","title":"YouTube"},{"id":"","title":"X"},{"id":"notitle"}]})"),
                      titles);
    PHI_CHECK(titles.size() == 2);
    PHI_CHECK(titles["netflix"] == "Netflix");
    PHI_CHECK(titles["youtube.leanback.v4"] == "YouTube");

    mergeLaunchPoints(parseObject(R"({"change":"added","launchPoint":{"id":"plex","title":"Plex"}})"), titles);
    PHI_CHECK(titles.size() == 3 && titles["plex"] == "Plex");

    mergeLaunchPoints(parseObject(R"({"change":"updated","launchPoint":{"id":"netflix","title":"Netflix 2"}})"), titles);
    PHI_CHECK(titles.size() == 3 && titles["netflix"] == "Netflix 2");

    mergeLaunchPoints(parseObject(R"({"change":"removed","launchPoint":{"id":"plex"}})"), titles);
    PHI_CHECK(titles.size() == 2 && titles.count("plex") == 0);

    // Removing something unknown, or an empty answer, changes nothing.
    mergeLaunchPoints(parseObject(R"({"change":"removed","launchPoint":{"id":"ghost"}})"), titles);
    mergeLaunchPoints(parseObject("{}"), titles);
    PHI_CHECK(titles.size() == 2);
}

v1::Adapter adapterWith(const char *ip, const char *host, std::uint16_t port, const char *externalId = "")
{
    v1::Adapter a;
    a.ip = ip;
    a.host = host;
    a.port = port;
    a.externalId = externalId;
    return a;
}

void testSettingsHost()
{
    // Meta host beats adapter.ip, which beats adapter.host.
    auto s = settingsFromAdapter(adapterWith("10.0.0.2", "tv.local", 0), parseObject(R"({"host":" 10.0.0.9 "})"));
    PHI_CHECK(s.host == "10.0.0.9");
    PHI_CHECK(s.hasHost());
    s = settingsFromAdapter(adapterWith("10.0.0.2", "tv.local", 0), Json::object());
    PHI_CHECK(s.host == "10.0.0.2");
    s = settingsFromAdapter(adapterWith("", "tv.local", 0), Json::object());
    PHI_CHECK(s.host == "tv.local");
    s = settingsFromAdapter(adapterWith("", "", 0), Json::object());
    PHI_CHECK(!s.hasHost());
}

void testSettingsPort()
{
    auto s = settingsFromAdapter(adapterWith("h", "", 0), Json::object());
    PHI_CHECK(s.port == 3001 && s.useTls);
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"useTls":false})"));
    PHI_CHECK(s.port == 3000 && !s.useTls);
    // The adapter's port is used when meta has none.
    s = settingsFromAdapter(adapterWith("h", "", 4444), Json::object());
    PHI_CHECK(s.port == 4444);
    s = settingsFromAdapter(adapterWith("h", "", 4444), parseObject(R"({"port":5555})"));
    PHI_CHECK(s.port == 5555);
    // A numeric string is a port too.
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"port":"3002"})"));
    PHI_CHECK(s.port == 3002);
    // Invalid ports fall back.
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"port":70000})"));
    PHI_CHECK(s.port == 3001);
    s = settingsFromAdapter(adapterWith("h", "", 4444), parseObject(R"({"port":-1})"));
    PHI_CHECK(s.port == 4444);
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"port":0,"useTls":false})"));
    PHI_CHECK(s.port == 3000);
}

void testSettingsKeysAndRetry()
{
    auto s = settingsFromAdapter(adapterWith("h", "", 0),
                            parseObject(R"({"clientKey":" k1 ","certFingerprint":"ABCDEF01"})"));
    PHI_CHECK(s.clientKey == "k1");
    PHI_CHECK(s.certFingerprint == "abcdef01");

    s = settingsFromAdapter(adapterWith("h", "", 0), Json::object());
    PHI_CHECK(s.retryIntervalMs == 10000);
    PHI_CHECK(s.clientKey.empty() && s.certFingerprint.empty());
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"retryIntervalMs":999})"));
    PHI_CHECK(s.retryIntervalMs == 10000);
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"retryIntervalMs":1000})"));
    PHI_CHECK(s.retryIntervalMs == 1000);
    s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"retryIntervalMs":30000})"));
    PHI_CHECK(s.retryIntervalMs == 30000);
}

void testSettingsUrl()
{
    WebosSettings s;
    s.host = "192.168.1.5";
    PHI_CHECK(s.url() == "wss://192.168.1.5:3001/");
    s.useTls = false;
    s.port = 3000;
    PHI_CHECK(s.url() == "ws://192.168.1.5:3000/");
    s.host = "fe80::1";
    PHI_CHECK(s.url() == "ws://[fe80::1]:3000/");
    s.useTls = true;
    s.port = 3001;
    PHI_CHECK(s.url() == "wss://[fe80::1]:3001/");
}

void testDeviceIdFor()
{
    WebosSettings s;
    s.host = "10.0.0.2";
    PHI_CHECK(deviceIdFor(adapterWith("", "", 0, "ext-1"), s) == "ext-1");
    PHI_CHECK(deviceIdFor(adapterWith("", "", 0), s) == "10.0.0.2");
    s.host.clear();
    PHI_CHECK(deviceIdFor(adapterWith("", "", 0), s) == "webos-tv");
}

bool hasRw(const v1::Channel &c, bool writable)
{
    return v1::hasFlag(c.flags, v1::ChannelFlag::Readable) && v1::hasFlag(c.flags, v1::ChannelFlag::Reportable)
           && v1::hasFlag(c.flags, v1::ChannelFlag::Writable) == writable;
}

bool sameChoices(const v1::AdapterConfigOptionList &a, const v1::AdapterConfigOptionList &b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].value != b[i].value || a[i].label != b[i].label)
            return false;
    }
    return true;
}

void testBuildChannels()
{
    const v1::AdapterConfigOptionList inputs{{"HDMI_1", "PlayStation"}, {"HDMI_2", "HDMI_2"}};
    const v1::AdapterConfigOptionList apps{{"netflix", "Netflix"}, {"youtube.leanback.v4", "YouTube"}, {"plex", "Plex"}};
    const v1::ChannelList channels = buildChannels(inputs, apps);
    PHI_CHECK(channels.size() == 7);
    if (channels.size() != 7)
        return;
    PHI_CHECK(channels[0].externalId == "power");
    PHI_CHECK(channels[1].externalId == "volume");
    PHI_CHECK(channels[2].externalId == "mute");
    PHI_CHECK(channels[3].externalId == "input");
    PHI_CHECK(channels[4].externalId == "app");
    PHI_CHECK(channels[5].externalId == "playState");
    PHI_CHECK(channels[6].externalId == "connectivity");

    PHI_CHECK(channels[0].kind == v1::ChannelKind::PowerOnOff);
    PHI_CHECK(channels[0].dataType == v1::ChannelDataType::Bool);
    PHI_CHECK(hasRw(channels[0], true));

    PHI_CHECK(channels[1].kind == v1::ChannelKind::Volume);
    PHI_CHECK(channels[1].dataType == v1::ChannelDataType::Int);
    PHI_CHECK(channels[1].minValue == 0.0 && channels[1].maxValue == 100.0 && channels[1].stepValue == 1.0);
    PHI_CHECK(hasRw(channels[1], true));

    PHI_CHECK(channels[2].kind == v1::ChannelKind::Mute);
    PHI_CHECK(channels[2].dataType == v1::ChannelDataType::Bool);
    PHI_CHECK(hasRw(channels[2], true));

    PHI_CHECK(channels[3].kind == v1::ChannelKind::HdmiInput);
    PHI_CHECK(channels[3].dataType == v1::ChannelDataType::String);
    PHI_CHECK(hasRw(channels[3], true));
    PHI_CHECK(sameChoices(channels[3].choices, inputs));

    PHI_CHECK(channels[4].kind == v1::ChannelKind::Unknown);
    PHI_CHECK(channels[4].dataType == v1::ChannelDataType::String);
    PHI_CHECK(hasRw(channels[4], true));
    PHI_CHECK(sameChoices(channels[4].choices, apps));

    PHI_CHECK(channels[5].dataType == v1::ChannelDataType::String);
    PHI_CHECK(hasRw(channels[5], false));
    std::vector<std::string> values;
    for (const auto &choice : channels[5].choices)
        values.push_back(choice.value);
    PHI_CHECK((values == std::vector<std::string>{"playing", "paused", "idle", "unknown"}));
    PHI_CHECK(channels[5].choices.size() == 4 && channels[5].choices[0].label == "Playing");

    PHI_CHECK(channels[6].kind == v1::ChannelKind::ConnectivityStatus);
    PHI_CHECK(channels[6].dataType == v1::ChannelDataType::Enum);
    PHI_CHECK(hasRw(channels[6], false));

    // Empty lists give empty choices, still seven channels.
    const v1::ChannelList bare = buildChannels({}, {});
    PHI_CHECK(bare.size() == 7 && bare[3].choices.empty() && bare[4].choices.empty());
}

void testBuildInputAndAppChannel()
{
    const v1::AdapterConfigOptionList inputs{{"HDMI_3", "Switch"}};
    const v1::Channel in = buildInputChannel(inputs);
    PHI_CHECK(in.externalId == "input" && in.kind == v1::ChannelKind::HdmiInput);
    PHI_CHECK(sameChoices(in.choices, inputs));
    const v1::AdapterConfigOptionList apps{{"netflix", "Netflix"}, {"plex", "Plex"}};
    const v1::Channel app = buildAppChannel(apps);
    PHI_CHECK(app.externalId == "app" && app.kind == v1::ChannelKind::Unknown);
    PHI_CHECK(sameChoices(app.choices, apps));
}

void testBuildDevice()
{
    const v1::Device d = buildDevice("tv1", "Living room", "OLED55C1");
    PHI_CHECK(d.externalId == "tv1");
    PHI_CHECK(d.deviceClass == v1::DeviceClass::MediaPlayer);
    PHI_CHECK(d.manufacturer == "LG Electronics");
    PHI_CHECK(d.name == "Living room");
    PHI_CHECK(d.model == "OLED55C1");
    PHI_CHECK(d.mediaPlayerFeatures
              == (v1::MediaPlayerFeature::Play | v1::MediaPlayerFeature::Pause | v1::MediaPlayerFeature::Stop));
}

void testIsSuccess()
{
    const auto ok = [](const char *text) { return isSuccess(parseMessage(text)); };
    PHI_CHECK(ok(R"({"type":"response","id":"x","payload":{}})"));
    PHI_CHECK(ok(R"({"type":"response","id":"x"})"));
    PHI_CHECK(ok(R"({"type":"response","id":"x","payload":{"returnValue":true}})"));
    PHI_CHECK(!ok(R"({"type":"response","id":"x","payload":{"returnValue":false}})"));
    PHI_CHECK(!ok(R"({"type":"error","id":"x","error":"401 insufficient permissions","payload":{}})"));
    PHI_CHECK(!ok(R"({"type":"registered","id":"register","payload":{"client-key":"k"}})"));
}

void testPowerOnFrom()
{
    const auto power = [](const char *text) { return powerOnFrom(parseObject(text)); };
    PHI_CHECK(power(R"({"state":"Active"})") == std::optional<bool>(true));
    PHI_CHECK(power(R"({"state":"Screen Off"})") == std::optional<bool>(true));
    PHI_CHECK(power(R"({"state":"Screen Saver"})") == std::optional<bool>(true));
    PHI_CHECK(power(R"({"state":"Active Standby"})") == std::optional<bool>(false));
    PHI_CHECK(power(R"({"state":"Suspend"})") == std::optional<bool>(false));
    PHI_CHECK(!power("{}").has_value());
}

void testVolumeFrom()
{
    auto v = volumeFrom(parseObject(R"({"volume":12,"muted":false})"));
    PHI_CHECK(v.volume == std::optional<int>(12) && v.muted == std::optional<bool>(false));

    v = volumeFrom(parseObject(R"({"volumeStatus":{"volume":30,"muteStatus":true}})"));
    PHI_CHECK(v.volume == std::optional<int>(30) && v.muted == std::optional<bool>(true));

    v = volumeFrom(parseObject(R"({"volume":150})"));
    PHI_CHECK(v.volume == std::optional<int>(100));
    v = volumeFrom(parseObject(R"({"volume":-5})"));
    PHI_CHECK(v.volume == std::optional<int>(0));

    v = volumeFrom(parseObject("{}"));
    PHI_CHECK(!v.volume.has_value() && !v.muted.has_value());

    v = volumeFrom(parseObject(R"({"volume":7,"muted":"yes"})"));
    PHI_CHECK(v.volume == std::optional<int>(7) && !v.muted.has_value());
}

void testInputsFrom()
{
    const auto inputs = inputsFrom(parseObject(
        R"({"devices":[{"id":"HDMI_1","label":"PlayStation","appId":"com.webos.app.hdmi1"},{"id":"HDMI_2","appId":"com.webos.app.hdmi2"},{"label":"no id"}]})"));
    PHI_CHECK(inputs.size() == 2);
    if (inputs.size() == 2) {
        PHI_CHECK(inputs[0].id == "HDMI_1" && inputs[0].label == "PlayStation" && inputs[0].appId == "com.webos.app.hdmi1");
        PHI_CHECK(inputs[1].id == "HDMI_2" && inputs[1].label == "HDMI_2" && inputs[1].appId == "com.webos.app.hdmi2");
    }
    PHI_CHECK(inputsFrom(parseObject("{}")).empty());
    PHI_CHECK(inputsFrom(parseObject(R"({"devices":"x"})")).empty());
    PHI_CHECK(inputsFrom(parseObject(R"({"devices":{"id":"HDMI_1"}})")).empty());
}

bool isMagicPacketFor(const std::string &p, const unsigned char (&mac)[6])
{
    if (p.size() != 102)
        return false;
    for (int i = 0; i < 6; ++i) {
        if (static_cast<unsigned char>(p[static_cast<std::size_t>(i)]) != 0xFF)
            return false;
    }
    for (int rep = 0; rep < 16; ++rep) {
        for (int i = 0; i < 6; ++i) {
            if (static_cast<unsigned char>(p[static_cast<std::size_t>(6 + rep * 6 + i)]) != mac[i])
                return false;
        }
    }
    return true;
}

void testWakeOnLanPacket()
{
    const unsigned char mac[6] = {0x44, 0xcb, 0x8b, 0x81, 0x89, 0xe4};
    PHI_CHECK(isMagicPacketFor(wakeOnLanPacket("44:cb:8b:81:89:e4"), mac));
    PHI_CHECK(isMagicPacketFor(wakeOnLanPacket("44-CB-8B-81-89-E4"), mac));
    PHI_CHECK(isMagicPacketFor(wakeOnLanPacket("44cb8b8189e4"), mac));

    PHI_CHECK(wakeOnLanPacket("").empty());
    PHI_CHECK(wakeOnLanPacket("44:cb:8b").empty());
    PHI_CHECK(wakeOnLanPacket("zz:zz:zz:zz:zz:zz").empty());
    PHI_CHECK(wakeOnLanPacket("44:cb:8b:81:89:e4:01").empty());
    PHI_CHECK(wakeOnLanPacket("44:cb:8b:81:89:e").empty());
    PHI_CHECK(wakeOnLanPacket("44cb8b8189e").empty());
}

void testSettingsMac()
{
    auto s = settingsFromAdapter(adapterWith("h", "", 0), parseObject(R"({"macAddress":" 44:CB:8B:81:89:E4 "})"));
    PHI_CHECK(s.macAddress == "44:cb:8b:81:89:e4");
    s = settingsFromAdapter(adapterWith("h", "", 0), Json::object());
    PHI_CHECK(s.macAddress.empty());
}

void testMacFromArpTable()
{
    // TEST-NET-3, never on a LAN: not in the neighbour table.
    PHI_CHECK(macFromArpTable("203.0.113.77").empty());
    PHI_CHECK(macFromArpTable("").empty());
    PHI_CHECK(macFromArpTable("not an ip").empty());
}

} // namespace

int main()
{
    testRegisterMessageWithoutKey();
    testRegisterMessageWithKey();
    testRequestAndSubscribeShapes();
    testParseMessage();
    testIsPairingPrompt();
    testPlayStateFrom();
    testPlayStateName();
    testForegroundAppId();
    testMergeLaunchPoints();
    testSettingsHost();
    testSettingsPort();
    testSettingsKeysAndRetry();
    testSettingsUrl();
    testDeviceIdFor();
    testBuildChannels();
    testBuildInputAndAppChannel();
    testBuildDevice();
    testIsSuccess();
    testPowerOnFrom();
    testVolumeFrom();
    testInputsFrom();
    testWakeOnLanPacket();
    testSettingsMac();
    testMacFromArpTable();
    return phi::testing::report("webos_protocol_tests");
}
