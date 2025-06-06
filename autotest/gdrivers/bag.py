#!/usr/bin/env pytest
# -*- coding: utf-8 -*-
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test read functionality for BAG driver.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
###############################################################################
# Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
#                     Frank Warmerdam <warmerdam@pobox.com>
#
# SPDX-License-Identifier: MIT
###############################################################################


import os
import struct
from xml.etree import ElementTree

import gdaltest
import pytest

from osgeo import gdal, ogr

pytestmark = pytest.mark.require_driver("BAG")


###############################################################################
@pytest.fixture(autouse=True, scope="module")
def module_disable_exceptions():
    with gdaltest.disable_exceptions():
        yield


@pytest.fixture(autouse=True, scope="module")
def check_no_file_leaks():
    num_files = len(gdaltest.get_opened_files())

    yield

    diff = len(gdaltest.get_opened_files()) - num_files
    if diff != 0 and "CI" in os.environ:
        print("Leak of file handles: %d leaked. Flaky on CI, so ignored" % diff)
    else:
        assert diff == 0, "Leak of file handles: %d leaked" % diff


###############################################################################
# Confirm various info on true_n_nominal 1.1 sample file.


def test_bag_2():

    ds = gdal.Open("data/bag/true_n_nominal.bag")

    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 1072, "Wrong checksum on band 1, got %d." % cs

    cs = ds.GetRasterBand(2).Checksum()
    assert cs == 150, "Wrong checksum on band 2, got %d." % cs

    cs = ds.GetRasterBand(3).Checksum()
    assert cs == 1315, "Wrong checksum on band 3, got %d." % cs

    b1 = ds.GetRasterBand(1)
    assert b1.GetMinimum() == pytest.approx(10, abs=0.01), "band 1 minimum wrong."

    assert b1.GetMaximum() == pytest.approx(19.8, abs=0.01), "band 1 maximum wrong."

    assert b1.GetNoDataValue() == pytest.approx(
        1000000.0, abs=0.1
    ), "band 1 nodata wrong."

    b2 = ds.GetRasterBand(2)
    assert b2.GetNoDataValue() == pytest.approx(
        1000000.0, abs=0.1
    ), "band 2 nodata wrong."

    b3 = ds.GetRasterBand(3)
    assert b3.GetNoDataValue() == pytest.approx(0.0, abs=0.1), "band 3 nodata wrong."

    # It would be nice to test srs and geotransform but they are
    # pretty much worthless on this dataset.

    # Test the xml:BAG metadata domain
    xmlBag = ds.GetMetadata("xml:BAG")[0]
    assert xmlBag.startswith("<?xml"), "did not get xml:BAG metadata"

    ds = None

    assert not gdaltest.is_file_open("data/true_n_nominal.bag"), "file still opened."


###############################################################################
# Test a southern hemisphere falseNorthing sample file.


def test_bag_3():

    ds = gdal.Open("data/bag/southern_hemi_false_northing.bag")

    nr = ds.RasterCount
    assert nr == 2, "Expected 2 bands, got %d." % nr

    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 21402, "Wrong checksum on band 1, got %d." % cs

    cs = ds.GetRasterBand(2).Checksum()
    assert cs == 33216, "Wrong checksum on band 2, got %d." % cs

    pj = ds.GetProjection()
    assert (
        'PARAMETER["central_meridian",-105]' in pj
    ), 'PARAMETER["central_meridian",-105] not in projection'
    assert (
        'PARAMETER["false_northing",10000000]' in pj
    ), "Did not find false_northing of 10000000"


###############################################################################
#


def test_bag_read_resolution():

    # BAG version 1.1
    ds = gdal.Open("data/bag/true_n_nominal.bag")
    gt = ds.GetGeoTransform()
    # UpperLeft corner, resX, resY
    got = (gt[0], gt[3], gt[1], gt[5])
    assert got == (12344.12345678, 22142.12345678, 2.0, -2.0)

    # BAG version 1.4
    ds = gdal.Open("data/bag/southern_hemi_false_northing.bag")
    gt = ds.GetGeoTransform()
    got = (gt[0], gt[3], gt[1], gt[5])
    assert got == (615037.5, 9559387.5, 75.0, -75.0)

    # BAG version 1.6
    ds = gdal.Open("data/bag/test_offset_ne_corner.bag")
    gt = ds.GetGeoTransform()
    got = (gt[0], gt[3], gt[1], gt[5])
    assert got == (85.0, 500112.0, 30.0, -32.0)


###############################################################################
#


