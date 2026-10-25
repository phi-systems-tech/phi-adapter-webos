#include "webos_model.h"

#include "webos_protocol.h"

#include <utility>

namespace phicore::webos::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

constexpr auto kReadReport = v1::ChannelFlag::Readable | v1::ChannelFlag::Reportable;
constexpr auto kReadWriteReport = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;

} // namespace

v1::Channel buildInputChannel(const v1::AdapterConfigOptionList &inputs)
{
    v1::Channel input(kChannelInput, "Input", v1::ChannelKind::HdmiInput, v1::ChannelDataType::String,
                      kReadWriteReport);
    input.setChoices(inputs);
    return input;
}

v1::Channel buildAppChannel(const v1::AdapterConfigOptionList &apps)
{
    v1::Channel app(kChannelApp, "App", v1::ChannelKind::Unknown, v1::ChannelDataType::String, kReadWriteReport);
    app.setChoices(apps);
    return app;
}

v1::ChannelList buildChannels(const v1::AdapterConfigOptionList &inputs, const v1::AdapterConfigOptionList &apps)
{
    v1::ChannelList channels;

    channels.push_back(v1::Channel(kChannelPower, "Power", v1::ChannelKind::PowerOnOff, v1::ChannelDataType::Bool,
                                   kReadWriteReport));

    v1::Channel volume(kChannelVolume, "Volume", v1::ChannelKind::Volume, v1::ChannelDataType::Int,
                       kReadWriteReport);
    volume.setRange(0.0, 100.0, 1.0);
    channels.push_back(std::move(volume));

    channels.push_back(v1::Channel(kChannelMute, "Mute", v1::ChannelKind::Mute, v1::ChannelDataType::Bool,
                                   kReadWriteReport));

    channels.push_back(buildInputChannel(inputs));
    channels.push_back(buildAppChannel(apps));

    // A string with its four values as choices: the SDK has no play-state
    // kind, and a rule wants "playing" to read as "playing".
    v1::Channel playState(kChannelPlayState, "Playback", v1::ChannelKind::Unknown, v1::ChannelDataType::String,
                          kReadReport);
    for (const PlayState state : {PlayState::Playing, PlayState::Paused, PlayState::Idle, PlayState::Unknown}) {
        const std::string value = playStateName(state);
        std::string label = value;
        label.front() = static_cast<char>(label.front() - 'a' + 'A');
        playState.addChoice(value, label);
    }
    channels.push_back(std::move(playState));

    channels.push_back(v1::Channel(kChannelConnectivity, "Connectivity", v1::ChannelKind::ConnectivityStatus,
                                   v1::ChannelDataType::Enum, kReadReport));

    return channels;
}

v1::Device buildDevice(const std::string &deviceId, const std::string &name, const std::string &model)
{
    v1::Device device;
    device.setExternalId(deviceId)
        .setDeviceClass(v1::DeviceClass::MediaPlayer)
        .setMediaPlayerFeatures(v1::MediaPlayerFeature::Play | v1::MediaPlayerFeature::Pause
                                | v1::MediaPlayerFeature::Stop)
        .setName(name)
        .setManufacturer("LG Electronics")
        .setModel(model);
    return device;
}

} // namespace phicore::webos::ipc
