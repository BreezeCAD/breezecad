#pragma once
// "Continue on device" over the local network: a server on the PC that
// lives only while the handoff window is open, and the device's two calls
// to it — take the project, give the changes back.
//
// One request per connection: a frame "BZH1" | type (1 byte) | length (4,
// big-endian) | body; every body is Seal()ed with the key from the QR, so
// only the device that scanned this project's QR is answered. Nothing goes
// beyond the local network: the PC never connects anywhere, it only listens.

#include "assembly/Handoff.h"
#include "core/Expected.h"

#include <cstdint>
#include <memory>
#include <string>

namespace bz::assembly::handoff {

/// The largest package taken over the network (a project of hundreds of parts fits).
inline constexpr std::size_t kMaxTransfer = std::size_t{256} << 20;

class Server {
public:
    /// Listens on every network of the PC on a free port. `package` is what
    /// Send() wrote, as bytes. The address is for the QR; its host is empty
    /// when the PC is in no network (the window says so).
    static Expected<Server> Start(const Session& session, std::string package);

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    ~Server();

    const Address& GetAddress() const;

    struct Event {
        enum class Kind {
            None,       ///< nobody came in time
            Sent,       ///< the device took the project
            Returned,   ///< the device gave changes back: `data` is the return package (PlanReturn)
            Refused,    ///< someone without this project's key: answered nothing useful
        };
        Kind        kind = Kind::None;
        std::string data;
    };

    /// Waits up to `timeoutMs` for one device and serves it. Blocks: the
    /// program calls it from a worker thread (the server holds its own copies).
    Event Serve(int timeoutMs);

    /// Stops listening (also done when the server is destroyed).
    void Stop();

private:
    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> myImpl;
};

/// The device: takes the project from the PC at the QR's address. The bytes
/// are a package for ReadTicket / UnpackProject.
Expected<std::string> Fetch(const Address& address, int timeoutMs = 15000);

/// The device: gives the changes (PackReturn's file, as bytes) back to the PC.
Status Give(const Address& address, const std::string& returned, int timeoutMs = 15000);

/// The PC's address in the local network (the one its default route uses),
/// empty when there is none. No packet is sent to find it.
std::string LocalAddress();

} // namespace bz::assembly::handoff
