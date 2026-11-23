#include "assembly/ProjectPack.h"
#include "assembly/AssemblyFile.h"
#include "core/I18n.h"
#include "document/BzcFile.h"
#include "document/Commands.h"
#include "document/FeatureRegistry.h"
#include "document/JsonIO.h"
#include "document/Origin.h"
#include "document/SketchFeature.h"
#include "kernel/AssemblyModel.h"
#include "kernel/ChangePreview.h"
#include "kernel/Topology.h"
#include "sketch/Shapes.h"

#include <BRepMesh_IncrementalMeshFactory.hxx>
#include <Standard_Failure.hxx>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <iostream>
#endif

namespace bz::web {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

[[noreturn]] void Fail(std::string message) { throw std::runtime_error(std::move(message)); }

template<class T> T Require(Expected<T> value) {
    if (!value) Fail(value.GetError().message);
    return std::move(value.Value());
}
void Require(Status value) {
    if (!value) Fail(value.GetError().message);
}

Json Point(Vec3 p) { return Json::array({p.x, p.y, p.z}); }

Json Flatten(const std::vector<Vec3>& points) {
    std::vector<float> values;
    values.reserve(points.size() * 3);
    for (auto p : points) {
        values.push_back(static_cast<float>(p.x));
        values.push_back(static_cast<float>(p.y));
        values.push_back(static_cast<float>(p.z));
    }
    return values;
}

Vec2 Point2(const Json& value) {
    Vec2 point;
    if (!doc::json_io::Read(value, point) || !std::isfinite(point.x) || !std::isfinite(point.y))
        Fail(Tr("Укажите координаты точки"));
    return point;
}

void ApplyFields(doc::Feature& feature, Json patch) {
    if (!patch.is_object()) Fail(Tr("Поля операции должны быть объектом"));
    Json fields;
    feature.SaveFields(fields);
    for (auto& [key, value] : patch.items()) {
        auto current = fields.find(key);
        if (current != fields.end() && current->is_object() && current->contains("expression") &&
            (value.is_number() || value.is_string())) {
            Json parameter = *current;
            parameter["expression"] = value.is_string() ? value.get<std::string>() : value.dump();
            value = std::move(parameter);
        }
    }
    fields.merge_patch(patch);
    Require(feature.LoadFields(fields));
}

class Preview {
public:
    Preview() {
        // Like the iOS build, this binary links OCCT statically.
        static const Handle(BRepMesh_IncrementalMeshFactory) mesher = new BRepMesh_IncrementalMeshFactory();
    }

    Json Run(const Json& args) {
        const auto command = args.at("command").get<std::string>();
        if (command == "app.new_part") {
            RequireClean(args);
            Reset({});
        } else if (command == "app.open") {
            RequireClean(args);
            Open(args.at("path").get<std::string>(), args.value("directory", std::string{}));
            if (args.contains("name")) myName = args.at("name").get<std::string>();
        } else if (command == "doc.save") {
            RequirePart();
            Require(doc::SaveBzc(myDoc, args.at("path").get<std::string>(), "BreezeCAD Web",
                                 kernel::GeometryKernelVersion()));
            myHistory.MarkSaved();
        } else if (command == "doc.undo" || command == "doc.redo") {
            RequirePart();
            if (command == "doc.undo") myHistory.Undo(myDoc);
            else myHistory.Redo(myDoc);
            myModel.Rebuild(myDoc);
        } else if (command == "sketch.create") {
            RequirePart();
            return AddSketch(args);
        } else if (command == "sketch.add") {
            RequirePart();
            return AddCurve(args);
        } else if (command == "part.create_feature" || command == "part.edit_feature") {
            RequirePart();
            return EditFeature(args, command == "part.create_feature");
        } else if (command == "part.feature_types" || command == "part.feature") {
            return FeatureFields(args, command == "part.feature_types");
        } else if (command == "part.faces" || command == "part.edges" || command == "part.measure") {
            return Topology(args, command);
        } else if (command == "part.properties") {
            return Properties();
        } else if (command == "view.mesh") {
            return Mesh();
        } else if (command != "part.tree" && command != "assembly.components") {
            Fail(Tr("Неизвестная команда"));
        }
        return Tree();
    }

private:
    doc::Document myDoc;
    doc::History myHistory;
    kernel::Model myModel;
    assembly::Assembly myAssembly;
    std::unique_ptr<kernel::AssemblyModel> myAssemblyModel;
    std::string myName;
    std::vector<std::string> myProblems;

