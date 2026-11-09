#include "assembly/Handoff.h"

#include "assembly/AssemblyFile.h"
#include "assembly/ProjectPack.h"
#include "core/PathText.h"
#include "document/BzcFile.h"

#include <miniz.h>
#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <utility>

namespace bz::assembly::handoff {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Entries = std::vector<std::pair<std::string, std::string>>;   // name in the archive → bytes

constexpr const char* kTicket = "handoff.json";
constexpr const char* kSentFormat = "bz-handoff";
constexpr const char* kReturnFormat = "bz-handoff-return";
constexpr std::string_view kQrPrefix = "breezecad://handoff?";

Expected<std::string> ReadBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);   // a wide path on Windows: names are often Cyrillic
    if (!in) return Error{std::format("Не удалось прочитать {}", ToUtf8(file.filename()))};
    return std::string(std::istreambuf_iterator<char>(in), {});
}

/// Written beside and renamed over: a failure half way leaves the old file whole.
Status WriteBytes(const fs::path& target, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    const fs::path temp = target.parent_path() / PathFromUtf8("." + ToUtf8(target.filename()) + ".bz-new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            return Error{std::format("Не удалось записать «{}» (диск заполнен или нет доступа к папке?)", ToUtf8(target.filename()))};
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        return Error{std::format("Не удалось заменить «{}»: файл открыт другой программой?", ToUtf8(target.filename()))};
    }
    return {};
}

Expected<Entries> ReadZip(const fs::path& file) {
    Expected<std::string> bytes = ReadBytes(file);
    if (!bytes) return bytes.GetError();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes->data(), bytes->size(), 0)) return Error{"Пакет передачи повреждён: это не ZIP-архив"};
    Entries out;
    bool ok = true;
    for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&zip); ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::array<char, 1024> name{};
        mz_zip_reader_get_filename(&zip, i, name.data(), static_cast<mz_uint>(name.size()));
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data && size) ok = false;
        out.emplace_back(name.data(), data ? std::string(static_cast<const char*>(data), size) : std::string());
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    if (!ok) return Error{"Пакет передачи повреждён: файл в нём не читается"};
    return out;
}

Status WriteZip(const fs::path& out, const Entries& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20)) return Error{"Не удалось подготовить архив"};
    bool ok = true;
    for (const auto& [name, bytes] : entries) {
        // Documents are ZIPs already: stored as they are; the instructions are compressed.
        const bool text = name.ends_with(".json");
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(),
                                         text ? MZ_BEST_COMPRESSION : MZ_NO_COMPRESSION);
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size);
    std::string archive = ok ? std::string(static_cast<const char*>(buffer), size) : std::string();
    mz_zip_writer_end(&zip);
    if (!ok) return Error{"Не удалось упаковать пакет передачи"};
    return WriteBytes(out, archive);
}

const std::string* Find(const Entries& entries, std::string_view name) {
    for (const auto& [n, bytes] : entries)
        if (n == name) return &bytes;
    return nullptr;
}

Expected<std::string> HashFile(const fs::path& file) {
    Expected<std::string> bytes = ReadBytes(file);
    if (!bytes) return bytes.GetError();
    return Sha256Hex(*bytes);
}

