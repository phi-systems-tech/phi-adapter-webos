#pragma once

// What one TV instance is configured with: where it is, and what earlier
// connections left behind - the client key from pairing and the certificate
// pinned on first contact.

#include "webos_json.h"

#include "phi/adapter/v1/schema.h"

#include <cstdint>
#include <string>

namespace phicore::webos::ipc {

inline constexpr std::uint16_t kDefaultTlsPort = 3001;
inline constexpr std::uint16_t kDefaultPlainPort = 3000;
inline constexpr int kDefaultRetryIntervalMs = 10000;

struct WebosSettings {
    std::string host;
    std::uint16_t port = kDefaultTlsPort;
    bool useTls = true;
    std::string clientKey;
    /// Lower-case hex SHA-256 of the TV's certificate; empty until first contact.
    std::string certFingerprint;
    /// For Wake-on-LAN; learnt from the neighbour table while the TV is on.
    std::string macAddress;
    int retryIntervalMs = kDefaultRetryIntervalMs;

    [[nodiscard]] bool hasHost() const { return !host.empty(); }
    /// `wss://host:3001/`, or `ws://` with TLS off.
    [[nodiscard]] std::string url() const;
};

/// Meta keys win over the adapter's own fields, which is what a form saves to.
WebosSettings settingsFromAdapter(const phicore::adapter::v1::Adapter &adapter, const Json &meta);

/// The device id: the adapter's external id, else the host.
std::string deviceIdFor(const phicore::adapter::v1::Adapter &adapter, const WebosSettings &settings);

} // namespace phicore::webos::ipc
