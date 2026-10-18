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
    v1::Channel input;
    input.externalId = kChannelInput;
    input.name = "Input";
    input.kind = v1::ChannelKind::HdmiInput;
    input.dataType = v1::ChannelDataType::String;
    input.flags = kReadWriteReport;
    input.choices = inputs;
    return input;
}

v1::Channel buildAppChannel(const v1::AdapterConfigOptionList &apps)
{
    v1::Channel app;
    app.externalId = kChannelApp;
    app.name = "App";
    app.kind = v1::ChannelKind::Unknown;
    app.dataType = v1::ChannelDataType::String;
    app.flags = kReadWriteReport;
    app.choices = apps;
    return app;
}

v1::ChannelList buildChannels(const v1::AdapterConfigOptionList &inputs, const v1::AdapterConfigOptionList &apps)
{
    v1::ChannelList channels;

    v1::Channel power;
    power.externalId = kChannelPower;
    power.name = "Power";
    power.kind = v1::ChannelKind::PowerOnOff;
    power.dataType = v1::ChannelDataType::Bool;
    power.flags = kReadWriteReport;
    channels.push_back(std::move(power));

    v1::Channel volume;
    volume.externalId = kChannelVolume;
    volume.name = "Volume";
    volume.kind = v1::ChannelKind::Volume;
    volume.dataType = v1::ChannelDataType::Int;
    volume.flags = kReadWriteReport;
    volume.minValue = 0.0;
    volume.maxValue = 100.0;
    volume.stepValue = 1.0;
    channels.push_back(std::move(volume));

    v1::Channel mute;
    mute.externalId = kChannelMute;
    mute.name = "Mute";
    mute.kind = v1::ChannelKind::Mute;
    mute.dataType = v1::ChannelDataType::Bool;
    mute.flags = kReadWriteReport;
    channels.push_back(std::move(mute));

    channels.push_back(buildInputChannel(inputs));
    channels.push_back(buildAppChannel(apps));

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

    v1::Channel connectivity;
    connectivity.externalId = kChannelConnectivity;
    connectivity.name = "Connectivity";
    connectivity.kind = v1::ChannelKind::ConnectivityStatus;
    connectivity.dataType = v1::ChannelDataType::Enum;
    connectivity.flags = kReadReport;
    channels.push_back(std::move(connectivity));

    return channels;
}

v1::Device buildDevice(const std::string &deviceId, const std::string &name, const std::string &model)
{
    v1::Device device;
    device.externalId = deviceId;
    device.deviceClass = v1::DeviceClass::MediaPlayer;
    device.mediaPlayerFeatures = v1::MediaPlayerFeature::Play | v1::MediaPlayerFeature::Pause
                                 | v1::MediaPlayerFeature::Stop;
    device.name = name;
    device.manufacturer = "LG Electronics";
    device.model = model;
    return device;
}

} // namespace phicore::webos::ipc