bool IsHex(std::string_view text, std::size_t length) {
    return text.size() == length &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string Slashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

/// "Вал.bzc" → "Вал (с устройства).bzc", then "(с устройства 2)"… while taken.
fs::path BesideName(const fs::path& target) {
    const std::string stem = ToUtf8(target.stem()), ext = ToUtf8(target.extension());
    std::error_code ec;
    for (int n = 1;; ++n) {
        const std::string tag = n == 1 ? "с устройства" : std::format("с устройства {}", n);
        const fs::path candidate = target.parent_path() / PathFromUtf8(std::format("{} ({}){}", stem, tag, ext));
        if (!fs::exists(candidate, ec)) return candidate;
    }
}

constexpr std::size_t kNonce = 24, kMac = 16, kKey = 32;

/// 64 hex digits → the 32-byte key; false when it is not one.
bool KeyBytes(const std::string& hex, std::array<std::uint8_t, kKey>& key) {
    if (!IsHex(hex, kKey * 2)) return false;
    for (std::size_t i = 0; i < kKey; ++i) {
        unsigned value = 0;
        std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, value, 16);
        key[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

const std::uint8_t* Raw(std::string_view s) { return reinterpret_cast<const std::uint8_t*>(s.data()); }
std::uint8_t*       Raw(std::string& s) { return reinterpret_cast<std::uint8_t*>(s.data()); }

Json SessionToJson(const Session& s) {
    Json files = Json::array();
    for (const SentFile& f : s.files)
        files.push_back({{"path", f.path},
                         {"source", ToUtf8(f.source)},
                         {"original", f.original},
                         {"sentHash", f.sentHash},
                         {"pcHash", f.pcHash}});
    return {{"format", "bz-handoff-session"}, {"version", kHandoffVersion}, {"id", s.id}, {"key", s.key},
            {"kind", s.kind}, {"main", s.main}, {"view", s.view}, {"files", files}};
}

} // namespace

std::string Sha256Hex(std::string_view bytes) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    std::string msg(bytes);
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));

    for (std::size_t block = 0; block < msg.size(); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            std::uint32_t word = 0;
            for (std::size_t b = 0; b < 4; ++b)
                word = (word << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(msg[block + i * 4 + b]));
            w[i] = word;
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> v = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = s0 + maj;
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) h[i] += v[i];
    }
    std::string out;
    for (std::uint32_t word : h) out += std::format("{:08x}", word);
    return out;
}

std::string RandomHex(int bytes) {
    // std::random_device is the system's generator on the platforms we build
    // for (Windows, Linux, macOS/iOS, Android); the key must not be guessable.
    std::random_device device;
    std::string out;
    for (int i = 0; i < bytes; ++i) out += std::format("{:02x}", device() & 0xFFu);
    return out;
}

Expected<Session> Send(const fs::path& document, const fs::path& package, std::string_view version, std::string view) {
    Expected<std::string> type = doc::ReadDocumentType(document);
    if (!type) return type.GetError();
    Session s;
    s.id = RandomHex(16);
    s.key = RandomHex(32);
    s.view = std::move(view);
    s.main = ToUtf8(document.filename());
    Expected<std::string> pcHash = HashFile(document);
    if (!pcHash) return pcHash.GetError();

    Entries entries;
    if (*type == "assembly") {
        s.kind = "assembly";
        const fs::path temp = package.parent_path() / PathFromUtf8("." + ToUtf8(package.filename()) + ".bzp");
        Expected<PackReport> report = PackProject(document, temp, version);
        if (!report) return report.GetError();
        Expected<Entries> packed = ReadZip(temp);
        std::error_code ec;
        fs::remove(temp, ec);
        if (!packed) return packed.GetError();
        entries = std::move(*packed);
        const std::string* main = Find(entries, s.main);
        if (!main) return Error{"Не удалось упаковать сборку"};
        s.files.push_back({s.main, document, {}, Sha256Hex(*main), *pcHash});
        for (const PackedPart& p : report->packed) {
            const std::string* bytes = Find(entries, p.path);
            if (!bytes) return Error{std::format("Не удалось упаковать «{}»", p.path)};
            s.files.push_back({p.path, p.source, p.original, Sha256Hex(*bytes), Sha256Hex(*bytes)});   // parts go as they are
        }
    } else if (*type == "part") {
        s.kind = "part";
        Expected<std::string> bytes = ReadBytes(document);
        if (!bytes) return bytes.GetError();
        // The same .bzp as for an assembly, with the part as its main document.
        const Json project{{"format", "bzp"},
                           {"version", kProjectFormatVersion},
                           {"createdBy", std::string(version)},
                           {"name", ToUtf8(document.stem())},
                           {"main", s.main},
                           {"files", Json::array({{{"path", s.main}, {"role", "part"}}})}};
        entries.emplace_back("project.json", project.dump(1, '\t'));
        entries.emplace_back(s.main, *bytes);
        if (auto preview = doc::ReadBzcPreview(document); preview && !preview->empty())
            entries.emplace_back("preview.png", std::string(preview->begin(), preview->end()));
        s.files.push_back({s.main, document, {}, Sha256Hex(*bytes), *pcHash});
    } else {
        return Error{std::format("«{}» нельзя передать: это не деталь и не сборка", s.main)};
    }

    Json files = Json::array();
    for (const SentFile& f : s.files) files.push_back({{"path", f.path}, {"hash", f.sentHash}});
    const Json ticket{{"format", kSentFormat}, {"version", kHandoffVersion}, {"createdBy", std::string(version)},
                      {"session", s.id},       {"kind", s.kind},              {"main", s.main},
                      {"view", s.view},        {"files", files}};
    entries.emplace_back(kTicket, ticket.dump(1, '\t'));
    if (Status st = WriteZip(package, entries); !st) return st.GetError();
    return s;
}