def test_bag_vr_normal():

    ds = gdal.Open("data/bag/test_vr.bag")
    assert ds is not None

    got_md = gdal.Info(ds, computeChecksum=True, format="json", wktFormat="WKT1")
    expected_md = {
        "bands": [
            {
                "band": 1,
                "block": [6, 1],
                "checksum": 65529,
                "colorInterpretation": "Undefined",
                "description": "elevation",
                "max": 10.0,
                "metadata": {},
                "min": -10.0,
                "noDataValue": 1000000.0,
                "type": "Float32",
            },
            {
                "band": 2,
                "block": [6, 1],
                "checksum": 60,
                "colorInterpretation": "Undefined",
                "description": "uncertainty",
                "max": 5.0,
                "metadata": {},
                "min": 0.0,
                "noDataValue": 1000000.0,
                "type": "Float32",
            },
        ],
        "coordinateSystem": {
            "dataAxisToSRSAxisMapping": [1, 2, 3],
            "wkt": 'COMPD_CS["NAD83 / UTM zone 10N + MLLW depth",\n    PROJCS["NAD83 / UTM zone 10N",\n        GEOGCS["NAD83",\n            DATUM["North_American_Datum_1983",\n                SPHEROID["GRS 1980",6378137,298.257222101004,\n                    AUTHORITY["EPSG","7019"]],\n                TOWGS84[0,0,0,0,0,0,0],\n                AUTHORITY["EPSG","6269"]],\n            PRIMEM["Greenwich",0,\n                AUTHORITY["EPSG","8901"]],\n            UNIT["degree",0.0174532925199433,\n                AUTHORITY["EPSG","9122"]],\n            AUTHORITY["EPSG","4269"]],\n        PROJECTION["Transverse_Mercator"],\n        PARAMETER["latitude_of_origin",0],\n        PARAMETER["central_meridian",-123],\n        PARAMETER["scale_factor",0.9996],\n        PARAMETER["false_easting",500000],\n        PARAMETER["false_northing",0],\n        UNIT["metre",1,\n            AUTHORITY["EPSG","9001"]],\n        AXIS["Easting",EAST],\n        AXIS["Northing",NORTH],\n        AUTHORITY["EPSG","26910"]],\n    VERT_CS["MLLW depth",\n        VERT_DATUM["Mean Lower Low Water",2005,\n            AUTHORITY["EPSG","1089"]],\n        UNIT["metre",1,\n            AUTHORITY["EPSG","9001"]],\n        AXIS["Depth",DOWN],\n        AUTHORITY["EPSG","5866"]]]',
        },
        "geoTransform": [85.0, 30.0, 0.0, 500112.0, 0.0, -32.0],
        "metadata": {
            "": {
                "AREA_OR_POINT": "Point",
                "BAG_DATETIME": "2018-08-08T12:34:56",
                "BagVersion": "1.6.2",
                "HAS_SUPERGRIDS": "TRUE",
                "MAX_RESOLUTION_X": "29.900000",
                "MAX_RESOLUTION_Y": "31.900000",
                "MIN_RESOLUTION_X": "4.983333",
                "MIN_RESOLUTION_Y": "5.316667",
            }
        },
        "size": [6, 4],
    }

    for key in expected_md:
        if key not in got_md or got_md[key] != expected_md[key]:
            import pprint

            pp = pprint.PrettyPrinter()
            pp.pprint(got_md)
            pytest.fail(key)

    with gdal.quiet_errors():
        ds = gdal.OpenEx(
            "data/bag/test_vr.bag", open_options=["MODE=LOW_RES_GRID", "MINX=0"]
        )
    assert gdal.GetLastErrorMsg() != "", "warning expected"

    got_md2 = gdal.Info(ds, computeChecksum=True, format="json", wktFormat="WKT1")
    assert got_md2 == got_md


###############################################################################
#


def test_bag_vr_list_supergrids():

    ds = gdal.OpenEx("data/bag/test_vr.bag", open_options=["MODE=LIST_SUPERGRIDS"])
    assert ds is not None
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 24

    assert sub_ds[0][0] == 'BAG:"data/bag/test_vr.bag":supergrid:0:0'

    with gdal.quiet_errors():
        # Bounding box filter ignored since only part of MINX, MINY, MAXX and
        # MAXY has been specified
        ds = gdal.OpenEx(
            "data/bag/test_vr.bag", open_options=["MODE=LIST_SUPERGRIDS", "MINX=200"]
        )
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 24

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=[
            "MODE=LIST_SUPERGRIDS",
            "MINX=100",
            "MAXX=220",
            "MINY=500000",
            "MAXY=500100",
        ],
    )
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 6
    assert sub_ds[0][0] == 'BAG:"data/bag/test_vr.bag":supergrid:1:1'

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=LIST_SUPERGRIDS", "RES_FILTER_MIN=5", "RES_FILTER_MAX=10"],
    )
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 12
    assert sub_ds[0][0] == 'BAG:"data/bag/test_vr.bag":supergrid:0:3'

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag", open_options=["SUPERGRIDS_INDICES=(2,1),(3,4)"]
    )
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 2
    assert (
        sub_ds[0][0] == 'BAG:"data/bag/test_vr.bag":supergrid:2:1'
        and sub_ds[1][0] == 'BAG:"data/bag/test_vr.bag":supergrid:3:4'
    )

    # One single tuple: open the subdataset directly
    ds = gdal.OpenEx("data/bag/test_vr.bag", open_options=["SUPERGRIDS_INDICES=(2,1)"])
    ds2 = gdal.Open('BAG:"data/bag/test_vr.bag":supergrid:2:1')
    assert gdal.Info(ds) == gdal.Info(ds2), sub_ds

    # Test invalid values for SUPERGRIDS_INDICES
    for invalid_val in ["", "x", "(", "(1", "(1,", "(1,)", "(1,2),", "(x,2)", "(2,x)"]:
        with gdal.quiet_errors():
            ds = gdal.OpenEx(
                "data/bag/test_vr.bag",
                open_options=["SUPERGRIDS_INDICES=" + invalid_val],
            )
        assert gdal.GetLastErrorMsg() != "", invalid_val