    const kernel::Model& Model() const { return myAssemblyModel ? myAssemblyModel->Shown() : myModel; }

    void RequirePart() const {
        if (myAssemblyModel) Fail(Tr("Сборка открыта для просмотра"));
    }

    void RequireClean(const Json& args) const {
        if (myHistory.IsModified() && !args.value("discard", false))
            Fail(Tr("Сначала сохраните изменения"));
    }

    void Reset(doc::Document doc) {
        myDoc = std::move(doc);
        myHistory.Clear();
        myAssemblyModel.reset();
        myAssembly = {};
        myProblems.clear();
        myName.clear();
        myModel = kernel::Model{};
        myModel.Rebuild(myDoc);
    }

    void Open(fs::path path, const fs::path& directory) {
        if (path.extension() == ".bzp") {
            if (directory.empty()) Fail(Tr("Укажите папку для открытия проекта"));
            path = Require(assembly::UnpackProject(path, directory));
        }
        const auto type = Require(doc::ReadDocumentType(path));
        if (type == "assembly") {
            auto assembly = Require(assembly::LoadAssembly(path));
            auto model = std::make_unique<kernel::AssemblyModel>();
            model->SetParallelPartLoad(false);
            std::vector<std::string> problems;
            model->Update(assembly, path, &problems);
            Reset({});
            myAssembly = std::move(assembly);
            myAssemblyModel = std::move(model);
            myProblems = std::move(problems);
        } else {
            Reset(Require(doc::LoadBzc(path)));
        }
        myName = path.stem().string();
    }

    Json Tree() const {
        Json nodes = Json::array();
        if (myAssemblyModel) {
            for (const auto& c : myAssembly.components)
                nodes.push_back({{"id", c.id.value}, {"name", c.name}, {"type", "component"},
                                 {"visible", c.visible}, {"message", myAssemblyModel->ProblemOf(c.id)}});
        } else {
            for (const auto& f : myDoc.Features()) {
                Json fields;
                f->SaveFields(fields);
                Json node = {{"id", f->id.value}, {"name", f->name}, {"type", f->Type()},
                             {"state", static_cast<int>(f->status.state)}, {"message", f->status.message}};
                if (const auto it = fields.find("distance"); it != fields.end() && it->is_object())
                    node["distance"] = it->value("expression", std::string{});
                nodes.push_back(std::move(node));
            }
        }
        return {{"kind", myAssemblyModel ? "assembly" : "part"}, {"name", myName},
                {"nodes", std::move(nodes)}, {"warnings", myProblems}, {"dirty", myHistory.IsModified()},
                {"undo", myHistory.CanUndo()}, {"redo", myHistory.CanRedo()}};
    }

    Json Mesh() const {
        auto mesh = kernel::MeshOf(Model(), 0.001);
        // Active sketches appear before they are extruded. The kernel already
        // computes their closed profiles in sketch coordinates.
        if (!myAssemblyModel) {
            for (const auto& f : myDoc.Features()) {
                const auto* sketch = dynamic_cast<const doc::SketchFeature*>(f.get());
                if (!sketch || sketch->hidden) continue;
                const auto* regions = myModel.Regions(f->id);
                if (!regions || regions->empty()) {
                    for (const auto& line : sketch->sketch.lines) {
                        if (line.construction) continue;
                        const auto* start = sketch->sketch.FindPoint(line.start);
                        const auto* end = sketch->sketch.FindPoint(line.end);
                        if (!start || !end) continue;
                        mesh.segments.push_back(sketch->placement.frame.ToWorld(start->pos));
                        mesh.segments.push_back(sketch->placement.frame.ToWorld(end->pos));
                    }
                    continue;
                }
                for (const auto& region : *regions)
                    for (const auto& loop : region.loops)
                        for (std::size_t i = 0; i < loop.size(); ++i) {
                            const auto& frame = sketch->placement.frame;
                            mesh.segments.push_back(frame.ToWorld(loop[i]));
                            mesh.segments.push_back(frame.ToWorld(loop[(i + 1) % loop.size()]));
                        }
            }
        }
        return {{"triangles", Flatten(mesh.triangles)}, {"segments", Flatten(mesh.segments)}};
    }

    Json Properties() const {
        const auto props = Model().Properties();
        if (!props) return nullptr;
        return {{"volume", props->volume}, {"area", props->area}, {"min", Point(props->bounds.min)},
                {"max", Point(props->bounds.max)}, {"size", Point(props->bounds.max - props->bounds.min)}};
    }

