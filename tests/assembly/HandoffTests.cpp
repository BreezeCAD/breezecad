// "Continue on device": a part or an assembly goes to the device and its
// changes come back where they were on the PC.
#include "../Fixtures.h"

#include "assembly/AssemblyFile.h"
#include "assembly/Handoff.h"
#include "assembly/HandoffNet.h"
#include "assembly/ProjectPack.h"
#include "core/PathText.h"
#include "document/BzcFile.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace bz;
using namespace bz::assembly;
using namespace bz::assembly::handoff;
namespace fs = std::filesystem;

namespace {

fs::path Fresh(const char* name) {
    const fs::path dir = fs::temp_directory_path() / PathFromUtf8(name);
    std::error_code ec;
    fs::remove_all(dir, ec);   // a leftover of a run that failed half way
    fs::create_directories(dir);
    return dir;
}

void WritePart(const fs::path& file, int rectangles = 1) {
    doc::Document d;
    for (int i = 0; i < rectangles; ++i) {
        doc::SketchFeature& sk = test::AddSketch(d);
        test::AddRect(d, sk, {0, 0}, {10.0 + i, 10});
    }
    fs::create_directories(file.parent_path());
    REQUIRE(doc::SaveBzc(d, file, "test"));
}

std::string Bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

/// The device's side: unpacks the package into its own folder.
Ticket Receive(const fs::path& package, const fs::path& folder) {
    Expected<Ticket> t = ReadTicket(package);
    REQUIRE(t);
    REQUIRE(UnpackProject(package, folder));
    return *t;
}

} // namespace

TEST_CASE("SHA-256 matches the standard's examples") {
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(RandomHex(16).size() == 32);
    CHECK(RandomHex(16) != RandomHex(16));
}

TEST_CASE("what goes over the network opens only with its key and for its project") {
    const std::string key = RandomHex(32), session = RandomHex(16);
    const std::string data = std::string("пакет\0с нулём", 22) + std::string(100000, 'x');
    Expected<std::string> sealed = Seal(data, key, session);
    REQUIRE(sealed);
    CHECK(sealed->size() == data.size() + 40);
    CHECK(sealed->find("пакет") == std::string::npos);   // not readable on the way
    Expected<std::string> opened = Open(*sealed, key, session);
    REQUIRE(opened);
    CHECK(*opened == data);
    CHECK(*Seal(data, key, session) != *sealed);   // a new nonce each time

    CHECK_FALSE(Open(*sealed, RandomHex(32), session));   // another key
    CHECK_FALSE(Open(*sealed, key, RandomHex(16)));       // another project
    std::string tampered = *sealed;
    tampered[tampered.size() / 2] ^= 1;
    CHECK_FALSE(Open(tampered, key, session));
    CHECK_FALSE(Open("short", key, session));
    CHECK_FALSE(Seal(data, "not a key", session));
    REQUIRE(Open(*Seal("", key, session), key, session));
}

TEST_CASE("the QR carries the address and the session, and nothing broken passes") {
    const Address a{"192.168.1.5", 50213, RandomHex(16), RandomHex(32)};
    Expected<Address> back = ParseQr(QrText(a));
    REQUIRE(back);
    CHECK(back->host == a.host);
    CHECK(back->port == a.port);
    CHECK(back->session == a.session);
    CHECK(back->key == a.key);

    CHECK_FALSE(ParseQr("https://example.com"));
    CHECK_FALSE(ParseQr(QrText({"192.168.1.5", 0, a.session, a.key})));
    CHECK_FALSE(ParseQr(QrText({"192.168.1.5", 70000, a.session, a.key})));
    CHECK_FALSE(ParseQr(QrText({"192.168.1.5", 80, a.session, "abc"})));
    CHECK_FALSE(ParseQr(QrText({"host/../x", 80, a.session, a.key})));
    CHECK_FALSE(ParseQr("breezecad://handoff?v=99&h=1.2.3.4&p=80&s=" + a.session + "&k=" + a.key));
}

