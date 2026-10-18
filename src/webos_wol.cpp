#include "webos_wol.h"

#include "webos_protocol.h"

#include "phi/runtime/str.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace phicore::webos::ipc {

namespace str = phi::str;

std::string macFromArpTable(const std::string &ip)
{
    // IP address, HW type, Flags, HW address, Mask, Device
    std::ifstream arp("/proc/net/arp");
    std::string line;
    std::getline(arp, line);
    while (std::getline(arp, line)) {
        std::istringstream fields(line);
        std::string address, type, flags, mac;
        if (!(fields >> address >> type >> flags >> mac))
            continue;
        // 0x0 is an entry that never resolved.
        if (address == ip && flags != "0x0" && mac != "00:00:00:00:00:00")
            return str::toLower(mac);
    }
    return {};
}

bool sendWakeOnLan(const std::string &mac, const std::string &ip, std::string *error)
{
    const std::string packet = wakeOnLanPacket(mac);
    if (packet.empty()) {
        *error = "No valid MAC address for Wake-on-LAN";
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::string("socket(): ") + std::strerror(errno);
        return false;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    int sent = 0;
    const auto sendTo = [&](const char *address) {
        ::sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        if (::inet_pton(AF_INET, address, &to.sin_addr) != 1)
            return;
        if (::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<const ::sockaddr *>(&to), sizeof(to))
            == static_cast<ssize_t>(packet.size()))
            ++sent;
        else
            *error = std::string("sendto(): ") + std::strerror(errno);
    };
    sendTo("255.255.255.255");
    // Directed as well: a network that drops the limited broadcast still
    // delivers to an address the switch remembers.
    if (!ip.empty())
        sendTo(ip.c_str());
    ::close(fd);
    return sent > 0;
}

} // namespace phicore::webos::ipc
