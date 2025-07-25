/******************************************************************************
 *
 * Project:  GDAL Utilities
 * Purpose:  Command line application to build VRT datasets from raster products
 *           or content of SHP tile index
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2007-2016, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_api.h"
#include "ogr_srs_api.h"

#include "cpl_port.h"
#include "gdal_utils.h"
#include "gdal_utils_priv.h"
#include "gdalargumentparser.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "commonutils.h"
#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_float.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "cpl_vsi_virtual.h"
#include "gdal.h"
#include "gdal_vrt.h"
#include "gdal_priv.h"
#include "gdal_proxy.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_srs_api.h"
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"
#include "vrtdataset.h"

#define GEOTRSFRM_TOPLEFT_X 0
#define GEOTRSFRM_WE_RES 1
#define GEOTRSFRM_ROTATION_PARAM1 2
#define GEOTRSFRM_TOPLEFT_Y 3
#define GEOTRSFRM_ROTATION_PARAM2 4
#define GEOTRSFRM_NS_RES 5

namespace gdal::GDALBuildVRT
{
typedef enum
{
    LOWEST_RESOLUTION,
    HIGHEST_RESOLUTION,
    AVERAGE_RESOLUTION,
    SAME_RESOLUTION,
    USER_RESOLUTION,
    COMMON_RESOLUTION,
} ResolutionStrategy;

struct DatasetProperty
{
    int isFileOK = FALSE;
    int nRasterXSize = 0;
    int nRasterYSize = 0;
    GDALGeoTransform gt{};
    int nBlockXSize = 0;
    int nBlockYSize = 0;
    std::vector<GDALDataType> aeBandType{};
    std::vector<bool> abHasNoData{};
    std::vector<double> adfNoDataValues{};
    std::vector<bool> abHasOffset{};
    std::vector<double> adfOffset{};
    std::vector<bool> abHasScale{};
    std::vector<bool> abHasMaskBand{};
    std::vector<double> adfScale{};
    int bHasDatasetMask = 0;
    bool bLastBandIsAlpha = false;
    int nMaskBlockXSize = 0;
    int nMaskBlockYSize = 0;
    std::vector<int> anOverviewFactors{};
};

struct BandProperty
{
    GDALColorInterp colorInterpretation = GCI_Undefined;
    GDALDataType dataType = GDT_Unknown;
    std::unique_ptr<GDALColorTable> colorTable{};
    bool bHasNoData = false;
    double noDataValue = 0;
    bool bHasOffset = false;
    double dfOffset = 0;
    bool bHasScale = false;
    double dfScale = 0;
};
}  // namespace gdal::GDALBuildVRT

using namespace gdal::GDALBuildVRT;

/************************************************************************/
/*                         GetSrcDstWin()                               */
/************************************************************************/

static int GetSrcDstWin(DatasetProperty *psDP, double we_res, double ns_res,
                        double minX, double minY, double maxX, double maxY,
                        int nTargetXSize, int nTargetYSize, double *pdfSrcXOff,
                        double *pdfSrcYOff, double *pdfSrcXSize,
                        double *pdfSrcYSize, double *pdfDstXOff,
                        double *pdfDstYOff, double *pdfDstXSize,
                        double *pdfDstYSize)
{
    if (we_res == 0 || ns_res == 0)
    {
        // should not happen. to please Coverity
        return FALSE;
    }

    /* Check that the destination bounding box intersects the source bounding
     * box */
    if (psDP->gt[GEOTRSFRM_TOPLEFT_X] +
            psDP->nRasterXSize * psDP->gt[GEOTRSFRM_WE_RES] <=
        minX)
        return FALSE;
    if (psDP->gt[GEOTRSFRM_TOPLEFT_X] >= maxX)
        return FALSE;
    if (psDP->gt[GEOTRSFRM_TOPLEFT_Y] +
            psDP->nRasterYSize * psDP->gt[GEOTRSFRM_NS_RES] >=
        maxY)
        return FALSE;
    if (psDP->gt[GEOTRSFRM_TOPLEFT_Y] <= minY)
        return FALSE;

    if (psDP->gt[GEOTRSFRM_TOPLEFT_X] < minX)
    {
        *pdfSrcXOff =
            (minX - psDP->gt[GEOTRSFRM_TOPLEFT_X]) / psDP->gt[GEOTRSFRM_WE_RES];
        *pdfDstXOff = 0.0;
    }
    else
    {
        *pdfSrcXOff = 0.0;
        *pdfDstXOff = ((psDP->gt[GEOTRSFRM_TOPLEFT_X] - minX) / we_res);
    }
    if (maxY < psDP->gt[GEOTRSFRM_TOPLEFT_Y])
    {
        *pdfSrcYOff = (psDP->gt[GEOTRSFRM_TOPLEFT_Y] - maxY) /
                      -psDP->gt[GEOTRSFRM_NS_RES];
        *pdfDstYOff = 0.0;
    }
    else
    {
        *pdfSrcYOff = 0.0;
        *pdfDstYOff = ((maxY - psDP->gt[GEOTRSFRM_TOPLEFT_Y]) / -ns_res);
    }

    *pdfSrcXSize = psDP->nRasterXSize;
    *pdfSrcYSize = psDP->nRasterYSize;
    if (*pdfSrcXOff > 0)
        *pdfSrcXSize -= *pdfSrcXOff;
    if (*pdfSrcYOff > 0)
        *pdfSrcYSize -= *pdfSrcYOff;

    const double dfSrcToDstXSize = psDP->gt[GEOTRSFRM_WE_RES] / we_res;
    *pdfDstXSize = *pdfSrcXSize * dfSrcToDstXSize;
    const double dfSrcToDstYSize = psDP->gt[GEOTRSFRM_NS_RES] / ns_res;
    *pdfDstYSize = *pdfSrcYSize * dfSrcToDstYSize;

    if (*pdfDstXOff + *pdfDstXSize > nTargetXSize)
    {
        *pdfDstXSize = nTargetXSize - *pdfDstXOff;
        *pdfSrcXSize = *pdfDstXSize / dfSrcToDstXSize;
    }

    if (*pdfDstYOff + *pdfDstYSize > nTargetYSize)
    {
        *pdfDstYSize = nTargetYSize - *pdfDstYOff;
        *pdfSrcYSize = *pdfDstYSize / dfSrcToDstYSize;
    }

    return *pdfSrcXSize > 0 && *pdfDstXSize > 0 && *pdfSrcYSize > 0 &&
           *pdfDstYSize > 0;
}

/************************************************************************/
/*                            VRTBuilder                                */
/************************************************************************/

class VRTBuilder
{
    /* Input parameters */
    bool bStrict = false;
    char *pszOutputFilename = nullptr;
    int nInputFiles = 0;
    char **ppszInputFilenames = nullptr;
    int nSrcDSCount = 0;
    GDALDatasetH *pahSrcDS = nullptr;
    int nTotalBands = 0;
    bool bLastBandIsAlpha = false;
    bool bExplicitBandList = false;
    int nMaxSelectedBandNo = 0;
    int nSelectedBands = 0;
    int *panSelectedBandList = nullptr;
    ResolutionStrategy resolutionStrategy = AVERAGE_RESOLUTION;
    int nCountValid = 0;
    double we_res = 0;
    double ns_res = 0;
    int bTargetAlignedPixels = 0;
    double minX = 0;
    double minY = 0;
    double maxX = 0;
    double maxY = 0;
    int bSeparate = 0;
    int bAllowProjectionDifference = 0;
    int bAddAlpha = 0;
    int bHideNoData = 0;
    int nSubdataset = 0;
    char *pszSrcNoData = nullptr;
    char *pszVRTNoData = nullptr;
    char *pszOutputSRS = nullptr;
    char *pszResampling = nullptr;
    char **papszOpenOptions = nullptr;
    bool bUseSrcMaskBand = true;
    bool bNoDataFromMask = false;
    double dfMaskValueThreshold = 0;
    const CPLStringList aosCreateOptions;
    std::string osPixelFunction{};
    const CPLStringList aosPixelFunctionArgs;
    const bool bWriteAbsolutePath;

    /* Internal variables */
    char *pszProjectionRef = nullptr;
    std::vector<BandProperty> asBandProperties{};
    int bFirst = TRUE;
    int bHasGeoTransform = 0;
    int nRasterXSize = 0;
    int nRasterYSize = 0;
    std::vector<DatasetProperty> asDatasetProperties{};
    int bUserExtent = 0;
    int bAllowSrcNoData = TRUE;
    double *padfSrcNoData = nullptr;
    int nSrcNoDataCount = 0;
    int bAllowVRTNoData = TRUE;
    double *padfVRTNoData = nullptr;
    int nVRTNoDataCount = 0;
    int bHasRunBuild = 0;
    int bHasDatasetMask = 0;

    std::string AnalyseRaster(GDALDatasetH hDS,
                              DatasetProperty *psDatasetProperties);

    void CreateVRTSeparate(VRTDataset *poVTDS);
    void CreateVRTNonSeparate(VRTDataset *poVRTDS);

    CPL_DISALLOW_COPY_ASSIGN(VRTBuilder)

  public:
    VRTBuilder(bool bStrictIn, const char *pszOutputFilename, int nInputFiles,
               const char *const *ppszInputFilenames, GDALDatasetH *pahSrcDSIn,
               const int *panSelectedBandListIn, int nBandCount,
               ResolutionStrategy resolutionStrategy, double we_res,
               double ns_res, int bTargetAlignedPixels, double minX,
               double minY, double maxX, double maxY, int bSeparate,
               int bAllowProjectionDifference, int bAddAlpha, int bHideNoData,
               int nSubdataset, const char *pszSrcNoData,
               const char *pszVRTNoData, bool bUseSrcMaskBand,
               bool bNoDataFromMask, double dfMaskValueThreshold,
               const char *pszOutputSRS, const char *pszResampling,
               const char *pszPixelFunctionName,
               const CPLStringList &aosPixelFunctionArgs,
               const char *const *papszOpenOptionsIn,
               const CPLStringList &aosCreateOptionsIn,
               bool bWriteAbsolutePathIn);

    ~VRTBuilder();

    std::unique_ptr<GDALDataset> Build(GDALProgressFunc pfnProgress,
                                       void *pProgressData);

    std::string m_osProgramName{};
};

/************************************************************************/
/*                          VRTBuilder()                                */
/************************************************************************/

