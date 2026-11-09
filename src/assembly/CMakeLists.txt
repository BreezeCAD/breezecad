# Assemblies: components, constraints, their solver and file format. No OCCT:
# the kernel (kernel/AssemblyModel) adds the geometry of the parts.
add_library(bz_assembly STATIC
    Assembly.cpp
    AssemblyFile.cpp
    Handoff.cpp
    HandoffNet.cpp
    ProjectPack.cpp
    Solver.cpp)
target_link_libraries(bz_assembly PUBLIC bz::core bz::document PRIVATE bz::json bz::miniz bz::monocypher)
bz_set_warnings(bz_assembly)
add_library(bz::assembly ALIAS bz_assembly)
if(WIN32)
    target_link_libraries(bz_assembly PRIVATE ws2_32)   # HandoffNet: sockets
endif()