###############################################################################
#


def test_bag_vr_open_supergrids():

    ds = gdal.OpenEx(
        'BAG:"data/bag/test_vr.bag":supergrid:0:0', open_options=["REPORT_VERTCRS=NO"]
    )
    assert ds is not None

    got_md = gdal.Info(ds, computeChecksum=True, format="json", wktFormat="WKT1")
    expected_md = {
        "bands": [
            {
                "band": 1,
                "block": [2, 1],
                "checksum": 65529,
                "colorInterpretation": "Undefined",
                "description": "elevation",
                "metadata": {},
                "noDataValue": 1000000.0,
                "type": "Float32",
            },
            {
                "band": 2,
                "block": [2, 1],
                "checksum": 13,
                "colorInterpretation": "Undefined",
                "description": "uncertainty",
                "metadata": {},
                "noDataValue": 1000000.0,
                "type": "Float32",
            },
        ],
        "coordinateSystem": {
            "dataAxisToSRSAxisMapping": [1, 2],
            "wkt": 'PROJCS["NAD83 / UTM zone 10N",\n    GEOGCS["NAD83",\n        DATUM["North_American_Datum_1983",\n            SPHEROID["GRS 1980",6378137,298.257222101004,\n                AUTHORITY["EPSG","7019"]],\n            TOWGS84[0,0,0,0,0,0,0],\n            AUTHORITY["EPSG","6269"]],\n        PRIMEM["Greenwich",0,\n            AUTHORITY["EPSG","8901"]],\n        UNIT["degree",0.0174532925199433,\n            AUTHORITY["EPSG","9122"]],\n        AUTHORITY["EPSG","4269"]],\n    PROJECTION["Transverse_Mercator"],\n    PARAMETER["latitude_of_origin",0],\n    PARAMETER["central_meridian",-123],\n    PARAMETER["scale_factor",0.9996],\n    PARAMETER["false_easting",500000],\n    PARAMETER["false_northing",0],\n    UNIT["metre",1,\n        AUTHORITY["EPSG","9001"]],\n    AXIS["Easting",EAST],\n    AXIS["Northing",NORTH],\n    AUTHORITY["EPSG","26910"]]',
        },
        "geoTransform": [
            70.10000038146973,
            29.899999618530273,
            0.0,
            500031.89999961853,
            0.0,
            -31.899999618530273,
        ],
        "metadata": {
            "": {
                "AREA_OR_POINT": "Point",
                "BAG_DATETIME": "2018-08-08T12:34:56",
                "BagVersion": "1.6.2",
            },
            "IMAGE_STRUCTURE": {"INTERLEAVE": "PIXEL"},
        },
        "size": [2, 2],
    }

    for key in expected_md:
        if key not in got_md or got_md[key] != expected_md[key]:
            import pprint

            pp = pprint.PrettyPrinter()
            pp.pprint(got_md)
            pytest.fail(key)

    with gdal.quiet_errors():
        ds = gdal.Open('BAG:"/vsimem/unexisting.bag":supergrid:0:0')
    assert ds is None

    with gdal.quiet_errors():
        ds = gdal.Open('BAG:"data/bag/test_vr.bag":supergrid:4:0')
    assert ds is None

    with gdal.quiet_errors():
        ds = gdal.Open('BAG:"data/bag/test_vr.bag":supergrid:0:6')
    assert ds is None

    with gdal.quiet_errors():
        ds = gdal.OpenEx(
            'BAG:"data/bag/test_vr.bag":supergrid:0:0', open_options=["MINX=0"]
        )
    assert gdal.GetLastErrorMsg() != "", "warning expected"


###############################################################################
#


