// Process entry point for the LG webOS sidecar: the adapter factory and the
// SDK's own main loop. One instance is one TV (webos_instance), the SSAP
// messages are in webos_protocol.

#include <csignal>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"

#include "webos_instance.h"
#include "webos_json.h"
#include "webos_pairing.h"
#include "webos_schema.h"
#include "webos_settings.h"

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

    // Pairing waits on the TV's socket and on someone at the remote; it needs
    // a loop of its own rather than the host's poll thread.
    std::unique_ptr<sdk::InstanceExecutionBackend> createFactoryExecutionBackend() override
    {
        return sdk::createLoopExecutionBackend("webos-factory");
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

    /// "pair": the form's host, a prompt on the TV, and the key and the
    /// certificate back into the form.
    void onFactoryActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        if (request.actionId != kPairActionId) {
            answer(request.cmdId, v1::CmdStatus::NotSupported, "Unsupported factory action");
            return;
        }
        const WebosSettings settings = settingsFromAdapter(v1::Adapter{}, parseObject(request.paramsJson));
        if (!m_pairing) {
            phi::runtime::Loop *loop = phi::runtime::Loop::current();
            if (!loop) {
                answer(request.cmdId, v1::CmdStatus::Failure, "No factory event loop");
                return;
            }
            m_pairing = std::make_unique<Pairing>(*loop);
        }

        const v1::CmdId cmdId = request.cmdId;
        std::string error;
        const bool started = m_pairing->start(settings, kPairingWait, [this, cmdId](const Pairing::Result &result) {
            if (!result.ok) {
                answer(cmdId, result.refused ? v1::CmdStatus::InvalidArgument : v1::CmdStatus::Failure,
                       result.error);
                return;
            }
            v1::ActionResponse response;
            response.id = cmdId;
            response.status = v1::CmdStatus::Success;
            response.resultType = v1::ActionResultType::String;
            response.resultValue = result.clientKey;
            response.formValues.push_back({"clientKey", v1::ScalarValue(result.clientKey)});
            if (!result.certFingerprint.empty())
                response.formValues.push_back({"certFingerprint", v1::ScalarValue(result.certFingerprint)});
            sendAction(std::move(response));
        }, &error);
        if (!started)
            answer(cmdId, v1::CmdStatus::InvalidArgument, error);
    }

    void onFactoryStopping() override
    {
        if (m_pairing)
            m_pairing->cancel();
        m_pairing.reset();
    }

private:
    void answer(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::ActionResponse response;
        response.id = cmdId;
        response.status = status;
        response.error = error;
        response.resultType = v1::ActionResultType::None;
        sendAction(std::move(response));
    }

    void sendAction(v1::ActionResponse response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the pairing result: %1", {error});
    }

    std::unique_ptr<Pairing> m_pairing;
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