VRTBuilder::VRTBuilder(
    bool bStrictIn, const char *pszOutputFilenameIn, int nInputFilesIn,
    const char *const *ppszInputFilenamesIn, GDALDatasetH *pahSrcDSIn,
    const int *panSelectedBandListIn, int nBandCount,
    ResolutionStrategy resolutionStrategyIn, double we_resIn, double ns_resIn,
    int bTargetAlignedPixelsIn, double minXIn, double minYIn, double maxXIn,
    double maxYIn, int bSeparateIn, int bAllowProjectionDifferenceIn,
    int bAddAlphaIn, int bHideNoDataIn, int nSubdatasetIn,
    const char *pszSrcNoDataIn, const char *pszVRTNoDataIn,
    bool bUseSrcMaskBandIn, bool bNoDataFromMaskIn,
    double dfMaskValueThresholdIn, const char *pszOutputSRSIn,
    const char *pszResamplingIn, const char *pszPixelFunctionIn,
    const CPLStringList &aosPixelFunctionArgsIn,
    const char *const *papszOpenOptionsIn,
    const CPLStringList &aosCreateOptionsIn, bool bWriteAbsolutePathIn)
    : bStrict(bStrictIn), aosCreateOptions(aosCreateOptionsIn),
      aosPixelFunctionArgs(aosPixelFunctionArgsIn),
      bWriteAbsolutePath(bWriteAbsolutePathIn)
{
    pszOutputFilename = CPLStrdup(pszOutputFilenameIn);
    nInputFiles = nInputFilesIn;
    papszOpenOptions = CSLDuplicate(const_cast<char **>(papszOpenOptionsIn));

    if (pszPixelFunctionIn != nullptr)
    {
        osPixelFunction = pszPixelFunctionIn;
    }

    if (ppszInputFilenamesIn)
    {
        ppszInputFilenames =
            static_cast<char **>(CPLMalloc(nInputFiles * sizeof(char *)));
        for (int i = 0; i < nInputFiles; i++)
        {
            ppszInputFilenames[i] = CPLStrdup(ppszInputFilenamesIn[i]);
        }
    }
    else if (pahSrcDSIn)
    {
        nSrcDSCount = nInputFiles;
        pahSrcDS = static_cast<GDALDatasetH *>(
            CPLMalloc(nInputFiles * sizeof(GDALDatasetH)));
        memcpy(pahSrcDS, pahSrcDSIn, nInputFiles * sizeof(GDALDatasetH));
        ppszInputFilenames =
            static_cast<char **>(CPLMalloc(nInputFiles * sizeof(char *)));
        for (int i = 0; i < nInputFiles; i++)
        {
            ppszInputFilenames[i] =
                CPLStrdup(GDALGetDescription(pahSrcDSIn[i]));
        }
    }

    bExplicitBandList = nBandCount != 0;
    nSelectedBands = nBandCount;
    if (nBandCount)
    {
        panSelectedBandList =
            static_cast<int *>(CPLMalloc(nSelectedBands * sizeof(int)));
        memcpy(panSelectedBandList, panSelectedBandListIn,
               nSelectedBands * sizeof(int));
    }

    resolutionStrategy = resolutionStrategyIn;
    we_res = we_resIn;
    ns_res = ns_resIn;
    bTargetAlignedPixels = bTargetAlignedPixelsIn;
    minX = minXIn;
    minY = minYIn;
    maxX = maxXIn;
    maxY = maxYIn;
    bSeparate = bSeparateIn;
    bAllowProjectionDifference = bAllowProjectionDifferenceIn;
    bAddAlpha = bAddAlphaIn;
    bHideNoData = bHideNoDataIn;
    nSubdataset = nSubdatasetIn;
    pszSrcNoData = (pszSrcNoDataIn) ? CPLStrdup(pszSrcNoDataIn) : nullptr;
    pszVRTNoData = (pszVRTNoDataIn) ? CPLStrdup(pszVRTNoDataIn) : nullptr;
    pszOutputSRS = (pszOutputSRSIn) ? CPLStrdup(pszOutputSRSIn) : nullptr;
    pszResampling = (pszResamplingIn) ? CPLStrdup(pszResamplingIn) : nullptr;
    bUseSrcMaskBand = bUseSrcMaskBandIn;
    bNoDataFromMask = bNoDataFromMaskIn;
    dfMaskValueThreshold = dfMaskValueThresholdIn;
}

/************************************************************************/
/*                         ~VRTBuilder()                                */
/************************************************************************/

VRTBuilder::~VRTBuilder()
{
    CPLFree(pszOutputFilename);
    CPLFree(pszSrcNoData);
    CPLFree(pszVRTNoData);
    CPLFree(panSelectedBandList);

    if (ppszInputFilenames)
    {
        for (int i = 0; i < nInputFiles; i++)
        {
            CPLFree(ppszInputFilenames[i]);
        }
    }
    CPLFree(ppszInputFilenames);
    CPLFree(pahSrcDS);

    CPLFree(pszProjectionRef);
    CPLFree(padfSrcNoData);
    CPLFree(padfVRTNoData);
    CPLFree(pszOutputSRS);
    CPLFree(pszResampling);
    CSLDestroy(papszOpenOptions);
}

/************************************************************************/
/*                           ProjAreEqual()                             */
/************************************************************************/

static int ProjAreEqual(const char *pszWKT1, const char *pszWKT2)
{
    if (EQUAL(pszWKT1, pszWKT2))
        return TRUE;

    OGRSpatialReferenceH hSRS1 = OSRNewSpatialReference(pszWKT1);
    OGRSpatialReferenceH hSRS2 = OSRNewSpatialReference(pszWKT2);
    int bRet = hSRS1 != nullptr && hSRS2 != nullptr && OSRIsSame(hSRS1, hSRS2);
    if (hSRS1)
        OSRDestroySpatialReference(hSRS1);
    if (hSRS2)
        OSRDestroySpatialReference(hSRS2);
    return bRet;
}

/************************************************************************/
/*                         GetProjectionName()                          */
/************************************************************************/

static CPLString GetProjectionName(const char *pszProjection)
{
    if (!pszProjection)
        return "(null)";

    OGRSpatialReference oSRS;
    oSRS.SetFromUserInput(pszProjection);
    const char *pszRet = nullptr;
    if (oSRS.IsProjected())
        pszRet = oSRS.GetAttrValue("PROJCS");
    else if (oSRS.IsGeographic())
        pszRet = oSRS.GetAttrValue("GEOGCS");
    return pszRet ? pszRet : "(null)";
}

/************************************************************************/
/*                           AnalyseRaster()                            */
/************************************************************************/

static void checkNoDataValues(const std::vector<BandProperty> &asProperties)
{
    for (const auto &oProps : asProperties)
    {
        if (oProps.bHasNoData && GDALDataTypeIsInteger(oProps.dataType) &&
            !GDALIsValueExactAs(oProps.noDataValue, oProps.dataType))
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Band data type of %s cannot represent the specified "
                     "NoData value of %g",
                     GDALGetDataTypeName(oProps.dataType), oProps.noDataValue);
        }
    }
}