def test_bag_vr_resampled():

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "REPORT_VERTCRS=NO"],
    )
    assert ds is not None

    got_md = gdal.Info(ds, computeChecksum=True, format="json", wktFormat="WKT1")
    expected_md = {
        "bands": [
            {
                "band": 1,
                "block": [36, 24],
                "checksum": 4555,
                "colorInterpretation": "Undefined",
                "description": "elevation",
                "max": 10.0,
                "metadata": {},
                "min": -10.0,
                "noDataValue": 1000000.0,
                "overviews": [
                    {"checksum": 681, "size": [18, 12]},
                    {"checksum": 340, "size": [9, 6]},
                    {"checksum": 65529, "size": [6, 4]},
                ],
                "type": "Float32",
            },
            {
                "band": 2,
                "block": [36, 24],
                "checksum": 6234,
                "colorInterpretation": "Undefined",
                "description": "uncertainty",
                "max": 10.0,
                "metadata": {},
                "min": 0.0,
                "noDataValue": 1000000.0,
                "overviews": [
                    {"checksum": 1344, "size": [18, 12]},
                    {"checksum": 420, "size": [9, 6]},
                    {"checksum": 60, "size": [6, 4]},
                ],
                "type": "Float32",
            },
        ],
        "coordinateSystem": {
            "dataAxisToSRSAxisMapping": [1, 2],
            "wkt": 'PROJCS["NAD83 / UTM zone 10N",\n    GEOGCS["NAD83",\n        DATUM["North_American_Datum_1983",\n            SPHEROID["GRS 1980",6378137,298.257222101004,\n                AUTHORITY["EPSG","7019"]],\n            TOWGS84[0,0,0,0,0,0,0],\n            AUTHORITY["EPSG","6269"]],\n        PRIMEM["Greenwich",0,\n            AUTHORITY["EPSG","8901"]],\n        UNIT["degree",0.0174532925199433,\n            AUTHORITY["EPSG","9122"]],\n        AUTHORITY["EPSG","4269"]],\n    PROJECTION["Transverse_Mercator"],\n    PARAMETER["latitude_of_origin",0],\n    PARAMETER["central_meridian",-123],\n    PARAMETER["scale_factor",0.9996],\n    PARAMETER["false_easting",500000],\n    PARAMETER["false_northing",0],\n    UNIT["metre",1,\n        AUTHORITY["EPSG","9001"]],\n    AXIS["Easting",EAST],\n    AXIS["Northing",NORTH],\n    AUTHORITY["EPSG","26910"]]',
        },
        "geoTransform": [
            85.0,
            4.983333110809326,
            0.0,
            500111.5999984741,
            0.0,
            -5.316666603088379,
        ],
        "metadata": {
            "": {
                "AREA_OR_POINT": "Point",
                "BAG_DATETIME": "2018-08-08T12:34:56",
                "BagVersion": "1.6.2",
            },
            "IMAGE_STRUCTURE": {"INTERLEAVE": "PIXEL"},
        },
        "size": [36, 24],
    }

    for key in expected_md:
        if key not in got_md or got_md[key] != expected_md[key]:
            import pprint

            pp = pprint.PrettyPrinter()
            pp.pprint(got_md)
            pytest.fail(key)

    data_ref = ds.ReadRaster()

    # Test that block size has no influence on the result
    for block_size in (2, 5, 9, 16):
        with gdaltest.config_option("GDAL_BAG_BLOCK_SIZE", str(block_size)):
            ds = gdal.OpenEx(
                "data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID"]
            )
            assert ds.GetRasterBand(1).GetBlockSize() == [block_size, block_size]
            data = ds.ReadRaster()

        assert data == data_ref, block_size

    # Test overviews
    with gdaltest.config_option("GDAL_BAG_MIN_OVR_SIZE", "4"):
        ds = gdal.OpenEx("data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID"])
    assert ds.GetRasterBand(1).GetOverviewCount() == 3
    assert ds.GetRasterBand(1).GetOverview(-1) is None
    assert ds.GetRasterBand(1).GetOverview(3) is None

    ovr = ds.GetRasterBand(1).GetOverview(0)
    cs = ovr.Checksum()
    assert cs == 681
    ovr = ds.GetRasterBand(2).GetOverview(0)
    cs = ovr.Checksum()
    assert cs == 1344
    ds.GetRasterBand(1).GetOverview(0).FlushCache()
    ds.GetRasterBand(1).GetOverview(1).FlushCache()
    cs = ovr.Checksum()
    assert cs == 1344
    ovr = ds.GetRasterBand(1).GetOverview(0)
    cs = ovr.Checksum()
    assert cs == 681

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "MINX=90", "MAXX=120", "MAXY=500112"],
    )
    gt = ds.GetGeoTransform()
    got = (gt[0], gt[3], ds.RasterXSize)
    assert got == (90.0, 500112.0, 6)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID", "MINY=500000"]
    )
    gt = ds.GetGeoTransform()
    got = (gt[3] + gt[5] * ds.RasterYSize, ds.RasterYSize)
    assert got == (500000.0, 21)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID", "RESX=5", "RESY=6"]
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (5.0, -6.0)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID", "RES_STRATEGY=MIN"]
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (4.983333110809326, -5.316666603088379)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID", "RES_STRATEGY=MAX"]
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (29.899999618530273, -31.899999618530273)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "RES_STRATEGY=MEAN"],
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (12.209166447321573, -13.025833209355673)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MIN=0", "RES_FILTER_MAX=8"],
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (8.0, -8.0)
    got = (ds.GetRasterBand(1).Checksum(), ds.GetRasterBand(2).Checksum())
    assert got == (2021, 2722)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag", open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MAX=8"]
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (8.0, -8.0)
    got = (ds.GetRasterBand(1).Checksum(), ds.GetRasterBand(2).Checksum())
    assert got == (2021, 2722)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MIN=8", "RES_FILTER_MAX=16"],
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (16.0, -16.0)
    got = (ds.GetRasterBand(1).Checksum(), ds.GetRasterBand(2).Checksum())
    assert got == (728, 848)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MIN=16", "RES_FILTER_MAX=32"],
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (32.0, -32.0)
    got = (ds.GetRasterBand(1).Checksum(), ds.GetRasterBand(2).Checksum())
    assert got == (207, 207)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MIN=16"],
    )
    gt = ds.GetGeoTransform()
    got = (gt[1], gt[5])
    assert got == (29.899999618530273, -31.899999618530273)
    got = (ds.GetRasterBand(1).Checksum(), ds.GetRasterBand(2).Checksum())
    assert got == (165, 205)

    # Too big RES_FILTER_MIN
    with gdal.quiet_errors():
        ds = gdal.OpenEx(
            "data/bag/test_vr.bag",
            open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MIN=32"],
        )
    assert ds is None

    # Too small RES_FILTER_MAX
    with gdal.quiet_errors():
        ds = gdal.OpenEx(
            "data/bag/test_vr.bag",
            open_options=["MODE=RESAMPLED_GRID", "RES_FILTER_MAX=4"],
        )
    assert ds is None

    # RES_FILTER_MIN >= RES_FILTER_MAX
    with gdal.quiet_errors():
        ds = gdal.OpenEx(
            "data/bag/test_vr.bag",
            open_options=[
                "MODE=RESAMPLED_GRID",
                "RES_FILTER_MIN=4",
                "RES_FILTER_MAX=4",
            ],
        )
    assert ds is None

    # Test VALUE_POPULATION
    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=[
            "MODE=RESAMPLED_GRID",
            "RES_STRATEGY=MEAN",
            "VALUE_POPULATION=MAX",
        ],
    )
    m1_max, M1_max, mean1_max, _ = ds.GetRasterBand(1).ComputeStatistics(False)
    m2_max, M2_max, mean2_max, _ = ds.GetRasterBand(2).ComputeStatistics(False)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=[
            "MODE=RESAMPLED_GRID",
            "RES_STRATEGY=MEAN",
            "VALUE_POPULATION=MEAN",
        ],
    )
    m1_mean, M1_mean, mean1_mean, _ = ds.GetRasterBand(1).ComputeStatistics(False)
    m2_mean, M2_mean, mean2_mean, _ = ds.GetRasterBand(2).ComputeStatistics(False)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=[
            "MODE=RESAMPLED_GRID",
            "RES_STRATEGY=MEAN",
            "VALUE_POPULATION=MIN",
        ],
    )
    m1_min, M1_min, mean1_min, _ = ds.GetRasterBand(1).ComputeStatistics(False)
    m2_min, M2_min, mean2_min, _ = ds.GetRasterBand(2).ComputeStatistics(False)

    if mean1_min >= mean1_mean or mean1_mean >= mean1_max:
        print(m1_max, M1_max, mean1_max)
        print(m2_max, M2_max, mean2_max)
        print(m1_mean, M1_mean, mean1_mean)
        print(m2_mean, M2_mean, mean2_mean)
        print(m1_min, M1_min, mean1_min)
        pytest.fail(m2_min, M2_min, mean2_min)

    if m2_min >= m2_max or (m2_mean, M2_mean, mean2_mean) != (
        m2_max,
        M2_max,
        mean2_max,
    ):
        print(m1_max, M1_max, mean1_max)
        print(m1_mean, M1_mean, mean1_mean)
        print(m1_min, M1_min, mean1_min)
        pytest.fail(m2_min, M2_min, mean2_min)

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=[
            "MODE=RESAMPLED_GRID",
            "RES_STRATEGY=MEAN",
            "VALUE_POPULATION=COUNT",
        ],
    )
    assert ds is not None
    assert ds.RasterCount == 1
    assert ds.GetRasterBand(1).DataType == gdal.GDT_UInt32
    assert ds.GetRasterBand(1).Checksum() == 549


