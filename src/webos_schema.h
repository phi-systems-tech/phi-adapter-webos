#pragma once

// What the adapter announces about itself: name, icon, capabilities and the
// settings form.

#include "phi/adapter/v1/schema.h"

namespace phicore::webos::ipc {

inline constexpr const char kPluginType[] = "webos";

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();
phicore::adapter::v1::AdapterCapabilities capabilities();
phicore::adapter::v1::AdapterConfigSchema configSchema();

} // namespace phicore::webos::ipc
