#pragma once
// "Разрез вида": the view cut by an origin plane moved through the part, with a
// small bar over the viewport to choose the plane, move it and flip it.

#include "app/AppContext.h"

#include <optional>

namespace bz::app {

/// The cutting plane for the current settings (normal points to the half
/// that is cut away), or nullopt when the section is off or there is no body.
std::optional<Frame> SectionFrame(const Session& session, const ShellState::Section& section);

/// Turns the cut to whoever looks: the half nearer the eye goes away, so the
/// cut faces the viewer (as in Inventor). For turning the section on and
/// choosing another plane; "Показать другую половину" still flips it after.
void FaceSectionToViewer(ShellState::Section& section, const Vec3& viewDirection);

/// The bar at the top of the viewport while the section is on.
void DrawSectionBar(AppContext& app, Session& session, const ViewportFrame& vf);

} // namespace bz::app
