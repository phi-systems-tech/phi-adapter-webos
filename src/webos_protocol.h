#pragma once

// SSAP, the JSON the TV speaks over its WebSocket: what goes out and what the
// answers mean, without a socket. Every message is
// `{"type", "id", "uri"?, "payload"}`; an answer carries the id of the request
// it belongs to, and a subscription keeps answering under that id.
//
// The shapes follow aiowebostv (Apache-2.0), which Home Assistant ships.

#include "webos_json.h"

#include <map>
#include <string>
#include <string_view>

namespace phicore::webos::ipc {

/// The ids this adapter sends under; an answer is routed by them.
inline constexpr const char kIdRegister[] = "register";
inline constexpr const char kIdSystemInfo[] = "systemInfo";
inline constexpr const char kIdMedia[] = "media";
inline constexpr const char kIdApp[] = "app";
inline constexpr const char kIdLaunchPoints[] = "launchPoints";

inline constexpr const char kUriSystemInfo[] = "ssap://system/getSystemInfo";
inline constexpr const char kUriMediaForeground[] = "ssap://com.webos.media/getForegroundAppInfo";
inline constexpr const char kUriAppForeground[] = "ssap://com.webos.applicationManager/getForegroundAppInfo";
inline constexpr const char kUriLaunchPoints[] = "ssap://com.webos.applicationManager/listLaunchPoints";

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

/// id -> title from a listLaunchPoints answer: the full list first, then one
/// launch point per later change.
void mergeLaunchPoints(const Json &payload, std::map<std::string, std::string> &titles);

} // namespace phicore::webos::ipc