TEST_CASE("a part goes to the device and comes back in its place") {
    const fs::path root = Fresh("bz_handoff_part");
    const fs::path part = root / PathFromUtf8("ПК") / PathFromUtf8("Вал.bzc");
    WritePart(part);
    const fs::path package = root / PathFromUtf8("send.bzp");
    Expected<Session> s = Send(part, package, "test", R"({"camera":1})");
    REQUIRE(s);
    CHECK(s->kind == "part");
    REQUIRE(SaveSession(*s, root / PathFromUtf8("session.json")));
    Expected<Session> kept = LoadSession(root / PathFromUtf8("session.json"));
    REQUIRE(kept);
    CHECK(kept->key == s->key);
    CHECK(kept->files.front().source == part);

    const fs::path device = root / PathFromUtf8("Устройство");
    const Ticket t = Receive(package, device);
    CHECK(t.session == s->id);
    CHECK(t.view == R"({"camera":1})");
    CHECK(Bytes(package).find(s->key) == std::string::npos);   // the key goes only through the QR

    // Nothing changed: nothing to send back.
    Expected<int> none = PackReturn(t, device, root / PathFromUtf8("none.zip"), "test");
    REQUIRE(none);
    CHECK(*none == 0);

    WritePart(device / PathFromUtf8("Вал.bzc"), 2);
    const fs::path back = root / PathFromUtf8("back.zip");
    Expected<int> count = PackReturn(t, device, back, "test");
    REQUIRE(count);
    CHECK(*count == 1);

    Expected<std::vector<Returned>> plan = PlanReturn(*kept, back);
    REQUIRE(plan);
    REQUIRE(plan->size() == 1);
    CHECK((*plan)[0].change == Change::Update);
    CHECK((*plan)[0].target == part);

    const std::string before = Bytes(part);
    Expected<ApplyReport> r = ApplyReturn(*kept, back, {Choice::TakeDevice}, root / PathFromUtf8("Копии"), "test");
    REQUIRE(r);
    CHECK(Bytes(part) == Bytes(device / PathFromUtf8("Вал.bzc")));
    REQUIRE(r->backups.size() == 1);
    CHECK(Bytes(r->backups[0]) == before);
}

TEST_CASE("an assembly comes back with its parts where they were on the PC") {
    const fs::path root = Fresh("bz_handoff_assembly");
    const fs::path dir = root / PathFromUtf8("Проект");
    WritePart(dir / PathFromUtf8("Основание.bzc"));
    WritePart(dir / PathFromUtf8("Стандартные изделия") / PathFromUtf8("Винт М4.bzc"));
    WritePart(root / PathFromUtf8("Общие") / PathFromUtf8("Ось.bzc"));
    const fs::path file = dir / PathFromUtf8("Узел.bza");
    {
        Assembly a;
        a.info = {"Узел", "БРЗ.200.000"};
        a.AddComponent("Основание.bzc", "Основание");
        a.AddComponent("Стандартные изделия/Винт М4.bzc", "Винт");
        a.AddComponent("../Общие/Ось.bzc", "Ось");
        REQUIRE(SaveAssembly(a, file, "test"));
    }
    const fs::path package = root / PathFromUtf8("send.bzp");
    Expected<Session> s = Send(file, package, "test");
    REQUIRE(s);
    CHECK(s->kind == "assembly");
    CHECK(s->files.size() == 4);   // the assembly, its 3 parts; standard parts are ordinary part files

    const fs::path device = root / PathFromUtf8("Устройство");
    const Ticket t = Receive(package, device);
    CHECK(fs::exists(device / PathFromUtf8("Ось.bzc")));   // was outside: now next to the assembly

    // On the device: the axle changes, a new part is added to the assembly.
    WritePart(device / PathFromUtf8("Ось.bzc"), 3);
    WritePart(device / PathFromUtf8("Крышка.bzc"));
    {
        Expected<Assembly> a = LoadAssembly(device / PathFromUtf8("Узел.bza"));
        REQUIRE(a);
        a->AddComponent("Крышка.bzc", "Крышка");
        REQUIRE(SaveAssembly(*a, device / PathFromUtf8("Узел.bza"), "test"));
    }
    const fs::path back = root / PathFromUtf8("back.zip");
    Expected<int> count = PackReturn(t, device, back, "test");
    REQUIRE(count);
    CHECK(*count == 3);

    Expected<std::vector<Returned>> plan = PlanReturn(*s, back);
    REQUIRE(plan);
    REQUIRE(plan->size() == 3);
    std::vector<Choice> choices;
    for (const Returned& r : *plan) {
        if (r.path == "Крышка.bzc") {
            CHECK(r.change == Change::Added);
            CHECK(r.target == dir / PathFromUtf8("Крышка.bzc"));
        } else {
            CHECK(r.change == Change::Update);
        }
        if (r.path == "Ось.bzc") CHECK(r.target == root / PathFromUtf8("Общие") / PathFromUtf8("Ось.bzc"));
        choices.push_back(Choice::TakeDevice);
    }
    REQUIRE(ApplyReturn(*s, back, choices, root / PathFromUtf8("Копии"), "test"));

    CHECK(Bytes(root / PathFromUtf8("Общие") / PathFromUtf8("Ось.bzc")) == Bytes(device / PathFromUtf8("Ось.bzc")));
    CHECK(fs::exists(dir / PathFromUtf8("Крышка.bzc")));
    Expected<Assembly> a = LoadAssembly(file);
    REQUIRE(a);
    REQUIRE(a->components.size() == 4);
    CHECK(a->components[0].path == "Основание.bzc");
    CHECK(a->components[1].path == "Стандартные изделия/Винт М4.bzc");
    CHECK(a->components[2].path == "../Общие/Ось.bzc");   // its path on the PC, not the package's
    CHECK(a->components[3].path == "Крышка.bzc");
}

