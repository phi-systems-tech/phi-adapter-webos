#include "webos_schema.h"

#include "webos_settings.h"

#include <utility>

namespace phicore::webos::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

constexpr const char kIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"TV icon\">"
    "<rect x=\"2.5\" y=\"4\" width=\"19\" height=\"12.5\" rx=\"1.8\" "
    "stroke=\"#2E3A4F\" stroke-width=\"1.6\" fill=\"#121A26\"/>"
    "<path d=\"M8 20h8M12 16.5V20\" stroke=\"#7A8AA4\" stroke-width=\"1.6\" stroke-linecap=\"round\"/>"
    "<path d=\"M10.2 8.2v4.6l4-2.3z\" fill=\"#A50034\"/>"
    "</svg>";

v1::AdapterConfigField field(const char *key, v1::AdapterConfigFieldType type, const char *label,
                             const char *description, v1::ScalarValue defaultValue = {})
{
    v1::AdapterConfigField out;
    out.key = key;
    out.type = type;
    out.label = label;
    out.description = description;
    out.defaultValue = std::move(defaultValue);
    return out;
}

/// Two columns: the address beside its port, TLS beside the retry interval,
/// then what pairing and the first connection filled in.
v1::AdapterConfigSection section()
{
    using Type = v1::AdapterConfigFieldType;
    using Flag = v1::AdapterConfigFieldFlag;
    v1::AdapterConfigSection out;
    out.title = "LG webOS TV";
    out.description = "The TV asks once to allow the connection; \"LG Connect Apps\" has to be on.";
    out.layout.columns = 2;

    v1::AdapterConfigField host = field("host", Type::Hostname, "TV host", "IP address or hostname of the TV.");
    host.flags = Flag::Required;
    v1::AdapterConfigField port = field("port", Type::Port, "Port", "3001 with TLS, 3000 on sets from before 2018.",
                                        std::int64_t{kDefaultTlsPort});
    port.layout.controlWidth = v1::AdapterConfigSize::Narrow;
    v1::AdapterConfigField useTls = field("useTls", Type::Boolean, "Use TLS",
                                          "The certificate is the TV's own; it is pinned on first contact.", true);
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval",
                                         "Reconnect interval while the TV is off or unreachable.",
                                         std::int64_t{kDefaultRetryIntervalMs});
    retry.layout.controlWidth = v1::AdapterConfigSize::Narrow;
    v1::AdapterConfigField clientKey = field("clientKey", Type::Password, "Client key",
                                             "Given by the TV when the prompt is accepted. Clear it to pair again.");
    clientKey.flags = Flag::Secret;
    v1::AdapterConfigField fingerprint = field("certFingerprint", Type::String, "Certificate (SHA-256)",
                                               "Pinned on first contact. Clear it if the TV got a new one.");

    out.fields = {host, port, useTls, retry, clientKey, fingerprint};
    return out;
}

} // namespace

v1::Utf8String displayName()
{
    return "LG webOS TV";
}

v1::Utf8String description()
{
    return "LG webOS TV over its local SSAP WebSocket";
}

v1::Utf8String iconSvg()
{
    return kIconSvg;
}

v1::AdapterCapabilities capabilities()
{
    v1::AdapterCapabilities caps;
    caps.required = v1::AdapterRequirement::Host | v1::AdapterRequirement::ManualConfirm
                    | v1::AdapterRequirement::UsesRetryInterval;
    caps.optional = v1::AdapterRequirement::Port | v1::AdapterRequirement::SupportsTls;
    caps.flags = v1::AdapterCapability::SupportsProbe;

    // The TV hands out a key only to a prompt someone accepts; the form gets
    // it here before the instance exists, and the certificate with it.
    v1::AdapterActionDescriptor pair;
    pair.id = kPairActionId;
    pair.label = "Pair with TV";
    pair.description = "Shows a prompt on the TV; accept it with the remote.";
    pair.resultField = "clientKey";
    // The pairing's own wait plus the connect, so the TV's answer is never
    // cut off by core's deadline.
    pair.timeoutMs = static_cast<int>(kPairingWait.count()) + 15000;
    caps.factoryActions.push_back(pair);
    return caps;
}

v1::AdapterConfigSchema configSchema()
{
    v1::AdapterConfigSchema schema;
    schema.factory = section();
    schema.instance = section();
    return schema;
}

} // namespace phicore::webos::ipc