std::string VRTBuilder::AnalyseRaster(GDALDatasetH hDS,
                                      DatasetProperty *psDatasetProperties)
{
    GDALDataset *poDS = GDALDataset::FromHandle(hDS);
    const char *dsFileName = poDS->GetDescription();
    char **papszMetadata = poDS->GetMetadata("SUBDATASETS");
    if (CSLCount(papszMetadata) > 0 && poDS->GetRasterCount() == 0)
    {
        ppszInputFilenames = static_cast<char **>(CPLRealloc(
            ppszInputFilenames,
            sizeof(char *) * (nInputFiles + CSLCount(papszMetadata))));
        if (nSubdataset < 0)
        {
            int count = 1;
            char subdatasetNameKey[80];
            snprintf(subdatasetNameKey, sizeof(subdatasetNameKey),
                     "SUBDATASET_%d_NAME", count);
            while (*papszMetadata != nullptr)
            {
                if (EQUALN(*papszMetadata, subdatasetNameKey,
                           strlen(subdatasetNameKey)))
                {
                    asDatasetProperties.resize(nInputFiles + 1);
                    ppszInputFilenames[nInputFiles] = CPLStrdup(
                        *papszMetadata + strlen(subdatasetNameKey) + 1);
                    nInputFiles++;
                    count++;
                    snprintf(subdatasetNameKey, sizeof(subdatasetNameKey),
                             "SUBDATASET_%d_NAME", count);
                }
                papszMetadata++;
            }
        }
        else
        {
            char subdatasetNameKey[80];
            const char *pszSubdatasetName;

            snprintf(subdatasetNameKey, sizeof(subdatasetNameKey),
                     "SUBDATASET_%d_NAME", nSubdataset);
            pszSubdatasetName =
                CSLFetchNameValue(papszMetadata, subdatasetNameKey);
            if (pszSubdatasetName)
            {
                asDatasetProperties.resize(nInputFiles + 1);
                ppszInputFilenames[nInputFiles] = CPLStrdup(pszSubdatasetName);
                nInputFiles++;
            }
        }
        return "SILENTLY_IGNORE";
    }

    const char *proj = poDS->GetProjectionRef();
    auto &gt = psDatasetProperties->gt;
    int bGotGeoTransform = poDS->GetGeoTransform(gt) == CE_None;
    if (bSeparate)
    {
        std::string osProgramName(m_osProgramName);
        if (osProgramName == "gdalbuildvrt")
            osProgramName += " -separate";

        if (bFirst)
        {
            bHasGeoTransform = bGotGeoTransform;
            if (!bHasGeoTransform)
            {
                if (bUserExtent)
                {
                    CPLError(CE_Warning, CPLE_NotSupported, "%s",
                             ("User extent ignored by " + osProgramName +
                              "with ungeoreferenced images.")
                                 .c_str());
                }
                if (resolutionStrategy == USER_RESOLUTION)
                {
                    CPLError(CE_Warning, CPLE_NotSupported, "%s",
                             ("User resolution ignored by " + osProgramName +
                              " with ungeoreferenced images.")
                                 .c_str());
                }
            }
        }
        else if (bHasGeoTransform != bGotGeoTransform)
        {
            return osProgramName + " cannot stack ungeoreferenced and "
                                   "georeferenced images.";
        }
        else if (!bHasGeoTransform && (nRasterXSize != poDS->GetRasterXSize() ||
                                       nRasterYSize != poDS->GetRasterYSize()))
        {
            return osProgramName + " cannot stack ungeoreferenced images "
                                   "that have not the same dimensions.";
        }
    }
    else
    {
        if (!bGotGeoTransform)
        {
            return m_osProgramName + " does not support ungeoreferenced image.";
        }
        bHasGeoTransform = TRUE;
    }

    if (bGotGeoTransform)
    {
        if (gt[GEOTRSFRM_ROTATION_PARAM1] != 0 ||
            gt[GEOTRSFRM_ROTATION_PARAM2] != 0)
        {
            return m_osProgramName +
                   " does not support rotated geo transforms.";
        }
        if (gt[GEOTRSFRM_NS_RES] >= 0)
        {
            return m_osProgramName +
                   " does not support positive NS resolution.";
        }
    }

    psDatasetProperties->nRasterXSize = poDS->GetRasterXSize();
    psDatasetProperties->nRasterYSize = poDS->GetRasterYSize();
    if (bFirst && bSeparate && !bGotGeoTransform)
    {
        nRasterXSize = poDS->GetRasterXSize();
        nRasterYSize = poDS->GetRasterYSize();
    }

    double ds_minX = gt[GEOTRSFRM_TOPLEFT_X];
    double ds_maxY = gt[GEOTRSFRM_TOPLEFT_Y];
    double ds_maxX = ds_minX + GDALGetRasterXSize(hDS) * gt[GEOTRSFRM_WE_RES];
    double ds_minY = ds_maxY + GDALGetRasterYSize(hDS) * gt[GEOTRSFRM_NS_RES];

    int _nBands = GDALGetRasterCount(hDS);
    if (_nBands == 0)
    {
        return "Dataset has no bands";
    }
    if (bNoDataFromMask &&
        poDS->GetRasterBand(_nBands)->GetColorInterpretation() == GCI_AlphaBand)
        _nBands--;

    GDALRasterBand *poFirstBand = poDS->GetRasterBand(1);
    poFirstBand->GetBlockSize(&psDatasetProperties->nBlockXSize,
                              &psDatasetProperties->nBlockYSize);

    /* For the -separate case */
    psDatasetProperties->aeBandType.resize(_nBands);

    psDatasetProperties->adfNoDataValues.resize(_nBands);
    psDatasetProperties->abHasNoData.resize(_nBands);

    psDatasetProperties->adfOffset.resize(_nBands);
    psDatasetProperties->abHasOffset.resize(_nBands);

    psDatasetProperties->adfScale.resize(_nBands);
    psDatasetProperties->abHasScale.resize(_nBands);

    psDatasetProperties->abHasMaskBand.resize(_nBands);

    psDatasetProperties->bHasDatasetMask =
        poFirstBand->GetMaskFlags() == GMF_PER_DATASET;
    if (psDatasetProperties->bHasDatasetMask)
        bHasDatasetMask = TRUE;
    poFirstBand->GetMaskBand()->GetBlockSize(
        &psDatasetProperties->nMaskBlockXSize,
        &psDatasetProperties->nMaskBlockYSize);

    psDatasetProperties->bLastBandIsAlpha = false;
    if (poDS->GetRasterBand(_nBands)->GetColorInterpretation() == GCI_AlphaBand)
        psDatasetProperties->bLastBandIsAlpha = true;

    // Collect overview factors. We only handle power-of-two situations for now
    const int nOverviews = poFirstBand->GetOverviewCount();
    int nExpectedOvFactor = 2;
    for (int j = 0; j < nOverviews; j++)
    {
        GDALRasterBand *poOverview = poFirstBand->GetOverview(j);
        if (!poOverview)
            continue;
        if (poOverview->GetXSize() < 128 && poOverview->GetYSize() < 128)
        {
            break;
        }

        const int nOvFactor = GDALComputeOvFactor(
            poOverview->GetXSize(), poFirstBand->GetXSize(),
            poOverview->GetYSize(), poFirstBand->GetYSize());

        if (nOvFactor != nExpectedOvFactor)
            break;

        psDatasetProperties->anOverviewFactors.push_back(nOvFactor);
        nExpectedOvFactor *= 2;
    }

    for (int j = 0; j < _nBands; j++)
    {
        GDALRasterBand *poBand = poDS->GetRasterBand(j + 1);

        psDatasetProperties->aeBandType[j] = poBand->GetRasterDataType();

        if (!bSeparate && nSrcNoDataCount > 0)
        {
            psDatasetProperties->abHasNoData[j] = true;
            if (j < nSrcNoDataCount)
                psDatasetProperties->adfNoDataValues[j] = padfSrcNoData[j];
            else
                psDatasetProperties->adfNoDataValues[j] =
                    padfSrcNoData[nSrcNoDataCount - 1];
        }
        else
        {
            int bHasNoData = false;
            psDatasetProperties->adfNoDataValues[j] =
                poBand->GetNoDataValue(&bHasNoData);
            psDatasetProperties->abHasNoData[j] = bHasNoData != 0;
        }

        int bHasOffset = false;
        psDatasetProperties->adfOffset[j] = poBand->GetOffset(&bHasOffset);
        psDatasetProperties->abHasOffset[j] =
            bHasOffset != 0 && psDatasetProperties->adfOffset[j] != 0.0;

        int bHasScale = false;
        psDatasetProperties->adfScale[j] = poBand->GetScale(&bHasScale);
        psDatasetProperties->abHasScale[j] =
            bHasScale != 0 && psDatasetProperties->adfScale[j] != 1.0;

        const int nMaskFlags = poBand->GetMaskFlags();
        psDatasetProperties->abHasMaskBand[j] =
            (nMaskFlags != GMF_ALL_VALID && nMaskFlags != GMF_NODATA) ||
            poBand->GetColorInterpretation() == GCI_AlphaBand;
    }

    if (bSeparate)
    {
        for (int j = 0; j < nSelectedBands; j++)
        {
            if (panSelectedBandList[j] > _nBands)
            {
                return CPLSPrintf("%s has %d bands, but %d is requested",
                                  dsFileName, _nBands, panSelectedBandList[j]);
            }
        }
    }

    if (bFirst)
    {
        nTotalBands = _nBands;
        if (bAddAlpha && psDatasetProperties->bLastBandIsAlpha)
        {
            bLastBandIsAlpha = true;
            nTotalBands--;
        }

        if (proj)
            pszProjectionRef = CPLStrdup(proj);
        if (!bUserExtent)
        {
            minX = ds_minX;
            minY = ds_minY;
            maxX = ds_maxX;
            maxY = ds_maxY;
        }

        if (!bSeparate)
        {
            // if not provided an explicit band list, take the one of the first
            // dataset
            if (nSelectedBands == 0)
            {
                nSelectedBands = nTotalBands;
                CPLFree(panSelectedBandList);
                panSelectedBandList =
                    static_cast<int *>(CPLMalloc(nSelectedBands * sizeof(int)));
                for (int j = 0; j < nSelectedBands; j++)
                {
                    panSelectedBandList[j] = j + 1;
                }
            }
            for (int j = 0; j < nSelectedBands; j++)
            {
                nMaxSelectedBandNo =
                    std::max(nMaxSelectedBandNo, panSelectedBandList[j]);
            }

            asBandProperties.resize(nSelectedBands);
            for (int j = 0; j < nSelectedBands; j++)
            {
                const int nSelBand = panSelectedBandList[j];
                if (nSelBand <= 0 || nSelBand > nTotalBands)
                {
                    return CPLSPrintf("Invalid band number: %d", nSelBand);
                }
                GDALRasterBand *poBand = poDS->GetRasterBand(nSelBand);
                asBandProperties[j].colorInterpretation =
                    poBand->GetColorInterpretation();
                asBandProperties[j].dataType = poBand->GetRasterDataType();
                if (asBandProperties[j].colorInterpretation == GCI_PaletteIndex)
                {
                    auto colorTable = poBand->GetColorTable();
                    if (colorTable)
                    {
                        asBandProperties[j].colorTable.reset(
                            colorTable->Clone());
                    }
                }
                else
                    asBandProperties[j].colorTable = nullptr;

                if (nVRTNoDataCount > 0)
                {
                    asBandProperties[j].bHasNoData = true;
                    if (j < nVRTNoDataCount)
                        asBandProperties[j].noDataValue = padfVRTNoData[j];
                    else
                        asBandProperties[j].noDataValue =
                            padfVRTNoData[nVRTNoDataCount - 1];
                }
                else
                {
                    int bHasNoData = false;
                    asBandProperties[j].noDataValue =
                        poBand->GetNoDataValue(&bHasNoData);
                    asBandProperties[j].bHasNoData = bHasNoData != 0;
                }

                int bHasOffset = false;
                asBandProperties[j].dfOffset = poBand->GetOffset(&bHasOffset);
                asBandProperties[j].bHasOffset =
                    bHasOffset != 0 && asBandProperties[j].dfOffset != 0.0;

                int bHasScale = false;
                asBandProperties[j].dfScale = poBand->GetScale(&bHasScale);
                asBandProperties[j].bHasScale =
                    bHasScale != 0 && asBandProperties[j].dfScale != 1.0;
            }
        }
    }
    else
    {
        if ((proj != nullptr && pszProjectionRef == nullptr) ||
            (proj == nullptr && pszProjectionRef != nullptr) ||
            (proj != nullptr && pszProjectionRef != nullptr &&
             ProjAreEqual(proj, pszProjectionRef) == FALSE))
        {
            if (!bAllowProjectionDifference)
            {
                CPLString osExpected = GetProjectionName(pszProjectionRef);
                CPLString osGot = GetProjectionName(proj);
                return m_osProgramName +
                       CPLSPrintf(" does not support heterogeneous "
                                  "projection: expected %s, got %s.",
                                  osExpected.c_str(), osGot.c_str());
            }
        }
        if (!bSeparate)
        {
            if (!bExplicitBandList && _nBands != nTotalBands)
            {
                if (bAddAlpha && _nBands == nTotalBands + 1 &&
                    psDatasetProperties->bLastBandIsAlpha)
                {
                    bLastBandIsAlpha = true;
                }
                else
                {
                    return m_osProgramName +
                           CPLSPrintf(" does not support heterogeneous band "
                                      "numbers: expected %d, got %d.",
                                      nTotalBands, _nBands);
                }
            }
            else if (bExplicitBandList && _nBands < nMaxSelectedBandNo)
            {
                return m_osProgramName +
                       CPLSPrintf(" does not support heterogeneous band "
                                  "numbers: expected at least %d, got %d.",
                                  nMaxSelectedBandNo, _nBands);
            }

            for (int j = 0; j < nSelectedBands; j++)
            {
                const int nSelBand = panSelectedBandList[j];
                CPLAssert(nSelBand >= 1 && nSelBand <= _nBands);
                GDALRasterBand *poBand = poDS->GetRasterBand(nSelBand);
                if (asBandProperties[j].colorInterpretation !=
                    poBand->GetColorInterpretation())
                {
                    return m_osProgramName +
                           CPLSPrintf(
                               " does not support heterogeneous "
                               "band color interpretation: expected %s, got "
                               "%s.",
                               GDALGetColorInterpretationName(
                                   asBandProperties[j].colorInterpretation),
                               GDALGetColorInterpretationName(
                                   poBand->GetColorInterpretation()));
                }
                if (asBandProperties[j].dataType != poBand->GetRasterDataType())
                {
                    return m_osProgramName +
                           CPLSPrintf(" does not support heterogeneous "
                                      "band data type: expected %s, got %s.",
                                      GDALGetDataTypeName(
                                          asBandProperties[j].dataType),
                                      GDALGetDataTypeName(
                                          poBand->GetRasterDataType()));
                }
                if (asBandProperties[j].colorTable)
                {
                    const GDALColorTable *colorTable = poBand->GetColorTable();
                    int nRefColorEntryCount =
                        asBandProperties[j].colorTable->GetColorEntryCount();
                    if (colorTable == nullptr ||
                        colorTable->GetColorEntryCount() != nRefColorEntryCount)
                    {
                        return m_osProgramName +
                               " does not support rasters with "
                               "different color tables (different number of "
                               "color table entries)";
                    }

                    /* Check that the palette are the same too */
                    /* We just warn and still process the file. It is not a
                     * technical no-go, but the user */
                    /* should check that the end result is OK for him. */
                    for (int i = 0; i < nRefColorEntryCount; i++)
                    {
                        const GDALColorEntry *psEntry =
                            colorTable->GetColorEntry(i);
                        const GDALColorEntry *psEntryRef =
                            asBandProperties[j].colorTable->GetColorEntry(i);
                        if (psEntry->c1 != psEntryRef->c1 ||
                            psEntry->c2 != psEntryRef->c2 ||
                            psEntry->c3 != psEntryRef->c3 ||
                            psEntry->c4 != psEntryRef->c4)
                        {
                            static int bFirstWarningPCT = TRUE;
                            if (bFirstWarningPCT)
                                CPLError(
                                    CE_Warning, CPLE_NotSupported,
                                    "%s has different values than the first "
                                    "raster for some entries in the color "
                                    "table.\n"
                                    "The end result might produce weird "
                                    "colors.\n"
                                    "You're advised to pre-process your "
                                    "rasters with other tools, such as "
                                    "pct2rgb.py or gdal_translate -expand RGB\n"
                                    "to operate %s on RGB rasters "
                                    "instead",
                                    dsFileName, m_osProgramName.c_str());
                            else
                                CPLError(CE_Warning, CPLE_NotSupported,
                                         "%s has different values than the "
                                         "first raster for some entries in the "
                                         "color table.",
                                         dsFileName);
                            bFirstWarningPCT = FALSE;
                            break;
                        }
                    }
                }

                if (psDatasetProperties->abHasOffset[j] !=
                        asBandProperties[j].bHasOffset ||
                    (asBandProperties[j].bHasOffset &&
                     psDatasetProperties->adfOffset[j] !=
                         asBandProperties[j].dfOffset))
                {
                    return m_osProgramName +
                           CPLSPrintf(
                               " does not support heterogeneous "
                               "band offset: expected (%d,%f), got (%d,%f).",
                               static_cast<int>(asBandProperties[j].bHasOffset),
                               asBandProperties[j].dfOffset,
                               static_cast<int>(
                                   psDatasetProperties->abHasOffset[j]),
                               psDatasetProperties->adfOffset[j]);
                }

                if (psDatasetProperties->abHasScale[j] !=
                        asBandProperties[j].bHasScale ||
                    (asBandProperties[j].bHasScale &&
                     psDatasetProperties->adfScale[j] !=
                         asBandProperties[j].dfScale))
                {
                    return m_osProgramName +
                           CPLSPrintf(
                               " does not support heterogeneous "
                               "band scale: expected (%d,%f), got (%d,%f).",
                               static_cast<int>(asBandProperties[j].bHasScale),
                               asBandProperties[j].dfScale,
                               static_cast<int>(
                                   psDatasetProperties->abHasScale[j]),
                               psDatasetProperties->adfScale[j]);
                }
            }
        }
        if (!bUserExtent)
        {
            if (ds_minX < minX)
                minX = ds_minX;
            if (ds_minY < minY)
                minY = ds_minY;
            if (ds_maxX > maxX)
                maxX = ds_maxX;
            if (ds_maxY > maxY)
                maxY = ds_maxY;
        }
    }

    if (resolutionStrategy == AVERAGE_RESOLUTION)
    {
        ++nCountValid;
        {
            const double dfDelta = gt[GEOTRSFRM_WE_RES] - we_res;
            we_res += dfDelta / nCountValid;
        }
        {
            const double dfDelta = gt[GEOTRSFRM_NS_RES] - ns_res;
            ns_res += dfDelta / nCountValid;
        }
    }
    else if (resolutionStrategy == SAME_RESOLUTION)
    {
        if (bFirst)
        {
            we_res = gt[GEOTRSFRM_WE_RES];
            ns_res = gt[GEOTRSFRM_NS_RES];
        }
        else if (we_res != gt[GEOTRSFRM_WE_RES] ||
                 ns_res != gt[GEOTRSFRM_NS_RES])
        {
            return CPLSPrintf("Dataset %s has resolution %f x %f, whereas "
                              "previous sources have resolution %f x %f",
                              dsFileName, gt[GEOTRSFRM_WE_RES],
                              gt[GEOTRSFRM_NS_RES], we_res, ns_res);
        }
    }
    else if (resolutionStrategy != USER_RESOLUTION)
    {
        if (bFirst)
        {
            we_res = gt[GEOTRSFRM_WE_RES];
            ns_res = gt[GEOTRSFRM_NS_RES];
        }
        else if (resolutionStrategy == HIGHEST_RESOLUTION)
        {
            we_res = std::min(we_res, gt[GEOTRSFRM_WE_RES]);
            // ns_res is negative, the highest resolution is the max value.
            ns_res = std::max(ns_res, gt[GEOTRSFRM_NS_RES]);
        }
        else if (resolutionStrategy == COMMON_RESOLUTION)
        {
            we_res = CPLGreatestCommonDivisor(we_res, gt[GEOTRSFRM_WE_RES]);
            if (we_res == 0)
            {
                return "Failed to get common resolution";
            }
            ns_res = CPLGreatestCommonDivisor(ns_res, gt[GEOTRSFRM_NS_RES]);
            if (ns_res == 0)
            {
                return "Failed to get common resolution";
            }
        }
        else
        {
            we_res = std::max(we_res, gt[GEOTRSFRM_WE_RES]);
            // ns_res is negative, the lowest resolution is the min value.
            ns_res = std::min(ns_res, gt[GEOTRSFRM_NS_RES]);
        }
    }

    checkNoDataValues(asBandProperties);

    return "";
}

