// Process entry point for the LG webOS sidecar: the adapter factory and the
// SDK's own main loop. One instance is one TV (webos_instance), the SSAP
// messages are in webos_protocol.

#include <csignal>
#include <cstdlib>
#include <memory>
#include <optional>

#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"

#include "webos_instance.h"
#include "webos_schema.h"

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;
using namespace phicore::webos::ipc;

namespace {

class WebosFactory final : public sdk::AdapterFactory
{
protected:
    v1::Utf8String pluginType() const override { return kPluginType; }
    v1::Utf8String displayName() const override { return phicore::webos::ipc::displayName(); }
    v1::Utf8String description() const override { return phicore::webos::ipc::description(); }
    v1::Utf8String iconSvg() const override { return phicore::webos::ipc::iconSvg(); }
    v1::Utf8String apiVersion() const override { return "1.0.0"; }
    int timeoutMs() const override { return 15000; }
    int maxInstances() const override { return 0; }

    v1::AdapterCapabilities capabilities() const override { return phicore::webos::ipc::capabilities(); }

    std::optional<v1::AdapterConfigSchema> configSchema() const override
    {
        return phicore::webos::ipc::configSchema();
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        // The socket, its timers and the reconnects all run on this loop.
        return sdk::createLoopExecutionBackend("webos-instance");
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return makeInstance();
    }
};

} // namespace

int main(int argc, char **argv)
{
    // SSL_write goes through write(), which has no MSG_NOSIGNAL: a TV that
    // reset the connection would otherwise kill the sidecar instead of
    // failing the write.
    std::signal(SIGPIPE, SIG_IGN);
    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = argc > 1
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-webos-ipc.sock"));
    WebosFactory factory;
    sdk::SidecarHost host(socketPath, factory);
    return sdk::runSidecarMain(host);
}
