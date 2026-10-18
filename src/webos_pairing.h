#pragma once

// One pairing attempt from the settings dialog: connect, pin the certificate,
// register, wait for someone to accept the prompt on the TV, and hand back
// the client key. The instance does the same on its own connection; this is
// the form's way of getting the key before the instance exists.

#include "webos_settings.h"
#include "webos_ws_client.h"

#include <phi/runtime/loop.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace phicore::webos::ipc {

class Pairing
{
public:
    struct Result {
        bool ok = false;
        bool refused = false; ///< the TV answered no, rather than not at all
        std::string error;
        std::string clientKey;
        std::string certFingerprint;
    };
    using Done = std::function<void(const Result &)>;

    explicit Pairing(phi::runtime::Loop &loop);
    ~Pairing();

    Pairing(const Pairing &) = delete;
    Pairing &operator=(const Pairing &) = delete;

    /// `done` runs exactly once, on this loop, unless cancel() comes first.
    /// False with `error` set when nothing was started.
    bool start(const WebosSettings &settings, std::chrono::milliseconds timeout, Done done, std::string *error);
    void cancel();
    [[nodiscard]] bool busy() const { return static_cast<bool>(m_done); }

private:
    void onOpened();
    void onText(std::string_view text);
    void finish(Result result);

    phi::runtime::Loop &m_loop;
    // Owned through a pointer so finish() can retire the client from inside
    // its own callback (Loop::deleteDeferred).
    WsClient *m_ws = nullptr;
    phi::runtime::Timer m_deadline;
    WebosSettings m_settings;
    std::string m_fingerprint;
    Done m_done;
};

} // namespace phicore::webos::ipc
