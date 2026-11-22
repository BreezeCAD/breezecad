// SPDX-License-Identifier: MPL-2.0
// Headless-диагностика медленного открытия: каждый файл из tests/data/regression
// загружается и пересчитывается без отрисовки, печатается время каждой
// операции и каждой детали и топ-20 самых медленных операций. Не входит в
// bz_tests: запускается вручную (scripts/dev/check.sh и разбор полётов).
//
//   ./build/linux-release/bin/bz_openbench [папка-с-файлами]
//
// Код возврата: 0 — все файлы открылись, 1 — хоть один не открылся или
// превысил бюджет (BZ_OPENBENCH_BUDGET_MS, по умолчанию 5000 мс на файл).

#include "assembly/AssemblyFile.h"
#include "assembly/ProjectPack.h"
#include "core/PathText.h"
#include "document/BzcFile.h"
#include "kernel/AssemblyModel.h"
#include "kernel/Model.h"
#include "kernel/occt/ModelImpl.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace bz;

namespace {

struct OpTime {
    std::string file;    ///< деталь, которой принадлежит операция
    std::string name;    ///< имя операции
    double      ms = 0;
};

/// Пересчёт детали: имя файла, имя и время каждой операции.
bool BenchPart(const std::filesystem::path& file, std::vector<OpTime>& ops, double& totalMs) {
    const auto t0 = std::chrono::steady_clock::now();
    Expected<doc::Document> doc = doc::LoadBzc(file);
    if (!doc) {
        std::cout << "  ОШИБКА ЗАГРУЗКИ " << ToUtf8(file.filename()) << ": " << doc.GetError().message << "\n";
        return false;
    }
    kernel::Model model;
    model.Rebuild(*doc);
    totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto& costs = model.GetImpl().costMs;
    for (const auto& f : doc->Features()) {
        const auto it = costs.find(f->id);
        if (it != costs.end()) ops.push_back({ToUtf8(file.filename()), f->name, it->second});
    }
    int errors = 0;
    for (const auto& f : doc->Features())
        if (f->status.state == doc::FeatureState::Error) ++errors;
    if (errors > 0) std::cout << "  внимание: " << errors << " операций с ошибкой в " << ToUtf8(file.filename()) << "\n";
    return true;
}

/// Распакованный проект: каждая уникальная деталь пересчитывается, сборка
/// решается поверх готовых деталей (как AssemblyModel::Update при открытии).
bool BenchProject(const std::filesystem::path& bzp, std::vector<OpTime>& ops, double& totalMs) {
    const auto t0 = std::chrono::steady_clock::now();
    const Expected<assembly::ProjectManifest> manifest = assembly::ReadProject(bzp);
    if (!manifest) {
        std::cout << "  ОШИБКА: " << manifest.GetError().message << "\n";
        return false;
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / PathFromUtf8("bz_openbench");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const Expected<std::filesystem::path> mainAsm = assembly::UnpackProject(bzp, dir);
    if (!mainAsm) {
        std::cout << "  ОШИБКА РАСПАКОВКИ: " << mainAsm.GetError().message << "\n";
        return false;
    }
    Expected<assembly::Assembly> asmly = assembly::LoadAssembly(*mainAsm);
    if (!asmly) {
        std::cout << "  ОШИБКА СБОРКИ: " << asmly.GetError().message << "\n";
        return false;
    }

    // Как при открытии в программе: AssemblyModel сама грузит уникальные
    // детали на своих потоках (параллельная загрузка по умолчанию включена).
    // Замер — то, что в действительности видит пользователь при открытии.
    kernel::AssemblyModel model;
    const auto tParts = std::chrono::steady_clock::now();
    model.Update(*asmly, *mainAsm, nullptr);
    const double partsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tParts).count();
    std::cout << "  компонентов: " << asmly->components.size()
              << ", загрузка+пересчёт деталей (AssemblyModel::Update): " << partsMs << " мс\n";
    bool ok = true;
    totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::filesystem::remove_all(dir, ec);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? std::filesystem::path(argv[1])
                                               : std::filesystem::current_path() / "tests" / "data" / "regression";
    const double budget = std::atof(std::getenv("BZ_OPENBENCH_BUDGET_MS") ? std::getenv("BZ_OPENBENCH_BUDGET_MS") : "5000");

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext == ".bzc" || ext == ".bzp") files.push_back(e.path());
    }
    std::ranges::sort(files);
    if (files.empty()) {
        std::cout << "в " << dir << " нет файлов .bzc/.bzp\n";
        return 1;
    }

    std::vector<OpTime> ops;
    bool ok = true;
    for (const std::filesystem::path& f : files) {
        const auto t0 = std::chrono::steady_clock::now();
        double ms = 0;
        std::cout << "=== " << ToUtf8(f.filename()) << "\n";
        bool opened;
        if (f.extension() == ".bzp") opened = BenchProject(f, ops, ms);
        else opened = BenchPart(f, ops, ms);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  итого: " << ms << " мс" << (ms > budget ? " — ПРЕВЫШЕН БЮДЖЕТ " + std::to_string(budget) : "") << "\n";
        if (!opened || ms > budget) ok = false;
    }

    std::ranges::sort(ops, [](const OpTime& a, const OpTime& b) { return a.ms > b.ms; });
    std::cout << "\n=== топ-" << std::min<std::size_t>(20, ops.size()) << " самых медленных операций\n";
    for (std::size_t i = 0; i < ops.size() && i < 20; ++i)
        std::cout << "  " << ops[i].ms << " мс  [" << ops[i].file << "] " << ops[i].name << "\n";
    return ok ? 0 : 1;
}
