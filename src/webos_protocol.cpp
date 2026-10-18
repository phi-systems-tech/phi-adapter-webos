#include "webos_protocol.h"

namespace phicore::webos::ipc {

namespace {

const Json &permissions()
{
    static const Json list = {
        "APP_TO_APP",
        "CLOSE",
        "CONTROL_AUDIO",
        "CONTROL_DISPLAY",
        "CONTROL_INPUT_JOYSTICK",
        "CONTROL_INPUT_MEDIA_PLAYBACK",
        "CONTROL_INPUT_MEDIA_RECORDING",
        "CONTROL_INPUT_TEXT",
        "CONTROL_INPUT_TV",
        "CONTROL_MOUSE_AND_KEYBOARD",
        "CONTROL_POWER",
        "CONTROL_TV_SCREEN",
        "LAUNCH",
        "LAUNCH_WEBAPP",
        "READ_APP_STATUS",
        "READ_COUNTRY_INFO",
        "READ_CURRENT_CHANNEL",
        "READ_INPUT_DEVICE_LIST",
        "READ_INSTALLED_APPS",
        "READ_LGE_SDX",
        "READ_LGE_TV_INPUT_EVENTS",
        "READ_NETWORK_STATE",
        "READ_NOTIFICATIONS",
        "READ_POWER_STATE",
        "READ_RUNNING_APPS",
        "READ_SETTINGS",
        "READ_TV_CHANNEL_LIST",
        "READ_TV_CURRENT_TIME",
        "READ_UPDATE_INFO",
        "SEARCH",
        "TEST_OPEN",
        "TEST_PROTECTED",
        "TEST_SECURE",
        "UPDATE_FROM_REMOTE_APP",
        "WRITE_NOTIFICATION_ALERT",
        "WRITE_NOTIFICATION_TOAST",
        "WRITE_SETTINGS",
    };
    return list;
}

std::string message(std::string_view type, std::string_view id, std::string_view uri, const Json &payload)
{
    Json out = Json::object();
    out["type"] = type;
    out["id"] = id;
    if (!uri.empty())
        out["uri"] = uri;
    out["payload"] = payload.is_object() ? payload : Json::object();
    return dump(out);
}

} // namespace

std::string registerMessage(const std::string &clientKey)
{
    Json payload = Json::object();
    payload["forcePairing"] = false;
    payload["pairingType"] = "PROMPT";
    payload["manifest"] = {{"appVersion", "1.1"}, {"manifestVersion", 1}, {"permissions", permissions()}};
    if (!clientKey.empty())
        payload["client-key"] = clientKey;
    return message("register", kIdRegister, {}, payload);
}

std::string requestMessage(std::string_view id, std::string_view uri, const Json &payload)
{
    return message("request", id, uri, payload);
}

std::string subscribeMessage(std::string_view id, std::string_view uri, const Json &payload)
{
    return message("subscribe", id, uri, payload);
}

SsapMessage parseMessage(std::string_view text)
{
    SsapMessage out;
    const Json parsed = parseObject(text);
    if (parsed.empty())
        return out;
    out.type = jsonString(parsed, "type");
    if (out.type.empty())
        return out;
    // The TV answers with the id it was sent, string or number.
    if (const auto id = parsed.find("id"); id != parsed.end()) {
        if (id->is_string())
            out.id = id->get<std::string>();
        else if (id->is_number_integer())
            out.id = std::to_string(id->get<std::int64_t>());
    }
    out.error = jsonString(parsed, "error");
    if (const auto payload = parsed.find("payload"); payload != parsed.end() && payload->is_object())
        out.payload = *payload;
    out.valid = true;
    return out;
}

bool isPairingPrompt(const SsapMessage &message)
{
    return message.type == "response" && jsonString(message.payload, "pairingType") == "PROMPT";
}

PlayState playStateFrom(const Json &payload)
{
    const auto info = payload.find("foregroundAppInfo");
    if (info == payload.end() || !info->is_array())
        return PlayState::Unknown;
    for (const Json &entry : *info) {
        const std::string state = jsonString(entry, "playState");
        if (state == "playing")
            return PlayState::Playing;
        if (state == "paused")
            return PlayState::Paused;
        // "unloaded", "loaded", "" and whatever a later firmware adds: an app
        // is in front with nothing running in it.
    }
    return PlayState::Idle;
}

const char *playStateName(PlayState state)
{
    switch (state) {
    case PlayState::Idle:
        return "idle";
    case PlayState::Playing:
        return "playing";
    case PlayState::Paused:
        return "paused";
    case PlayState::Unknown:
        break;
    }
    return "unknown";
}

std::string foregroundAppId(const Json &payload)
{
    return jsonString(payload, "appId");
}

void mergeLaunchPoints(const Json &payload, std::map<std::string, std::string> &titles)
{
    const auto add = [&](const Json &point) {
        std::string id = jsonString(point, "id");
        if (id.empty())
            id = jsonString(point, "appId");
        const std::string title = jsonString(point, "title");
        if (!id.empty() && !title.empty())
            titles[id] = title;
    };
    if (const auto list = payload.find("launchPoints"); list != payload.end() && list->is_array()) {
        for (const Json &point : *list)
            add(point);
    }
    // A change after the first answer names one launch point and why.
    if (const auto one = payload.find("launchPoint"); one != payload.end() && one->is_object()) {
        if (jsonString(payload, "change") == "removed")
            titles.erase(jsonString(*one, "id"));
        else
            add(*one);
    }
}

} // namespace phicore::webos::ipc
