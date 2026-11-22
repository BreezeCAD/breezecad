#include "../Fixtures.h"
#include "document/Origin.h"
#include "document/SectionFeature.h"
#include "kernel/Model.h"
#include "kernel/occt/Section.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Plane.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <doctest.h>

// The fast set: every pull request runs it (bz_tests -ts=fast, STAB-0 step 4).
TEST_SUITE_BEGIN("fast");

using namespace bz;
using namespace bz::test;
using doc::FeatureState;

TEST_CASE("section keeps one side of the plane and switches off by suppression") {
    doc::Document d;
    auto& sk = AddSketch(d);
    AddRect(d, sk, {-50, -30}, {50, 30});
    AddExtrude(d, sk.id, "40");
    auto section = std::make_unique<doc::SectionFeature>();
    section->id = d.Ids().Next();
    section->name = "Сечение1";
    section->plane = doc::origin::kPlaneYZ;   // x = 0, normal +X
    auto* f = section.get();
    d.Insert(std::move(section), d.FeatureCount());

    kernel::Model model;
    model.Rebuild(d);
    REQUIRE(f->status.state == FeatureState::Ok);
    CHECK(model.Volume() == doctest::Approx(50.0 * 60 * 40));   // the +X half is cut away

    f->flipped = true;
    d.MarkChanged(2);
    model.Rebuild(d);
    CHECK(model.Volume() == doctest::Approx(50.0 * 60 * 40));

    f->suppressed = true;
    d.MarkChanged(2);
    model.Rebuild(d);
    CHECK(model.Volume() == doctest::Approx(100.0 * 60 * 40));
}

// "Разрез вида" hatches the faces where its plane cuts the material
// (kernel::occt::CutFaces): their area is the area of the cut.
TEST_CASE("the cut faces of a view section are where the plane meets the material") {
    // A plate 80 x 50 x 10 with a through hole of 8 mm in the middle.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-40, -25, 0), 80, 50, 10).Shape();
    const TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), 4, 12).Shape();
    const TopoDS_Shape plate = BRepAlgoAPI_Cut(box, hole).Shape();
    auto area = [](const std::vector<TopoDS_Face>& faces) {
        double sum = 0;
        for (const TopoDS_Face& f : faces) {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            sum += props.Mass();
        }
        return sum;
    };
    auto frame = [](Vec3 origin, Vec3 normal) {
        Frame f;
        f.origin = origin;
        f.normal = normal;   // xDir left as it is: along X, the normal of YZ itself
        return f;
    };

    SUBCASE("across the plate: the plate's outline less the hole") {
        const auto cut = kernel::occt::CutFaces(plate, frame({0, 0, 5}, {0, 0, 1}), 0.01);
        REQUIRE(cut);
        CHECK(area(*cut) == doctest::Approx(80.0 * 50 - 3.14159265358979 * 16).epsilon(1e-3));
    }
    SUBCASE("through the hole along YZ (its frame's x along the normal): two strips") {
        const auto cut = kernel::occt::CutFaces(plate, frame({0, 0, 0}, {1, 0, 0}), 0.01);
        REQUIRE(cut);
        CHECK(area(*cut) == doctest::Approx((50.0 - 8) * 10));
        for (const TopoDS_Face& f : *cut) {   // facing the half that is cut away
            const Handle(Geom_Plane) p = Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(f));
            REQUIRE(!p.IsNull());
            gp_Dir n = p->Pln().Axis().Direction();
            if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
            CHECK(n.X() > 0.99);
        }
    }
    SUBCASE("a plane past the body: nothing") {
        const auto cut = kernel::occt::CutFaces(plate, frame({0, 0, 50}, {0, 0, 1}), 0.01);
        REQUIRE(cut);
        CHECK(cut->empty());
    }
}

TEST_SUITE_END();
