add_gdal_driver(
  TARGET gdal_MSGN
  SOURCES msg_basic_types.cpp
          msg_basic_types.h
          msg_reader_core.cpp
          msg_reader_core.h
          msgndataset.cpp
  PLUGIN_CAPABLE
  NO_DEPS
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(gdal_MSGN)
target_compile_definitions(gdal_MSGN PRIVATE -DGDAL_SUPPORT)
