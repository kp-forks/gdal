#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test multidimensional support with numpy
# Author:   Even Rouault <even.rouault@spatialys.com>
#
###############################################################################
# Copyright (c) 2019, Even Rouault <even.rouault@spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################


import struct

import gdaltest
import pytest

from osgeo import gdal

###############################################################################
# verify that we can load Numeric python, and find the Numpy driver.


@pytest.fixture(scope="module", autouse=True)
def setup_and_cleanup():

    # importing gdal_array will allow numpy driver registration
    gdaltest.importorskip_gdal_array()

    gdal.AllRegister()

    gdaltest.numpy_drv = gdal.GetDriverByName("NUMPY")
    assert gdaltest.numpy_drv is not None, "NUMPY driver not found!"


###############################################################################


def test_numpy_rw_multidim_readasarray_writearray():

    import numpy as np

    drv = gdal.GetDriverByName("MEM")
    ds = drv.CreateMultiDimensional("myds")
    rg = ds.GetRootGroup()
    dim0 = rg.CreateDimension("dim0", None, None, 2)
    dim1 = rg.CreateDimension("dim1", None, None, 3)
    myarray = rg.CreateMDArray(
        "myarray", [dim0, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
    )
    assert myarray
    ar = np.array([[1, 2, 3], [4, 5, 6]], dtype=np.uint8)
    assert myarray.WriteArray(ar) == gdal.CE_None
    got_ar = myarray.ReadAsArray()
    assert got_ar.shape == (2, 3)
    assert np.array_equal(got_ar, ar)
    # Check algo with non-numpy method so as to detect issues with buffer striding
    assert struct.unpack("B" * 6, myarray.Read()) == (1, 2, 3, 4, 5, 6)
    assert struct.unpack("B" * 6, myarray.Read(buffer_stride=[3, 1])) == (
        1,
        2,
        3,
        4,
        5,
        6,
    )
    assert struct.unpack("B" * 6, myarray.Read(buffer_stride=[1, 2])) == (
        1,
        4,
        2,
        5,
        3,
        6,
    )


###############################################################################


def test_numpy_rw_multidim_numpy_array_as_dataset():

    import numpy as np

    from osgeo import gdal_array

    for typ in (
        np.int8,
        np.uint8,
        np.uint16,
        np.int16,
        np.uint32,
        np.int32,
        np.float32,
        np.float64,
        np.complex64,
        np.complex128,
    ):
        ar = np.array([[1, 2, 3], [4, 5, 6]], dtype=typ)
        ds = gdal_array.OpenMultiDimensionalNumPyArray(ar)
        assert ds
        rg = ds.GetRootGroup()
        assert rg
        myarray = rg.OpenMDArray("array")
        assert myarray
        assert np.array_equal(myarray.ReadAsArray(), ar)

    ar = np.array([[1, 2, 3], [4, 5, 6]], dtype=np.str_)
    with pytest.raises(
        Exception, match="Unable to access numpy arrays of typecode `U'"
    ):
        gdal_array.OpenMultiDimensionalNumPyArray(ar)


###############################################################################


def test_numpy_rw_multidim_readasarray_writearray_negative_strides():

    import numpy as np

    drv = gdal.GetDriverByName("MEM")
    ds = drv.CreateMultiDimensional("myds")
    rg = ds.GetRootGroup()
    dim0 = rg.CreateDimension("dim0", None, None, 2)
    dim1 = rg.CreateDimension("dim1", None, None, 3)
    myarray = rg.CreateMDArray(
        "myarray", [dim0, dim1], gdal.ExtendedDataType.Create(gdal.GDT_Byte)
    )
    assert myarray
    ar = np.array([[1, 2, 3], [4, 5, 6]], dtype=np.uint8)
    ar = ar[::-1, ::-1]  # Test negative strides
    assert myarray.WriteArray(ar) == gdal.CE_None
    got_ar = myarray.ReadAsArray()
    assert got_ar.shape == (2, 3)
    assert np.array_equal(got_ar, ar)
    # Check algo with non-numpy method so as to detect issues with buffer striding
    assert struct.unpack("B" * 6, myarray.Read()) == (6, 5, 4, 3, 2, 1)
    assert struct.unpack("B" * 6, myarray.Read(buffer_stride=[3, 1])) == (
        6,
        5,
        4,
        3,
        2,
        1,
    )
    assert struct.unpack("B" * 6, myarray.Read(buffer_stride=[1, 2])) == (
        6,
        3,
        5,
        2,
        4,
        1,
    )


###############################################################################


def test_numpy_rw_multidim_numpy_array_as_dataset_negative_strides():

    import numpy as np

    from osgeo import gdal_array

    for typ in (
        np.int8,
        np.uint8,
        np.uint16,
        np.int16,
        np.uint32,
        np.int32,
        np.float32,
        np.float64,
        np.complex64,
        np.complex128,
    ):
        ar = np.array([[1, 2, 3], [4, 5, 6]], dtype=typ)
        ar = ar[::-1, ::-1]  # Test negative strides
        ds = gdal_array.OpenMultiDimensionalNumPyArray(ar)
        assert ds
        rg = ds.GetRootGroup()
        assert rg
        myarray = rg.OpenMDArray("array")
        assert myarray
        assert np.array_equal(myarray.ReadAsArray(), ar)


###############################################################################


def test_numpy_rw_multidim_compound_datatype():

    import numpy as np

    from osgeo import gdal_array

    drv = gdal.GetDriverByName("MEM")
    ds = drv.CreateMultiDimensional("myds")
    rg = ds.GetRootGroup()
    dim = rg.CreateDimension("dim0", None, None, 2)
    comp0 = gdal.EDTComponent.Create(
        "x", 0, gdal.ExtendedDataType.Create(gdal.GDT_Int16)
    )
    comp1 = gdal.EDTComponent.Create(
        "y", 4, gdal.ExtendedDataType.Create(gdal.GDT_Int32)
    )
    dt = gdal.ExtendedDataType.CreateCompound("mytype", 8, [comp0, comp1])
    myarray = rg.CreateMDArray("myarray", [dim], dt)
    assert myarray

    numpydt = gdal_array.ExtendedDataTypeToNumPyDataType(dt)
    assert numpydt.itemsize == 8
    assert numpydt.names == ("x", "y")
    assert numpydt.fields["x"] == (np.int16, 0)
    assert numpydt.fields["y"] == (np.int32, 4)

    assert (
        myarray.Write(struct.pack("hi" * 2, 32767, 1000000, -32768, -1000000))
        == gdal.CE_None
    )
    res = myarray.ReadAsArray()
    assert res.dtype == numpydt
    assert np.array_equal(
        res, np.array([(32767, 1000000), (-32768, -1000000)], dtype=res.dtype)
    )

    ar = np.array([(-32768, -1000000), (-32767, 1000000)], dtype=numpydt)
    assert myarray.WriteArray(ar) == gdal.CE_None
    res = myarray.ReadAsArray()
    assert np.array_equal(res, ar)


###############################################################################


@pytest.mark.parametrize(
    "datatype",
    [
        gdal.GDT_Byte,
        gdal.GDT_Int16,
        gdal.GDT_UInt16,
        gdal.GDT_Int32,
        gdal.GDT_UInt32,
        gdal.GDT_Float32,
        gdal.GDT_Float64,
        gdal.GDT_CInt16,
        gdal.GDT_CInt32,
        gdal.GDT_CFloat32,
        gdal.GDT_CFloat64,
    ],
    ids=gdal.GetDataTypeName,
)
def test_numpy_rw_multidim_datatype(datatype):

    import numpy as np

    drv = gdal.GetDriverByName("MEM")
    ds = drv.CreateMultiDimensional("myds")
    rg = ds.GetRootGroup()
    dim = rg.CreateDimension("dim0", None, None, 2)
    myarray = rg.CreateMDArray("myarray", [dim], gdal.ExtendedDataType.Create(datatype))
    assert myarray
    numpy_ar = np.reshape(np.arange(0, 2, dtype=np.uint16), (2,))
    assert myarray.WriteArray(numpy_ar) == gdal.CE_None
    got = myarray.ReadAsArray()
    assert np.array_equal(got, numpy_ar)
    assert np.array_equal(
        myarray.ReadAsArray(buf_obj=np.zeros(got.shape, got.dtype)), numpy_ar
    )