###############################################################################
#


def test_bag_vr_resampled_mask():

    ds = gdal.OpenEx(
        "data/bag/test_vr.bag",
        open_options=["MODE=RESAMPLED_GRID", "SUPERGRIDS_MASK=YES"],
    )
    assert ds is not None
    assert ds.RasterCount == 1
    assert ds.GetRasterBand(1).DataType == gdal.GDT_Byte
    assert ds.GetRasterBand(1).GetNoDataValue() is None
    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 4552


###############################################################################
#


def test_bag_vr_interpolated():

    ds = gdal.OpenEx(
        "data/bag/test_interpolated.bag",
        open_options=["MODE=INTERPOLATED", "REPORT_VERTCRS=NO"],
    )

    got_md = gdal.Info(ds, computeChecksum=True, format="json", wktFormat="WKT1")
    expected_md = {
        "bands": [
            {
                "band": 1,
                "block": [180, 128],
                # "checksum": 9896,
                "colorInterpretation": "Undefined",
                "description": "elevation",
                "metadata": {},
                "noDataValue": 1000000.0,
                "overviews": [
                    {"checksum": 51693, "size": [90, 64]},
                    {"checksum": 12949, "size": [45, 32]},
                    {"checksum": 3168, "size": [22, 16]},
                    {"checksum": 792, "size": [11, 8]},
                    {"checksum": 165, "size": [6, 4]},
                ],
                "type": "Float32",
            },
            {
                "band": 2,
                "block": [180, 128],
                # "checksum": 9896,
                "colorInterpretation": "Undefined",
                "description": "uncertainty",
                "metadata": {},
                "noDataValue": 1000000.0,
                "overviews": [
                    {"checksum": 51693, "size": [90, 64]},
                    {"checksum": 12949, "size": [45, 32]},
                    {"checksum": 3168, "size": [22, 16]},
                    {"checksum": 792, "size": [11, 8]},
                    {"checksum": 0, "size": [6, 4]},
                ],
                "type": "Float32",
            },
        ],
        "coordinateSystem": {
            "dataAxisToSRSAxisMapping": [1, 2],
            "wkt": 'PROJCS["NAD83 / UTM zone 10N",\n    GEOGCS["NAD83",\n        DATUM["North_American_Datum_1983",\n            SPHEROID["GRS 1980",6378137,298.257222101004,\n                AUTHORITY["EPSG","7019"]],\n            TOWGS84[0,0,0,0,0,0,0],\n            AUTHORITY["EPSG","6269"]],\n        PRIMEM["Greenwich",0,\n            AUTHORITY["EPSG","8901"]],\n        UNIT["degree",0.0174532925199433,\n            AUTHORITY["EPSG","9122"]],\n        AUTHORITY["EPSG","4269"]],\n    PROJECTION["Transverse_Mercator"],\n    PARAMETER["latitude_of_origin",0],\n    PARAMETER["central_meridian",-123],\n    PARAMETER["scale_factor",0.9996],\n    PARAMETER["false_easting",500000],\n    PARAMETER["false_northing",0],\n    UNIT["metre",1,\n        AUTHORITY["EPSG","9001"]],\n    AXIS["Easting",EAST],\n    AXIS["Northing",NORTH],\n    AUTHORITY["EPSG","26910"]]',
        },
        "cornerCoordinates": {
            "center": [175.0, 500048.0],
            "lowerLeft": [85.0, 499984.0],
            "lowerRight": [265.0, 499984.0],
            "upperLeft": [85.0, 500112.0],
            "upperRight": [265.0, 500112.0],
        },
        "geoTransform": [85.0, 1.0, 0.0, 500112.0, 0.0, -1.0],
        "metadata": {
            "": {
                "AREA_OR_POINT": "Point",
                "BAG_DATETIME": "2018-08-08T12:34:56",
                "BagVersion": "2.0.0",
            },
            "IMAGE_STRUCTURE": {"INTERLEAVE": "PIXEL"},
        },
        "size": [180, 128],
    }

    del got_md["bands"][0]["checksum"]
    del got_md["bands"][1]["checksum"]

    for key in expected_md:
        if key not in got_md or got_md[key] != expected_md[key]:
            import pprint

            pp = pprint.PrettyPrinter()
            pp.pprint(got_md)
            pytest.fail(key)

    assert ds.GetRasterBand(1).Checksum() in (9896, 9899)