/************************************************************************/
/*                         WriteAbsolutePath()                          */
/************************************************************************/

static void WriteAbsolutePath(VRTSimpleSource *poSource, const char *dsFileName)
{
    if (dsFileName[0])
    {
        if (CPLIsFilenameRelative(dsFileName))
        {
            VSIStatBufL sStat;
            if (VSIStatL(dsFileName, &sStat) == 0)
            {
                if (char *pszCurDir = CPLGetCurrentDir())
                {
                    poSource->SetSourceDatasetName(
                        CPLFormFilenameSafe(pszCurDir, dsFileName, nullptr)
                            .c_str(),
                        false);
                    CPLFree(pszCurDir);
                }
            }
        }
        else
        {
            poSource->SetSourceDatasetName(dsFileName, false);
        }
    }
}

/************************************************************************/
/*                         CreateVRTSeparate()                          */
/************************************************************************/

void VRTBuilder::CreateVRTSeparate(VRTDataset *poVRTDS)
{
    int iBand = 1;
    for (int i = 0; ppszInputFilenames != nullptr && i < nInputFiles; i++)
    {
        DatasetProperty *psDatasetProperties = &asDatasetProperties[i];

        if (psDatasetProperties->isFileOK == FALSE)
            continue;

        const char *dsFileName = ppszInputFilenames[i];

        double dfSrcXOff, dfSrcYOff, dfSrcXSize, dfSrcYSize, dfDstXOff,
            dfDstYOff, dfDstXSize, dfDstYSize;
        if (bHasGeoTransform)
        {
            if (!GetSrcDstWin(psDatasetProperties, we_res, ns_res, minX, minY,
                              maxX, maxY, nRasterXSize, nRasterYSize,
                              &dfSrcXOff, &dfSrcYOff, &dfSrcXSize, &dfSrcYSize,
                              &dfDstXOff, &dfDstYOff, &dfDstXSize, &dfDstYSize))
            {
                CPLDebug("BuildVRT",
                         "Skipping %s as not intersecting area of interest",
                         dsFileName);
                continue;
            }
        }
        else
        {
            dfSrcXOff = dfSrcYOff = dfDstXOff = dfDstYOff = 0;
            dfSrcXSize = dfDstXSize = nRasterXSize;
            dfSrcYSize = dfDstYSize = nRasterYSize;
        }

        GDALDatasetH hSourceDS;
        bool bDropRef = false;
        if (nSrcDSCount == nInputFiles &&
            GDALGetDatasetDriver(pahSrcDS[i]) != nullptr &&
            (dsFileName[0] == '\0' ||  // could be a unnamed VRT file
             EQUAL(GDALGetDescription(GDALGetDatasetDriver(pahSrcDS[i])),
                   "MEM")))
        {
            hSourceDS = pahSrcDS[i];
        }
        else
        {
            bDropRef = true;
            GDALProxyPoolDatasetH hProxyDS = GDALProxyPoolDatasetCreate(
                dsFileName, psDatasetProperties->nRasterXSize,
                psDatasetProperties->nRasterYSize, GA_ReadOnly, TRUE,
                pszProjectionRef, psDatasetProperties->gt.data());
            hSourceDS = static_cast<GDALDatasetH>(hProxyDS);
            cpl::down_cast<GDALProxyPoolDataset *>(
                GDALDataset::FromHandle(hProxyDS))
                ->SetOpenOptions(papszOpenOptions);

            for (int jBand = 0;
                 jBand <
                 static_cast<int>(psDatasetProperties->aeBandType.size());
                 ++jBand)
            {
                GDALProxyPoolDatasetAddSrcBandDescription(
                    hProxyDS, psDatasetProperties->aeBandType[jBand],
                    psDatasetProperties->nBlockXSize,
                    psDatasetProperties->nBlockYSize);
            }
        }

        const int nBandsToIter =
            nSelectedBands > 0
                ? nSelectedBands
                : static_cast<int>(psDatasetProperties->aeBandType.size());
        for (int iBandToIter = 0; iBandToIter < nBandsToIter; ++iBandToIter)
        {
            // 0-based
            const int nSrcBandIdx = nSelectedBands > 0
                                        ? panSelectedBandList[iBandToIter] - 1
                                        : iBandToIter;
            assert(nSrcBandIdx >= 0);
            poVRTDS->AddBand(psDatasetProperties->aeBandType[nSrcBandIdx],
                             nullptr);

            VRTSourcedRasterBand *poVRTBand =
                static_cast<VRTSourcedRasterBand *>(
                    poVRTDS->GetRasterBand(iBand));

            if (bHideNoData)
                poVRTBand->SetMetadataItem("HideNoDataValue", "1", nullptr);

            if (bAllowVRTNoData)
            {
                if (nVRTNoDataCount > 0)
                {
                    if (iBand - 1 < nVRTNoDataCount)
                        poVRTBand->SetNoDataValue(padfVRTNoData[iBand - 1]);
                    else
                        poVRTBand->SetNoDataValue(
                            padfVRTNoData[nVRTNoDataCount - 1]);
                }
                else if (psDatasetProperties->abHasNoData[nSrcBandIdx])
                {
                    poVRTBand->SetNoDataValue(
                        psDatasetProperties->adfNoDataValues[nSrcBandIdx]);
                }
            }

            VRTSimpleSource *poSimpleSource;
            if (bAllowSrcNoData &&
                (nSrcNoDataCount > 0 ||
                 psDatasetProperties->abHasNoData[nSrcBandIdx]))
            {
                auto poComplexSource = new VRTComplexSource();
                poSimpleSource = poComplexSource;
                if (nSrcNoDataCount > 0)
                {
                    if (iBand - 1 < nSrcNoDataCount)
                        poComplexSource->SetNoDataValue(
                            padfSrcNoData[iBand - 1]);
                    else
                        poComplexSource->SetNoDataValue(
                            padfSrcNoData[nSrcNoDataCount - 1]);
                }
                else /* if (psDatasetProperties->abHasNoData[nSrcBandIdx]) */
                {
                    poComplexSource->SetNoDataValue(
                        psDatasetProperties->adfNoDataValues[nSrcBandIdx]);
                }
            }
            else if (bUseSrcMaskBand &&
                     psDatasetProperties->abHasMaskBand[nSrcBandIdx])
            {
                auto poSource = new VRTComplexSource();
                poSource->SetUseMaskBand(true);
                poSimpleSource = poSource;
            }
            else
                poSimpleSource = new VRTSimpleSource();

            if (pszResampling)
                poSimpleSource->SetResampling(pszResampling);
            poVRTBand->ConfigureSource(
                poSimpleSource,
                static_cast<GDALRasterBand *>(
                    GDALGetRasterBand(hSourceDS, nSrcBandIdx + 1)),
                FALSE, dfSrcXOff, dfSrcYOff, dfSrcXSize, dfSrcYSize, dfDstXOff,
                dfDstYOff, dfDstXSize, dfDstYSize);

            if (bWriteAbsolutePath)
                WriteAbsolutePath(poSimpleSource, dsFileName);

            if (psDatasetProperties->abHasOffset[nSrcBandIdx])
                poVRTBand->SetOffset(
                    psDatasetProperties->adfOffset[nSrcBandIdx]);

            if (psDatasetProperties->abHasScale[nSrcBandIdx])
                poVRTBand->SetScale(psDatasetProperties->adfScale[nSrcBandIdx]);

            poVRTBand->AddSource(poSimpleSource);

            iBand++;
        }

        if (bDropRef)
        {
            GDALDereferenceDataset(hSourceDS);
        }
    }
}

