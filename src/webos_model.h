#pragma once

// The TV as phi-core sees it: one media player with power, volume, mute, the
// input and the app in front - all of them set as well as read - plus
// whether it answers and, where the TV says, what is playing.

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/device.h"

#include <string>

namespace phicore::webos::ipc {

inline constexpr const char kChannelConnectivity[] = "connectivity";
inline constexpr const char kChannelPower[] = "power";
inline constexpr const char kChannelVolume[] = "volume";
inline constexpr const char kChannelMute[] = "mute";
inline constexpr const char kChannelInput[] = "input";
inline constexpr const char kChannelApp[] = "app";
inline constexpr const char kChannelPlayState[] = "playState";

/// The input selector; its choices are the TV's own list.
phicore::adapter::v1::Channel buildInputChannel(const phicore::adapter::v1::AdapterConfigOptionList &inputs);
/// The app selector; its choices are the launch points the TV lists.
phicore::adapter::v1::Channel buildAppChannel(const phicore::adapter::v1::AdapterConfigOptionList &apps);

phicore::adapter::v1::ChannelList buildChannels(const phicore::adapter::v1::AdapterConfigOptionList &inputs,
                                                const phicore::adapter::v1::AdapterConfigOptionList &apps);

phicore::adapter::v1::Device buildDevice(const std::string &deviceId,
                                         const std::string &name,
                                         const std::string &model);

} // namespace phicore::webos::ipc
