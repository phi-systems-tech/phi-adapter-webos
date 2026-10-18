#pragma once

// What the adapter announces about itself: name, icon, capabilities and the
// settings form.

#include "phi/adapter/v1/schema.h"

#include <chrono>

namespace phicore::webos::ipc {

inline constexpr const char kPluginType[] = "webos";
/// The factory action that pairs from the settings form.
inline constexpr const char kPairActionId[] = "probe";
/// How long the form waits for the prompt on the TV to be accepted.
inline constexpr std::chrono::milliseconds kPairingWait{60000};

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();
phicore::adapter::v1::AdapterCapabilities capabilities();
phicore::adapter::v1::AdapterConfigSchema configSchema();

} // namespace phicore::webos::ipc
