#include "assembly/HandoffNet.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace bz::assembly::handoff {
namespace {

#if defined(_WIN32)
using Native = SOCKET;
using IoSize = int;   // send/recv take int lengths here
const Native kNone = INVALID_SOCKET;
void CloseNative(Native s) { closesocket(s); }
#else
using Native = int;
using IoSize = std::size_t;
constexpr Native kNone = -1;
void CloseNative(Native s) { ::close(s); }
#endif

constexpr std::array<char, 4> kMagic = {'B', 'Z', 'H', '1'};
constexpr std::size_t kHeader = 9;   // magic, type, length
constexpr char kGet = 'G', kPackage = 'P', kReturn = 'R', kOk = 'K', kRefused = 'E';
constexpr int kConnectionMs = 15000;   // a slow phone Wi-Fi still makes it; a stuck one does not hold the PC

bool NetInit() {
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

/// A socket closed when it goes out of scope.
class Socket {
public:
    Socket() = default;
    explicit Socket(Native s) : myS(s) {}
    ~Socket() { Close(); }
    Socket(Socket&& o) noexcept : myS(std::exchange(o.myS, kNone)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            Close();
            myS = std::exchange(o.myS, kNone);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool   IsOpen() const { return myS != kNone; }
    Native Get() const { return myS; }
    void   Close() {
        if (myS != kNone) CloseNative(myS);
        myS = kNone;
    }

    void Prepare(int ms) const {
#if defined(SO_NOSIGPIPE)
        // macOS/iOS have no MSG_NOSIGNAL: a write to a closed peer must not end the program.
        int yes = 1;
        setsockopt(myS, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
#if defined(_WIN32)
        const DWORD t = static_cast<DWORD>(ms);
#else
        timeval t{};
        t.tv_sec = ms / 1000;
        t.tv_usec = (ms % 1000) * 1000;
#endif
        setsockopt(myS, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        setsockopt(myS, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    }

    bool WriteAll(std::string_view data) const {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto chunk = static_cast<IoSize>(std::min<std::size_t>(data.size() - sent, 1u << 20));
#if defined(MSG_NOSIGNAL)
            const auto n = ::send(myS, data.data() + sent, chunk, MSG_NOSIGNAL);
#else
            const auto n = ::send(myS, data.data() + sent, chunk, 0);
#endif
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool ReadExact(std::string& out, std::size_t size) const {
        out.resize(size);
        std::size_t got = 0;
        while (got < size) {
            const auto chunk = static_cast<IoSize>(std::min<std::size_t>(size - got, 1u << 20));
            const auto n = ::recv(myS, out.data() + got, chunk, 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);
        }
        return true;
    }

private:
    Native myS = kNone;
};

bool WriteFrame(const Socket& s, char type, std::string_view body) {
    std::string header(kMagic.begin(), kMagic.end());
    header.push_back(type);
    const auto size = static_cast<std::uint32_t>(body.size());
    for (int shift = 24; shift >= 0; shift -= 8) header.push_back(static_cast<char>((size >> shift) & 0xFFu));
    return s.WriteAll(header) && s.WriteAll(body);
}

struct Frame {
    char        type = 0;
    std::string body;
};

/// One frame; false when broken, not ours, or larger than a transfer may be.
bool ReadFrame(const Socket& s, Frame& f) {
    std::string header;
    if (!s.ReadExact(header, kHeader) || !std::equal(kMagic.begin(), kMagic.end(), header.begin())) return false;
    f.type = header[4];
    std::uint32_t size = 0;
    for (std::size_t i = 5; i < kHeader; ++i) size = (size << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(header[i]));
    // The seal adds 40 bytes to the largest package.
    if (size > kMaxTransfer + 64) return false;
    return s.ReadExact(f.body, size);
}

Expected<Socket> ConnectTo(const Address& a, int timeoutMs) {
    if (!NetInit()) return Error{"Сеть недоступна"};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(a.port));
    if (inet_pton(AF_INET, a.host.c_str(), &addr.sin_addr) != 1) return Error{"В QR-коде неверный адрес компьютера"};
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.IsOpen()) return Error{"Сеть недоступна"};
    s.Prepare(timeoutMs);
    if (::connect(s.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        return Error{"Компьютер не отвечает: окно передачи на нём закрыто, или устройство в другой сети Wi-Fi"};
    return s;
}

/// Sends one request and reads the answer, opened with the key.
Expected<Frame> Ask(const Address& a, char type, std::string_view body, int timeoutMs) {
    Expected<Socket> s = ConnectTo(a, timeoutMs);
    if (!s) return s.GetError();
    Expected<std::string> sealed = Seal(body, a.key, a.session);
    if (!sealed) return sealed.GetError();
    Frame answer;
    if (!WriteFrame(*s, type, *sealed) || !ReadFrame(*s, answer)) return Error{"Связь с компьютером прервалась"};
    if (answer.type == kRefused) return Error{"Компьютер не принял: QR-код устарел или от другого проекта"};
    Expected<std::string> opened = Open(answer.body, a.key, a.session);
    if (!opened) return opened.GetError();
    answer.body = std::move(*opened);
    return answer;
}

} // namespace

std::string LocalAddress() {
    if (!NetInit()) return {};
    // "Connecting" a UDP socket only picks the route: no packet leaves. The
    // target is TEST-NET-1 (RFC 5737), an address that is never used.
    Socket s(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!s.IsOpen()) return {};
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &target.sin_addr);
    if (::connect(s.Get(), reinterpret_cast<const sockaddr*>(&target), sizeof target) != 0) return {};
    sockaddr_in local{};
    socklen_t size = sizeof local;
    if (::getsockname(s.Get(), reinterpret_cast<sockaddr*>(&local), &size) != 0 || local.sin_addr.s_addr == htonl(INADDR_ANY)) return {};
    std::array<char, INET_ADDRSTRLEN> text{};
    if (!inet_ntop(AF_INET, &local.sin_addr, text.data(), text.size())) return {};
    return text.data();
}

struct Server::Impl {
    Session     session;
    std::string package;
    Address     address;
    Socket      listener;
};

Server::Server(std::unique_ptr<Impl> impl) : myImpl(std::move(impl)) {}
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

const Address& Server::GetAddress() const { return myImpl->address; }

void Server::Stop() {
    if (myImpl) myImpl->listener.Close();
}

Expected<Server> Server::Start(const Session& session, std::string package) {
    if (package.size() > kMaxTransfer) return Error{"Проект слишком большой для передачи по сети"};
    if (!NetInit()) return Error{"Сеть недоступна"};
    auto impl = std::make_unique<Impl>();
    impl->session = session;
    impl->package = std::move(package);
    impl->listener = Socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!impl->listener.IsOpen()) return Error{"Не удалось открыть приём по сети"};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;                           // any free port
    addr.sin_addr.s_addr = htonl(INADDR_ANY);    // the device is another computer in the same network
    sockaddr_in bound{};
    socklen_t size = sizeof bound;
    if (::bind(impl->listener.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(impl->listener.Get(), 4) != 0 ||
        ::getsockname(impl->listener.Get(), reinterpret_cast<sockaddr*>(&bound), &size) != 0)
        return Error{"Не удалось открыть приём по сети"};
    impl->address = {LocalAddress(), ntohs(bound.sin_port), session.id, session.key};
    return Server(std::move(impl));
}

Server::Event Server::Serve(int timeoutMs) {
    Event event;
    if (!myImpl || !myImpl->listener.IsOpen()) return event;
    const Native l = myImpl->listener.Get();
    // select(): the same on every system (WSAPoll needs a newer Windows SDK setting).
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(l, &ready);
    timeval wait{};
    wait.tv_sec = timeoutMs / 1000;
    wait.tv_usec = (timeoutMs % 1000) * 1000;
    if (::select(static_cast<int>(l) + 1, &ready, nullptr, nullptr, &wait) <= 0) return event;
    Socket c(::accept(l, nullptr, nullptr));
    if (!c.IsOpen()) return event;
    c.Prepare(kConnectionMs);

    const std::string& key = myImpl->session.key;
    const std::string& id = myImpl->session.id;
    Frame request;
    Expected<std::string> body = Error{""};
    if (ReadFrame(c, request)) body = Open(request.body, key, id);
    if (!body) {
        // Not this project's device: it learns nothing, not even why.
        WriteFrame(c, kRefused, {});
        event.kind = Event::Kind::Refused;
        return event;
    }
    if (request.type == kGet && *body == "get") {
        Expected<std::string> sealed = Seal(myImpl->package, key, id);
        if (sealed && WriteFrame(c, kPackage, *sealed)) event.kind = Event::Kind::Sent;
    } else if (request.type == kReturn) {
        Expected<std::string> ok = Seal("ok", key, id);
        if (ok && WriteFrame(c, kOk, *ok)) {
            event.kind = Event::Kind::Returned;
            event.data = std::move(*body);
        }
    } else {
        WriteFrame(c, kRefused, {});
        event.kind = Event::Kind::Refused;
    }
    return event;
}

Expected<std::string> Fetch(const Address& address, int timeoutMs) {
    Expected<Frame> answer = Ask(address, kGet, "get", timeoutMs);
    if (!answer) return answer.GetError();
    if (answer->type != kPackage) return Error{"Компьютер ответил не тем"};
    return std::move(answer->body);
}

Status Give(const Address& address, const std::string& returned, int timeoutMs) {
    if (returned.size() > kMaxTransfer) return Error{"Изменения слишком большие для передачи по сети"};
    Expected<Frame> answer = Ask(address, kReturn, returned, timeoutMs);
    if (!answer) return answer.GetError();
    if (answer->type != kOk || answer->body != "ok") return Error{"Компьютер не подтвердил приём"};
    return {};
}

} // namespace bz::assembly::handoff
