#pragma once

// One TV: the connection, pairing, the subscriptions and what they report.
// The class stays private to the .cpp - the factory only ever needs one.

#include <memory>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::webos::ipc {

std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance();

} // namespace phicore::webos::ipc