###############################################################################
#


def test_bag_write_single_band():

    tst = gdaltest.GDALTest("BAG", "byte.tif", 1, 4672)
    tst.testCreateCopy(quiet_error_handler=False, new_filename="/vsimem/out.bag")


###############################################################################
#


def test_bag_write_two_bands():

    tst = gdaltest.GDALTest(
        "BAG",
        "bag/test_vr.bag",
        2,
        60,
        options=[
            "BLOCK_SIZE=2",
            "VAR_ABSTRACT=foo",
            "VAR_XML_IDENTIFICATION_CITATION=<bar/>",
        ],
    )
    tst.testCreateCopy(
        quiet_error_handler=False, delete_copy=False, new_filename="/vsimem/out.bag"
    )

    ds = gdal.Open("/vsimem/out.bag")
    xml = ds.GetMetadata_List("xml:BAG")[0]
    assert "<bar />" in xml
    assert "Generated by GDAL " in xml
    assert 'VERT_CS["MLLW depth"' in xml

    gdal.Unlink("/vsimem/out.bag")


###############################################################################
#


def test_bag_write_south_up():

    # Generate a south-up dataset
    src_ds = gdal.Warp(
        "",
        "data/byte.tif",
        format="MEM",
        outputBounds=[440720, 3751320, 441920, 3750120],
    )

    # Translate it into BAG
    ds = gdal.Translate("/vsimem/out.bag", src_ds)

    # Check that it is presented as north-up
    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 4672

    gt = ds.GetGeoTransform()
    assert gt == (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0)

    ds = None

    gdal.Unlink("/vsimem/out.bag")


###############################################################################
#


def test_bag_read_invalid_bag_vlen_bag_version():

    os.stat("data/bag/invalid_bag_vlen_bag_version.bag")
    ds = gdal.Open("data/bag/invalid_bag_vlen_bag_version.bag")
    assert not ds


def test_bag_read_incorrect_northeast_corner():

    ds = gdal.Open("data/bag/test_offset_ne_corner.bag")

    geotransform = ds.GetGeoTransform()
    assert geotransform == (85.0, 30.0, 0.0, 500112.0, 0.0, -32.0)

    corner_points = ElementTree.fromstring(ds.GetMetadata("xml:BAG")[0])[8][0][6][0][
        0
    ].text
    assert (
        corner_points
        == "100.000000000000,500000.000000000000 250.000000000000,500096.000000000000"
    )

    del ds


###############################################################################
# Test reading georeferenced metadata


