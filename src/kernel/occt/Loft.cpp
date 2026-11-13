#include "kernel/occt/Loft.h"
#include "kernel/occt/Ops.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <Standard_Failure.hxx>
#include <format>

namespace bz::kernel::occt {

Expected<TopoDS_Shape> MakeLoft(const std::vector<TopoDS_Wire>& outlines, bool ruled) {
    try {
        BRepOffsetAPI_ThruSections loft(/*isSolid=*/true, ruled);
        loft.CheckCompatibility(true);   // matches the outlines' starts, so the loft does not twist
        for (const TopoDS_Wire& w : outlines) loft.AddWire(w);
        Handle(Deadline) deadline = new Deadline(ops::kSweepSeconds);
        loft.Build(Message_ProgressRange(deadline->Start()));
        if (deadline->Passed()) return Error{"Лофт не уложился в отведённое время — упростите сечения"};
        if (!loft.IsDone()) return Error{"Лофт не строится через эти сечения"};
        const TopoDS_Shape result = loft.Shape();
        if (!BRepCheck_Analyzer(result).IsValid()) return Error{"Лофт получился некорректным"};
        return result;
    } catch (const Standard_Failure& failure) {
        return ops::FromFailure(failure, "Ошибка лофта");
    }
}

} // namespace bz::kernel::occt
