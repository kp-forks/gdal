add_gdal_driver(TARGET gdal_STACIT SOURCES stacitdataset.cpp PLUGIN_CAPABLE NO_DEPS)
gdal_standard_includes(gdal_STACIT)
target_include_directories(gdal_STACIT PRIVATE ${GDAL_RASTER_FORMAT_SOURCE_DIR}/vrt)