Status SaveSession(const Session& session, const fs::path& file) { return WriteBytes(file, SessionToJson(session).dump(1, '\t')); }

Expected<Session> LoadSession(const fs::path& file) {
    Expected<std::string> text = ReadBytes(file);
    if (!text) return text.GetError();
    try {
        const Json j = Json::parse(*text);
        if (j.value("format", std::string{}) != "bz-handoff-session") return Error{"Это не сеанс передачи BreezeCAD"};
        Session s;
        s.id = j.value("id", std::string{});
        s.key = j.value("key", std::string{});
        s.kind = j.value("kind", std::string{});
        s.main = j.value("main", std::string{});
        s.view = j.value("view", std::string{});
        for (const Json& f : j.value("files", Json::array()))
            s.files.push_back({f.value("path", std::string{}), PathFromUtf8(f.value("source", std::string{})),
                               f.value("original", std::string{}), f.value("sentHash", std::string{}),
                               f.value("pcHash", std::string{})});
        if (!IsHex(s.id, 32) || s.files.empty()) return Error{"Сеанс передачи повреждён"};
        return s;
    } catch (const Json::exception&) {
        return Error{"Сеанс передачи повреждён"};
    }
}

Expected<Ticket> ReadTicket(const fs::path& package) {
    Expected<Entries> entries = ReadZip(package);
    if (!entries) return entries.GetError();
    const std::string* text = Find(*entries, kTicket);
    if (!text) return Error{"Это не пакет передачи: в нём нет handoff.json"};
    try {
        const Json j = Json::parse(*text);
        if (j.value("format", std::string{}) != kSentFormat) return Error{"Это не пакет передачи BreezeCAD"};
        if (j.value("version", 0) > kHandoffVersion) return Error{"Пакет передан более новой версией BreezeCAD: обновите программу"};
        Ticket t;
        t.session = j.value("session", std::string{});
        t.kind = j.value("kind", std::string{});
        t.main = j.value("main", std::string{});
        t.view = j.value("view", std::string{});
        for (const Json& f : j.value("files", Json::array())) {
            Ticket::File file{f.value("path", std::string{}), f.value("hash", std::string{})};
            if (!SafeArchivePath(file.path)) return Error{std::format("Пакет небезопасен: путь «{}» ведёт за пределы папки", file.path)};
            t.files.push_back(std::move(file));
        }
        if (!IsHex(t.session, 32) || !SafeArchivePath(t.main)) return Error{"Пакет передачи повреждён"};
        return t;
    } catch (const Json::exception&) {
        return Error{"Пакет передачи повреждён: handoff.json не читается"};
    }
}

Expected<int> PackReturn(const Ticket& ticket, const fs::path& folder, const fs::path& out, std::string_view version) {
    std::map<std::string, std::string> sent;
    for (const Ticket::File& f : ticket.files) sent[f.path] = f.hash;

    std::map<std::string, std::pair<std::string, std::string>> changed;   // path → (bytes, base hash)
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = ToUtf8(it->path().filename());
        if (name.starts_with('.')) continue;   // temporary files of a save in progress
        const std::string path = Slashes(ToUtf8(fs::relative(it->path(), folder, ec)));
        if (ec || !SafeArchivePath(path) || path == kTicket) continue;
        const auto known = sent.find(path);
        // New files: only documents (by their type, not their name).
        if (known == sent.end() && !doc::ReadDocumentType(it->path())) continue;
        Expected<std::string> bytes = ReadBytes(it->path());
        if (!bytes) return bytes.GetError();
        const std::string hash = Sha256Hex(*bytes);
        if (known != sent.end() && known->second == hash) continue;
        changed[path] = {std::move(*bytes), known != sent.end() ? known->second : std::string()};
    }
    if (ec) return Error{std::format("Не удалось прочитать папку проекта: {}", ec.message())};

    Entries entries;
    Json files = Json::array();
    for (auto& [path, item] : changed) {
        files.push_back({{"path", path}, {"base", item.second}, {"hash", Sha256Hex(item.first)}});
        entries.emplace_back(path, std::move(item.first));
    }
    const Json ticketOut{{"format", kReturnFormat}, {"version", kHandoffVersion}, {"createdBy", std::string(version)},
                         {"session", ticket.session}, {"files", files}};
    entries.emplace_back(kTicket, ticketOut.dump(1, '\t'));
    if (Status st = WriteZip(out, entries); !st) return st.GetError();
    return static_cast<int>(changed.size());
}

