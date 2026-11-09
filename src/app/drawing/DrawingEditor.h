#pragma once
// The drawing editor (ADR-0012, step 2b): the sheet with its frame and
// title block, the drawing's tree and its ribbon — «Виды», «Обозначения»,
// «Оформление», laid out as in КОМПАС-3D inside BreezeCAD's shell. Views and
// dimensions come with the next steps; their buttons are already in place.

#include "app/AppContext.h"

#include <imgui.h>

#include <string>

namespace bz::app {

class RibbonSink;
class Session;
class DrawingState;

/// «Создать → Чертёж»: an empty drawing, one A3 sheet.
void NewDrawing(AppContext& app);
/// «Создать чертёж» of the open part or assembly (saved: the drawing keeps
/// its path): the title block takes its name, designation and material.
void NewDrawingFromModel(AppContext& app);
bool CanDrawingFromModel(AppContext& app);

/// The drawing's commands (drawing.*), with the others in RegisterActions.
void RegisterDrawingActions(AppContext& app);

/// The ribbon of a drawing: «Виды» (RibbonTab::Model), «Обозначения»
/// (Sketch), «Оформление» (View).
void FillDrawingRibbon(AppContext& app, RibbonSink& sink);

/// Left: the drawing's tree (sheets, then what is on the sheet shown).
void DrawDrawingTree(AppContext& app, Session& session, ImVec2 pos, ImVec2 size);
/// The sheet: wheel — zoom, drag — move, double click on the title block — fill it in.
void DrawDrawingSheet(AppContext& app, Session& session, ImVec2 pos, ImVec2 size);
/// «Лист 2 из 3, А3» — for the status bar.
std::string DrawingSheetCaption(const DrawingState& drawing);
/// The whole sheet in view (on the next frame).
void FitDrawingSheet(Session& session);

/// The «Лист и основная надпись» window (with the other dialogs).
void DrawDrawingDialogs(AppContext& app);

} // namespace bz::app
