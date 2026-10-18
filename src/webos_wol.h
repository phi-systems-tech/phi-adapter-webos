#pragma once

// Switching a TV on: it has no socket open while it is off, so the only way in
// is Wake-on-LAN to its MAC - learnt from the neighbour table while it was on.

#include <string>

namespace phicore::webos::ipc {

/// The MAC the kernel's neighbour table has for `ip` (IPv4), lower case with
/// colons; empty when there is none.
std::string macFromArpTable(const std::string &ip);

/// The magic packet to the broadcast address and to `ip` directly, both on
/// UDP 9. False with `error` set when neither went out.
bool sendWakeOnLan(const std::string &mac, const std::string &ip, std::string *error);

} // namespace phicore::webos::ipc