/************************************************************************/
/*                       CreateVRTNonSeparate()                         */
/************************************************************************/

void VRTBuilder::CreateVRTNonSeparate(VRTDataset *poVRTDS)
{
    CPLStringList aosOptions;

    if (!osPixelFunction.empty())
    {
        aosOptions.AddNameValue("subclass", "VRTDerivedRasterBand");
        aosOptions.AddNameValue("PixelFunctionType", osPixelFunction.c_str());
        aosOptions.AddNameValue("SkipNonContributingSources", "1");
        CPLString osName;
        for (const auto &[pszKey, pszValue] :
             cpl::IterateNameValue(aosPixelFunctionArgs))
        {
            osName.Printf("_PIXELFN_ARG_%s", pszKey);
            aosOptions.AddNameValue(osName.c_str(), pszValue);
        }
    }

    for (int j = 0; j < nSelectedBands; j++)
    {
        const char *pszSourceTransferType = "Float64";
        if (osPixelFunction == "mean" || osPixelFunction == "min" ||
            osPixelFunction == "max")
        {
            pszSourceTransferType =
                GDALGetDataTypeName(asBandProperties[j].dataType);
        }
        aosOptions.AddNameValue("SourceTransferType", pszSourceTransferType);

        poVRTDS->AddBand(asBandProperties[j].dataType, aosOptions.List());
        GDALRasterBand *poBand = poVRTDS->GetRasterBand(j + 1);
        poBand->SetColorInterpretation(asBandProperties[j].colorInterpretation);
        if (asBandProperties[j].colorInterpretation == GCI_PaletteIndex)
        {
            poBand->SetColorTable(asBandProperties[j].colorTable.get());
        }
        if (bAllowVRTNoData && asBandProperties[j].bHasNoData)
            poBand->SetNoDataValue(asBandProperties[j].noDataValue);
        if (bHideNoData)
            poBand->SetMetadataItem("HideNoDataValue", "1");

        if (asBandProperties[j].bHasOffset)
            poBand->SetOffset(asBandProperties[j].dfOffset);

        if (asBandProperties[j].bHasScale)
            poBand->SetScale(asBandProperties[j].dfScale);
    }

    VRTSourcedRasterBand *poMaskVRTBand = nullptr;
    if (bAddAlpha)
    {
        poVRTDS->AddBand(GDT_Byte);
        GDALRasterBand *poBand = poVRTDS->GetRasterBand(nSelectedBands + 1);
        poBand->SetColorInterpretation(GCI_AlphaBand);
    }
    else if (bHasDatasetMask)
    {
        poVRTDS->CreateMaskBand(GMF_PER_DATASET);
        poMaskVRTBand = static_cast<VRTSourcedRasterBand *>(
            poVRTDS->GetRasterBand(1)->GetMaskBand());
    }

    bool bCanCollectOverviewFactors = true;
    std::set<int> anOverviewFactorsSet;
    std::vector<int> anIdxValidDatasets;

    for (int i = 0; ppszInputFilenames != nullptr && i < nInputFiles; i++)
    {
        DatasetProperty *psDatasetProperties = &asDatasetProperties[i];

        if (psDatasetProperties->isFileOK == FALSE)
            continue;

        const char *dsFileName = ppszInputFilenames[i];

        double dfSrcXOff;
        double dfSrcYOff;
        double dfSrcXSize;
        double dfSrcYSize;
        double dfDstXOff;
        double dfDstYOff;
        double dfDstXSize;
        double dfDstYSize;
        if (!GetSrcDstWin(psDatasetProperties, we_res, ns_res, minX, minY, maxX,
                          maxY, nRasterXSize, nRasterYSize, &dfSrcXOff,
                          &dfSrcYOff, &dfSrcXSize, &dfSrcYSize, &dfDstXOff,
                          &dfDstYOff, &dfDstXSize, &dfDstYSize))
        {
            CPLDebug("BuildVRT",
                     "Skipping %s as not intersecting area of interest",
                     dsFileName);
            continue;
        }

        anIdxValidDatasets.push_back(i);

        if (bCanCollectOverviewFactors)
        {
            if (std::abs(psDatasetProperties->gt[1] - we_res) >
                    1e-8 * std::abs(we_res) ||
                std::abs(psDatasetProperties->gt[5] - ns_res) >
                    1e-8 * std::abs(ns_res))
            {
                bCanCollectOverviewFactors = false;
                anOverviewFactorsSet.clear();
            }
        }
        if (bCanCollectOverviewFactors)
        {
            for (int nOvFactor : psDatasetProperties->anOverviewFactors)
                anOverviewFactorsSet.insert(nOvFactor);
        }

        GDALDatasetH hSourceDS;
        bool bDropRef = false;

        if (nSrcDSCount == nInputFiles &&
            GDALGetDatasetDriver(pahSrcDS[i]) != nullptr &&
            (dsFileName[0] == '\0' ||  // could be a unnamed VRT file
             EQUAL(GDALGetDescription(GDALGetDatasetDriver(pahSrcDS[i])),
                   "MEM")))
        {
            hSourceDS = pahSrcDS[i];
        }
        else
        {
            bDropRef = true;
            GDALProxyPoolDatasetH hProxyDS = GDALProxyPoolDatasetCreate(
                dsFileName, psDatasetProperties->nRasterXSize,
                psDatasetProperties->nRasterYSize, GA_ReadOnly, TRUE,
                pszProjectionRef, psDatasetProperties->gt.data());
            cpl::down_cast<GDALProxyPoolDataset *>(
                GDALDataset::FromHandle(hProxyDS))
                ->SetOpenOptions(papszOpenOptions);

            for (int j = 0;
                 j < nMaxSelectedBandNo +
                         (bAddAlpha && psDatasetProperties->bLastBandIsAlpha
                              ? 1
                              : 0);
                 j++)
            {
                GDALProxyPoolDatasetAddSrcBandDescription(
                    hProxyDS,
                    j < static_cast<int>(asBandProperties.size())
                        ? asBandProperties[j].dataType
                        : GDT_Byte,
                    psDatasetProperties->nBlockXSize,
                    psDatasetProperties->nBlockYSize);
            }
            if (bHasDatasetMask && !bAddAlpha)
            {
                static_cast<GDALProxyPoolRasterBand *>(
                    cpl::down_cast<GDALProxyPoolDataset *>(
                        GDALDataset::FromHandle(hProxyDS))
                        ->GetRasterBand(1))
                    ->AddSrcMaskBandDescription(
                        GDT_Byte, psDatasetProperties->nMaskBlockXSize,
                        psDatasetProperties->nMaskBlockYSize);
            }

            hSourceDS = static_cast<GDALDatasetH>(hProxyDS);
        }

        for (int j = 0;
             j <
             nSelectedBands +
                 (bAddAlpha && psDatasetProperties->bLastBandIsAlpha ? 1 : 0);
             j++)
        {
            VRTSourcedRasterBandH hVRTBand = static_cast<VRTSourcedRasterBandH>(
                poVRTDS->GetRasterBand(j + 1));
            const int nSelBand = j == nSelectedBands ? nSelectedBands + 1
                                                     : panSelectedBandList[j];

            /* Place the raster band at the right position in the VRT */
            VRTSourcedRasterBand *poVRTBand =
                static_cast<VRTSourcedRasterBand *>(hVRTBand);

            VRTSimpleSource *poSimpleSource;
            if (bNoDataFromMask)
            {
                auto poNoDataFromMaskSource = new VRTNoDataFromMaskSource();
                poSimpleSource = poNoDataFromMaskSource;
                poNoDataFromMaskSource->SetParameters(
                    (nVRTNoDataCount > 0)
                        ? ((j < nVRTNoDataCount)
                               ? padfVRTNoData[j]
                               : padfVRTNoData[nVRTNoDataCount - 1])
                        : 0,
                    dfMaskValueThreshold);
            }
            else if (bAllowSrcNoData &&
                     psDatasetProperties->abHasNoData[nSelBand - 1])
            {
                auto poComplexSource = new VRTComplexSource();
                poSimpleSource = poComplexSource;
                poComplexSource->SetNoDataValue(
                    psDatasetProperties->adfNoDataValues[nSelBand - 1]);
            }
            else if (bUseSrcMaskBand &&
                     psDatasetProperties->abHasMaskBand[nSelBand - 1])
            {
                auto poSource = new VRTComplexSource();
                poSource->SetUseMaskBand(true);
                poSimpleSource = poSource;
            }
            else
                poSimpleSource = new VRTSimpleSource();
            if (pszResampling)
                poSimpleSource->SetResampling(pszResampling);
            auto poSrcBand = GDALRasterBand::FromHandle(
                GDALGetRasterBand(hSourceDS, nSelBand));
            poVRTBand->ConfigureSource(poSimpleSource, poSrcBand, FALSE,
                                       dfSrcXOff, dfSrcYOff, dfSrcXSize,
                                       dfSrcYSize, dfDstXOff, dfDstYOff,
                                       dfDstXSize, dfDstYSize);

            if (bWriteAbsolutePath)
                WriteAbsolutePath(poSimpleSource, dsFileName);

            poVRTBand->AddSource(poSimpleSource);
        }

        if (bAddAlpha && !psDatasetProperties->bLastBandIsAlpha)
        {
            VRTSourcedRasterBand *poVRTBand =
                static_cast<VRTSourcedRasterBand *>(
                    poVRTDS->GetRasterBand(nSelectedBands + 1));
            /* Little trick : we use an offset of 255 and a scaling of 0, so
             * that in areas covered */
            /* by the source, the value of the alpha band will be 255, otherwise
             * it will be 0 */
            poVRTBand->AddComplexSource(
                GDALRasterBand::FromHandle(GDALGetRasterBand(hSourceDS, 1)),
                dfSrcXOff, dfSrcYOff, dfSrcXSize, dfSrcYSize, dfDstXOff,
                dfDstYOff, dfDstXSize, dfDstYSize, 255, 0, VRT_NODATA_UNSET);
        }
        else if (bHasDatasetMask)
        {
            VRTSimpleSource *poSource;
            if (bUseSrcMaskBand)
            {
                auto poComplexSource = new VRTComplexSource();
                poComplexSource->SetUseMaskBand(true);
                poSource = poComplexSource;
            }
            else
            {
                poSource = new VRTSimpleSource();
            }
            if (pszResampling)
                poSource->SetResampling(pszResampling);
            assert(poMaskVRTBand);
            poMaskVRTBand->ConfigureSource(
                poSource,
                static_cast<GDALRasterBand *>(GDALGetRasterBand(hSourceDS, 1)),
                TRUE, dfSrcXOff, dfSrcYOff, dfSrcXSize, dfSrcYSize, dfDstXOff,
                dfDstYOff, dfDstXSize, dfDstYSize);

            if (bWriteAbsolutePath)
                WriteAbsolutePath(poSource, dsFileName);

            poMaskVRTBand->AddSource(poSource);
        }

        if (bDropRef)
        {
            GDALDereferenceDataset(hSourceDS);
        }
    }

    for (int i : anIdxValidDatasets)
    {
        const DatasetProperty *psDatasetProperties = &asDatasetProperties[i];
        for (auto oIter = anOverviewFactorsSet.begin();
             oIter != anOverviewFactorsSet.end();)
        {
            const int nGlobalOvrFactor = *oIter;
            auto oIterNext = oIter;
            ++oIterNext;

            if (psDatasetProperties->nRasterXSize / nGlobalOvrFactor < 128 &&
                psDatasetProperties->nRasterYSize / nGlobalOvrFactor < 128)
            {
                break;
            }
            if (std::find(psDatasetProperties->anOverviewFactors.begin(),
                          psDatasetProperties->anOverviewFactors.end(),
                          nGlobalOvrFactor) ==
                psDatasetProperties->anOverviewFactors.end())
            {
                anOverviewFactorsSet.erase(oIter);
            }

            oIter = oIterNext;
        }
    }
    if (!anOverviewFactorsSet.empty() &&
        CPLTestBool(CPLGetConfigOption("VRT_VIRTUAL_OVERVIEWS", "YES")))
    {
        std::vector<int> anOverviewFactors;
        anOverviewFactors.insert(anOverviewFactors.end(),
                                 anOverviewFactorsSet.begin(),
                                 anOverviewFactorsSet.end());
        const char *const apszOptions[] = {"VRT_VIRTUAL_OVERVIEWS=YES",
                                           nullptr};
        poVRTDS->BuildOverviews(pszResampling ? pszResampling : "nearest",
                                static_cast<int>(anOverviewFactors.size()),
                                &anOverviewFactors[0], 0, nullptr, nullptr,
                                nullptr, apszOptions);
    }
}

