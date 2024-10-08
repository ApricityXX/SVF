add_subdirectory(CUDD)

# Due to a mutual dependencies, all the sub projects of the SVG are merged here
# Otherwise it is impossible to load the dependencies in opt
# NOTE: if the SVF should be linked into opt, we should probably use the individual sub projects here, rather than the combined project 

if (LLVM_LINK_LLVM_DYLIB)
    set(llvm_libs LLVM)
else()
    llvm_map_components_to_libnames(llvm_libs bitwriter core ipo irreader instcombine instrumentation target linker analysis scalaropts support transformutils)
endif()

add_compile_definitions(MEMVIEW_WITH_SVF)

file (GLOB SOURCES
        SVF-FE/*.cpp
        Graphs/*.cpp
        Util/*.cpp
        MemoryModel/*.cpp
        MSSA/*.cpp
        WPA/*.cpp
        MTA/*.cpp
        SABER/*.cpp
        DDA/*.cpp
        MemoryViewSwitcher/InsertSwitch/src/lib/*.cpp)
add_llvm_library(Svf STATIC ${SOURCES} LINK_LIBS Cudd ${Z3_LIBRARIES})
add_llvm_library(SvfShared SHARED ${SOURCES} LINK_LIBS Cudd ${Z3_LIBRARIES})


link_directories(${CMAKE_BINARY_DIR}/lib/Cudd)


if(DEFINED IN_SOURCE_BUILD)
    add_dependencies(Svf intrinsics_gen)
endif()