Expected<std::vector<Returned>> PlanReturn(const Session& session, const fs::path& returned) {
    Expected<Entries> entries = ReadZip(returned);
    if (!entries) return entries.GetError();
    const std::string* text = Find(*entries, kTicket);
    if (!text) return Error{"Это не изменения с устройства: в пакете нет handoff.json"};
    if (session.files.empty()) return Error{"Сеанс передачи повреждён"};
    const fs::path base = session.files.front().source.parent_path();
    std::map<std::string, const SentFile*> sent;
    for (const SentFile& f : session.files) sent[f.path] = &f;
    try {
        const Json j = Json::parse(*text);
        if (j.value("format", std::string{}) != kReturnFormat) return Error{"Это не изменения с устройства"};
        if (j.value("version", 0) > kHandoffVersion) return Error{"Изменения отданы более новой версией BreezeCAD: обновите программу"};
        // The QR is for one project: changes for any other are not taken.
        if (j.value("session", std::string{}) != session.id) return Error{"Эти изменения — для другого проекта"};
        std::vector<Returned> plan;
        std::error_code ec;
        for (const Json& f : j.value("files", Json::array())) {
            Returned r;
            r.path = f.value("path", std::string{});
            if (!SafeArchivePath(r.path)) return Error{std::format("Пакет небезопасен: путь «{}» ведёт за пределы папки", r.path)};
            if (!Find(*entries, r.path)) return Error{std::format("Пакет повреждён: в нём нет «{}»", r.path)};
            if (const auto known = sent.find(r.path); known != sent.end()) {
                r.target = known->second->source;
                // Gone from the PC meanwhile: nothing to lose, it comes back.
                Expected<std::string> now = HashFile(r.target);
                r.change = !now || *now == known->second->pcHash ? Change::Update : Change::Conflict;
            } else {
                r.target = base / PathFromUtf8(r.path);
                r.change = fs::exists(r.target, ec) ? Change::Conflict : Change::Added;
            }
            plan.push_back(std::move(r));
        }
        return plan;
    } catch (const Json::exception&) {
        return Error{"Пакет повреждён: handoff.json не читается"};
    }
}

Expected<ApplyReport> ApplyReturn(const Session& session, const fs::path& returned, const std::vector<Choice>& choices,
                                  const fs::path& backups, std::string_view version) {
    Expected<std::vector<Returned>> plan = PlanReturn(session, returned);
    if (!plan) return plan.GetError();
    if (choices.size() != plan->size()) return Error{"Не для всех файлов выбрано, что с ними делать"};
    Expected<Entries> entries = ReadZip(returned);
    if (!entries) return entries.GetError();

    // The package moved parts from elsewhere next to the assembly: the
    // assembly gets back the paths it had on the PC.
    std::map<std::string, std::string> original;
    for (const SentFile& f : session.files)
        if (!f.original.empty()) original[f.path] = f.original;

    ApplyReport report;
    std::error_code ec;
    for (std::size_t i = 0; i < plan->size(); ++i) {
        const Returned& r = (*plan)[i];
        if (choices[i] == Choice::KeepMine) continue;
        std::string bytes = *Find(*entries, r.path);

        if (session.kind == "assembly" && r.path == session.main) {
            const fs::path temp = fs::temp_directory_path(ec) / PathFromUtf8(std::format("bz-handoff-{}.bza", session.id));
            const fs::path fixed = fs::temp_directory_path(ec) / PathFromUtf8(std::format("bz-handoff-{}-fixed.bza", session.id));
            if (Status st = WriteBytes(temp, bytes); !st) return st.GetError();
            Expected<Assembly> a = LoadAssembly(temp);
            if (!a) {
                fs::remove(temp, ec);
                return a.GetError();
            }
            for (Component& c : a->components)
                if (const auto it = original.find(c.path); it != original.end()) c.path = it->second;
            std::vector<std::uint8_t> preview;
            if (auto p = doc::ReadBzcPreview(temp); p) preview = std::move(*p);
            const Status saved = SaveAssembly(*a, fixed, std::format("BreezeCAD {}", version), preview);
            fs::remove(temp, ec);
            if (!saved) return saved.GetError();
            Expected<std::string> fixedBytes = ReadBytes(fixed);
            fs::remove(fixed, ec);
            if (!fixedBytes) return fixedBytes.GetError();
            bytes = std::move(*fixedBytes);
        }

        fs::path target = r.target;
        if (choices[i] == Choice::KeepBoth && fs::exists(target, ec)) target = BesideName(target);
        if (choices[i] == Choice::TakeDevice && fs::exists(target, ec)) {
            const fs::path backup = backups / PathFromUtf8(session.id) / PathFromUtf8(r.path);
            fs::create_directories(backup.parent_path(), ec);
            fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
            if (ec) return Error{std::format("Не удалось сохранить прежний «{}» перед заменой: замена отменена", r.path)};
            report.backups.push_back(backup);
        }
        if (Status st = WriteBytes(target, bytes); !st) return st.GetError();
        report.written.push_back(target);
    }
    return report;
}

