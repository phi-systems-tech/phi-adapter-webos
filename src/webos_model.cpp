#include "webos_model.h"

#include "webos_protocol.h"

#include <utility>

namespace phicore::webos::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

constexpr auto kReadReport = v1::ChannelFlag::Readable | v1::ChannelFlag::Reportable;

} // namespace

v1::ChannelList buildChannels()
{
    v1::ChannelList channels;

    v1::Channel connectivity;
    connectivity.externalId = kChannelConnectivity;
    connectivity.name = "Connectivity";
    connectivity.kind = v1::ChannelKind::ConnectivityStatus;
    connectivity.dataType = v1::ChannelDataType::Enum;
    connectivity.flags = kReadReport;
    channels.push_back(std::move(connectivity));

    // A string with its four values as choices: the SDK has no play-state
    // kind, and a rule wants "playing" to read as "playing".
    v1::Channel playState;
    playState.externalId = kChannelPlayState;
    playState.name = "Playback";
    playState.kind = v1::ChannelKind::Unknown;
    playState.dataType = v1::ChannelDataType::String;
    playState.flags = kReadReport;
    for (const PlayState state : {PlayState::Playing, PlayState::Paused, PlayState::Idle, PlayState::Unknown}) {
        const std::string value = playStateName(state);
        std::string label = value;
        label.front() = static_cast<char>(label.front() - 'a' + 'A');
        playState.choices.push_back({value, label});
    }
    channels.push_back(std::move(playState));

    v1::Channel app;
    app.externalId = kChannelApp;
    app.name = "App";
    app.kind = v1::ChannelKind::Unknown;
    app.dataType = v1::ChannelDataType::String;
    app.flags = kReadReport;
    channels.push_back(std::move(app));

    return channels;
}

v1::Device buildDevice(const std::string &deviceId, const std::string &name, const std::string &model)
{
    v1::Device device;
    device.externalId = deviceId;
    device.deviceClass = v1::DeviceClass::MediaPlayer;
    // Nothing is controlled yet; the mask grows with the commands.
    device.mediaPlayerFeatures = v1::MediaPlayerFeature::None;
    device.name = name;
    device.manufacturer = "LG Electronics";
    device.model = model;
    return device;
}

} // namespace phicore::webos::ipc
