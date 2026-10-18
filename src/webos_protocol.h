#pragma once

// SSAP, the JSON the TV speaks over its WebSocket: what goes out and what the
// answers mean, without a socket. Every message is
// `{"type", "id", "uri"?, "payload"}`; an answer carries the id of the request
// it belongs to, and a subscription keeps answering under that id.
//
// The shapes follow aiowebostv (Apache-2.0), which Home Assistant ships.

#include "webos_json.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::webos::ipc {

/// The ids this adapter sends under; an answer is routed by them.
inline constexpr const char kIdRegister[] = "register";
inline constexpr const char kIdSystemInfo[] = "systemInfo";
inline constexpr const char kIdMedia[] = "media";
inline constexpr const char kIdApp[] = "app";
inline constexpr const char kIdLaunchPoints[] = "launchPoints";
inline constexpr const char kIdPower[] = "power";
inline constexpr const char kIdVolume[] = "volume";
inline constexpr const char kIdInputs[] = "inputs";
/// Commands go out as `cmd:<n>`; their answers are routed by the prefix.
inline constexpr std::string_view kCommandIdPrefix = "cmd:";

inline constexpr const char kUriSystemInfo[] = "ssap://system/getSystemInfo";
inline constexpr const char kUriMediaForeground[] = "ssap://com.webos.media/getForegroundAppInfo";
inline constexpr const char kUriAppForeground[] = "ssap://com.webos.applicationManager/getForegroundAppInfo";
inline constexpr const char kUriLaunchPoints[] = "ssap://com.webos.applicationManager/listLaunchPoints";
inline constexpr const char kUriPowerState[] = "ssap://com.webos.service.tvpower/power/getPowerState";
inline constexpr const char kUriVolume[] = "ssap://audio/getVolume";
inline constexpr const char kUriInputs[] = "ssap://tv/getExternalInputList";

inline constexpr const char kUriTurnOff[] = "ssap://system/turnOff";
inline constexpr const char kUriScreenOn[] = "ssap://com.webos.service.tvpower/power/turnOnScreen";
inline constexpr const char kUriSetVolume[] = "ssap://audio/setVolume";
inline constexpr const char kUriSetMute[] = "ssap://audio/setMute";
inline constexpr const char kUriSwitchInput[] = "ssap://tv/switchInput";
inline constexpr const char kUriLaunch[] = "ssap://system.launcher/launch";
inline constexpr const char kUriPlay[] = "ssap://media.controls/play";
inline constexpr const char kUriPause[] = "ssap://media.controls/pause";
inline constexpr const char kUriStop[] = "ssap://media.controls/stop";

/**
 * @brief The register message.
 *
 * Without `clientKey` the TV shows a prompt and, once it is accepted, answers
 * `registered` with a new key; with a key it knows, it answers `registered` at
 * once. The permissions are asked for in full now, so that adding control later
 * does not mean pairing again.
 */
std::string registerMessage(const std::string &clientKey);

std::string requestMessage(std::string_view id, std::string_view uri, const Json &payload = Json::object());
std::string subscribeMessage(std::string_view id, std::string_view uri, const Json &payload = Json::object());

struct SsapMessage {
    std::string type; ///< response, registered, error, ...
    std::string id;
    std::string error; ///< set on type "error"
    Json payload = Json::object();
    bool valid = false;
};

SsapMessage parseMessage(std::string_view text);

/// A `response` to the register request that only says the prompt is up.
bool isPairingPrompt(const SsapMessage &message);

/// What the TV says is happening in the app in front.
enum class PlayState { Unknown, Idle, Playing, Paused };

/**
 * @brief The play state from a `com.webos.media/getForegroundAppInfo` answer.
 *
 * `Unknown` when the answer has no `foregroundAppInfo` at all - what webOS 4.5
 * sets have been seen to send. An empty list is a TV with nothing loaded,
 * `Idle`. Of several entries the first with a state counts.
 */
PlayState playStateFrom(const Json &payload);

/// `playing`, `paused`, `idle`, `unknown` - the channel's values.
const char *playStateName(PlayState state);

/// `appId` from an applicationManager foreground answer; empty when none.
std::string foregroundAppId(const Json &payload);

/// A `response` that did not say `returnValue: false`. An `error` is never one.
bool isSuccess(const SsapMessage &message);

/**
 * @brief On or off from a getPowerState answer.
 *
 * "Active" and "Screen Off" are a TV that is on (the second plays sound with
 * the panel dark); "Active Standby", "Suspend" and "Power Off" are not. Empty
 * when the answer names no state.
 */
std::optional<bool> powerOnFrom(const Json &payload);

struct VolumeState {
    std::optional<int> volume; ///< 0..100
    std::optional<bool> muted;
};

/// From getVolume: `volume`/`muted` at the top on webOS 4, inside
/// `volumeStatus` (`volume`/`muteStatus`) on later sets.
VolumeState volumeFrom(const Json &payload);

struct ExternalInput {
    std::string id;    ///< HDMI_1, what switchInput takes
    std::string label; ///< as the TV names it
    std::string appId; ///< the app that is in front while the input shows
};

std::vector<ExternalInput> inputsFrom(const Json &payload);

/// The magic packet for `mac` (any of `aa:bb:..`, `aa-bb-..`, `aabb..`); empty
/// when it is not a MAC.
std::string wakeOnLanPacket(std::string_view mac);

/// id -> title from a listLaunchPoints answer: the full list first, then one
/// launch point per later change.
void mergeLaunchPoints(const Json &payload, std::map<std::string, std::string> &titles);

} // namespace phicore::webos::ipc
