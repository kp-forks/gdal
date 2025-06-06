#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test OZI projection and datum support
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2012, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################


import gdaltest

from osgeo import osr

###############################################################################
# Test with WGS 84 datum


def test_osr_ozi_1():

    srs = osr.SpatialReference()
    srs.ImportFromOzi(
        [
            "OziExplorer Map Data File Version 2.2",
            "Test_Map",
            "Test_Map.png",
            "1 ,Map Code,",
            "WGS 84,WGS 84,   0.0000,   0.0000,WGS 84",
            "Map Projection,Lambert Conformal Conic,PolyCal,No,AutoCalOnly,No,BSBUseWPX,No",
            "Projection Setup,     4.000000000,    10.000000000,,,,    40.000000000,    56.000000000,,,",
        ]
    )

    expected = 'PROJCS["unnamed",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]],PROJECTION["Lambert_Conformal_Conic_2SP"],PARAMETER["standard_parallel_1",40],PARAMETER["standard_parallel_2",56],PARAMETER["latitude_of_origin",4],PARAMETER["central_meridian",10],PARAMETER["false_easting",0],PARAMETER["false_northing",0],UNIT["Meter",1]]'

    assert gdaltest.equal_srs_from_wkt(expected, srs.ExportToWkt())


###############################################################################
# Test with another datum known by OZI and whose EPSG code is known


def test_osr_ozi_2():

    srs = osr.SpatialReference()
    srs.ImportFromOzi(
        [
            "OziExplorer Map Data File Version 2.2",
            "Test_Map",
            "Test_Map.png",
            "1 ,Map Code,",
            "Tokyo,",
            "Map Projection,Latitude/Longitude,,,,,,",
            "Projection Setup,,,,,,,,,,",
        ]
    )

    srs_ref = osr.SpatialReference()
    srs_ref.ImportFromEPSG(4301)

    assert gdaltest.equal_srs_from_wkt(srs_ref.ExportToWkt(), srs.ExportToWkt())


###############################################################################
# Test with another datum known by OZI and whose EPSG code is unknown


def test_osr_ozi_3():

    srs = osr.SpatialReference()
    srs.SetAxisMappingStrategy(osr.OAMS_AUTHORITY_COMPLIANT)
    srs.ImportFromOzi(
        [
            "OziExplorer Map Data File Version 2.2",
            "Test_Map",
            "Test_Map.png",
            "1 ,Map Code,",
            "European 1950 (Mean France),",
            "Map Projection,Latitude/Longitude,,,,,,",
            "Projection Setup,,,,,,,,,,",
        ]
    )

    expected = 'GEOGCS["European 1950 (Mean France)",DATUM["European 1950 (Mean France)",SPHEROID["International 1924",6378388,297],TOWGS84[-87,-96,-120,0,0,0,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]]'

    assert gdaltest.equal_srs_from_wkt(expected, srs.ExportToWkt())