def test_bag_read_georef_metadata():

    ds = gdal.Open("data/bag/test_georef_metadata.bag")
    assert ds is not None
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 3

    assert sub_ds[0][0] == 'BAG:"data/bag/test_georef_metadata.bag":bathymetry_coverage'
    assert (
        sub_ds[1][0]
        == 'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_keys_values'
    )
    assert (
        sub_ds[2][0]
        == 'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_values_only'
    )

    ds = gdal.Open('BAG:"data/bag/test_georef_metadata.bag":bathymetry_coverage')
    assert ds is not None
    assert len(ds.GetSubDatasets()) == 0
    assert ds.RasterXSize == 6
    assert ds.RasterYSize == 4
    assert ds.RasterCount == 2

    ds = gdal.OpenEx(
        "data/bag/test_georef_metadata.bag", open_options=["MODE=LIST_SUPERGRIDS"]
    )
    assert ds is not None
    sub_ds = ds.GetSubDatasets()
    assert len(sub_ds) == 74
    assert (
        sub_ds[1][0]
        == 'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_keys_values:0:0'
    )

    with gdal.quiet_errors():
        assert (
            gdal.Open(
                'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:not_existing'
            )
            is None
        )

    ds = gdal.Open(
        'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_keys_values'
    )
    assert ds is not None
    assert len(ds.GetSubDatasets()) == 0
    assert ds.RasterXSize == 6
    assert ds.RasterYSize == 4
    assert ds.RasterCount == 1
    band = ds.GetRasterBand(1)
    data = band.ReadRaster()
    data = struct.unpack("i" * 24, data)
    assert data == (
        0,
        1,
        2,
        3,
        4,
        5,
        0,
        1,
        2,
        3,
        4,
        5,
        0,
        1,
        2,
        3,
        4,
        5,
        0,
        1,
        2,
        3,
        4,
        5,
    )
    assert band.GetNoDataValue() == 0
    rat = band.GetDefaultRAT()
    assert rat is not None
    assert rat.GetRowCount() == 6
    assert rat.GetColumnCount() == 3
    assert rat.GetNameOfCol(0) == "int"
    assert rat.GetTypeOfCol(0) == gdal.GFT_Integer
    assert rat.GetNameOfCol(1) == "str"
    assert rat.GetTypeOfCol(1) == gdal.GFT_String
    assert rat.GetNameOfCol(2) == "float64"
    assert rat.GetTypeOfCol(2) == gdal.GFT_Real
    assert rat.GetValueAsInt(0, 0) == 0
    assert rat.GetValueAsString(0, 1) == "Val   "
    assert rat.GetValueAsDouble(0, 2) == 1.25
    assert rat.GetValueAsInt(1, 0) == 1

    ds = gdal.Open(
        'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_values_only'
    )
    assert ds is not None
    assert ds.RasterXSize == 6
    assert ds.RasterYSize == 4
    assert ds.RasterCount == 1
    band = ds.GetRasterBand(1)
    data = band.ReadRaster()
    data = struct.unpack("B" * 24, data)
    assert data == (
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        0,
        1,
        1,
        1,
        1,
        1,
    )

    ds = gdal.Open(
        'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_keys_values:0:0'
    )
    assert ds is not None
    assert ds.RasterXSize == 2
    assert ds.RasterYSize == 2
    assert ds.RasterCount == 1
    band = ds.GetRasterBand(1)
    data = band.ReadRaster()
    data = struct.unpack("i" * 4, data)
    assert data == (1, 0, 1, 1)

    ds = gdal.Open(
        'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_keys_values:0:1'
    )
    assert ds is not None
    assert ds.RasterXSize == 2
    assert ds.RasterYSize == 2
    assert ds.RasterCount == 1
    band = ds.GetRasterBand(1)
    data = band.ReadRaster()
    data = struct.unpack("i" * 4, data)
    assert data == (1, 1, 0, 1)

    ds = gdal.Open(
        'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_values_only:0:0'
    )
    assert ds is not None
    assert ds.RasterXSize == 2
    assert ds.RasterYSize == 2
    assert ds.RasterCount == 1
    band = ds.GetRasterBand(1)
    data = band.ReadRaster()
    data = struct.unpack("B" * 4, data)
    assert data == (1, 1, 0, 1)

    ds = gdal.Open(
        'BAG:"data/bag/test_georef_metadata.bag":georef_metadata:layer_with_values_only:0:1'
    )
    assert ds is not None
    assert ds.RasterXSize == 2
    assert ds.RasterYSize == 2
    assert ds.RasterCount == 1
    band = ds.GetRasterBand(1)
    data = band.ReadRaster()
    data = struct.unpack("B" * 4, data)
    assert data == (1, 0, 1, 1)


###############################################################################
#


def test_bag_write_single_band_create():

    tmpfilename = "/vsimem/out.bag"
    gdal.Warp(tmpfilename, "data/byte.tif", options="-ot Float32")
    ds = gdal.Open(tmpfilename)
    srs = ds.GetSpatialRef()
    assert srs.GetAuthorityCode("PROJCS") == "26711"
    assert ds.GetGeoTransform() == (440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0)
    assert ds.GetRasterBand(1).Checksum() == 4672
    assert ds.GetRasterBand(1).GetMinimum() == 74.0
    assert ds.GetRasterBand(1).GetMaximum() == 255.0
    ds = None
    gdal.GetDriverByName("BAG").Delete(tmpfilename)


###############################################################################
#


