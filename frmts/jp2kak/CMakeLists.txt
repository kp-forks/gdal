include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/helpers/GdalCMakeMinimumRequired.cmake")
cmake_minimum_required(VERSION ${GDAL_CMAKE_VERSION_MIN}...${GDAL_CMAKE_VERSION_MAX})

if(NOT DEFINED PROJECT_SOURCE_DIR)
    # Standalone plugin building
    project(gdal_JP2KAK)
    include("${PROJECT_SOURCE_DIR}/../../cmake/helpers/SetupStandalonePlugin.cmake" )
    include(CheckDependentLibrariesKakadu)
    standalone_driver_finalize(GDAL_ENABLE_DRIVER_JP2KAK)
endif()

add_gdal_driver(TARGET gdal_JP2KAK
                SOURCES jp2kakdataset.cpp
                        jp2kak_headers.h
                        jp2kakdataset.h
                        subfile_source.h
                        vsil_target.h
                CORE_SOURCES jp2kakdrivercore.cpp
                PLUGIN_CAPABLE
                NO_SHARED_SYMBOL_WITH_CORE
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)

if(TARGET gdal_JP2KAK_core)
    target_include_directories(gdal_JP2KAK_core PRIVATE $<TARGET_PROPERTY:KDU::KDU,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(gdal_JP2KAK_core PRIVATE $<TARGET_PROPERTY:KDU::KDU,INTERFACE_COMPILE_DEFINITIONS>)
    if("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "x86_64" OR "${CMAKE_SYSTEM_PROCESSOR}" MATCHES "AMD64")
        target_compile_definitions(gdal_JP2KAK_core PRIVATE -DKDU_X86_INTRINSICS)
    endif()
endif()

if(NOT TARGET gdal_JP2KAK)
    return()
endif()

gdal_standard_includes(gdal_JP2KAK)
gdal_target_link_libraries(gdal_JP2KAK PRIVATE KDU::KDU)

# this appears necessary on x64 machines to match the compilation of the library
# But for ARM we don't need to define KDU_NEON_INTRINSICS, because
# according to Compiling_Instructions.txt: "Note that this macro does not normally need to
#             be set explicitly, since it is automatically configured
#             by "kdu_arch.h" if the usual "__ARM_NEON__" is found"
# __ARM_NEON__ appears to be defined when NEON extensions available (https://developer.arm.com/documentation/dui0491/i/Compiler-specific-Features/Predefined-macros)
if("${CMAKE_SYSTEM_PROCESSOR}" MATCHES "x86_64" OR "${CMAKE_SYSTEM_PROCESSOR}" MATCHES "AMD64")
    target_compile_definitions(gdal_JP2KAK PRIVATE -DKDU_X86_INTRINSICS)
endif()