Expected<std::string> Seal(std::string_view bytes, const std::string& keyHex, const std::string& session) {
    std::array<std::uint8_t, kKey> key{};
    if (!KeyBytes(keyHex, key)) return Error{"Неверный ключ передачи"};
    std::string out(kNonce + kMac + bytes.size(), '\0');
    // A fresh random nonce for every message: XChaCha20's 24 bytes make a repeat practically impossible.
    std::random_device device;
    for (std::size_t i = 0; i < kNonce; ++i) out[i] = static_cast<char>(device() & 0xFFu);
    crypto_aead_lock(Raw(out) + kNonce + kMac, Raw(out) + kNonce, key.data(), Raw(out), Raw(session), session.size(), Raw(bytes),
                     bytes.size());
    crypto_wipe(key.data(), key.size());
    return out;
}

Expected<std::string> Open(std::string_view sealed, const std::string& keyHex, const std::string& session) {
    if (sealed.size() < kNonce + kMac) return Error{"Передача повреждена"};
    std::array<std::uint8_t, kKey> key{};
    if (!KeyBytes(keyHex, key)) return Error{"Неверный ключ передачи"};
    std::string out(sealed.size() - kNonce - kMac, '\0');
    const int failed = crypto_aead_unlock(Raw(out), Raw(sealed) + kNonce, key.data(), Raw(sealed), Raw(session), session.size(),
                                          Raw(sealed) + kNonce + kMac, out.size());
    crypto_wipe(key.data(), key.size());
    if (failed) return Error{"Передача не прошла проверку: другой ключ или проект, или данные повреждены"};
    return out;
}

std::string QrText(const Address& a) {
    return std::format("{}v={}&h={}&p={}&s={}&k={}", kQrPrefix, kHandoffVersion, a.host, a.port, a.session, a.key);
}

Expected<Address> ParseQr(std::string_view text) {
    if (!text.starts_with(kQrPrefix)) return Error{"Это не QR-код BreezeCAD"};
    text.remove_prefix(kQrPrefix.size());
    std::map<std::string, std::string, std::less<>> fields;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('&'), text.size());
        const std::string_view pair = text.substr(0, end);
        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos) fields[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
        text.remove_prefix(std::min(end + 1, text.size()));
    }
    const auto get = [&](const char* name) { return fields.contains(name) ? fields[name] : std::string(); };

    int v = 0;
    const std::string vText = get("v");
    if (std::from_chars(vText.data(), vText.data() + vText.size(), v).ec != std::errc{} || v < 1)
        return Error{"QR-код BreezeCAD повреждён"};
    if (v > kHandoffVersion) return Error{"QR-код от более новой версии BreezeCAD: обновите программу"};
    Address a;
    a.host = get("h");
    const std::string port = get("p");
    a.session = get("s");
    a.key = get("k");
    const bool hostOk = !a.host.empty() && a.host.size() <= 253 && std::all_of(a.host.begin(), a.host.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':';
    });
    const auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), a.port);
    if (!hostOk || ec != std::errc{} || ptr != port.data() + port.size() || a.port < 1 || a.port > 65535 || !IsHex(a.session, 32) ||
        !IsHex(a.key, 64))
        return Error{"QR-код BreezeCAD повреждён"};
    return a;
}

} // namespace bz::assembly::handoff