/************************************************************************/
/*                             Build()                                  */
/************************************************************************/

std::unique_ptr<GDALDataset> VRTBuilder::Build(GDALProgressFunc pfnProgress,
                                               void *pProgressData)
{
    if (bHasRunBuild)
        return nullptr;
    bHasRunBuild = TRUE;

    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    bUserExtent = (minX != 0 || minY != 0 || maxX != 0 || maxY != 0);
    if (bUserExtent)
    {
        if (minX >= maxX || minY >= maxY)
        {
            CPLError(CE_Failure, CPLE_IllegalArg, "Invalid user extent");
            return nullptr;
        }
    }

    if (resolutionStrategy == USER_RESOLUTION)
    {
        if (we_res <= 0 || ns_res <= 0)
        {
            CPLError(CE_Failure, CPLE_IllegalArg, "Invalid user resolution");
            return nullptr;
        }

        /* We work with negative north-south resolution in all the following
         * code */
        ns_res = -ns_res;
    }
    else
    {
        we_res = ns_res = 0;
    }

    asDatasetProperties.resize(nInputFiles);

    if (pszSrcNoData != nullptr)
    {
        if (EQUAL(pszSrcNoData, "none"))
        {
            bAllowSrcNoData = FALSE;
        }
        else
        {
            char **papszTokens = CSLTokenizeString(pszSrcNoData);
            nSrcNoDataCount = CSLCount(papszTokens);
            padfSrcNoData = static_cast<double *>(
                CPLMalloc(sizeof(double) * nSrcNoDataCount));
            for (int i = 0; i < nSrcNoDataCount; i++)
            {
                if (!ArgIsNumeric(papszTokens[i]) &&
                    !EQUAL(papszTokens[i], "nan") &&
                    !EQUAL(papszTokens[i], "-inf") &&
                    !EQUAL(papszTokens[i], "inf"))
                {
                    CPLError(CE_Failure, CPLE_IllegalArg,
                             "Invalid -srcnodata value");
                    CSLDestroy(papszTokens);
                    return nullptr;
                }
                padfSrcNoData[i] = CPLAtofM(papszTokens[i]);
            }
            CSLDestroy(papszTokens);
        }
    }

    if (pszVRTNoData != nullptr)
    {
        if (EQUAL(pszVRTNoData, "none"))
        {
            bAllowVRTNoData = FALSE;
        }
        else
        {
            char **papszTokens = CSLTokenizeString(pszVRTNoData);
            nVRTNoDataCount = CSLCount(papszTokens);
            padfVRTNoData = static_cast<double *>(
                CPLMalloc(sizeof(double) * nVRTNoDataCount));
            for (int i = 0; i < nVRTNoDataCount; i++)
            {
                if (!ArgIsNumeric(papszTokens[i]) &&
                    !EQUAL(papszTokens[i], "nan") &&
                    !EQUAL(papszTokens[i], "-inf") &&
                    !EQUAL(papszTokens[i], "inf"))
                {
                    CPLError(CE_Failure, CPLE_IllegalArg,
                             "Invalid -vrtnodata value");
                    CSLDestroy(papszTokens);
                    return nullptr;
                }
                padfVRTNoData[i] = CPLAtofM(papszTokens[i]);
            }
            CSLDestroy(papszTokens);
        }
    }

    bool bFoundValid = false;
    for (int i = 0; ppszInputFilenames != nullptr && i < nInputFiles; i++)
    {
        const char *dsFileName = ppszInputFilenames[i];

        if (!pfnProgress(1.0 * (i + 1) / nInputFiles, nullptr, pProgressData))
        {
            return nullptr;
        }

        GDALDatasetH hDS = (pahSrcDS)
                               ? pahSrcDS[i]
                               : GDALOpenEx(dsFileName, GDAL_OF_RASTER, nullptr,
                                            papszOpenOptions, nullptr);
        asDatasetProperties[i].isFileOK = FALSE;

        if (hDS)
        {
            const auto osErrorMsg = AnalyseRaster(hDS, &asDatasetProperties[i]);
            if (osErrorMsg.empty())
            {
                asDatasetProperties[i].isFileOK = TRUE;
                bFoundValid = true;
                bFirst = FALSE;
            }
            if (pahSrcDS == nullptr)
                GDALClose(hDS);
            if (!osErrorMsg.empty() && osErrorMsg != "SILENTLY_IGNORE")
            {
                if (bStrict)
                {
                    CPLError(CE_Failure, CPLE_AppDefined, "%s",
                             osErrorMsg.c_str());
                    return nullptr;
                }
                else
                {
                    CPLError(CE_Warning, CPLE_AppDefined, "%s Skipping %s",
                             osErrorMsg.c_str(), dsFileName);
                }
            }
        }
        else
        {
            if (bStrict)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Can't open %s.",
                         dsFileName);
                return nullptr;
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Can't open %s. Skipping it", dsFileName);
            }
        }
    }

    if (!bFoundValid)
        return nullptr;

    if (bHasGeoTransform)
    {
        if (bTargetAlignedPixels)
        {
            minX = floor(minX / we_res) * we_res;
            maxX = ceil(maxX / we_res) * we_res;
            minY = floor(minY / -ns_res) * -ns_res;
            maxY = ceil(maxY / -ns_res) * -ns_res;
        }

        nRasterXSize = static_cast<int>(0.5 + (maxX - minX) / we_res);
        nRasterYSize = static_cast<int>(0.5 + (maxY - minY) / -ns_res);
    }

    if (nRasterXSize == 0 || nRasterYSize == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Computed VRT dimension is invalid. You've probably "
                 "specified inappropriate resolution.");
        return nullptr;
    }

    auto poDS = VRTDataset::CreateVRTDataset(pszOutputFilename, nRasterXSize,
                                             nRasterYSize, 0, GDT_Unknown,
                                             aosCreateOptions.List());
    if (!poDS)
    {
        return nullptr;
    }

    if (pszOutputSRS)
    {
        poDS->SetProjection(pszOutputSRS);
    }
    else if (pszProjectionRef)
    {
        poDS->SetProjection(pszProjectionRef);
    }

    if (bHasGeoTransform)
    {
        GDALGeoTransform gt;
        gt[GEOTRSFRM_TOPLEFT_X] = minX;
        gt[GEOTRSFRM_WE_RES] = we_res;
        gt[GEOTRSFRM_ROTATION_PARAM1] = 0;
        gt[GEOTRSFRM_TOPLEFT_Y] = maxY;
        gt[GEOTRSFRM_ROTATION_PARAM2] = 0;
        gt[GEOTRSFRM_NS_RES] = ns_res;
        poDS->SetGeoTransform(gt);
    }

    if (bSeparate)
    {
        CreateVRTSeparate(poDS.get());
    }
    else
    {
        CreateVRTNonSeparate(poDS.get());
    }

    return poDS;
}

/************************************************************************/
/*                        add_file_to_list()                            */
/************************************************************************/

static bool add_file_to_list(const char *filename, const char *tile_index,
                             CPLStringList &aosList)
{

    if (EQUAL(CPLGetExtensionSafe(filename).c_str(), "SHP"))
    {
        /* Handle gdaltindex Shapefile as a special case */
        auto poDS = std::unique_ptr<GDALDataset>(GDALDataset::Open(filename));
        if (poDS == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to open shapefile `%s'.", filename);
            return false;
        }

        auto poLayer = poDS->GetLayer(0);
        const auto poFDefn = poLayer->GetLayerDefn();

        if (poFDefn->GetFieldIndex("LOCATION") >= 0 &&
            strcmp("LOCATION", tile_index) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "This shapefile seems to be a tile index of "
                     "OGR features and not GDAL products.");
        }
        const int ti_field = poFDefn->GetFieldIndex(tile_index);
        if (ti_field < 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to find field `%s' in DBF file `%s'.", tile_index,
                     filename);
            return false;
        }

        /* Load in memory existing file names in SHP */
        const auto nTileIndexFiles = poLayer->GetFeatureCount(TRUE);
        if (nTileIndexFiles == 0)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Tile index %s is empty. Skipping it.", filename);
            return true;
        }
        if (nTileIndexFiles > 100 * 1024 * 1024)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Too large feature count in tile index");
            return false;
        }

        for (auto &&poFeature : poLayer)
        {
            aosList.AddString(poFeature->GetFieldAsString(ti_field));
        }
    }
    else
    {
        aosList.AddString(filename);
    }

    return true;
}

/************************************************************************/
/*                        GDALBuildVRTOptions                           */
/************************************************************************/

/** Options for use with GDALBuildVRT(). GDALBuildVRTOptions* must be allocated
 * and freed with GDALBuildVRTOptionsNew() and GDALBuildVRTOptionsFree()
 * respectively.
 */
struct GDALBuildVRTOptions
{
    std::string osProgramName = "gdalbuildvrt";
    std::string osTileIndex = "location";
    bool bStrict = false;
    std::string osResolution{};
    bool bSeparate = false;
    bool bAllowProjectionDifference = false;
    double we_res = 0;
    double ns_res = 0;
    bool bTargetAlignedPixels = false;
    double xmin = 0;
    double ymin = 0;
    double xmax = 0;
    double ymax = 0;
    bool bAddAlpha = false;
    bool bHideNoData = false;
    int nSubdataset = -1;
    std::string osSrcNoData{};
    std::string osVRTNoData{};
    std::string osOutputSRS{};
    std::vector<int> anSelectedBandList{};
    std::string osResampling{};
    CPLStringList aosOpenOptions{};
    CPLStringList aosCreateOptions{};
    bool bUseSrcMaskBand = true;
    bool bNoDataFromMask = false;
    double dfMaskValueThreshold = 0;
    bool bWriteAbsolutePath = false;
    std::string osPixelFunction{};
    CPLStringList aosPixelFunctionArgs{};