def test_bag_write_single_band_create_two_bands():

    tmpfilename = "/vsimem/out.bag"
    gdal.Warp(
        tmpfilename,
        "data/bag/test_vr.bag",
        options="-co BLOCK_SIZE=2 -co VAR_XML_IDENTIFICATION_CITATION=<bar/>",
    )
    ds = gdal.Open(tmpfilename)
    assert ds.GetRasterBand(1).Checksum() == 65529
    assert ds.GetRasterBand(2).Checksum() == 60
    xml = ds.GetMetadata_List("xml:BAG")[0]
    assert "<bar />" in xml
    ds = None
    gdal.GetDriverByName("BAG").Delete(tmpfilename)


###############################################################################
#


def test_bag_read_tracking_list():

    ds = ogr.Open("data/bag/test_georef_metadata.bag")
    assert ds is not None
    assert ds.GetLayerCount() == 1
    assert ds.GetLayer(1) is None
    lyr = ds.GetLayer(0)
    assert lyr.GetFeatureCount() == 2
    lyr.ResetReading()

    f = lyr.GetNextFeature()
    assert f["row"] == 0
    assert f["col"] == 1
    assert f["depth"] == 2.5
    assert f["uncertainty"] == 3.5
    assert f["track_code"] == 4
    assert f["list_series"] == 5

    f = lyr.GetNextFeature()
    assert f["row"] == 6
    assert f["col"] == 7
    assert f["depth"] == 8.5
    assert f["uncertainty"] == 9.5
    assert f["track_code"] == 10
    assert f["list_series"] == 11


###############################################################################
#


def test_bag_write_and_check_xml_size_and_res():

    tmpfilename = "/vsimem/out.bag"
    gdal.Translate(tmpfilename, "data/byte.tif", options="-ot Float32 -outsize 20 10")

    ds = gdal.Open(tmpfilename)
    xml = ds.GetMetadata_List("xml:BAG")[0]
    xml = xml.replace("  ", "").replace("\n", "").replace("> <", "><")
    assert (
        '<gmd:axisDimensionProperties><gmd:MD_Dimension><gmd:dimensionName><gmd:MD_DimensionNameTypeCode codeList="http://www.isotc211.org/2005/resources/Codelist/gmxCodelists.xml#MD_DimensionNameTypeCode" codeListValue="row">row</gmd:MD_DimensionNameTypeCode></gmd:dimensionName><gmd:dimensionSize><gco:Integer>10</gco:Integer></gmd:dimensionSize><gmd:resolution><gco:Measure uom="m">120</gco:Measure></gmd:resolution></gmd:MD_Dimension></gmd:axisDimensionProperties><gmd:axisDimensionProperties><gmd:MD_Dimension><gmd:dimensionName><gmd:MD_DimensionNameTypeCode codeList="http://www.isotc211.org/2005/resources/Codelist/gmxCodelists.xml#MD_DimensionNameTypeCode" codeListValue="column">column</gmd:MD_DimensionNameTypeCode></gmd:dimensionName><gmd:dimensionSize><gco:Integer>20</gco:Integer></gmd:dimensionSize><gmd:resolution><gco:Measure uom="m">60</gco:Measure></gmd:resolution></gmd:MD_Dimension></gmd:axisDimensionProperties>'
        in xml
    )

    gdal.Unlink(tmpfilename)


###############################################################################
# Test fix for https://github.com/OSGeo/gdal/issues/4057


def test_bag_write_values_at_nodata():

    tmpfilename = "/vsimem/out.bag"
    ds = gdal.GetDriverByName("BAG").Create(tmpfilename, 1, 3, 2, gdal.GDT_Float32)
    assert ds.GetRasterBand(1).SetNoDataValue(1000000) == gdal.CE_None
    assert ds.GetRasterBand(2).SetNoDataValue(1000000) == gdal.CE_None
    ds.GetRasterBand(1).WriteRaster(0, 0, 1, 3, struct.pack("f" * 3, -12, 1000000, -11))
    ds.GetRasterBand(2).WriteRaster(0, 0, 1, 3, struct.pack("f" * 3, 5, 1000000, 4))
    ds = None

    ds = gdal.Open(tmpfilename)
    assert ds.GetRasterBand(1).GetMinimum() == -12
    assert ds.GetRasterBand(1).GetMaximum() == -11
    assert ds.GetRasterBand(2).GetMinimum() == 4
    assert ds.GetRasterBand(2).GetMaximum() == 5
    ds = None

    gdal.Unlink(tmpfilename)


###############################################################################
# Test force opening


def test_bag_force_opening():

    drv = gdal.IdentifyDriverEx("data/netcdf/trmm-nc4.nc", allowed_drivers=["BAG"])
    assert drv.GetDescription() == "BAG"


###############################################################################
# Test force opening, but provided file is still not recognized (for good reasons)


def test_bag_force_opening_no_match():

    drv = gdal.IdentifyDriverEx("data/byte.tif", allowed_drivers=["BAG"])
    assert drv is None


###############################################################################
@gdaltest.enable_exceptions()
def test_bag_open_larger_than_INT_MAX_pixels():

    with pytest.raises(Exception, match="At least one dimension size exceeds INT_MAX"):
        gdal.Open("data/bag/larger_than_INT_MAX_pixels.bag")
