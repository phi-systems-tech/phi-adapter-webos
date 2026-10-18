#include "webos_settings.h"

#include "phi/runtime/str.h"

namespace phicore::webos::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

std::string WebosSettings::url() const
{
    const bool v6 = host.find(':') != std::string::npos;
    return std::string(useTls ? "wss://" : "ws://") + (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port)
           + "/";
}

WebosSettings settingsFromAdapter(const v1::Adapter &adapter, const Json &meta)
{
    WebosSettings out;
    out.host = str::trimmed(jsonString(meta, "host"));
    if (out.host.empty())
        out.host = str::trimmed(adapter.ip);
    if (out.host.empty())
        out.host = str::trimmed(adapter.host);

    if (meta.contains("useTls") && meta["useTls"].is_boolean())
        out.useTls = meta["useTls"].get<bool>();

    const int fallbackPort = adapter.port != 0 ? adapter.port : (out.useTls ? kDefaultTlsPort : kDefaultPlainPort);
    const int port = jsonInt(meta, "port", fallbackPort);
    out.port = static_cast<std::uint16_t>(port > 0 && port <= 0xFFFF ? port : fallbackPort);

    out.clientKey = str::trimmed(jsonString(meta, "clientKey"));
    out.certFingerprint = str::toLower(str::trimmed(jsonString(meta, "certFingerprint")));
    out.macAddress = str::toLower(str::trimmed(jsonString(meta, "macAddress")));
    const int retry = jsonInt(meta, "retryIntervalMs", kDefaultRetryIntervalMs);
    out.retryIntervalMs = retry >= 1000 ? retry : kDefaultRetryIntervalMs;
    return out;
}

std::string deviceIdFor(const v1::Adapter &adapter, const WebosSettings &settings)
{
    if (!adapter.externalId.empty())
        return adapter.externalId;
    if (!settings.host.empty())
        return settings.host;
    return "webos-tv";
}

} // namespace phicore::webos::ipc
