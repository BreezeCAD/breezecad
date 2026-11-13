# Чертежи (docs/ADR/0012-drawings.md): документ .bzd, лист и основная надпись,
# вывод в PDF (свой маленький писатель, шрифт TrueType встраивается) и в DXF
# (через sheetmetal/DxfDrawing, как развёртка).
# Без OpenCASCADE — виды из модели считает kernel (шаг 3 ADR).
add_library(bz_drawing STATIC
    Balloon.cpp
    Dimension.cpp
    Drawing.cpp
    DrawingDxf.cpp
    DrawingFile.cpp
    DrawingPdf.cpp
    DrawingView.cpp
    Pdf.cpp
    SheetLayout.cpp
    Specification.cpp
    TrueType.cpp
    ViewPlacement.cpp)
target_link_libraries(bz_drawing
    PUBLIC  bz::core bz::json
    PRIVATE bz::document bz::miniz bz::sheetmetal)
bz_set_warnings(bz_drawing)
add_library(bz::drawing ALIAS bz_drawing)
