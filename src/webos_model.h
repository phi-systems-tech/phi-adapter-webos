#pragma once

// The TV as phi-core sees it: one media player, and for now only what it
// reports - whether it answers, what is playing, which app is in front.
// Control (power, volume, input, transport) comes with the next step.

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/device.h"

#include <string>

namespace phicore::webos::ipc {

inline constexpr const char kChannelConnectivity[] = "connectivity";
inline constexpr const char kChannelPlayState[] = "playState";
inline constexpr const char kChannelApp[] = "app";

phicore::adapter::v1::ChannelList buildChannels();

phicore::adapter::v1::Device buildDevice(const std::string &deviceId,
                                         const std::string &name,
                                         const std::string &model);

} // namespace phicore::webos::ipc