    Json Topology(const Json& args, const std::string& command) const {
        if (command == "part.measure") {
            const auto& a = args.at("a");
            const auto& b = args.at("b");
            const auto measure = kernel::MeasureBetween(Model(), a.at("face").get<bool>(), a.at("index").get<int>(),
                                                         b.at("face").get<bool>(), b.at("index").get<int>());
            if (!measure) Fail(Tr("Элемент для измерения не найден"));
            Json out = {{"distance", measure->distance}, {"from", Point(measure->from)}, {"to", Point(measure->to)}};
            if (measure->angle) out["angle"] = *measure->angle;
            return out;
        }
        const auto topology = kernel::DescribeTopology(Model());
        Json out = Json::array();
        if (command == "part.faces") {
            for (const auto& f : topology.faces)
                out.push_back({{"index", f.index}, {"body", f.body.value}, {"area", f.area},
                               {"point", Point(f.point)}, {"normal", Point(f.normal)}, {"edges", f.edges},
                               {"ref", doc::json_io::ToJson(f.ref)}});
        } else {
            for (const auto& e : topology.edges)
                out.push_back({{"index", e.index}, {"body", e.body.value}, {"length", e.length},
                               {"radius", e.radius}, {"start", Point(e.start)}, {"end", Point(e.end)},
                               {"middle", Point(e.middle)}, {"faces", e.faces}, {"ref", doc::json_io::ToJson(e.ref)}});
        }
        return out;
    }

    Json FeatureFields(const Json& args, bool types) const {
        if (types) {
            auto& registry = doc::FeatureRegistry::Default();
            Json out = Json::object();
            for (const auto& type : registry.Types()) registry.Create(type)->SaveFields(out[type]);
            return out;
        }
        const auto* feature = myDoc.Find(ObjectId{args.at("id").get<std::uint64_t>()});
        if (!feature) Fail(Tr("Операция не найдена"));
        Json fields;
        feature->SaveFields(fields);
        return fields;
    }

    Json Apply(std::unique_ptr<doc::Feature> feature, bool create, Json result = Json::object()) {
        const ObjectId id = feature->id;
        auto makeCommand = [&](std::unique_ptr<doc::Feature> copy) {
            if (create) return doc::AddFeature(std::move(copy), myDoc.EndMarker());
            return doc::ReplaceFeature(std::move(copy), Tr("Изменить операцию"));
        };
        // Validate the edited feature before committing, to preserve the redo
        // stack and the last successful geometry on a failed AI command.
        auto candidate = myDoc.Clone();
        makeCommand(feature->Clone())->Do(candidate);
        kernel::Model check;
        check.Rebuild(candidate);
        const auto& status = candidate.Find(id)->status;
        if (status.state == doc::FeatureState::Error) Fail(status.message);
        myHistory.Execute(myDoc, makeCommand(std::move(feature)));
        myDoc.TakeRebuilt(candidate);
        myModel = std::move(check);
        result["id"] = id.value;
        result["tree"] = Tree();
        return result;
    }

    doc::SketchPlacement SketchPlane(const Json& args) const {
        if (args.contains("face")) {
            const auto index = args.at("face").get<int>();
            const auto topology = kernel::DescribeTopology(myModel);
            for (const auto& face : topology.faces)
                if (face.index == index && face.kind == doc::SurfaceKind::Plane)
                    return {face.body, doc::PlaneFrame(face.ref), true, face.ref};
            Fail(Tr("Выберите плоскую грань"));
        }
        if (args.contains("workplane")) {
            const auto id = ObjectId{args.at("workplane").get<std::uint64_t>()};
            const auto* plane = myDoc.FindAs<doc::WorkPlaneFeature>(id);
            if (!plane || !plane->frame) Fail(Tr("Рабочая плоскость не найдена"));
            return {id, *plane->frame, false, {}};
        }
        const auto plane = args.value("plane", std::string{"XY"});
        ObjectId support;
        if (plane == "XY") support = doc::origin::kPlaneXY;
        else if (plane == "XZ") support = doc::origin::kPlaneXZ;
        else if (plane == "YZ") support = doc::origin::kPlaneYZ;
        else Fail(Tr("Выберите плоскость XY, XZ или YZ"));
        return {support, *doc::origin::PlaneFrame(support), false, {}};
    }