    /*! allow or suppress progress monitor and other non-error output */
    bool bQuiet = true;

    /*! the progress function to use */
    GDALProgressFunc pfnProgress = GDALDummyProgress;

    /*! pointer to the progress data variable */
    void *pProgressData = nullptr;
};

/************************************************************************/
/*                           GDALBuildVRT()                             */
/************************************************************************/

/* clang-format off */
/**
 * Build a VRT from a list of datasets.
 *
 * This is the equivalent of the
 * <a href="/programs/gdalbuildvrt.html">gdalbuildvrt</a> utility.
 *
 * GDALBuildVRTOptions* must be allocated and freed with
 * GDALBuildVRTOptionsNew() and GDALBuildVRTOptionsFree() respectively. pahSrcDS
 * and papszSrcDSNames cannot be used at the same time.
 *
 * @param pszDest the destination dataset path.
 * @param nSrcCount the number of input datasets.
 * @param pahSrcDS the list of input datasets (or NULL, exclusive with
 * papszSrcDSNames). For practical purposes, the type
 * of this argument should be considered as "const GDALDatasetH* const*", that
 * is neither the array nor its values are mutated by this function.
 * @param papszSrcDSNames the list of input dataset names (or NULL, exclusive
 * with pahSrcDS)
 * @param psOptionsIn the options struct returned by GDALBuildVRTOptionsNew() or
 * NULL.
 * @param pbUsageError pointer to a integer output variable to store if any
 * usage error has occurred.
 * @return the output dataset (new dataset that must be closed using
 * GDALClose()) or NULL in case of error. If using pahSrcDS, the returned VRT
 * dataset has a reference to each pahSrcDS[] element. Hence pahSrcDS[] elements
 * should be closed after the returned dataset if using GDALClose().
 * A safer alternative is to use GDALReleaseDataset() instead of using
 * GDALClose(), in which case you can close datasets in any order.

 *
 * @since GDAL 2.1
 */
/* clang-format on */

GDALDatasetH GDALBuildVRT(const char *pszDest, int nSrcCount,
                          GDALDatasetH *pahSrcDS,
                          const char *const *papszSrcDSNames,
                          const GDALBuildVRTOptions *psOptionsIn,
                          int *pbUsageError)
{
    if (pszDest == nullptr)
        pszDest = "";

    if (nSrcCount == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "No input dataset specified.");

        if (pbUsageError)
            *pbUsageError = TRUE;
        return nullptr;
    }

    // cppcheck-suppress unreadVariable
    GDALBuildVRTOptions sOptions(psOptionsIn ? *psOptionsIn
                                             : GDALBuildVRTOptions());

    if (sOptions.we_res != 0 && sOptions.ns_res != 0 &&
        !sOptions.osResolution.empty() &&
        !EQUAL(sOptions.osResolution.c_str(), "user"))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "-tr option is not compatible with -resolution %s",
                 sOptions.osResolution.c_str());
        if (pbUsageError)
            *pbUsageError = TRUE;
        return nullptr;
    }

    if (sOptions.bTargetAlignedPixels && sOptions.we_res == 0 &&
        sOptions.ns_res == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "-tap option cannot be used without using -tr");
        if (pbUsageError)
            *pbUsageError = TRUE;
        return nullptr;
    }

    if (sOptions.bAddAlpha && sOptions.bSeparate)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "-addalpha option is not compatible with -separate.");
        if (pbUsageError)
            *pbUsageError = TRUE;
        return nullptr;
    }

    ResolutionStrategy eStrategy = AVERAGE_RESOLUTION;
    if (sOptions.osResolution.empty() ||
        EQUAL(sOptions.osResolution.c_str(), "user"))
    {
        if (sOptions.we_res != 0 || sOptions.ns_res != 0)
            eStrategy = USER_RESOLUTION;
        else if (EQUAL(sOptions.osResolution.c_str(), "user"))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "-tr option must be used with -resolution user.");
            if (pbUsageError)
                *pbUsageError = TRUE;
            return nullptr;
        }
    }
    else if (EQUAL(sOptions.osResolution.c_str(), "average"))
        eStrategy = AVERAGE_RESOLUTION;
    else if (EQUAL(sOptions.osResolution.c_str(), "highest"))
        eStrategy = HIGHEST_RESOLUTION;
    else if (EQUAL(sOptions.osResolution.c_str(), "lowest"))
        eStrategy = LOWEST_RESOLUTION;
    else if (EQUAL(sOptions.osResolution.c_str(), "same"))
        eStrategy = SAME_RESOLUTION;
    else if (EQUAL(sOptions.osResolution.c_str(), "common"))
        eStrategy = COMMON_RESOLUTION;

    /* If -srcnodata is specified, use it as the -vrtnodata if the latter is not
     */
    /* specified */
    if (!sOptions.osSrcNoData.empty() && sOptions.osVRTNoData.empty())
        sOptions.osVRTNoData = sOptions.osSrcNoData;

    VRTBuilder oBuilder(
        sOptions.bStrict, pszDest, nSrcCount, papszSrcDSNames, pahSrcDS,
        sOptions.anSelectedBandList.empty()
            ? nullptr
            : sOptions.anSelectedBandList.data(),
        static_cast<int>(sOptions.anSelectedBandList.size()), eStrategy,
        sOptions.we_res, sOptions.ns_res, sOptions.bTargetAlignedPixels,
        sOptions.xmin, sOptions.ymin, sOptions.xmax, sOptions.ymax,
        sOptions.bSeparate, sOptions.bAllowProjectionDifference,
        sOptions.bAddAlpha, sOptions.bHideNoData, sOptions.nSubdataset,
        sOptions.osSrcNoData.empty() ? nullptr : sOptions.osSrcNoData.c_str(),
        sOptions.osVRTNoData.empty() ? nullptr : sOptions.osVRTNoData.c_str(),
        sOptions.bUseSrcMaskBand, sOptions.bNoDataFromMask,
        sOptions.dfMaskValueThreshold,
        sOptions.osOutputSRS.empty() ? nullptr : sOptions.osOutputSRS.c_str(),
        sOptions.osResampling.empty() ? nullptr : sOptions.osResampling.c_str(),
        sOptions.osPixelFunction.empty() ? nullptr
                                         : sOptions.osPixelFunction.c_str(),
        sOptions.aosPixelFunctionArgs, sOptions.aosOpenOptions.List(),
        sOptions.aosCreateOptions, sOptions.bWriteAbsolutePath);
    oBuilder.m_osProgramName = sOptions.osProgramName;

    return GDALDataset::ToHandle(
        oBuilder.Build(sOptions.pfnProgress, sOptions.pProgressData).release());
}

/************************************************************************/
/*                             SanitizeSRS                              */
/************************************************************************/

static char *SanitizeSRS(const char *pszUserInput)

{
    OGRSpatialReferenceH hSRS;
    char *pszResult = nullptr;

    CPLErrorReset();

    hSRS = OSRNewSpatialReference(nullptr);
    if (OSRSetFromUserInput(hSRS, pszUserInput) == OGRERR_NONE)
        OSRExportToWkt(hSRS, &pszResult);
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Translating SRS failed:\n%s",
                 pszUserInput);
    }

    OSRDestroySpatialReference(hSRS);

    return pszResult;
}

/************************************************************************/
/*                     GDALBuildVRTOptionsGetParser()                    */
/************************************************************************/