TEST_CASE("a file changed on the PC too is a conflict; changes for another project are refused") {
    const fs::path root = Fresh("bz_handoff_conflict");
    const fs::path part = root / PathFromUtf8("Вал.bzc");
    WritePart(part);
    const fs::path package = root / PathFromUtf8("send.bzp");
    Expected<Session> s = Send(part, package, "test");
    REQUIRE(s);
    const fs::path device = root / PathFromUtf8("Устройство");
    const Ticket t = Receive(package, device);

    WritePart(device / PathFromUtf8("Вал.bzc"), 2);
    WritePart(part, 3);   // meanwhile on the PC
    const fs::path back = root / PathFromUtf8("back.zip");
    REQUIRE(PackReturn(t, device, back, "test"));

    Expected<std::vector<Returned>> plan = PlanReturn(*s, back);
    REQUIRE(plan);
    REQUIRE(plan->size() == 1);
    CHECK((*plan)[0].change == Change::Conflict);

    const std::string mine = Bytes(part);
    Expected<ApplyReport> r = ApplyReturn(*s, back, {Choice::KeepBoth}, root / PathFromUtf8("Копии"), "test");
    REQUIRE(r);
    CHECK(Bytes(part) == mine);
    REQUIRE(r->written.size() == 1);
    CHECK(r->written[0] == root / PathFromUtf8("Вал (с устройства).bzc"));

    CHECK(ApplyReturn(*s, back, {Choice::KeepMine}, root / PathFromUtf8("Копии"), "test"));
    CHECK(Bytes(part) == mine);
    CHECK_FALSE(ApplyReturn(*s, back, {}, root / PathFromUtf8("Копии"), "test"));   // a choice for every file

    Session other = *s;
    other.id = RandomHex(16);
    CHECK_FALSE(PlanReturn(other, back));
}

TEST_CASE("over the network: the device takes the project and gives the changes back; strangers get nothing") {
    const fs::path root = Fresh("bz_handoff_net");
    const fs::path part = root / PathFromUtf8("Вал.bzc");
    WritePart(part);
    const fs::path package = root / PathFromUtf8("send.bzp");
    Expected<Session> s = Send(part, package, "test");
    REQUIRE(s);
    Expected<Server> server = Server::Start(*s, Bytes(package));
    REQUIRE(server);
    CHECK(server->GetAddress().port > 0);
    Address here = server->GetAddress();
    here.host = "127.0.0.1";   // the test machine may be in no network at all

    // The device takes the project.
    Server::Event event;
    std::thread serving([&] { event = server->Serve(5000); });
    Expected<std::string> got = Fetch(here, 5000);
    serving.join();
    REQUIRE(got);
    CHECK(*got == Bytes(package));
    CHECK(event.kind == Server::Event::Kind::Sent);

    // A QR of another project (or an old one): refused.
    Address stranger = here;
    stranger.key = RandomHex(32);
    serving = std::thread([&] { event = server->Serve(5000); });
    CHECK_FALSE(Fetch(stranger, 5000));
    serving.join();
    CHECK(event.kind == Server::Event::Kind::Refused);

    // The changes come back.
    const std::string changes = std::string(300000, 'z') + "конец";
    serving = std::thread([&] { event = server->Serve(5000); });
    CHECK(Give(here, changes, 5000));
    serving.join();
    CHECK(event.kind == Server::Event::Kind::Returned);
    CHECK(event.data == changes);

    // Nobody: the wait ends; stopped: nothing answers.
    CHECK(server->Serve(50).kind == Server::Event::Kind::None);
    server->Stop();
    CHECK_FALSE(Fetch(here, 2000));
}