    Json AddSketch(const Json& args) {
        auto sketch = std::make_unique<doc::SketchFeature>();
        sketch->placement = SketchPlane(args);
        sketch->id = myDoc.Ids().Next();
        sketch->name = args.value("name", myDoc.UniqueFeatureName(Tr("Эскиз")));
        sketch->sketch.EnsureOrigin(myDoc.Ids());
        if (args.contains("profile")) AddGeometry(*sketch, args.at("profile"));
        const Frame& frame = sketch->placement.frame;
        Json result = {{"plane", doc::json_io::ToJson(frame)}};
        return Apply(std::move(sketch), true, std::move(result));
    }

    Json AddCurve(const Json& args) {
        const auto* original = myDoc.FindAs<doc::SketchFeature>(ObjectId{args.at("sketch").get<std::uint64_t>()});
        if (!original) Fail(Tr("Эскиз не найден"));
        auto feature = std::make_unique<doc::SketchFeature>(*original);
        auto result = AddGeometry(*feature, args);
        return Apply(std::move(feature), false, std::move(result));
    }

    Json AddGeometry(doc::SketchFeature& feature, const Json& args) {
        auto& sketch = feature.sketch;
        auto& ids = myDoc.Ids();
        const auto type = args.at("type").get<std::string>();
        auto point = [&](Vec2 position) {
            for (const auto& existing : sketch.points)
                if (!existing.projected && (existing.pos - position).Length() < 1e-3) return existing.id;
            return sketch.AddPoint(ids, position);
        };
        Json result;
        if (type == "circle") {
            const auto radius = args.at("radius").get<double>();
            if (!std::isfinite(radius) || radius <= 0) Fail(Tr("Радиус должен быть больше нуля"));
            result["entity"] = sketch.AddCircle(ids, point(Point2(args.at("center"))), radius).value;
        } else if (type == "line" || type == "rectangle") {
            const auto a = Point2(args.at("a"));
            const auto b = Point2(args.at("b"));
            if ((a - b).Length() < 1e-8) Fail(Tr("Точки должны различаться"));
            if (type == "line") {
                const auto start = point(a);
                const auto end = point(b);
                result["entity"] = sketch.AddLine(ids, start, end).value;
            } else {
                if (std::abs(a.x - b.x) < 1e-8 || std::abs(a.y - b.y) < 1e-8)
                    Fail(Tr("У прямоугольника должна быть ширина и высота"));
                const auto start = point(a);
                const auto end = point(b);
                sketch::AddCornerRectangle(sketch, ids, start, end);
            }
        } else {
            Fail(Tr("Выберите линию, окружность или прямоугольник"));
        }
        return result.is_null() ? Json::object() : result;
    }

    Json EditFeature(const Json& args, bool create) {
        std::unique_ptr<doc::Feature> feature;
        if (create) {
            const auto type = args.at("type").get<std::string>();
            if (type == "sketch") return AddSketch(args);
            feature = doc::FeatureRegistry::Default().Create(type);
            if (!feature) Fail(Tr("Неизвестный тип операции"));
            feature->id = myDoc.Ids().Next();
            feature->name = myDoc.UniqueFeatureName(type == "extrude" ? Tr("Выдавливание") : type);
        } else {
            const auto* original = myDoc.Find(ObjectId{args.at("id").get<std::uint64_t>()});
            if (!original) Fail(Tr("Операция не найдена"));
            feature = original->Clone();
        }
        ApplyFields(*feature, args.value("fields", Json::object()));
        if (args.contains("name")) feature->name = args.at("name").get<std::string>();
        std::vector<Parameter*> parameters;
        feature->CollectParameters(parameters);
        int next = std::stoi(myDoc.NextParameterName().substr(1));
        for (auto* parameter : parameters)
            if (parameter->name.empty()) parameter->name = "d" + std::to_string(next++);
        feature->ReserveIds(myDoc.Ids());
        return Apply(std::move(feature), create);
    }
};

} // namespace
} // namespace bz::web

extern "C" const char* bz_web_request(const char* input) {
    static bz::web::Preview preview;
    static std::string output;
    using nlohmann::json;
    try {
        output = json{{"ok", true}, {"data", preview.Run(json::parse(input))}}.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (const Standard_Failure& e) {
        output = json{{"ok", false}, {"error", e.what() ? e.what() : "Open CASCADE"}}.dump(
            -1, ' ', false, json::error_handler_t::replace);
    } catch (const std::exception& e) {
        output = json{{"ok", false}, {"error", e.what()}}.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    return output.c_str();
}

#ifndef __EMSCRIPTEN__
int main() {
    for (std::string line; std::getline(std::cin, line);)
        std::cout << bz_web_request(line.c_str()) << '\n';
}
#endif
