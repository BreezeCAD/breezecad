# Чертежи (docs/ADR/0012-drawings.md): документ .bzd, лист и основная надпись.
# Без OpenCASCADE — виды из модели считает kernel (шаг 3 ADR).
add_library(bz_drawing STATIC
    Drawing.cpp
    DrawingFile.cpp
    SheetLayout.cpp)
target_link_libraries(bz_drawing
    PUBLIC  bz::core bz::json
    PRIVATE bz::document)
bz_set_warnings(bz_drawing)
add_library(bz::drawing ALIAS bz_drawing)