static std::unique_ptr<GDALArgumentParser>
GDALBuildVRTOptionsGetParser(GDALBuildVRTOptions *psOptions,
                             GDALBuildVRTOptionsForBinary *psOptionsForBinary)
{
    auto argParser = std::make_unique<GDALArgumentParser>(
        "gdalbuildvrt", /* bForBinary=*/psOptionsForBinary != nullptr);

    argParser->add_description(_("Builds a VRT from a list of datasets."));

    argParser->add_epilog(_(
        "\n"
        "e.g.\n"
        "  % gdalbuildvrt doq_index.vrt doq/*.tif\n"
        "  % gdalbuildvrt -input_file_list my_list.txt doq_index.vrt\n"
        "\n"
        "NOTES:\n"
        "  o With -separate, each files goes into a separate band in the VRT "
        "band.\n"
        "    Otherwise, the files are considered as tiles of a larger mosaic.\n"
        "  o -b option selects a band to add into vrt.  Multiple bands can be "
        "listed.\n"
        "    By default all bands are queried.\n"
        "  o The default tile index field is 'location' unless otherwise "
        "specified by\n"
        "    -tileindex.\n"
        "  o In case the resolution of all input files is not the same, the "
        "-resolution\n"
        "    flag enable the user to control the way the output resolution is "
        "computed.\n"
        "    Average is the default.\n"
        "  o Input files may be any valid GDAL dataset or a GDAL raster tile "
        "index.\n"
        "  o For a GDAL raster tile index, all entries will be added to the "
        "VRT.\n"
        "  o If one GDAL dataset is made of several subdatasets and has 0 "
        "raster bands,\n"
        "    its datasets will be added to the VRT rather than the dataset "
        "itself.\n"
        "    Single subdataset could be selected by its number using the -sd "
        "option.\n"
        "  o By default, only datasets of same projection and band "
        "characteristics\n"
        "    may be added to the VRT.\n"
        "\n"
        "For more details, consult "
        "https://gdal.org/programs/gdalbuildvrt.html"));

    argParser->add_quiet_argument(
        psOptionsForBinary ? &psOptionsForBinary->bQuiet : nullptr);

    {
        auto &group = argParser->add_mutually_exclusive_group();

        group.add_argument("-strict")
            .flag()
            .store_into(psOptions->bStrict)
            .help(_("Turn warnings as failures."));

        group.add_argument("-non_strict")
            .flag()
            .action([psOptions](const std::string &)
                    { psOptions->bStrict = false; })
            .help(_("Skip source datasets that have issues with warnings, and "
                    "continue processing."));
    }

    argParser->add_argument("-tile_index")
        .metavar("<field_name>")
        .store_into(psOptions->osTileIndex)
        .help(_("Use the specified value as the tile index field, instead of "
                "the default value which is 'location'."));

    argParser->add_argument("-resolution")
        .metavar("user|average|common|highest|lowest|same")
        .action(
            [psOptions](const std::string &s)
            {
                psOptions->osResolution = s;
                if (!EQUAL(psOptions->osResolution.c_str(), "user") &&
                    !EQUAL(psOptions->osResolution.c_str(), "average") &&
                    !EQUAL(psOptions->osResolution.c_str(), "highest") &&
                    !EQUAL(psOptions->osResolution.c_str(), "lowest") &&
                    !EQUAL(psOptions->osResolution.c_str(), "same") &&
                    !EQUAL(psOptions->osResolution.c_str(), "common"))
                {
                    throw std::invalid_argument(
                        CPLSPrintf("Illegal resolution value (%s).",
                                   psOptions->osResolution.c_str()));
                }
            })
        .help(_("Control the way the output resolution is computed."));

    argParser->add_argument("-tr")
        .metavar("<xres> <yes>")
        .nargs(2)
        .scan<'g', double>()
        .help(_("Set target resolution."));

    if (psOptionsForBinary)
    {
        argParser->add_argument("-input_file_list")
            .metavar("<filename>")
            .action(
                [psOptions, psOptionsForBinary](const std::string &s)
                {
                    const char *input_file_list = s.c_str();
                    auto f = VSIVirtualHandleUniquePtr(
                        VSIFOpenL(input_file_list, "r"));
                    if (f)
                    {
                        while (1)
                        {
                            const char *filename = CPLReadLineL(f.get());
                            if (filename == nullptr)
                                break;
                            if (!add_file_to_list(
                                    filename, psOptions->osTileIndex.c_str(),
                                    psOptionsForBinary->aosSrcFiles))
                            {
                                throw std::invalid_argument(
                                    std::string("Cannot add ")
                                        .append(filename)
                                        .append(" to input file list"));
                            }
                        }
                    }
                })
            .help(_("Text file with an input filename on each line"));
    }

    {
        auto &group = argParser->add_mutually_exclusive_group();

        group.add_argument("-separate")
            .flag()
            .store_into(psOptions->bSeparate)
            .help(_("Place each input file into a separate band."));

        group.add_argument("-pixel-function")
            .metavar("<function>")
            .action(
                [psOptions](const std::string &s)
                {
                    auto *poPixFun =
                        VRTDerivedRasterBand::GetPixelFunction(s.c_str());
                    if (poPixFun == nullptr)
                    {
                        throw std::invalid_argument(
                            s + " is not a registered pixel function.");
                    }

                    psOptions->osPixelFunction = s;
                })

            .help("Function to calculate value from overlapping inputs");
    }

    argParser->add_argument("-pixel-function-arg")
        .metavar("<NAME>=<VALUE>")
        .append()
        .action([psOptions](const std::string &s)
                { psOptions->aosPixelFunctionArgs.AddString(s); })
        .help(_("Pixel function argument(s)"));

    argParser->add_argument("-allow_projection_difference")
        .flag()
        .store_into(psOptions->bAllowProjectionDifference)
        .help(_("Accept source files not in the same projection (but without "
                "reprojecting them!)."));

    argParser->add_argument("-sd")
        .metavar("<n>")
        .store_into(psOptions->nSubdataset)
        .help(_("Use subdataset of specified index (starting at 1), instead of "
                "the source dataset itself."));

    argParser->add_argument("-tap")
        .flag()
        .store_into(psOptions->bTargetAlignedPixels)
        .help(_("Align the coordinates of the extent of the output file to the "
                "values of the resolution."));

    argParser->add_argument("-te")
        .metavar("<xmin> <ymin> <xmax> <ymax>")
        .nargs(4)
        .scan<'g', double>()
        .help(_("Set georeferenced extents of output file to be created."));

    argParser->add_argument("-addalpha")
        .flag()
        .store_into(psOptions->bAddAlpha)
        .help(_("Adds an alpha mask band to the VRT when the source raster "
                "have none."));

    argParser->add_argument("-b")
        .metavar("<band>")
        .append()
        .store_into(psOptions->anSelectedBandList)
        .help(_("Specify input band(s) number."));

    argParser->add_argument("-hidenodata")
        .flag()
        .store_into(psOptions->bHideNoData)
        .help(_("Makes the VRT band not report the NoData."));

    if (psOptionsForBinary)
    {
        argParser->add_argument("-overwrite")
            .flag()
            .store_into(psOptionsForBinary->bOverwrite)
            .help(_("Overwrite the VRT if it already exists."));
    }

    argParser->add_argument("-srcnodata")
        .metavar("\"<value>[ <value>]...\"")
        .store_into(psOptions->osSrcNoData)
        .help(_("Set nodata values for input bands."));

    argParser->add_argument("-vrtnodata")
        .metavar("\"<value>[ <value>]...\"")
        .store_into(psOptions->osVRTNoData)
        .help(_("Set nodata values at the VRT band level."));

    argParser->add_argument("-a_srs")
        .metavar("<srs_def>")
        .action(
            [psOptions](const std::string &s)
            {
                char *pszSRS = SanitizeSRS(s.c_str());
                if (pszSRS == nullptr)
                {
                    throw std::invalid_argument("Invalid value for -a_srs");
                }
                psOptions->osOutputSRS = pszSRS;
                CPLFree(pszSRS);
            })
        .help(_("Override the projection for the output file.."));

    argParser->add_argument("-r")
        .metavar("nearest|bilinear|cubic|cubicspline|lanczos|average|mode")
        .store_into(psOptions->osResampling)
        .help(_("Resampling algorithm."));

    argParser->add_open_options_argument(&psOptions->aosOpenOptions);

    argParser->add_creation_options_argument(psOptions->aosCreateOptions);

    argParser->add_argument("-write_absolute_path")
        .flag()
        .store_into(psOptions->bWriteAbsolutePath)
        .help(_("Write the absolute path of the raster files in the tile index "
                "file."));

    argParser->add_argument("-ignore_srcmaskband")
        .flag()
        .action([psOptions](const std::string &)
                { psOptions->bUseSrcMaskBand = false; })
        .help(_("Cause mask band of sources will not be taken into account."));

    argParser->add_argument("-nodata_max_mask_threshold")
        .metavar("<threshold>")
        .scan<'g', double>()
        .action(
            [psOptions](const std::string &s)
            {
                psOptions->bNoDataFromMask = true;
                psOptions->dfMaskValueThreshold = CPLAtofM(s.c_str());
            })
        .help(_("Replaces the value of the source with the value of -vrtnodata "
                "when the value of the mask band of the source is less or "
                "equal to the threshold."));

    argParser->add_argument("-program_name")
        .store_into(psOptions->osProgramName)
        .hidden();

    if (psOptionsForBinary)
    {
        if (psOptionsForBinary->osDstFilename.empty())
        {
            // We normally go here, unless undocumented -o switch is used
            argParser->add_argument("vrt_dataset_name")
                .metavar("<vrt_dataset_name>")
                .store_into(psOptionsForBinary->osDstFilename)
                .help(_("Output VRT."));
        }

        argParser->add_argument("src_dataset_name")
            .metavar("<src_dataset_name>")
            .nargs(argparse::nargs_pattern::any)
            .action(
                [psOptions, psOptionsForBinary](const std::string &s)
                {
                    if (!add_file_to_list(s.c_str(),
                                          psOptions->osTileIndex.c_str(),
                                          psOptionsForBinary->aosSrcFiles))
                    {
                        throw std::invalid_argument(
                            std::string("Cannot add ")
                                .append(s)
                                .append(" to input file list"));
                    }
                })
            .help(_("Input dataset(s)."));
    }

    return argParser;
}

/************************************************************************/
/*                       GDALBuildVRTGetParserUsage()                   */
/************************************************************************/

std::string GDALBuildVRTGetParserUsage()
{
    try
    {
        GDALBuildVRTOptions sOptions;
        GDALBuildVRTOptionsForBinary sOptionsForBinary;
        auto argParser =
            GDALBuildVRTOptionsGetParser(&sOptions, &sOptionsForBinary);
        return argParser->usage();
    }
    catch (const std::exception &err)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unexpected exception: %s",
                 err.what());
        return std::string();
    }
}

/************************************************************************/
/*                             GDALBuildVRTOptionsNew()                  */
/************************************************************************/

/**
 * Allocates a GDALBuildVRTOptions struct.
 *
 * @param papszArgv NULL terminated list of options (potentially including
 * filename and open options too), or NULL. The accepted options are the ones of
 * the <a href="/programs/gdalbuildvrt.html">gdalbuildvrt</a> utility.
 * @param psOptionsForBinary (output) may be NULL (and should generally be
 * NULL), otherwise (gdalbuildvrt_bin.cpp use case) must be allocated with
 * GDALBuildVRTOptionsForBinaryNew() prior to this function. Will be filled
 * with potentially present filename, open options,...
 * @return pointer to the allocated GDALBuildVRTOptions struct. Must be freed
 * with GDALBuildVRTOptionsFree().
 *
 * @since GDAL 2.1
 */

GDALBuildVRTOptions *
GDALBuildVRTOptionsNew(char **papszArgv,
                       GDALBuildVRTOptionsForBinary *psOptionsForBinary)
{
    auto psOptions = std::make_unique<GDALBuildVRTOptions>();

    CPLStringList aosArgv;
    const int nArgc = CSLCount(papszArgv);
    for (int i = 0;
         i < nArgc && papszArgv != nullptr && papszArgv[i] != nullptr; i++)
    {
        if (psOptionsForBinary && EQUAL(papszArgv[i], "-o") && i + 1 < nArgc &&
            papszArgv[i + 1] != nullptr)
        {
            // Undocumented alternate way of specifying the destination file
            psOptionsForBinary->osDstFilename = papszArgv[i + 1];
            ++i;
        }
        // argparser will be confused if the value of a string argument
        // starts with a negative sign.
        else if (EQUAL(papszArgv[i], "-srcnodata") && i + 1 < nArgc)
        {
            ++i;
            psOptions->osSrcNoData = papszArgv[i];
        }
        // argparser will be confused if the value of a string argument
        // starts with a negative sign.
        else if (EQUAL(papszArgv[i], "-vrtnodata") && i + 1 < nArgc)
        {
            ++i;
            psOptions->osVRTNoData = papszArgv[i];
        }

        else
        {
            aosArgv.AddString(papszArgv[i]);
        }
    }

    try
    {
        auto argParser =
            GDALBuildVRTOptionsGetParser(psOptions.get(), psOptionsForBinary);

        argParser->parse_args_without_binary_name(aosArgv.List());

        if (auto adfTargetRes = argParser->present<std::vector<double>>("-tr"))
        {
            psOptions->we_res = (*adfTargetRes)[0];
            psOptions->ns_res = (*adfTargetRes)[1];
        }

        if (auto oTE = argParser->present<std::vector<double>>("-te"))
        {
            psOptions->xmin = (*oTE)[0];
            psOptions->ymin = (*oTE)[1];
            psOptions->xmax = (*oTE)[2];
            psOptions->ymax = (*oTE)[3];
        }

        if (psOptions->osPixelFunction.empty() &&
            !psOptions->aosPixelFunctionArgs.empty())
        {
            throw std::runtime_error(
                "Pixel function arguments provided without a pixel function");
        }

        return psOptions.release();
    }
    catch (const std::exception &err)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", err.what());
        return nullptr;
    }
}

/************************************************************************/
/*                        GDALBuildVRTOptionsFree()                     */
/************************************************************************/

/**
 * Frees the GDALBuildVRTOptions struct.
 *
 * @param psOptions the options struct for GDALBuildVRT().
 *
 * @since GDAL 2.1
 */

void GDALBuildVRTOptionsFree(GDALBuildVRTOptions *psOptions)
{
    delete psOptions;
}

/************************************************************************/
/*                 GDALBuildVRTOptionsSetProgress()                    */
/************************************************************************/

/**
 * Set a progress function.
 *
 * @param psOptions the options struct for GDALBuildVRT().
 * @param pfnProgress the progress callback.
 * @param pProgressData the user data for the progress callback.
 *
 * @since GDAL 2.1
 */

void GDALBuildVRTOptionsSetProgress(GDALBuildVRTOptions *psOptions,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData)
{
    psOptions->pfnProgress = pfnProgress ? pfnProgress : GDALDummyProgress;
    psOptions->pProgressData = pProgressData;
    if (pfnProgress == GDALTermProgress)
        psOptions->bQuiet = false;
}
