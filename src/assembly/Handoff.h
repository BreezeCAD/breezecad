#pragma once
// "Continue on device": an open part or assembly goes from the PC to a
// tablet or phone and its changes come back (docs/dev/CONTINUE-ON-DEVICE.md).
// This is the part without the network and the window: what is sent, how
// the device packs what it changed, and how the PC puts it back in place.
//
// PC → device: a .bzp (ProjectPack.h) with one more entry, handoff.json —
// the ticket: the session (= the project the QR is for), the main document,
// the view to continue from and a hash of every file as it was sent. Where
// the files lie on the PC never leaves the PC: it stays in the Session.
//
// Device → PC: a ZIP of the files the device changed or added, with its
// own handoff.json. The PC accepts it only for the same session, then each
// file goes back where it came from; a file changed on the PC too since it
// was sent is a conflict for the user to decide.

#include "core/Expected.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bz::assembly::handoff {

inline constexpr int kHandoffVersion = 1;

/// A file of the project as it was sent (kept by the PC).
struct SentFile {
    std::string           path;       ///< inside the package, '/' separated
    std::filesystem::path source;     ///< the file on the PC
    std::string           original;   ///< parts of an assembly: the component path as the assembly had it
    std::string           sentHash;   ///< SHA-256 of the bytes in the package
    std::string           pcHash;     ///< SHA-256 of the file on the PC when it was sent
};

/// What the PC remembers about one handoff. Not sent whole: the key and the
/// paths on the PC stay there.
struct Session {
    std::string           id;     ///< 32 hex digits: the project this handoff is for
    std::string           key;    ///< 64 hex digits: for the transfer, given only through the QR
    std::string           kind;   ///< "part" or "assembly"
    std::string           main;   ///< the document to open, inside the package
    std::string           view;   ///< where the user was (camera, open sketch): the program's JSON, kept as is
    std::vector<SentFile> files;  ///< the main document first
};

/// What the device gets in the package.
struct Ticket {
    struct File {
        std::string path, hash;
    };
    std::string       session, kind, main, view;
    std::vector<File> files;
};

/// Packs the saved part (.bzc) or assembly (.bza with its parts) at
/// `document` into `package`. The kind is taken from the file, not its name.
Expected<Session> Send(const std::filesystem::path& document, const std::filesystem::path& package, std::string_view version,
                       std::string view = {});

/// The PC keeps the session while it waits for the changes.
Status            SaveSession(const Session& session, const std::filesystem::path& file);
Expected<Session> LoadSession(const std::filesystem::path& file);

/// The device: the ticket of a package (then ProjectPack's UnpackProject).
Expected<Ticket> ReadTicket(const std::filesystem::path& package);

/// The device: packs into `out` what in `folder` (where the package was
/// unpacked) differs from the ticket, and the documents added there.
/// Returns how many files went in (0: nothing changed).
Expected<int> PackReturn(const Ticket& ticket, const std::filesystem::path& folder, const std::filesystem::path& out,
                         std::string_view version);

enum class Change {
    Update,     ///< changed on the device, not on the PC: simply replaces it
    Conflict,   ///< changed on both, or a new file whose name is taken on the PC
    Added,      ///< a new document from the device
};

struct Returned {
    std::string           path;     ///< inside the return package
    Change                change = Change::Update;
    std::filesystem::path target;   ///< where it goes on the PC
};

/// The PC: what the device sent back and what each file would do. An error
/// when the changes are for another project (another session).
Expected<std::vector<Returned>> PlanReturn(const Session& session, const std::filesystem::path& returned);

enum class Choice {
    TakeDevice,   ///< the device's file replaces the PC's (the old one goes to `backups`)
    KeepMine,     ///< the device's file is dropped
    KeepBoth,     ///< the device's file goes beside: "Вал (с устройства).bzc"
};

struct ApplyReport {
    std::vector<std::filesystem::path> written;
    std::vector<std::filesystem::path> backups;
};

/// The PC: puts the changes in place, one choice per PlanReturn entry. An
/// assembly gets its parts' paths back as they were on the PC.
Expected<ApplyReport> ApplyReturn(const Session& session, const std::filesystem::path& returned,
                                  const std::vector<Choice>& choices, const std::filesystem::path& backups,
                                  std::string_view version);

/// What the QR carries: where the PC listens and for which session.
struct Address {
    std::string host;
    int         port = 0;
    std::string session, key;
};

/// "breezecad://handoff?v=1&h=192.168.1.5&p=50213&s=…&k=…"
std::string       QrText(const Address& address);
Expected<Address> ParseQr(std::string_view text);

/// What goes over the network: `bytes` encrypted and signed with the key
/// from the QR (XChaCha20-Poly1305, Monocypher), bound to the session — a
/// package for another project does not open. Layout: nonce (24) | MAC (16) | data.
Expected<std::string> Seal(std::string_view bytes, const std::string& keyHex, const std::string& session);
/// The bytes back; an error if the key or session differs or a byte was changed.
Expected<std::string> Open(std::string_view sealed, const std::string& keyHex, const std::string& session);

/// Lowercase hex SHA-256 (FIPS 180-4).
std::string Sha256Hex(std::string_view bytes);

/// `bytes` random bytes from the system's generator, as hex.
std::string RandomHex(int bytes);

} // namespace bz::assembly::handoff
