/******************************************************************************
 *
 * Project:  PDS 4 Driver; Planetary Data System Format
 * Purpose:  Implementation of PDS4Dataset
 * Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2017, Hobu Inc
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_vsi_error.h"
#include "gdal_proxy.h"
#include "rawdataset.h"
#include "vrtdataset.h"
#include "ogrsf_frmts.h"
#include "ogr_spatialref.h"
#include "gdal_priv_templates.hpp"
#include "ogreditablelayer.h"
#include "pds4dataset.h"
#include "pdsdrivercore.h"

#ifdef EMBED_RESOURCE_FILES
#include "embedded_resources.h"
#endif

#include <cstdlib>
#include <vector>
#include <algorithm>

#define TIFF_GEOTIFF_STRING "TIFF 6.0"
#define BIGTIFF_GEOTIFF_STRING "TIFF 6.0"
#define PREEXISTING_BINARY_FILE                                                \
    "Binary file pre-existing PDS4 label. This comment is used by GDAL to "    \
    "avoid deleting the binary file when the label is deleted. Keep it to "    \
    "preserve this behavior."

#define CURRENT_CART_VERSION "1G00_1950"

extern "C" void GDALRegister_PDS4();

/************************************************************************/
/*                        PDS4WrapperRasterBand()                      */
/************************************************************************/

PDS4WrapperRasterBand::PDS4WrapperRasterBand(GDALRasterBand *poBaseBandIn)
    : m_poBaseBand(poBaseBandIn), m_bHasOffset(false), m_bHasScale(false),
      m_bHasNoData(false), m_dfOffset(0.0), m_dfScale(1.0), m_dfNoData(0.0)
{
    eDataType = m_poBaseBand->GetRasterDataType();
    m_poBaseBand->GetBlockSize(&nBlockXSize, &nBlockYSize);
}

/************************************************************************/
/*                             SetMaskBand()                            */
/************************************************************************/

void PDS4WrapperRasterBand::SetMaskBand(
    std::unique_ptr<GDALRasterBand> poMaskBand)
{
    poMask.reset(std::move(poMaskBand));
    nMaskFlags = 0;
}

/************************************************************************/
/*                              GetOffset()                             */
/************************************************************************/

double PDS4WrapperRasterBand::GetOffset(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = m_bHasOffset;
    return m_dfOffset;
}

/************************************************************************/
/*                              GetScale()                              */
/************************************************************************/

double PDS4WrapperRasterBand::GetScale(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = m_bHasScale;
    return m_dfScale;
}

/************************************************************************/
/*                              SetOffset()                             */
/************************************************************************/

CPLErr PDS4WrapperRasterBand::SetOffset(double dfNewOffset)
{
    m_dfOffset = dfNewOffset;
    m_bHasOffset = true;

    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (poGDS->m_poExternalDS && eAccess == GA_Update)
        poGDS->m_poExternalDS->GetRasterBand(nBand)->SetOffset(dfNewOffset);

    return CE_None;
}

/************************************************************************/
/*                              SetScale()                              */
/************************************************************************/

CPLErr PDS4WrapperRasterBand::SetScale(double dfNewScale)
{
    m_dfScale = dfNewScale;
    m_bHasScale = true;

    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (poGDS->m_poExternalDS && eAccess == GA_Update)
        poGDS->m_poExternalDS->GetRasterBand(nBand)->SetScale(dfNewScale);

    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double PDS4WrapperRasterBand::GetNoDataValue(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = m_bHasNoData;
    return m_dfNoData;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr PDS4WrapperRasterBand::SetNoDataValue(double dfNewNoData)
{
    m_dfNoData = dfNewNoData;
    m_bHasNoData = true;

    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (poGDS->m_poExternalDS && eAccess == GA_Update)
        poGDS->m_poExternalDS->GetRasterBand(nBand)->SetNoDataValue(
            dfNewNoData);

    return CE_None;
}

/************************************************************************/
/*                               Fill()                                 */
/************************************************************************/

CPLErr PDS4WrapperRasterBand::Fill(double dfRealValue, double dfImaginaryValue)
{
    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (poGDS->m_bMustInitImageFile)
    {
        if (!poGDS->InitImageFile())
            return CE_Failure;
    }
    return GDALProxyRasterBand::Fill(dfRealValue, dfImaginaryValue);
}

/************************************************************************/
/*                             IWriteBlock()                             */
/************************************************************************/

CPLErr PDS4WrapperRasterBand::IWriteBlock(int nXBlock, int nYBlock,
                                          void *pImage)

{
    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (poGDS->m_bMustInitImageFile)
    {
        if (!poGDS->InitImageFile())
            return CE_Failure;
    }
    return GDALProxyRasterBand::IWriteBlock(nXBlock, nYBlock, pImage);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr PDS4WrapperRasterBand::IRasterIO(
    GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
    void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    GSpacing nPixelSpace, GSpacing nLineSpace, GDALRasterIOExtraArg *psExtraArg)

{
    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (eRWFlag == GF_Write && poGDS->m_bMustInitImageFile)
    {
        if (!poGDS->InitImageFile())
            return CE_Failure;
    }
    return GDALProxyRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                          pData, nBufXSize, nBufYSize, eBufType,
                                          nPixelSpace, nLineSpace, psExtraArg);
}

/************************************************************************/
/*                       PDS4RawRasterBand()                            */
/************************************************************************/

PDS4RawRasterBand::PDS4RawRasterBand(GDALDataset *l_poDS, int l_nBand,
                                     VSILFILE *l_fpRaw,
                                     vsi_l_offset l_nImgOffset,
                                     int l_nPixelOffset, int l_nLineOffset,
                                     GDALDataType l_eDataType,
                                     RawRasterBand::ByteOrder eByteOrderIn)
    : RawRasterBand(l_poDS, l_nBand, l_fpRaw, l_nImgOffset, l_nPixelOffset,
                    l_nLineOffset, l_eDataType, eByteOrderIn,
                    RawRasterBand::OwnFP::NO),
      m_bHasOffset(false), m_bHasScale(false), m_bHasNoData(false),
      m_dfOffset(0.0), m_dfScale(1.0), m_dfNoData(0.0)
{
}

/************************************************************************/
/*                             SetMaskBand()                            */
/************************************************************************/

void PDS4RawRasterBand::SetMaskBand(std::unique_ptr<GDALRasterBand> poMaskBand)
{
    poMask.reset(std::move(poMaskBand));
    nMaskFlags = 0;
}

/************************************************************************/
/*                              GetOffset()                             */
/************************************************************************/

double PDS4RawRasterBand::GetOffset(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = m_bHasOffset;
    return m_dfOffset;
}

/************************************************************************/
/*                              GetScale()                              */
/************************************************************************/

double PDS4RawRasterBand::GetScale(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = m_bHasScale;
    return m_dfScale;
}

/************************************************************************/
/*                              SetOffset()                             */
/************************************************************************/

CPLErr PDS4RawRasterBand::SetOffset(double dfNewOffset)
{
    m_dfOffset = dfNewOffset;
    m_bHasOffset = true;
    return CE_None;
}

/************************************************************************/
/*                              SetScale()                              */
/************************************************************************/

CPLErr PDS4RawRasterBand::SetScale(double dfNewScale)
{
    m_dfScale = dfNewScale;
    m_bHasScale = true;
    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double PDS4RawRasterBand::GetNoDataValue(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = m_bHasNoData;
    return m_dfNoData;
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr PDS4RawRasterBand::SetNoDataValue(double dfNewNoData)
{
    m_dfNoData = dfNewNoData;
    m_bHasNoData = true;
    return CE_None;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr PDS4RawRasterBand::IWriteBlock(int nXBlock, int nYBlock, void *pImage)

{
    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (poGDS->m_bMustInitImageFile)
    {
        if (!poGDS->InitImageFile())
            return CE_Failure;
    }

    return RawRasterBand::IWriteBlock(nXBlock, nYBlock, pImage);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr PDS4RawRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                    int nXSize, int nYSize, void *pData,
                                    int nBufXSize, int nBufYSize,
                                    GDALDataType eBufType, GSpacing nPixelSpace,
                                    GSpacing nLineSpace,
                                    GDALRasterIOExtraArg *psExtraArg)

{
    PDS4Dataset *poGDS = cpl::down_cast<PDS4Dataset *>(poDS);
    if (eRWFlag == GF_Write && poGDS->m_bMustInitImageFile)
    {
        if (!poGDS->InitImageFile())
            return CE_Failure;
    }

    return RawRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                    pData, nBufXSize, nBufYSize, eBufType,
                                    nPixelSpace, nLineSpace, psExtraArg);
}

/************************************************************************/
/*                            PDS4MaskBand()                            */
/************************************************************************/

PDS4MaskBand::PDS4MaskBand(GDALRasterBand *poBaseBand,
                           const std::vector<double> &adfConstants)
    : m_poBaseBand(poBaseBand), m_pBuffer(nullptr), m_adfConstants(adfConstants)
{
    eDataType = GDT_Byte;
    poBaseBand->GetBlockSize(&nBlockXSize, &nBlockYSize);
    nRasterXSize = poBaseBand->GetXSize();
    nRasterYSize = poBaseBand->GetYSize();
}

/************************************************************************/
/*                           ~PDS4MaskBand()                            */
/************************************************************************/

PDS4MaskBand::~PDS4MaskBand()
{
    VSIFree(m_pBuffer);
}

/************************************************************************/
/*                             FillMask()                               */
/************************************************************************/

template <class T>
static void FillMask(void *pvBuffer, GByte *pabyDst, int nReqXSize,
                     int nReqYSize, int nBlockXSize,
                     const std::vector<double> &adfConstants)
{
    const T *pSrc = static_cast<T *>(pvBuffer);
    std::vector<T> aConstants;
    for (size_t i = 0; i < adfConstants.size(); i++)
    {
        T cst;
        GDALCopyWord(adfConstants[i], cst);
        aConstants.push_back(cst);
    }

    for (int y = 0; y < nReqYSize; y++)
    {
        for (int x = 0; x < nReqXSize; x++)
        {
            const T nSrc = pSrc[y * nBlockXSize + x];
            if (std::find(aConstants.begin(), aConstants.end(), nSrc) !=
                aConstants.end())
            {
                pabyDst[y * nBlockXSize + x] = 0;
            }
            else
            {
                pabyDst[y * nBlockXSize + x] = 255;
            }
        }
    }
}

/************************************************************************/
/*                           IReadBlock()                               */
/************************************************************************/

CPLErr PDS4MaskBand::IReadBlock(int nXBlock, int nYBlock, void *pImage)

{
    const GDALDataType eSrcDT = m_poBaseBand->GetRasterDataType();
    const int nSrcDTSize = GDALGetDataTypeSizeBytes(eSrcDT);
    if (m_pBuffer == nullptr)
    {
        m_pBuffer = VSI_MALLOC3_VERBOSE(nBlockXSize, nBlockYSize, nSrcDTSize);
        if (m_pBuffer == nullptr)
            return CE_Failure;
    }

    int nXOff = nXBlock * nBlockXSize;
    int nReqXSize = nBlockXSize;
    if (nXOff + nReqXSize > nRasterXSize)
        nReqXSize = nRasterXSize - nXOff;
    int nYOff = nYBlock * nBlockYSize;
    int nReqYSize = nBlockYSize;
    if (nYOff + nReqYSize > nRasterYSize)
        nReqYSize = nRasterYSize - nYOff;

    if (m_poBaseBand->RasterIO(GF_Read, nXOff, nYOff, nReqXSize, nReqYSize,
                               m_pBuffer, nReqXSize, nReqYSize, eSrcDT,
                               nSrcDTSize,
                               static_cast<GSpacing>(nSrcDTSize) * nBlockXSize,
                               nullptr) != CE_None)
    {
        return CE_Failure;
    }

    GByte *pabyDst = static_cast<GByte *>(pImage);
    if (eSrcDT == GDT_Byte)
    {
        FillMask<GByte>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                        m_adfConstants);
    }
    else if (eSrcDT == GDT_Int8)
    {
        FillMask<GInt8>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                        m_adfConstants);
    }
    else if (eSrcDT == GDT_UInt16)
    {
        FillMask<GUInt16>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                          m_adfConstants);
    }
    else if (eSrcDT == GDT_Int16)
    {
        FillMask<GInt16>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                         m_adfConstants);
    }
    else if (eSrcDT == GDT_UInt32)
    {
        FillMask<GUInt32>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                          m_adfConstants);
    }
    else if (eSrcDT == GDT_Int32)
    {
        FillMask<GInt32>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                         m_adfConstants);
    }
    else if (eSrcDT == GDT_Float32)
    {
        FillMask<float>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                        m_adfConstants);
    }
    else if (eSrcDT == GDT_Float64)
    {
        FillMask<double>(m_pBuffer, pabyDst, nReqXSize, nReqYSize, nBlockXSize,
                         m_adfConstants);
    }

    return CE_None;
}

/************************************************************************/
/*                            PDS4Dataset()                             */
/************************************************************************/

PDS4Dataset::PDS4Dataset()
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                           ~PDS4Dataset()                             */
/************************************************************************/

PDS4Dataset::~PDS4Dataset()
{
    PDS4Dataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr PDS4Dataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (m_bMustInitImageFile)
        {
            if (!InitImageFile())
                eErr = CE_Failure;
        }

        if (PDS4Dataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        if (m_bCreateHeader || m_bDirtyHeader)
            WriteHeader();
        if (m_fpImage)
            VSIFCloseL(m_fpImage);
        CSLDestroy(m_papszCreationOptions);
        PDS4Dataset::CloseDependentDatasets();

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                        GetRawBinaryLayout()                          */
/************************************************************************/

bool PDS4Dataset::GetRawBinaryLayout(GDALDataset::RawBinaryLayout &sLayout)
{
    if (!RawDataset::GetRawBinaryLayout(sLayout))
        return false;
    sLayout.osRawFilename = m_osImageFilename;
    return true;
}

/************************************************************************/
/*                        CloseDependentDatasets()                      */
/************************************************************************/

int PDS4Dataset::CloseDependentDatasets()
{
    int bHasDroppedRef = GDALPamDataset::CloseDependentDatasets();

    if (m_poExternalDS)
    {
        bHasDroppedRef = FALSE;
        delete m_poExternalDS;
        m_poExternalDS = nullptr;

        for (int iBand = 0; iBand < nBands; iBand++)
        {
            delete papoBands[iBand];
            papoBands[iBand] = nullptr;
        }
        nBands = 0;
    }

    return bHasDroppedRef;
}

/************************************************************************/
/*                         GetSpatialRef()                              */
/************************************************************************/

const OGRSpatialReference *PDS4Dataset::GetSpatialRef() const
{
    if (!m_oSRS.IsEmpty())
        return &m_oSRS;
    return GDALPamDataset::GetSpatialRef();
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

CPLErr PDS4Dataset::SetSpatialRef(const OGRSpatialReference *poSRS)

{
    if (eAccess == GA_ReadOnly)
        return CE_Failure;
    m_oSRS.Clear();
    if (poSRS)
        m_oSRS = *poSRS;
    if (m_poExternalDS)
        m_poExternalDS->SetSpatialRef(poSRS);
    return CE_None;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr PDS4Dataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    if (m_bGotTransform)
    {
        gt = m_gt;
        return CE_None;
    }

    return GDALPamDataset::GetGeoTransform(gt);
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr PDS4Dataset::SetGeoTransform(const GDALGeoTransform &gt)

{
    if (!((gt[1] > 0.0 && gt[2] == 0.0 && gt[4] == 0.0 && gt[5] < 0.0) ||
          (gt[1] == 0.0 && gt[2] > 0.0 && gt[4] > 0.0 && gt[5] == 0.0)))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Only north-up geotransform or map_projection_rotation=90 "
                 "supported");
        return CE_Failure;
    }
    m_gt = gt;
    m_bGotTransform = true;
    if (m_poExternalDS)
        m_poExternalDS->SetGeoTransform(m_gt);
    return CE_None;
}

/************************************************************************/
/*                             SetMetadata()                            */
/************************************************************************/

CPLErr PDS4Dataset::SetMetadata(char **papszMD, const char *pszDomain)
{
    if (m_bUseSrcLabel && eAccess == GA_Update && pszDomain != nullptr &&
        EQUAL(pszDomain, "xml:PDS4"))
    {
        if (papszMD != nullptr && papszMD[0] != nullptr)
        {
            m_osXMLPDS4 = papszMD[0];
        }
        return CE_None;
    }
    return GDALPamDataset::SetMetadata(papszMD, pszDomain);
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **PDS4Dataset::GetFileList()
{
    char **papszFileList = GDALPamDataset::GetFileList();
    if (!m_osXMLFilename.empty() &&
        CSLFindString(papszFileList, m_osXMLFilename) < 0)
    {
        papszFileList = CSLAddString(papszFileList, m_osXMLFilename);
    }
    if (!m_osImageFilename.empty())
    {
        papszFileList = CSLAddString(papszFileList, m_osImageFilename);
    }
    for (const auto &poLayer : m_apoLayers)
    {
        auto papszTemp = poLayer->GetFileList();
        papszFileList = CSLInsertStrings(papszFileList, -1, papszTemp);
        CSLDestroy(papszTemp);
    }
    return papszFileList;
}

/************************************************************************/
/*                            GetLinearValue()                          */
/************************************************************************/

static const struct
{
    const char *pszUnit;
    double dfToMeter;
} apsLinearUnits[] = {
    {"AU", 149597870700.0}, {"Angstrom", 1e-10}, {"cm", 1e-2}, {"km", 1e3},
    {"micrometer", 1e-6},   {"mm", 1e-3},        {"nm", 1e-9}};

static double GetLinearValue(const CPLXMLNode *psParent,
                             const char *pszElementName)
{
    const CPLXMLNode *psNode = CPLGetXMLNode(psParent, pszElementName);
    if (psNode == nullptr)
        return 0.0;
    double dfVal = CPLAtof(CPLGetXMLValue(psNode, nullptr, ""));
    const char *pszUnit = CPLGetXMLValue(psNode, "unit", nullptr);
    if (pszUnit && !EQUAL(pszUnit, "m"))
    {
        bool bFound = false;
        for (size_t i = 0; i < CPL_ARRAYSIZE(apsLinearUnits); i++)
        {
            if (EQUAL(pszUnit, apsLinearUnits[i].pszUnit))
            {
                dfVal *= apsLinearUnits[i].dfToMeter;
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Unknown unit '%s' for '%s'",
                     pszUnit, pszElementName);
        }
    }
    return dfVal;
}

/************************************************************************/
/*                          GetResolutionValue()                        */
/************************************************************************/

static const struct
{
    const char *pszUnit;
    double dfToMeter;
} apsResolutionUnits[] = {
    {"km/pixel", 1e3},
    {"mm/pixel", 1e-3},
};

static double GetResolutionValue(CPLXMLNode *psParent,
                                 const char *pszElementName)
{
    CPLXMLNode *psNode = CPLGetXMLNode(psParent, pszElementName);
    if (psNode == nullptr)
        return 0.0;
    double dfVal = CPLAtof(CPLGetXMLValue(psNode, nullptr, ""));
    const char *pszUnit = CPLGetXMLValue(psNode, "unit", nullptr);
    if (pszUnit && !EQUAL(pszUnit, "m/pixel"))
    {
        bool bFound = false;
        for (size_t i = 0; i < CPL_ARRAYSIZE(apsResolutionUnits); i++)
        {
            if (EQUAL(pszUnit, apsResolutionUnits[i].pszUnit))
            {
                dfVal *= apsResolutionUnits[i].dfToMeter;
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Unknown unit '%s' for '%s'",
                     pszUnit, pszElementName);
        }
    }
    return dfVal;
}

/************************************************************************/
/*                            GetAngularValue()                         */
/************************************************************************/

static const struct
{
    const char *pszUnit;
    double dfToDeg;
} apsAngularUnits[] = {{"arcmin", 1. / 60.},
                       {"arcsec", 1. / 3600},
                       {"hr", 15.0},
                       {"mrad", 180.0 / M_PI / 1000.},
                       {"rad", 180.0 / M_PI}};

static double GetAngularValue(CPLXMLNode *psParent, const char *pszElementName,
                              bool *pbGotVal = nullptr)
{
    CPLXMLNode *psNode = CPLGetXMLNode(psParent, pszElementName);
    if (psNode == nullptr)
    {
        if (pbGotVal)
            *pbGotVal = false;
        return 0.0;
    }
    double dfVal = CPLAtof(CPLGetXMLValue(psNode, nullptr, ""));
    const char *pszUnit = CPLGetXMLValue(psNode, "unit", nullptr);
    if (pszUnit && !EQUAL(pszUnit, "deg"))
    {
        bool bFound = false;
        for (size_t i = 0; i < CPL_ARRAYSIZE(apsAngularUnits); i++)
        {
            if (EQUAL(pszUnit, apsAngularUnits[i].pszUnit))
            {
                dfVal *= apsAngularUnits[i].dfToDeg;
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            CPLError(CE_Warning, CPLE_AppDefined, "Unknown unit '%s' for '%s'",
                     pszUnit, pszElementName);
        }
    }
    if (pbGotVal)
        *pbGotVal = true;
    return dfVal;
}

/************************************************************************/
/*                          ReadGeoreferencing()                       */
/************************************************************************/

// See https://pds.nasa.gov/pds4/cart/v1/PDS4_CART_1G00_1950.xsd, (GDAL 3.4)
//     https://pds.nasa.gov/pds4/cart/v1/PDS4_CART_1D00_1933.xsd,
//     https://raw.githubusercontent.com/nasa-pds-data-dictionaries/ldd-cart/master/build/1.B.0.0/PDS4_CART_1B00.xsd,
//     https://pds.nasa.gov/pds4/cart/v1/PDS4_CART_1700.xsd
// and the corresponding .sch files
void PDS4Dataset::ReadGeoreferencing(CPLXMLNode *psProduct)
{
    CPLXMLNode *psCart = CPLGetXMLNode(
        psProduct, "Observation_Area.Discipline_Area.Cartography");
    if (psCart == nullptr)
    {
        CPLDebug("PDS4",
                 "Did not find Observation_Area.Discipline_Area.Cartography");
        return;
    }

    // Bounding box: informative only
    CPLXMLNode *psBounding =
        CPLGetXMLNode(psCart, "Spatial_Domain.Bounding_Coordinates");
    if (psBounding)
    {
        const char *pszWest =
            CPLGetXMLValue(psBounding, "west_bounding_coordinate", nullptr);
        const char *pszEast =
            CPLGetXMLValue(psBounding, "east_bounding_coordinate", nullptr);
        const char *pszNorth =
            CPLGetXMLValue(psBounding, "north_bounding_coordinate", nullptr);
        const char *pszSouth =
            CPLGetXMLValue(psBounding, "south_bounding_coordinate", nullptr);
        if (pszWest)
            CPLDebug("PDS4", "West: %s", pszWest);
        if (pszEast)
            CPLDebug("PDS4", "East: %s", pszEast);
        if (pszNorth)
            CPLDebug("PDS4", "North: %s", pszNorth);
        if (pszSouth)
            CPLDebug("PDS4", "South: %s", pszSouth);
    }

    CPLXMLNode *psSR =
        CPLGetXMLNode(psCart, "Spatial_Reference_Information.Horizontal_"
                              "Coordinate_System_Definition");
    if (psSR == nullptr)
    {
        CPLDebug("PDS4", "Did not find Spatial_Reference_Information."
                         "Horizontal_Coordinate_System_Definition");
        return;
    }

    double dfLongitudeMultiplier = 1;
    const CPLXMLNode *psGeodeticModel = CPLGetXMLNode(psSR, "Geodetic_Model");
    if (psGeodeticModel != nullptr)
    {
        if (EQUAL(CPLGetXMLValue(psGeodeticModel, "longitude_direction", ""),
                  "Positive West"))
        {
            dfLongitudeMultiplier = -1;
        }
    }

    OGRSpatialReference oSRS;
    CPLXMLNode *psGridCoordinateSystem =
        CPLGetXMLNode(psSR, "Planar.Grid_Coordinate_System");
    CPLXMLNode *psMapProjection = CPLGetXMLNode(psSR, "Planar.Map_Projection");
    CPLString osProjName;
    double dfCenterLon = 0.0;
    double dfCenterLat = 0.0;
    double dfStdParallel1 = 0.0;
    double dfStdParallel2 = 0.0;
    double dfScale = 1.0;
    double dfMapProjectionRotation = 0.0;
    if (psGridCoordinateSystem != nullptr)
    {
        osProjName = CPLGetXMLValue(psGridCoordinateSystem,
                                    "grid_coordinate_system_name", "");
        if (!osProjName.empty())
        {
            if (osProjName == "Universal Transverse Mercator")
            {
                CPLXMLNode *psUTMZoneNumber = CPLGetXMLNode(
                    psGridCoordinateSystem,
                    "Universal_Transverse_Mercator.utm_zone_number");
                if (psUTMZoneNumber)
                {
                    int nZone =
                        atoi(CPLGetXMLValue(psUTMZoneNumber, nullptr, ""));
                    oSRS.SetUTM(std::abs(nZone), nZone >= 0);
                }
            }
            else if (osProjName == "Universal Polar Stereographic")
            {
                CPLXMLNode *psProjParamNode = CPLGetXMLNode(
                    psGridCoordinateSystem,
                    "Universal_Polar_Stereographic.Polar_Stereographic");
                if (psProjParamNode)
                {
                    dfCenterLon =
                        GetAngularValue(psProjParamNode,
                                        "longitude_of_central_meridian") *
                        dfLongitudeMultiplier;
                    dfCenterLat = GetAngularValue(
                        psProjParamNode, "latitude_of_projection_origin");
                    dfScale = CPLAtof(CPLGetXMLValue(
                        psProjParamNode, "scale_factor_at_projection_origin",
                        "1"));
                    oSRS.SetPS(dfCenterLat, dfCenterLon, dfScale, 0, 0);
                }
            }
            else
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "grid_coordinate_system_name = %s not supported",
                         osProjName.c_str());
            }
        }
    }
    else if (psMapProjection != nullptr)
    {
        osProjName = CPLGetXMLValue(psMapProjection, "map_projection_name", "");
        if (!osProjName.empty())
        {
            CPLXMLNode *psProjParamNode = CPLGetXMLNode(
                psMapProjection,
                CPLString(osProjName).replaceAll(' ', '_').c_str());
            if (psProjParamNode == nullptr &&
                // typo in https://pds.nasa.gov/pds4/cart/v1/PDS4_CART_1700.sch
                EQUAL(osProjName, "Orothographic"))
            {
                psProjParamNode =
                    CPLGetXMLNode(psMapProjection, "Orthographic");
            }
            bool bGotStdParallel1 = false;
            bool bGotStdParallel2 = false;
            bool bGotScale = false;
            if (psProjParamNode)
            {
                bool bGotCenterLon = false;
                dfCenterLon = GetAngularValue(psProjParamNode,
                                              "longitude_of_central_meridian",
                                              &bGotCenterLon) *
                              dfLongitudeMultiplier;
                if (!bGotCenterLon)
                {
                    dfCenterLon =
                        GetAngularValue(psProjParamNode,
                                        "straight_vertical_longitude_from_pole",
                                        &bGotCenterLon) *
                        dfLongitudeMultiplier;
                }
                dfCenterLat = GetAngularValue(psProjParamNode,
                                              "latitude_of_projection_origin");
                dfStdParallel1 = GetAngularValue(
                    psProjParamNode, "standard_parallel_1", &bGotStdParallel1);
                dfStdParallel2 = GetAngularValue(
                    psProjParamNode, "standard_parallel_2", &bGotStdParallel2);
                const char *pszScaleParam =
                    (osProjName == "Transverse Mercator")
                        ? "scale_factor_at_central_meridian"
                        : "scale_factor_at_projection_origin";
                const char *pszScaleVal =
                    CPLGetXMLValue(psProjParamNode, pszScaleParam, nullptr);
                bGotScale = pszScaleVal != nullptr;
                dfScale = (pszScaleVal) ? CPLAtof(pszScaleVal) : 1.0;

                dfMapProjectionRotation =
                    GetAngularValue(psProjParamNode, "map_projection_rotation");
            }

            CPLXMLNode *psObliqueAzimuth =
                CPLGetXMLNode(psProjParamNode, "Oblique_Line_Azimuth");
            CPLXMLNode *psObliquePoint =
                CPLGetXMLNode(psProjParamNode, "Oblique_Line_Point");

            if (EQUAL(osProjName, "Equirectangular"))
            {
                oSRS.SetEquirectangular2(dfCenterLat, dfCenterLon,
                                         dfStdParallel1, 0, 0);
            }
            else if (EQUAL(osProjName, "Lambert Conformal Conic"))
            {
                if (bGotScale)
                {
                    if ((bGotStdParallel1 && dfStdParallel1 != dfCenterLat) ||
                        (bGotStdParallel2 && dfStdParallel2 != dfCenterLat))
                    {
                        CPLError(
                            CE_Warning, CPLE_AppDefined,
                            "Ignoring standard_parallel_1 and/or "
                            "standard_parallel_2 with LCC_1SP formulation");
                    }
                    oSRS.SetLCC1SP(dfCenterLat, dfCenterLon, dfScale, 0, 0);
                }
                else
                {
                    oSRS.SetLCC(dfStdParallel1, dfStdParallel2, dfCenterLat,
                                dfCenterLon, 0, 0);
                }
            }
            else if (EQUAL(osProjName, "Mercator"))
            {
                if (bGotScale)
                {
                    oSRS.SetMercator(dfCenterLat,  // should be 0 normally
                                     dfCenterLon, dfScale, 0.0, 0.0);
                }
                else
                {
                    oSRS.SetMercator2SP(dfStdParallel1,
                                        dfCenterLat,  // should be 0 normally
                                        dfCenterLon, 0.0, 0.0);
                }
            }
            else if (EQUAL(osProjName, "Orthographic"))
            {
                oSRS.SetOrthographic(dfCenterLat, dfCenterLon, 0.0, 0.0);
            }
            else if (EQUAL(osProjName, "Oblique Mercator") &&
                     (psObliqueAzimuth != nullptr || psObliquePoint != nullptr))
            {
                if (psObliqueAzimuth)
                {
                    // Not sure of this
                    dfCenterLon = CPLAtof(
                        CPLGetXMLValue(psObliqueAzimuth,
                                       "azimuth_measure_point_longitude", "0"));

                    double dfAzimuth = CPLAtof(CPLGetXMLValue(
                        psObliqueAzimuth, "azimuthal_angle", "0"));
                    oSRS.SetProjection(
                        SRS_PT_HOTINE_OBLIQUE_MERCATOR_AZIMUTH_CENTER);
                    oSRS.SetNormProjParm(SRS_PP_LATITUDE_OF_CENTER,
                                         dfCenterLat);
                    oSRS.SetNormProjParm(SRS_PP_LONGITUDE_OF_CENTER,
                                         dfCenterLon);
                    oSRS.SetNormProjParm(SRS_PP_AZIMUTH, dfAzimuth);
                    // SetNormProjParm( SRS_PP_RECTIFIED_GRID_ANGLE,
                    // dfRectToSkew );
                    oSRS.SetNormProjParm(SRS_PP_SCALE_FACTOR, dfScale);
                    oSRS.SetNormProjParm(SRS_PP_FALSE_EASTING, 0.0);
                    oSRS.SetNormProjParm(SRS_PP_FALSE_NORTHING, 0.0);
                }
                else
                {
                    double dfLat1 = 0.0;
                    double dfLong1 = 0.0;
                    double dfLat2 = 0.0;
                    double dfLong2 = 0.0;
                    CPLXMLNode *psPoint = CPLGetXMLNode(
                        psObliquePoint, "Oblique_Line_Point_Group");
                    if (psPoint)
                    {
                        dfLat1 = CPLAtof(CPLGetXMLValue(
                            psPoint, "oblique_line_latitude", "0.0"));
                        dfLong1 = CPLAtof(CPLGetXMLValue(
                            psPoint, "oblique_line_longitude", "0.0"));
                        psPoint = psPoint->psNext;
                        if (psPoint && psPoint->eType == CXT_Element &&
                            EQUAL(psPoint->pszValue,
                                  "Oblique_Line_Point_Group"))
                        {
                            dfLat2 = CPLAtof(CPLGetXMLValue(
                                psPoint, "oblique_line_latitude", "0.0"));
                            dfLong2 = CPLAtof(CPLGetXMLValue(
                                psPoint, "oblique_line_longitude", "0.0"));
                        }
                    }
                    oSRS.SetHOM2PNO(dfCenterLat, dfLat1, dfLong1, dfLat2,
                                    dfLong2, dfScale, 0.0, 0.0);
                }
            }
            else if (EQUAL(osProjName, "Polar Stereographic"))
            {
                oSRS.SetPS(dfCenterLat, dfCenterLon, dfScale, 0, 0);
            }
            else if (EQUAL(osProjName, "Polyconic"))
            {
                oSRS.SetPolyconic(dfCenterLat, dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Sinusoidal"))
            {
                oSRS.SetSinusoidal(dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Transverse Mercator"))
            {
                oSRS.SetTM(dfCenterLat, dfCenterLon, dfScale, 0, 0);
            }

            // Below values are valid map_projection_name according to
            // the schematron but they don't have a dedicated element to
            // hold the projection parameter. Assumed the schema is extended
            // similarly to the existing for a few obvious ones
            else if (EQUAL(osProjName, "Albers Conical Equal Area"))
            {
                oSRS.SetACEA(dfStdParallel1, dfStdParallel2, dfCenterLat,
                             dfCenterLon, 0.0, 0.0);
            }
            else if (EQUAL(osProjName, "Azimuthal Equidistant"))
            {
                oSRS.SetAE(dfCenterLat, dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Equidistant Conic"))
            {
                oSRS.SetEC(dfStdParallel1, dfStdParallel2, dfCenterLat,
                           dfCenterLon, 0.0, 0.0);
            }
            // Unhandled: General Vertical Near-sided Projection
            else if (EQUAL(osProjName, "Gnomonic"))
            {
                oSRS.SetGnomonic(dfCenterLat, dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Lambert Azimuthal Equal Area"))
            {
                oSRS.SetLAEA(dfCenterLat, dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Miller Cylindrical"))
            {
                oSRS.SetMC(dfCenterLat, dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Orothographic")  // typo
                     || EQUAL(osProjName, "Orthographic"))
            {
                osProjName = "Orthographic";
                oSRS.SetOrthographic(dfCenterLat, dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Robinson"))
            {
                oSRS.SetRobinson(dfCenterLon, 0, 0);
            }
            // Unhandled: Space Oblique Mercator
            else if (EQUAL(osProjName, "Stereographic"))
            {
                oSRS.SetStereographic(dfCenterLat, dfCenterLon, dfScale, 0, 0);
            }
            else if (EQUAL(osProjName, "van der Grinten"))
            {
                oSRS.SetVDG(dfCenterLon, 0, 0);
            }
            else if (EQUAL(osProjName, "Oblique Cylindrical"))
            {
                const double poleLatitude = GetAngularValue(
                    psProjParamNode, "oblique_proj_pole_latitude");
                const double poleLongitude =
                    GetAngularValue(psProjParamNode,
                                    "oblique_proj_pole_longitude") *
                    dfLongitudeMultiplier;
                const double poleRotation = GetAngularValue(
                    psProjParamNode, "oblique_proj_pole_rotation");

                CPLString oProj4String;
                // Cf isis3dataset.cpp comments for ObliqueCylindrical
                oProj4String.Printf("+proj=ob_tran +o_proj=eqc +o_lon_p=%.17g "
                                    "+o_lat_p=%.17g +lon_0=%.17g",
                                    -poleRotation, 180 - poleLatitude,
                                    poleLongitude);
                oSRS.SetFromUserInput(oProj4String);
            }
            else
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "map_projection_name = %s not supported",
                         osProjName.c_str());
            }
        }
    }
    else
    {
        CPLXMLNode *psGeographic = CPLGetXMLNode(psSR, "Geographic");
        if (GetLayerCount() && psGeographic)
        {
            // do nothing
        }
        else
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Planar.Map_Projection not found");
        }
    }

    if (oSRS.IsProjected())
    {
        oSRS.SetLinearUnits("Metre", 1.0);
    }

    if (psGeodeticModel != nullptr)
    {
        const char *pszLatitudeType =
            CPLGetXMLValue(psGeodeticModel, "latitude_type", "");
        bool bIsOgraphic = EQUAL(pszLatitudeType, "Planetographic");

        const bool bUseLDD1930RadiusNames =
            CPLGetXMLNode(psGeodeticModel, "a_axis_radius") != nullptr;

        // Before PDS CART schema pre-1.B.10.0 (pre LDD version 1.9.3.0),
        // the confusing semi_major_radius, semi_minor_radius and polar_radius
        // were used but did not follow the recommended
        // FGDC names. Using both "semi" and "radius" in the same keyword,
        // which both mean half, does not make sense.
        const char *pszAAxis =
            bUseLDD1930RadiusNames ? "a_axis_radius" : "semi_major_radius";
        const char *pszBAxis =
            bUseLDD1930RadiusNames ? "b_axis_radius" : "semi_minor_radius";
        const char *pszCAxis =
            bUseLDD1930RadiusNames ? "c_axis_radius" : "polar_radius";

        const double dfSemiMajor = GetLinearValue(psGeodeticModel, pszAAxis);

        // a_axis_radius and b_axis_radius should be the same in most cases
        // unless a triaxial body is being defined. This should be extremely
        // rare (and not used) since the IAU generally defines a best-fit sphere
        // for triaxial bodies: https://astrogeology.usgs.gov/groups/IAU-WGCCRE
        const double dfBValue = GetLinearValue(psGeodeticModel, pszBAxis);
        if (dfSemiMajor != dfBValue)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "%s = %f m, different from "
                     "%s = %f, will be ignored",
                     pszBAxis, dfBValue, pszAAxis, dfSemiMajor);
        }

        const double dfPolarRadius = GetLinearValue(psGeodeticModel, pszCAxis);
        // Use the polar_radius as the actual semi minor
        const double dfSemiMinor = dfPolarRadius;

        // Compulsory
        const char *pszTargetName = CPLGetXMLValue(
            psProduct, "Observation_Area.Target_Identification.name",
            "unknown");

        if (oSRS.IsProjected())
        {
            CPLString osProjTargetName = osProjName + " " + pszTargetName;
            oSRS.SetProjCS(osProjTargetName);
        }

        CPLString osGeogName = CPLString("GCS_") + pszTargetName;

        CPLString osSphereName =
            CPLGetXMLValue(psGeodeticModel, "spheroid_name", pszTargetName);
        CPLString osDatumName = "D_" + osSphereName;

        // calculate inverse flattening from major and minor axis: 1/f = a/(a-b)
        double dfInvFlattening = 0;
        if ((dfSemiMajor - dfSemiMinor) >= 0.00000001)
        {
            dfInvFlattening = dfSemiMajor / (dfSemiMajor - dfSemiMinor);
        }

        //(if stereographic with center lat ==90) or (polar stereographic )
        if ((EQUAL(osProjName, "STEREOGRAPHIC") && fabs(dfCenterLat) == 90) ||
            (EQUAL(osProjName, "POLAR STEREOGRAPHIC")))
        {
            if (bIsOgraphic)
            {
                oSRS.SetGeogCS(osGeogName, osDatumName, osSphereName,
                               dfSemiMajor, dfInvFlattening,
                               "Reference_Meridian", 0.0);
            }
            else
            {
                osSphereName += "_polarRadius";
                oSRS.SetGeogCS(osGeogName, osDatumName, osSphereName,
                               dfPolarRadius, 0.0, "Reference_Meridian", 0.0);
            }
        }
        else if ((EQUAL(osProjName, "EQUIRECTANGULAR")) ||
                 (EQUAL(osProjName, "ORTHOGRAPHIC")) ||
                 (EQUAL(osProjName, "STEREOGRAPHIC")) ||
                 (EQUAL(osProjName, "SINUSOIDAL")))
        {
            oSRS.SetGeogCS(osGeogName, osDatumName, osSphereName, dfSemiMajor,
                           0.0, "Reference_Meridian", 0.0);
        }
        else
        {
            if (bIsOgraphic)
            {
                oSRS.SetGeogCS(osGeogName, osDatumName, osSphereName,
                               dfSemiMajor, dfInvFlattening,
                               "Reference_Meridian", 0.0);
            }
            else
            {
                oSRS.SetGeogCS(osGeogName, osDatumName, osSphereName,
                               dfSemiMajor, 0.0, "Reference_Meridian", 0.0);
            }
        }
    }

    CPLXMLNode *psPCI =
        CPLGetXMLNode(psSR, "Planar.Planar_Coordinate_Information");
    CPLXMLNode *psGT = CPLGetXMLNode(psSR, "Planar.Geo_Transformation");
    if (psPCI && psGT)
    {
        const char *pszPCIEncoding =
            CPLGetXMLValue(psPCI, "planar_coordinate_encoding_method", "");
        CPLXMLNode *psCR = CPLGetXMLNode(psPCI, "Coordinate_Representation");
        if (!EQUAL(pszPCIEncoding, "Coordinate Pair"))
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "planar_coordinate_encoding_method = %s not supported",
                     pszPCIEncoding);
        }
        else if (psCR != nullptr)
        {
            double dfXRes = GetResolutionValue(psCR, "pixel_resolution_x");
            double dfYRes = GetResolutionValue(psCR, "pixel_resolution_y");
            double dfULX = GetLinearValue(psGT, "upperleft_corner_x");
            double dfULY = GetLinearValue(psGT, "upperleft_corner_y");

            // The PDS4 specification is not really clear about the
            // origin convention, but it appears from
            // https://github.com/OSGeo/gdal/issues/735 that it matches GDAL
            // top-left corner of top-left pixel
            m_gt[0] = dfULX;
            m_gt[1] = dfXRes;
            m_gt[2] = 0.0;
            m_gt[3] = dfULY;
            m_gt[4] = 0.0;
            m_gt[5] = -dfYRes;
            m_bGotTransform = true;

            if (dfMapProjectionRotation != 0)
            {
                const double sin_rot =
                    dfMapProjectionRotation == 90
                        ? 1.0
                        : sin(dfMapProjectionRotation / 180 * M_PI);
                const double cos_rot =
                    dfMapProjectionRotation == 90
                        ? 0.0
                        : cos(dfMapProjectionRotation / 180 * M_PI);
                const double gt_1 = cos_rot * m_gt[1] - sin_rot * m_gt[4];
                const double gt_2 = cos_rot * m_gt[2] - sin_rot * m_gt[5];
                const double gt_0 = cos_rot * m_gt[0] - sin_rot * m_gt[3];
                const double gt_4 = sin_rot * m_gt[1] + cos_rot * m_gt[4];
                const double gt_5 = sin_rot * m_gt[2] + cos_rot * m_gt[5];
                const double gt_3 = sin_rot * m_gt[0] + cos_rot * m_gt[3];
                m_gt[1] = gt_1;
                m_gt[2] = gt_2;
                m_gt[0] = gt_0;
                m_gt[4] = gt_4;
                m_gt[5] = gt_5;
                m_gt[3] = gt_3;
            }
        }
    }

    if (!oSRS.IsEmpty())
    {
        if (GetRasterCount())
        {
            m_oSRS = std::move(oSRS);
        }
        else if (GetLayerCount())
        {
            for (auto &poLayer : m_apoLayers)
            {
                if (poLayer->GetGeomType() != wkbNone)
                {
                    auto poSRSClone = oSRS.Clone();
                    poLayer->SetSpatialRef(poSRSClone);
                    poSRSClone->Release();
                }
            }
        }
    }
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *PDS4Dataset::GetLayer(int nIndex)
{
    if (nIndex < 0 || nIndex >= GetLayerCount())
        return nullptr;
    return m_apoLayers[nIndex].get();
}

/************************************************************************/
/*                       FixupTableFilename()                           */
/************************************************************************/

static std::string FixupTableFilename(const std::string &osFilename)
{
    VSIStatBufL sStat;
    if (VSIStatL(osFilename.c_str(), &sStat) == 0)
    {
        return osFilename;
    }
    const std::string osExt = CPLGetExtensionSafe(osFilename.c_str());
    if (!osExt.empty())
    {
        std::string osTry(osFilename);
        if (osExt[0] >= 'a' && osExt[0] <= 'z')
        {
            osTry = CPLResetExtensionSafe(osFilename.c_str(),
                                          CPLString(osExt).toupper());
        }
        else
        {
            osTry = CPLResetExtensionSafe(osFilename.c_str(),
                                          CPLString(osExt).tolower());
        }
        if (VSIStatL(osTry.c_str(), &sStat) == 0)
        {
            return osTry;
        }
    }
    return osFilename;
}

/************************************************************************/
/*                       OpenTableCharacter()                           */
/************************************************************************/

bool PDS4Dataset::OpenTableCharacter(const char *pszFilename,
                                     const CPLXMLNode *psTable)
{
    const std::string osLayerName(CPLGetBasenameSafe(pszFilename));
    const std::string osFullFilename = FixupTableFilename(CPLFormFilenameSafe(
        CPLGetPathSafe(m_osXMLFilename.c_str()).c_str(), pszFilename, nullptr));
    std::unique_ptr<PDS4TableCharacter> poLayer(new PDS4TableCharacter(
        this, osLayerName.c_str(), osFullFilename.c_str()));
    if (!poLayer->ReadTableDef(psTable))
    {
        return false;
    }
    std::unique_ptr<PDS4EditableLayer> poEditableLayer(
        new PDS4EditableLayer(poLayer.release()));
    m_apoLayers.push_back(std::move(poEditableLayer));
    return true;
}

/************************************************************************/
/*                       OpenTableBinary()                              */
/************************************************************************/

bool PDS4Dataset::OpenTableBinary(const char *pszFilename,
                                  const CPLXMLNode *psTable)
{
    const std::string osLayerName(CPLGetBasenameSafe(pszFilename));
    const std::string osFullFilename = FixupTableFilename(CPLFormFilenameSafe(
        CPLGetPathSafe(m_osXMLFilename.c_str()).c_str(), pszFilename, nullptr));
    std::unique_ptr<PDS4TableBinary> poLayer(
        new PDS4TableBinary(this, osLayerName.c_str(), osFullFilename.c_str()));
    if (!poLayer->ReadTableDef(psTable))
    {
        return false;
    }
    std::unique_ptr<PDS4EditableLayer> poEditableLayer(
        new PDS4EditableLayer(poLayer.release()));
    m_apoLayers.push_back(std::move(poEditableLayer));
    return true;
}

/************************************************************************/
/*                      OpenTableDelimited()                            */
/************************************************************************/

bool PDS4Dataset::OpenTableDelimited(const char *pszFilename,
                                     const CPLXMLNode *psTable)
{
    const std::string osLayerName(CPLGetBasenameSafe(pszFilename));
    const std::string osFullFilename = FixupTableFilename(CPLFormFilenameSafe(
        CPLGetPathSafe(m_osXMLFilename.c_str()).c_str(), pszFilename, nullptr));
    std::unique_ptr<PDS4DelimitedTable> poLayer(new PDS4DelimitedTable(
        this, osLayerName.c_str(), osFullFilename.c_str()));
    if (!poLayer->ReadTableDef(psTable))
    {
        return false;
    }
    std::unique_ptr<PDS4EditableLayer> poEditableLayer(
        new PDS4EditableLayer(poLayer.release()));
    m_apoLayers.push_back(std::move(poEditableLayer));
    return true;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

// See https://pds.nasa.gov/pds4/pds/v1/PDS4_PDS_1800.xsd
// and https://pds.nasa.gov/pds4/pds/v1/PDS4_PDS_1800.sch
PDS4Dataset *PDS4Dataset::OpenInternal(GDALOpenInfo *poOpenInfo)
{
    if (!PDS4DriverIdentify(poOpenInfo))
        return nullptr;

    CPLString osXMLFilename(poOpenInfo->pszFilename);
    int nFAOIdxLookup = -1;
    int nArrayIdxLookup = -1;
    if (STARTS_WITH_CI(poOpenInfo->pszFilename, "PDS4:"))
    {
        char **papszTokens =
            CSLTokenizeString2(poOpenInfo->pszFilename, ":", 0);
        int nCount = CSLCount(papszTokens);
        if (nCount == 5 && strlen(papszTokens[1]) == 1 &&
            (papszTokens[2][0] == '\\' || papszTokens[2][0] == '/'))
        {
            osXMLFilename = CPLString(papszTokens[1]) + ":" + papszTokens[2];
            nFAOIdxLookup = atoi(papszTokens[3]);
            nArrayIdxLookup = atoi(papszTokens[4]);
        }
        else if (nCount == 5 && (EQUAL(papszTokens[1], "/vsicurl/http") ||
                                 EQUAL(papszTokens[1], "/vsicurl/https")))
        {
            osXMLFilename = CPLString(papszTokens[1]) + ":" + papszTokens[2];
            nFAOIdxLookup = atoi(papszTokens[3]);
            nArrayIdxLookup = atoi(papszTokens[4]);
        }
        else if (nCount == 4)
        {
            osXMLFilename = papszTokens[1];
            nFAOIdxLookup = atoi(papszTokens[2]);
            nArrayIdxLookup = atoi(papszTokens[3]);
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid syntax for PDS4 subdataset name");
            CSLDestroy(papszTokens);
            return nullptr;
        }
        CSLDestroy(papszTokens);
    }

    CPLXMLTreeCloser oCloser(CPLParseXMLFile(osXMLFilename));
    CPLXMLNode *psRoot = oCloser.get();
    CPLStripXMLNamespace(psRoot, nullptr, TRUE);

    GDALAccess eAccess = STARTS_WITH_CI(poOpenInfo->pszFilename, "PDS4:")
                             ? GA_ReadOnly
                             : poOpenInfo->eAccess;

    CPLXMLNode *psProduct = CPLGetXMLNode(psRoot, "=Product_Observational");
    if (psProduct == nullptr)
    {
        eAccess = GA_ReadOnly;
        psProduct = CPLGetXMLNode(psRoot, "=Product_Ancillary");
        if (psProduct == nullptr)
        {
            psProduct = CPLGetXMLNode(psRoot, "=Product_Collection");
        }
    }
    if (psProduct == nullptr)
    {
        return nullptr;
    }

    // Test case:
    // https://starbase.jpl.nasa.gov/pds4/1700/dph_example_products/test_Images_DisplaySettings/TestPattern_Image/TestPattern.xml
    const char *pszVertDir = CPLGetXMLValue(
        psProduct,
        "Observation_Area.Discipline_Area.Display_Settings.Display_Direction."
        "vertical_display_direction",
        "");
    const bool bBottomToTop = EQUAL(pszVertDir, "Bottom to Top");

    const char *pszHorizDir = CPLGetXMLValue(
        psProduct,
        "Observation_Area.Discipline_Area.Display_Settings.Display_Direction."
        "horizontal_display_direction",
        "");
    const bool bRightToLeft = EQUAL(pszHorizDir, "Right to Left");

    auto poDS = std::make_unique<PDS4Dataset>();
    poDS->m_osXMLFilename = osXMLFilename;
    poDS->eAccess = eAccess;
    poDS->papszOpenOptions = CSLDuplicate(poOpenInfo->papszOpenOptions);

    CPLStringList aosSubdatasets;
    int nFAOIdx = 0;
    for (CPLXMLNode *psIter = psProduct->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element ||
            (strcmp(psIter->pszValue, "File_Area_Observational") != 0 &&
             strcmp(psIter->pszValue, "File_Area_Ancillary") != 0 &&
             strcmp(psIter->pszValue, "File_Area_Inventory") != 0))
        {
            continue;
        }

        nFAOIdx++;
        CPLXMLNode *psFile = CPLGetXMLNode(psIter, "File");
        if (psFile == nullptr)
        {
            continue;
        }
        const char *pszFilename = CPLGetXMLValue(psFile, "file_name", nullptr);
        if (pszFilename == nullptr)
        {
            continue;
        }

        for (CPLXMLNode *psSubIter = psFile->psChild; psSubIter != nullptr;
             psSubIter = psSubIter->psNext)
        {
            if (psSubIter->eType == CXT_Comment &&
                EQUAL(psSubIter->pszValue, PREEXISTING_BINARY_FILE))
            {
                poDS->m_bCreatedFromExistingBinaryFile = true;
            }
        }

        int nArrayIdx = 0;
        for (CPLXMLNode *psSubIter = psIter->psChild;
             (nFAOIdxLookup < 0 || nFAOIdxLookup == nFAOIdx) &&
             psSubIter != nullptr;
             psSubIter = psSubIter->psNext)
        {
            if (psSubIter->eType != CXT_Element)
            {
                continue;
            }
            int nDIM = 0;
            if (STARTS_WITH(psSubIter->pszValue, "Array_1D"))
            {
                nDIM = 1;
            }
            else if (STARTS_WITH(psSubIter->pszValue, "Array_2D"))
            {
                nDIM = 2;
            }
            else if (STARTS_WITH(psSubIter->pszValue, "Array_3D"))
            {
                nDIM = 3;
            }
            else if (strcmp(psSubIter->pszValue, "Array") == 0)
            {
                nDIM = atoi(CPLGetXMLValue(psSubIter, "axes", "0"));
            }
            else if (strcmp(psSubIter->pszValue, "Table_Character") == 0)
            {
                poDS->OpenTableCharacter(pszFilename, psSubIter);
                continue;
            }
            else if (strcmp(psSubIter->pszValue, "Table_Binary") == 0)
            {
                poDS->OpenTableBinary(pszFilename, psSubIter);
                continue;
            }
            else if (strcmp(psSubIter->pszValue, "Table_Delimited") == 0 ||
                     strcmp(psSubIter->pszValue, "Inventory") == 0)
            {
                poDS->OpenTableDelimited(pszFilename, psSubIter);
                continue;
            }
            if (nDIM == 0)
            {
                continue;
            }

            nArrayIdx++;
            // Does it match a selected subdataset ?
            if (nArrayIdxLookup > 0 && nArrayIdx != nArrayIdxLookup)
            {
                continue;
            }

            const char *pszArrayName =
                CPLGetXMLValue(psSubIter, "name", nullptr);
            const char *pszArrayId =
                CPLGetXMLValue(psSubIter, "local_identifier", nullptr);
            vsi_l_offset nOffset = static_cast<vsi_l_offset>(
                CPLAtoGIntBig(CPLGetXMLValue(psSubIter, "offset", "0")));

            const char *pszAxisIndexOrder =
                CPLGetXMLValue(psSubIter, "axis_index_order", "");
            if (!EQUAL(pszAxisIndexOrder, "Last Index Fastest"))
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "axis_index_order = '%s' unhandled",
                         pszAxisIndexOrder);
                continue;
            }

            // Figure out data type
            const char *pszDataType =
                CPLGetXMLValue(psSubIter, "Element_Array.data_type", "");
            GDALDataType eDT = GDT_Byte;
            bool bLSBOrder = strstr(pszDataType, "LSB") != nullptr;

            // ComplexLSB16', 'ComplexLSB8', 'ComplexMSB16', 'ComplexMSB8',
            // 'IEEE754LSBDouble', 'IEEE754LSBSingle', 'IEEE754MSBDouble',
            // 'IEEE754MSBSingle', 'SignedBitString', 'SignedByte',
            // 'SignedLSB2', 'SignedLSB4', 'SignedLSB8', 'SignedMSB2',
            // 'SignedMSB4', 'SignedMSB8', 'UnsignedBitString', 'UnsignedByte',
            // 'UnsignedLSB2', 'UnsignedLSB4', 'UnsignedLSB8', 'UnsignedMSB2',
            // 'UnsignedMSB4', 'UnsignedMSB8'
            if (EQUAL(pszDataType, "ComplexLSB16") ||
                EQUAL(pszDataType, "ComplexMSB16"))
            {
                eDT = GDT_CFloat64;
            }
            else if (EQUAL(pszDataType, "ComplexLSB8") ||
                     EQUAL(pszDataType, "ComplexMSB8"))
            {
                eDT = GDT_CFloat32;
            }
            else if (EQUAL(pszDataType, "IEEE754LSBDouble") ||
                     EQUAL(pszDataType, "IEEE754MSBDouble"))
            {
                eDT = GDT_Float64;
            }
            else if (EQUAL(pszDataType, "IEEE754LSBSingle") ||
                     EQUAL(pszDataType, "IEEE754MSBSingle"))
            {
                eDT = GDT_Float32;
            }
            // SignedBitString unhandled
            else if (EQUAL(pszDataType, "SignedByte"))
            {
                eDT = GDT_Int8;
            }
            else if (EQUAL(pszDataType, "SignedLSB2") ||
                     EQUAL(pszDataType, "SignedMSB2"))
            {
                eDT = GDT_Int16;
            }
            else if (EQUAL(pszDataType, "SignedLSB4") ||
                     EQUAL(pszDataType, "SignedMSB4"))
            {
                eDT = GDT_Int32;
            }
            // SignedLSB8 and SignedMSB8 unhandled
            else if (EQUAL(pszDataType, "UnsignedByte"))
            {
                eDT = GDT_Byte;
            }
            else if (EQUAL(pszDataType, "UnsignedLSB2") ||
                     EQUAL(pszDataType, "UnsignedMSB2"))
            {
                eDT = GDT_UInt16;
            }
            else if (EQUAL(pszDataType, "UnsignedLSB4") ||
                     EQUAL(pszDataType, "UnsignedMSB4"))
            {
                eDT = GDT_UInt32;
            }
            // UnsignedLSB8 and UnsignedMSB8 unhandled
            else
            {
                CPLDebug("PDS4", "data_type = '%s' unhandled", pszDataType);
                continue;
            }

            poDS->m_osUnits =
                CPLGetXMLValue(psSubIter, "Element_Array.unit", "");

            double dfValueOffset = CPLAtof(
                CPLGetXMLValue(psSubIter, "Element_Array.value_offset", "0"));
            double dfValueScale = CPLAtof(
                CPLGetXMLValue(psSubIter, "Element_Array.scaling_factor", "1"));

            // Parse Axis_Array elements
            char szOrder[4] = {0};
            int l_nBands = 1;
            int nLines = 0;
            int nSamples = 0;
            int nAxisFound = 0;
            int anElements[3] = {0};
            for (CPLXMLNode *psAxisIter = psSubIter->psChild;
                 psAxisIter != nullptr; psAxisIter = psAxisIter->psNext)
            {
                if (psAxisIter->eType != CXT_Element ||
                    strcmp(psAxisIter->pszValue, "Axis_Array") != 0)
                {
                    continue;
                }
                const char *pszAxisName =
                    CPLGetXMLValue(psAxisIter, "axis_name", nullptr);
                const char *pszElements =
                    CPLGetXMLValue(psAxisIter, "elements", nullptr);
                const char *pszSequenceNumber =
                    CPLGetXMLValue(psAxisIter, "sequence_number", nullptr);
                if (pszAxisName == nullptr || pszElements == nullptr ||
                    pszSequenceNumber == nullptr)
                {
                    continue;
                }
                int nSeqNumber = atoi(pszSequenceNumber);
                if (nSeqNumber < 1 || nSeqNumber > nDIM)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Invalid sequence_number = %s", pszSequenceNumber);
                    continue;
                }
                int nElements = atoi(pszElements);
                if (nElements <= 0)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Invalid elements = %s", pszElements);
                    continue;
                }
                nSeqNumber--;
                if (szOrder[nSeqNumber] != '\0')
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Invalid sequence_number = %s", pszSequenceNumber);
                    continue;
                }
                if (EQUAL(pszAxisName, "Band") && nDIM == 3)
                {
                    szOrder[nSeqNumber] = 'B';
                    l_nBands = nElements;
                    anElements[nSeqNumber] = nElements;
                    nAxisFound++;
                }
                else if (EQUAL(pszAxisName, "Line"))
                {
                    szOrder[nSeqNumber] = 'L';
                    nLines = nElements;
                    anElements[nSeqNumber] = nElements;
                    nAxisFound++;
                }
                else if (EQUAL(pszAxisName, "Sample"))
                {
                    szOrder[nSeqNumber] = 'S';
                    nSamples = nElements;
                    anElements[nSeqNumber] = nElements;
                    nAxisFound++;
                }
                else
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Unsupported axis_name = %s", pszAxisName);
                    continue;
                }
            }
            if (nAxisFound != nDIM)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Found only %d Axis_Array elements. %d expected",
                         nAxisFound, nDIM);
                continue;
            }

            if (!GDALCheckDatasetDimensions(nSamples, nLines) ||
                !GDALCheckBandCount(l_nBands, FALSE))
            {
                continue;
            }

            // Compute pixel, line and band spacing
            vsi_l_offset nSpacing = GDALGetDataTypeSizeBytes(eDT);
            int nPixelOffset = 0;
            int nLineOffset = 0;
            vsi_l_offset nBandOffset = 0;
            int nCountPreviousDim = 1;
            for (int i = nDIM - 1; i >= 0; i--)
            {
                if (szOrder[i] == 'S')
                {
                    if (nSpacing >
                        static_cast<vsi_l_offset>(INT_MAX / nCountPreviousDim))
                    {
                        CPLError(CE_Failure, CPLE_NotSupported,
                                 "Integer overflow");
                        return nullptr;
                    }
                    nPixelOffset =
                        static_cast<int>(nSpacing * nCountPreviousDim);
                    nSpacing = nPixelOffset;
                }
                else if (szOrder[i] == 'L')
                {
                    if (nSpacing >
                        static_cast<vsi_l_offset>(INT_MAX / nCountPreviousDim))
                    {
                        CPLError(CE_Failure, CPLE_NotSupported,
                                 "Integer overflow");
                        return nullptr;
                    }
                    nLineOffset =
                        static_cast<int>(nSpacing * nCountPreviousDim);
                    nSpacing = nLineOffset;
                }
                else
                {
                    nBandOffset = nSpacing * nCountPreviousDim;
                    nSpacing = nBandOffset;
                }
                nCountPreviousDim = anElements[i];
            }

            // Retrieve no data value
            bool bNoDataSet = false;
            double dfNoData = 0.0;
            std::vector<double> adfConstants;
            CPLXMLNode *psSC = CPLGetXMLNode(psSubIter, "Special_Constants");
            if (psSC)
            {
                const char *pszMC =
                    CPLGetXMLValue(psSC, "missing_constant", nullptr);
                if (pszMC)
                {
                    bNoDataSet = true;
                    dfNoData = CPLAtof(pszMC);
                }

                const char *apszConstantNames[] = {
                    "saturated_constant",
                    "missing_constant",
                    "error_constant",
                    "invalid_constant",
                    "unknown_constant",
                    "not_applicable_constant",
                    "high_instrument_saturation",
                    "high_representation_saturation",
                    "low_instrument_saturation",
                    "low_representation_saturation"};
                for (size_t i = 0; i < CPL_ARRAYSIZE(apszConstantNames); i++)
                {
                    const char *pszConstant =
                        CPLGetXMLValue(psSC, apszConstantNames[i], nullptr);
                    if (pszConstant)
                        adfConstants.push_back(CPLAtof(pszConstant));
                }
            }

            // Add subdatasets
            const int nSDSIdx = 1 + aosSubdatasets.size() / 2;
            aosSubdatasets.SetNameValue(
                CPLSPrintf("SUBDATASET_%d_NAME", nSDSIdx),
                CPLSPrintf("PDS4:%s:%d:%d", osXMLFilename.c_str(), nFAOIdx,
                           nArrayIdx));
            aosSubdatasets.SetNameValue(
                CPLSPrintf("SUBDATASET_%d_DESC", nSDSIdx),
                CPLSPrintf("Image file %s, array %s", pszFilename,
                           pszArrayName ? pszArrayName
                           : pszArrayId ? pszArrayId
                                        : CPLSPrintf("%d", nArrayIdx)));

            if (poDS->nBands != 0)
                continue;

            const std::string osImageFullFilename = CPLFormFilenameSafe(
                CPLGetPathSafe(osXMLFilename.c_str()).c_str(), pszFilename,
                nullptr);
            VSILFILE *fp = VSIFOpenExL(
                osImageFullFilename.c_str(),
                (poOpenInfo->eAccess == GA_Update) ? "rb+" : "rb", true);
            if (fp == nullptr)
            {
                CPLError(CE_Warning, CPLE_FileIO, "Cannt open %s: %s",
                         osImageFullFilename.c_str(), VSIGetLastErrorMsg());
                continue;
            }
            poDS->nRasterXSize = nSamples;
            poDS->nRasterYSize = nLines;
            poDS->m_osImageFilename = osImageFullFilename;
            poDS->m_fpImage = fp;
            poDS->m_bIsLSB = bLSBOrder;

            if (memcmp(szOrder, "BLS", 3) == 0)
            {
                poDS->GDALDataset::SetMetadataItem("INTERLEAVE", "BAND",
                                                   "IMAGE_STRUCTURE");
            }
            else if (memcmp(szOrder, "LSB", 3) == 0)
            {
                poDS->GDALDataset::SetMetadataItem("INTERLEAVE", "PIXEL",
                                                   "IMAGE_STRUCTURE");
            }

            CPLXMLNode *psOS = CPLGetXMLNode(psSubIter, "Object_Statistics");
            const char *pszMin = nullptr;
            const char *pszMax = nullptr;
            const char *pszMean = nullptr;
            const char *pszStdDev = nullptr;
            if (psOS)
            {
                pszMin = CPLGetXMLValue(psOS, "minimum", nullptr);
                pszMax = CPLGetXMLValue(psOS, "maximum", nullptr);
                pszMean = CPLGetXMLValue(psOS, "mean", nullptr);
                pszStdDev = CPLGetXMLValue(psOS, "standard_deviation", nullptr);
            }

            for (int i = 0; i < l_nBands; i++)
            {
                vsi_l_offset nThisBandOffset = nOffset + nBandOffset * i;
                if (bBottomToTop)
                {
                    nThisBandOffset +=
                        static_cast<vsi_l_offset>(nLines - 1) * nLineOffset;
                }
                if (bRightToLeft)
                {
                    nThisBandOffset +=
                        static_cast<vsi_l_offset>(nSamples - 1) * nPixelOffset;
                }
                auto poBand = std::make_unique<PDS4RawRasterBand>(
                    poDS.get(), i + 1, poDS->m_fpImage, nThisBandOffset,
                    bRightToLeft ? -nPixelOffset : nPixelOffset,
                    bBottomToTop ? -nLineOffset : nLineOffset, eDT,
                    bLSBOrder ? RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN
                              : RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN);
                if (!poBand->IsValid())
                    return nullptr;
                if (bNoDataSet)
                {
                    poBand->SetNoDataValue(dfNoData);
                }
                poBand->SetOffset(dfValueOffset);
                poBand->SetScale(dfValueScale);

                if (l_nBands == 1)
                {
                    if (pszMin)
                    {
                        poBand->GDALRasterBand::SetMetadataItem(
                            "STATISTICS_MINIMUM", pszMin);
                    }
                    if (pszMax)
                    {
                        poBand->GDALRasterBand::SetMetadataItem(
                            "STATISTICS_MAXIMUM", pszMax);
                    }
                    if (pszMean)
                    {
                        poBand->GDALRasterBand::SetMetadataItem(
                            "STATISTICS_MEAN", pszMean);
                    }
                    if (pszStdDev)
                    {
                        poBand->GDALRasterBand::SetMetadataItem(
                            "STATISTICS_STDDEV", pszStdDev);
                    }
                }

                // Only instantiate explicit mask band if we have at least one
                // special constant (that is not the missing_constant,
                // already exposed as nodata value)
                if (!GDALDataTypeIsComplex(eDT) &&
                    (CPLTestBool(CPLGetConfigOption("PDS4_FORCE_MASK", "NO")) ||
                     adfConstants.size() >= 2 ||
                     (adfConstants.size() == 1 && !bNoDataSet)))
                {
                    poBand->SetMaskBand(std::make_unique<PDS4MaskBand>(
                        poBand.get(), adfConstants));
                }

                poDS->SetBand(i + 1, std::move(poBand));
            }
        }
    }

    if (nFAOIdxLookup < 0 && aosSubdatasets.size() > 2)
    {
        poDS->GDALDataset::SetMetadata(aosSubdatasets.List(), "SUBDATASETS");
    }
    else if (poDS->nBands == 0 &&
             (poOpenInfo->nOpenFlags & GDAL_OF_RASTER) != 0 &&
             (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) == 0)
    {
        return nullptr;
    }
    else if (poDS->m_apoLayers.empty() &&
             (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 &&
             (poOpenInfo->nOpenFlags & GDAL_OF_RASTER) == 0)
    {
        return nullptr;
    }

    // Expose XML content in xml:PDS4 metadata domain
    GByte *pabyRet = nullptr;
    CPL_IGNORE_RET_VAL(VSIIngestFile(nullptr, osXMLFilename, &pabyRet, nullptr,
                                     10 * 1024 * 1024));
    if (pabyRet)
    {
        char *apszXML[2] = {reinterpret_cast<char *>(pabyRet), nullptr};
        poDS->GDALDataset::SetMetadata(apszXML, "xml:PDS4");
    }
    VSIFree(pabyRet);

    /*--------------------------------------------------------------------------*/
    /*  Parse georeferencing info */
    /*--------------------------------------------------------------------------*/
    poDS->ReadGeoreferencing(psProduct);

    /*--------------------------------------------------------------------------*/
    /*  Check for overviews */
    /*--------------------------------------------------------------------------*/
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    /*--------------------------------------------------------------------------*/
    /*  Initialize any PAM information */
    /*--------------------------------------------------------------------------*/
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    return poDS.release();
}

/************************************************************************/
/*                         IsCARTVersionGTE()                           */
/************************************************************************/

// Returns true is pszCur >= pszRef
// Must be things like 1900, 1B00, 1D00_1933 ...
static bool IsCARTVersionGTE(const char *pszCur, const char *pszRef)
{
    return strcmp(pszCur, pszRef) >= 0;
}

/************************************************************************/
/*                         WriteGeoreferencing()                        */
/************************************************************************/

void PDS4Dataset::WriteGeoreferencing(CPLXMLNode *psCart,
                                      const char *pszCARTVersion)
{
    bool bHasBoundingBox = false;
    double adfX[4] = {0};
    double adfY[4] = {0};
    CPLString osPrefix;
    const char *pszColon = strchr(psCart->pszValue, ':');
    if (pszColon)
        osPrefix.assign(psCart->pszValue, pszColon - psCart->pszValue + 1);

    if (m_bGotTransform)
    {
        bHasBoundingBox = true;

        // upper left
        adfX[0] = m_gt[0];
        adfY[0] = m_gt[3];

        // upper right
        adfX[1] = m_gt[0] + m_gt[1] * nRasterXSize;
        adfY[1] = m_gt[3];

        // lower left
        adfX[2] = m_gt[0];
        adfY[2] = m_gt[3] + m_gt[5] * nRasterYSize;

        // lower right
        adfX[3] = m_gt[0] + m_gt[1] * nRasterXSize;
        adfY[3] = m_gt[3] + m_gt[5] * nRasterYSize;
    }
    else
    {
        OGRLayer *poLayer = GetLayer(0);
        OGREnvelope sEnvelope;
        if (poLayer->GetExtent(&sEnvelope) == OGRERR_NONE)
        {
            bHasBoundingBox = true;

            adfX[0] = sEnvelope.MinX;
            adfY[0] = sEnvelope.MaxY;

            adfX[1] = sEnvelope.MaxX;
            adfY[1] = sEnvelope.MaxY;

            adfX[2] = sEnvelope.MinX;
            adfY[2] = sEnvelope.MinY;

            adfX[3] = sEnvelope.MaxX;
            adfY[3] = sEnvelope.MinY;
        }
    }

    if (bHasBoundingBox && !m_oSRS.IsGeographic())
    {
        bHasBoundingBox = false;
        OGRSpatialReference *poSRSLongLat = m_oSRS.CloneGeogCS();
        if (poSRSLongLat)
        {
            poSRSLongLat->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            OGRCoordinateTransformation *poCT =
                OGRCreateCoordinateTransformation(&m_oSRS, poSRSLongLat);
            if (poCT)
            {
                if (poCT->Transform(4, adfX, adfY))
                {
                    bHasBoundingBox = true;
                }
                delete poCT;
            }
            delete poSRSLongLat;
        }
    }

    if (!bHasBoundingBox)
    {
        // Write dummy values
        adfX[0] = -180.0;
        adfY[0] = 90.0;
        adfX[1] = 180.0;
        adfY[1] = 90.0;
        adfX[2] = -180.0;
        adfY[2] = -90.0;
        adfX[3] = 180.0;
        adfY[3] = -90.0;
    }

    const char *pszLongitudeDirection = CSLFetchNameValueDef(
        m_papszCreationOptions, "LONGITUDE_DIRECTION", "Positive East");
    const double dfLongitudeMultiplier =
        EQUAL(pszLongitudeDirection, "Positive West") ? -1 : 1;
    const auto FixLong = [dfLongitudeMultiplier](double dfLon)
    { return dfLon * dfLongitudeMultiplier; };

    // Note: starting with CART 1900, Spatial_Domain is actually optional
    CPLXMLNode *psSD = CPLCreateXMLNode(psCart, CXT_Element,
                                        (osPrefix + "Spatial_Domain").c_str());
    CPLXMLNode *psBC = CPLCreateXMLNode(
        psSD, CXT_Element, (osPrefix + "Bounding_Coordinates").c_str());

    const char *pszBoundingDegrees =
        CSLFetchNameValue(m_papszCreationOptions, "BOUNDING_DEGREES");
    double dfWest = FixLong(
        std::min(std::min(adfX[0], adfX[1]), std::min(adfX[2], adfX[3])));
    double dfEast = FixLong(
        std::max(std::max(adfX[0], adfX[1]), std::max(adfX[2], adfX[3])));
    double dfNorth =
        std::max(std::max(adfY[0], adfY[1]), std::max(adfY[2], adfY[3]));
    double dfSouth =
        std::min(std::min(adfY[0], adfY[1]), std::min(adfY[2], adfY[3]));
    if (pszBoundingDegrees)
    {
        char **papszTokens = CSLTokenizeString2(pszBoundingDegrees, ",", 0);
        if (CSLCount(papszTokens) == 4)
        {
            dfWest = CPLAtof(papszTokens[0]);
            dfSouth = CPLAtof(papszTokens[1]);
            dfEast = CPLAtof(papszTokens[2]);
            dfNorth = CPLAtof(papszTokens[3]);
        }
        CSLDestroy(papszTokens);
    }

    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psBC, (osPrefix + "west_bounding_coordinate").c_str(),
            CPLSPrintf("%.17g", dfWest)),
        "unit", "deg");
    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psBC, (osPrefix + "east_bounding_coordinate").c_str(),
            CPLSPrintf("%.17g", dfEast)),
        "unit", "deg");
    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psBC, (osPrefix + "north_bounding_coordinate").c_str(),
            CPLSPrintf("%.17g", dfNorth)),
        "unit", "deg");
    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psBC, (osPrefix + "south_bounding_coordinate").c_str(),
            CPLSPrintf("%.17g", dfSouth)),
        "unit", "deg");

    CPLXMLNode *psSRI =
        CPLCreateXMLNode(psCart, CXT_Element,
                         (osPrefix + "Spatial_Reference_Information").c_str());
    CPLXMLNode *psHCSD = CPLCreateXMLNode(
        psSRI, CXT_Element,
        (osPrefix + "Horizontal_Coordinate_System_Definition").c_str());

    double dfUnrotatedULX = m_gt[0];
    double dfUnrotatedULY = m_gt[3];
    double dfUnrotatedResX = m_gt[1];
    double dfUnrotatedResY = m_gt[5];
    double dfMapProjectionRotation = 0.0;
    if (m_gt[1] == 0.0 && m_gt[2] > 0.0 && m_gt[4] > 0.0 && m_gt[5] == 0.0)
    {
        dfUnrotatedULX = m_gt[3];
        dfUnrotatedULY = -m_gt[0];
        dfUnrotatedResX = m_gt[4];
        dfUnrotatedResY = -m_gt[2];
        dfMapProjectionRotation = 90.0;
    }

    if (GetRasterCount() || m_oSRS.IsProjected())
    {
        CPLXMLNode *psPlanar = CPLCreateXMLNode(psHCSD, CXT_Element,
                                                (osPrefix + "Planar").c_str());
        CPLXMLNode *psMP = CPLCreateXMLNode(
            psPlanar, CXT_Element, (osPrefix + "Map_Projection").c_str());
        const char *pszProjection = m_oSRS.GetAttrValue("PROJECTION");
        CPLString pszPDS4ProjectionName = "";
        typedef std::pair<const char *, double> ProjParam;
        std::vector<ProjParam> aoProjParams;

        const bool bUse_CART_1933_Or_Later =
            IsCARTVersionGTE(pszCARTVersion, "1D00_1933");

        const bool bUse_CART_1950_Or_Later =
            IsCARTVersionGTE(pszCARTVersion, "1G00_1950");

        if (pszProjection == nullptr)
        {
            pszPDS4ProjectionName = "Equirectangular";
            if (bUse_CART_1933_Or_Later)
            {
                aoProjParams.push_back(
                    ProjParam("latitude_of_projection_origin", 0.0));
                aoProjParams.push_back(ProjParam("standard_parallel_1", 0.0));
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian", 0.0));
            }
            else
            {
                aoProjParams.push_back(ProjParam("standard_parallel_1", 0.0));
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian", 0.0));
                aoProjParams.push_back(
                    ProjParam("latitude_of_projection_origin", 0.0));
            }
        }

        else if (EQUAL(pszProjection, SRS_PT_EQUIRECTANGULAR))
        {
            pszPDS4ProjectionName = "Equirectangular";
            if (bUse_CART_1933_Or_Later)
            {
                aoProjParams.push_back(ProjParam(
                    "latitude_of_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
                aoProjParams.push_back(ProjParam(
                    "standard_parallel_1",
                    m_oSRS.GetNormProjParm(SRS_PP_STANDARD_PARALLEL_1, 1.0)));
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian",
                              FixLong(m_oSRS.GetNormProjParm(
                                  SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            }
            else
            {
                aoProjParams.push_back(ProjParam(
                    "standard_parallel_1",
                    m_oSRS.GetNormProjParm(SRS_PP_STANDARD_PARALLEL_1, 1.0)));
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian",
                              FixLong(m_oSRS.GetNormProjParm(
                                  SRS_PP_CENTRAL_MERIDIAN, 0.0))));
                aoProjParams.push_back(ProjParam(
                    "latitude_of_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
            }
        }

        else if (EQUAL(pszProjection, SRS_PT_LAMBERT_CONFORMAL_CONIC_1SP))
        {
            pszPDS4ProjectionName = "Lambert Conformal Conic";
            if (bUse_CART_1933_Or_Later)
            {
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian",
                              FixLong(m_oSRS.GetNormProjParm(
                                  SRS_PP_CENTRAL_MERIDIAN, 0.0))));
                aoProjParams.push_back(ProjParam(
                    "latitude_of_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
                aoProjParams.push_back(ProjParam(
                    "scale_factor_at_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 1.0)));
            }
            else
            {
                aoProjParams.push_back(ProjParam(
                    "scale_factor_at_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 1.0)));
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian",
                              FixLong(m_oSRS.GetNormProjParm(
                                  SRS_PP_CENTRAL_MERIDIAN, 0.0))));
                aoProjParams.push_back(ProjParam(
                    "latitude_of_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
            }
        }

        else if (EQUAL(pszProjection, SRS_PT_LAMBERT_CONFORMAL_CONIC_2SP))
        {
            pszPDS4ProjectionName = "Lambert Conformal Conic";
            aoProjParams.push_back(ProjParam(
                "standard_parallel_1",
                m_oSRS.GetNormProjParm(SRS_PP_STANDARD_PARALLEL_1, 0.0)));
            aoProjParams.push_back(ProjParam(
                "standard_parallel_2",
                m_oSRS.GetNormProjParm(SRS_PP_STANDARD_PARALLEL_2, 0.0)));
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                FixLong(m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }

        else if (EQUAL(pszProjection,
                       SRS_PT_HOTINE_OBLIQUE_MERCATOR_AZIMUTH_CENTER))
        {
            pszPDS4ProjectionName = "Oblique Mercator";
            // Proj params defined later
        }

        else if (EQUAL(pszProjection,
                       SRS_PT_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN))
        {
            pszPDS4ProjectionName = "Oblique Mercator";
            // Proj params defined later
        }

        else if (EQUAL(pszProjection, SRS_PT_POLAR_STEREOGRAPHIC))
        {
            pszPDS4ProjectionName = "Polar Stereographic";
            if (bUse_CART_1950_Or_Later)
            {
                aoProjParams.push_back(
                    ProjParam("longitude_of_central_meridian",
                              FixLong(m_oSRS.GetNormProjParm(
                                  SRS_PP_CENTRAL_MERIDIAN, 0.0))));
                aoProjParams.push_back(ProjParam(
                    "latitude_of_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
                aoProjParams.push_back(ProjParam(
                    "scale_factor_at_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 1.0)));
            }
            else
            {
                aoProjParams.push_back(
                    ProjParam(bUse_CART_1933_Or_Later
                                  ? "longitude_of_central_meridian"
                                  : "straight_vertical_longitude_from_pole",
                              FixLong(m_oSRS.GetNormProjParm(
                                  SRS_PP_CENTRAL_MERIDIAN, 0.0))));
                aoProjParams.push_back(ProjParam(
                    "scale_factor_at_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 1.0)));
                aoProjParams.push_back(ProjParam(
                    "latitude_of_projection_origin",
                    m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
            }
        }

        else if (EQUAL(pszProjection, SRS_PT_POLYCONIC))
        {
            pszPDS4ProjectionName = "Polyconic";
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0)));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }
        else if (EQUAL(pszProjection, SRS_PT_SINUSOIDAL))
        {
            pszPDS4ProjectionName = "Sinusoidal";
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                FixLong(m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }

        else if (EQUAL(pszProjection, SRS_PT_TRANSVERSE_MERCATOR))
        {
            pszPDS4ProjectionName = "Transverse Mercator";
            aoProjParams.push_back(
                ProjParam("scale_factor_at_central_meridian",
                          m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 1.0)));
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0)));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }
        else if (EQUAL(pszProjection, SRS_PT_ORTHOGRAPHIC))
        {
            pszPDS4ProjectionName = "Orthographic";
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                FixLong(m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }

        else if (EQUAL(pszProjection, SRS_PT_MERCATOR_1SP))
        {
            pszPDS4ProjectionName = "Mercator";
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                FixLong(m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
            aoProjParams.push_back(
                ProjParam("scale_factor_at_projection_origin",
                          m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 1.0)));
        }

        else if (EQUAL(pszProjection, SRS_PT_MERCATOR_2SP))
        {
            pszPDS4ProjectionName = "Mercator";
            aoProjParams.push_back(ProjParam(
                "standard_parallel_1",
                m_oSRS.GetNormProjParm(SRS_PP_STANDARD_PARALLEL_1, 0.0)));
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                FixLong(m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }

        else if (EQUAL(pszProjection, SRS_PT_LAMBERT_AZIMUTHAL_EQUAL_AREA))
        {
            pszPDS4ProjectionName = "Lambert Azimuthal Equal Area";
            aoProjParams.push_back(ProjParam(
                "longitude_of_central_meridian",
                FixLong(m_oSRS.GetNormProjParm(SRS_PP_CENTRAL_MERIDIAN, 0.0))));
            aoProjParams.push_back(ProjParam(
                "latitude_of_projection_origin",
                m_oSRS.GetNormProjParm(SRS_PP_LATITUDE_OF_ORIGIN, 0.0)));
        }

        else if (EQUAL(pszProjection, "custom_proj4"))
        {
            const char *pszProj4 =
                m_oSRS.GetExtension("PROJCS", "PROJ4", nullptr);
            if (pszProj4 && strstr(pszProj4, "+proj=ob_tran") &&
                strstr(pszProj4, "+o_proj=eqc"))
            {
                pszPDS4ProjectionName = "Oblique Cylindrical";
                const auto FetchParam =
                    [](const char *pszProj4Str, const char *pszKey)
                {
                    CPLString needle;
                    needle.Printf("+%s=", pszKey);
                    const char *pszVal = strstr(pszProj4Str, needle.c_str());
                    if (pszVal)
                        return CPLAtof(pszVal + needle.size());
                    return 0.0;
                };

                double dfLonP = FetchParam(pszProj4, "o_lon_p");
                double dfLatP = FetchParam(pszProj4, "o_lat_p");
                double dfLon0 = FetchParam(pszProj4, "lon_0");
                double dfPoleRotation = -dfLonP;
                double dfPoleLatitude = 180 - dfLatP;
                double dfPoleLongitude = dfLon0;

                aoProjParams.push_back(ProjParam("map_projection_rotation",
                                                 dfMapProjectionRotation));
                aoProjParams.push_back(
                    ProjParam("oblique_proj_pole_latitude", dfPoleLatitude));
                aoProjParams.push_back(ProjParam("oblique_proj_pole_longitude",
                                                 FixLong(dfPoleLongitude)));
                aoProjParams.push_back(
                    ProjParam("oblique_proj_pole_rotation", dfPoleRotation));
            }
            else
            {
                CPLError(CE_Warning, CPLE_NotSupported,
                         "Projection %s not supported", pszProjection);
            }
        }
        else
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Projection %s not supported", pszProjection);
        }
        CPLCreateXMLElementAndValue(psMP,
                                    (osPrefix + "map_projection_name").c_str(),
                                    pszPDS4ProjectionName);
        CPLXMLNode *psProj = CPLCreateXMLNode(
            psMP, CXT_Element,
            CPLString(osPrefix + pszPDS4ProjectionName).replaceAll(' ', '_'));
        for (size_t i = 0; i < aoProjParams.size(); i++)
        {
            CPLXMLNode *psParam = CPLCreateXMLElementAndValue(
                psProj, (osPrefix + aoProjParams[i].first).c_str(),
                CPLSPrintf("%.17g", aoProjParams[i].second));
            if (!STARTS_WITH(aoProjParams[i].first, "scale_factor"))
            {
                CPLAddXMLAttributeAndValue(psParam, "unit", "deg");
            }
        }

        if (pszProjection &&
            EQUAL(pszProjection, SRS_PT_HOTINE_OBLIQUE_MERCATOR_AZIMUTH_CENTER))
        {
            CPLXMLNode *psOLA =
                CPLCreateXMLNode(nullptr, CXT_Element,
                                 (osPrefix + "Oblique_Line_Azimuth").c_str());
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psOLA, (osPrefix + "azimuthal_angle").c_str(),
                    CPLSPrintf("%.17g",
                               m_oSRS.GetNormProjParm(SRS_PP_AZIMUTH, 0.0))),
                "unit", "deg");
            ;
            // Not completely sure of this
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psOLA,
                    (osPrefix + "azimuth_measure_point_longitude").c_str(),
                    CPLSPrintf("%.17g", FixLong(m_oSRS.GetNormProjParm(
                                            SRS_PP_CENTRAL_MERIDIAN, 0.0)))),
                "unit", "deg");

            if (bUse_CART_1933_Or_Later)
            {
                CPLAddXMLChild(psProj, psOLA);

                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psProj,
                        (osPrefix + "longitude_of_central_meridian").c_str(),
                        "0"),
                    "unit", "deg");

                const double dfScaleFactor =
                    m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 0.0);
                if (dfScaleFactor != 1.0)
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Scale factor on initial support = %.17g cannot "
                             "be encoded in PDS4",
                             dfScaleFactor);
                }
            }
            else
            {
                CPLCreateXMLElementAndValue(
                    psProj,
                    (osPrefix + "scale_factor_at_projection_origin").c_str(),
                    CPLSPrintf("%.17g", m_oSRS.GetNormProjParm(
                                            SRS_PP_SCALE_FACTOR, 0.0)));

                CPLAddXMLChild(psProj, psOLA);
            }

            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psProj,
                    (osPrefix + "latitude_of_projection_origin").c_str(),
                    CPLSPrintf("%.17g", m_oSRS.GetNormProjParm(
                                            SRS_PP_LATITUDE_OF_ORIGIN, 0.0))),
                "unit", "deg");
        }
        else if (pszProjection &&
                 EQUAL(pszProjection,
                       SRS_PT_HOTINE_OBLIQUE_MERCATOR_TWO_POINT_NATURAL_ORIGIN))
        {
            if (bUse_CART_1933_Or_Later)
            {
                const double dfScaleFactor =
                    m_oSRS.GetNormProjParm(SRS_PP_SCALE_FACTOR, 0.0);
                if (dfScaleFactor != 1.0)
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Scale factor on initial support = %.17g cannot "
                             "be encoded in PDS4",
                             dfScaleFactor);
                }
            }
            else
            {
                CPLCreateXMLElementAndValue(
                    psProj,
                    (osPrefix + "scale_factor_at_projection_origin").c_str(),
                    CPLSPrintf("%.17g", m_oSRS.GetNormProjParm(
                                            SRS_PP_SCALE_FACTOR, 0.0)));
            }

            CPLXMLNode *psOLP = CPLCreateXMLNode(
                psProj, CXT_Element, (osPrefix + "Oblique_Line_Point").c_str());
            CPLXMLNode *psOLPG1 = CPLCreateXMLNode(
                psOLP, CXT_Element,
                (osPrefix + "Oblique_Line_Point_Group").c_str());
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psOLPG1, (osPrefix + "oblique_line_latitude").c_str(),
                    CPLSPrintf("%.17g", m_oSRS.GetNormProjParm(
                                            SRS_PP_LATITUDE_OF_POINT_1, 0.0))),
                "unit", "deg");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psOLPG1, (osPrefix + "oblique_line_longitude").c_str(),
                    CPLSPrintf("%.17g",
                               FixLong(m_oSRS.GetNormProjParm(
                                   SRS_PP_LONGITUDE_OF_POINT_1, 0.0)))),
                "unit", "deg");
            CPLXMLNode *psOLPG2 = CPLCreateXMLNode(
                psOLP, CXT_Element,
                (osPrefix + "Oblique_Line_Point_Group").c_str());
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psOLPG2, (osPrefix + "oblique_line_latitude").c_str(),
                    CPLSPrintf("%.17g", m_oSRS.GetNormProjParm(
                                            SRS_PP_LATITUDE_OF_POINT_2, 0.0))),
                "unit", "deg");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psOLPG2, (osPrefix + "oblique_line_longitude").c_str(),
                    CPLSPrintf("%.17g", m_oSRS.GetNormProjParm(
                                            SRS_PP_LONGITUDE_OF_POINT_2, 0.0))),
                "unit", "deg");

            if (bUse_CART_1933_Or_Later)
            {
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psProj,
                        (osPrefix + "longitude_of_central_meridian").c_str(),
                        "0"),
                    "unit", "deg");
            }

            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psProj,
                    (osPrefix + "latitude_of_projection_origin").c_str(),
                    CPLSPrintf("%.17g", FixLong(m_oSRS.GetNormProjParm(
                                            SRS_PP_LATITUDE_OF_ORIGIN, 0.0)))),
                "unit", "deg");
        }

        CPLXMLNode *psCR = nullptr;
        if (m_bGotTransform || !IsCARTVersionGTE(pszCARTVersion, "1B00"))
        {
            CPLXMLNode *psPCI = CPLCreateXMLNode(
                psPlanar, CXT_Element,
                (osPrefix + "Planar_Coordinate_Information").c_str());
            CPLCreateXMLElementAndValue(
                psPCI, (osPrefix + "planar_coordinate_encoding_method").c_str(),
                "Coordinate Pair");
            psCR = CPLCreateXMLNode(
                psPCI, CXT_Element,
                (osPrefix + "Coordinate_Representation").c_str());
        }
        const double dfLinearUnits = m_oSRS.GetLinearUnits();
        const double dfDegToMeter = m_oSRS.GetSemiMajor() * M_PI / 180.0;

        if (psCR == nullptr)
        {
            // do nothing
        }
        else if (!m_bGotTransform)
        {
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_resolution_x").c_str(), "0"),
                "unit", "m/pixel");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_resolution_y").c_str(), "0"),
                "unit", "m/pixel");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_scale_x").c_str(), "0"),
                "unit", "pixel/deg");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_scale_y").c_str(), "0"),
                "unit", "pixel/deg");
        }
        else if (m_oSRS.IsGeographic())
        {
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_resolution_x").c_str(),
                    CPLSPrintf("%.17g", dfUnrotatedResX * dfDegToMeter)),
                "unit", "m/pixel");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_resolution_y").c_str(),
                    CPLSPrintf("%.17g", -dfUnrotatedResY * dfDegToMeter)),
                "unit", "m/pixel");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_scale_x").c_str(),
                    CPLSPrintf("%.17g", 1.0 / (dfUnrotatedResX))),
                "unit", "pixel/deg");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_scale_y").c_str(),
                    CPLSPrintf("%.17g", 1.0 / (-dfUnrotatedResY))),
                "unit", "pixel/deg");
        }
        else if (m_oSRS.IsProjected())
        {
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_resolution_x").c_str(),
                    CPLSPrintf("%.17g", dfUnrotatedResX * dfLinearUnits)),
                "unit", "m/pixel");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_resolution_y").c_str(),
                    CPLSPrintf("%.17g", -dfUnrotatedResY * dfLinearUnits)),
                "unit", "m/pixel");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_scale_x").c_str(),
                    CPLSPrintf("%.17g", dfDegToMeter /
                                            (dfUnrotatedResX * dfLinearUnits))),
                "unit", "pixel/deg");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psCR, (osPrefix + "pixel_scale_y").c_str(),
                    CPLSPrintf("%.17g", dfDegToMeter / (-dfUnrotatedResY *
                                                        dfLinearUnits))),
                "unit", "pixel/deg");
        }

        if (m_bGotTransform)
        {
            CPLXMLNode *psGT =
                CPLCreateXMLNode(psPlanar, CXT_Element,
                                 (osPrefix + "Geo_Transformation").c_str());
            const double dfFalseEasting =
                m_oSRS.GetNormProjParm(SRS_PP_FALSE_EASTING, 0.0);
            const double dfFalseNorthing =
                m_oSRS.GetNormProjParm(SRS_PP_FALSE_NORTHING, 0.0);
            const double dfULX = -dfFalseEasting + dfUnrotatedULX;
            const double dfULY = -dfFalseNorthing + dfUnrotatedULY;
            if (m_oSRS.IsGeographic())
            {
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psGT, (osPrefix + "upperleft_corner_x").c_str(),
                        CPLSPrintf("%.17g", dfULX * dfDegToMeter)),
                    "unit", "m");
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psGT, (osPrefix + "upperleft_corner_y").c_str(),
                        CPLSPrintf("%.17g", dfULY * dfDegToMeter)),
                    "unit", "m");
            }
            else if (m_oSRS.IsProjected())
            {
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psGT, (osPrefix + "upperleft_corner_x").c_str(),
                        CPLSPrintf("%.17g", dfULX * dfLinearUnits)),
                    "unit", "m");
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psGT, (osPrefix + "upperleft_corner_y").c_str(),
                        CPLSPrintf("%.17g", dfULY * dfLinearUnits)),
                    "unit", "m");
            }
        }
    }
    else
    {
        CPLXMLNode *psGeographic = CPLCreateXMLNode(
            psHCSD, CXT_Element, (osPrefix + "Geographic").c_str());
        if (!IsCARTVersionGTE(pszCARTVersion, "1B00"))
        {
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psGeographic, (osPrefix + "latitude_resolution").c_str(),
                    "0"),
                "unit", "deg");
            CPLAddXMLAttributeAndValue(
                CPLCreateXMLElementAndValue(
                    psGeographic, (osPrefix + "longitude_resolution").c_str(),
                    "0"),
                "unit", "deg");
        }
    }

    CPLXMLNode *psGM = CPLCreateXMLNode(psHCSD, CXT_Element,
                                        (osPrefix + "Geodetic_Model").c_str());
    const char *pszLatitudeType = CSLFetchNameValueDef(
        m_papszCreationOptions, "LATITUDE_TYPE", "Planetocentric");
    // Fix case
    if (EQUAL(pszLatitudeType, "Planetocentric"))
        pszLatitudeType = "Planetocentric";
    else if (EQUAL(pszLatitudeType, "Planetographic"))
        pszLatitudeType = "Planetographic";
    CPLCreateXMLElementAndValue(psGM, (osPrefix + "latitude_type").c_str(),
                                pszLatitudeType);

    const char *pszDatum = m_oSRS.GetAttrValue("DATUM");
    if (pszDatum && STARTS_WITH(pszDatum, "D_"))
    {
        CPLCreateXMLElementAndValue(psGM, (osPrefix + "spheroid_name").c_str(),
                                    pszDatum + 2);
    }
    else if (pszDatum)
    {
        CPLCreateXMLElementAndValue(psGM, (osPrefix + "spheroid_name").c_str(),
                                    pszDatum);
    }

    double dfSemiMajor = m_oSRS.GetSemiMajor();
    double dfSemiMinor = m_oSRS.GetSemiMinor();
    const char *pszRadii = CSLFetchNameValue(m_papszCreationOptions, "RADII");
    if (pszRadii)
    {
        char **papszTokens = CSLTokenizeString2(pszRadii, " ,", 0);
        if (CSLCount(papszTokens) == 2)
        {
            dfSemiMajor = CPLAtof(papszTokens[0]);
            dfSemiMinor = CPLAtof(papszTokens[1]);
        }
        CSLDestroy(papszTokens);
    }

    const bool bUseLDD1930RadiusNames =
        IsCARTVersionGTE(pszCARTVersion, "1B10_1930");

    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psGM,
            (osPrefix +
             (bUseLDD1930RadiusNames ? "a_axis_radius" : "semi_major_radius"))
                .c_str(),
            CPLSPrintf("%.17g", dfSemiMajor)),
        "unit", "m");
    // No, this is not a bug. The PDS4  b_axis_radius/semi_minor_radius is the
    // minor radius on the equatorial plane. Which in WKT doesn't really exist,
    // so reuse the WKT semi major
    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psGM,
            (osPrefix +
             (bUseLDD1930RadiusNames ? "b_axis_radius" : "semi_minor_radius"))
                .c_str(),
            CPLSPrintf("%.17g", dfSemiMajor)),
        "unit", "m");
    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(
            psGM,
            (osPrefix +
             (bUseLDD1930RadiusNames ? "c_axis_radius" : "polar_radius"))
                .c_str(),
            CPLSPrintf("%.17g", dfSemiMinor)),
        "unit", "m");

    // Fix case
    if (EQUAL(pszLongitudeDirection, "Positive East"))
        pszLongitudeDirection = "Positive East";
    else if (EQUAL(pszLongitudeDirection, "Positive West"))
        pszLongitudeDirection = "Positive West";
    CPLCreateXMLElementAndValue(psGM,
                                (osPrefix + "longitude_direction").c_str(),
                                pszLongitudeDirection);
}

/************************************************************************/
/*                         SubstituteVariables()                        */
/************************************************************************/

void PDS4Dataset::SubstituteVariables(CPLXMLNode *psNode, char **papszDict)
{
    if (psNode->eType == CXT_Text && psNode->pszValue &&
        strstr(psNode->pszValue, "${"))
    {
        CPLString osVal(psNode->pszValue);

        if (strstr(psNode->pszValue, "${TITLE}") != nullptr &&
            CSLFetchNameValue(papszDict, "VAR_TITLE") == nullptr)
        {
            const CPLString osTitle(CPLGetFilename(GetDescription()));
            CPLError(CE_Warning, CPLE_AppDefined,
                     "VAR_TITLE not defined. Using %s by default",
                     osTitle.c_str());
            osVal.replaceAll("${TITLE}", osTitle);
        }

        for (char **papszIter = papszDict; papszIter && *papszIter; papszIter++)
        {
            if (STARTS_WITH_CI(*papszIter, "VAR_"))
            {
                char *pszKey = nullptr;
                const char *pszValue = CPLParseNameValue(*papszIter, &pszKey);
                if (pszKey && pszValue)
                {
                    const char *pszVarName = pszKey + strlen("VAR_");
                    osVal.replaceAll(
                        (CPLString("${") + pszVarName + "}").c_str(), pszValue);
                    osVal.replaceAll(
                        CPLString(CPLString("${") + pszVarName + "}")
                            .tolower()
                            .c_str(),
                        CPLString(pszValue).tolower());
                    CPLFree(pszKey);
                }
            }
        }
        if (osVal.find("${") != std::string::npos)
        {
            CPLError(CE_Warning, CPLE_AppDefined, "%s could not be substituted",
                     osVal.c_str());
        }
        CPLFree(psNode->pszValue);
        psNode->pszValue = CPLStrdup(osVal);
    }

    for (CPLXMLNode *psIter = psNode->psChild; psIter; psIter = psIter->psNext)
    {
        SubstituteVariables(psIter, papszDict);
    }
}

/************************************************************************/
/*                         InitImageFile()                             */
/************************************************************************/

bool PDS4Dataset::InitImageFile()
{
    m_bMustInitImageFile = false;

    if (m_poExternalDS)
    {
        int nBlockXSize, nBlockYSize;
        GetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
        const GDALDataType eDT = GetRasterBand(1)->GetRasterDataType();
        const int nDTSize = GDALGetDataTypeSizeBytes(eDT);
        const int nBlockSizeBytes = nBlockXSize * nBlockYSize * nDTSize;
        const int l_nBlocksPerColumn = DIV_ROUND_UP(nRasterYSize, nBlockYSize);

        int bHasNoData = FALSE;
        double dfNoData = GetRasterBand(1)->GetNoDataValue(&bHasNoData);
        if (!bHasNoData)
            dfNoData = 0;

        if (nBands == 1 || EQUAL(m_osInterleave, "BSQ"))
        {
            // We need to make sure that blocks are written in the right order
            for (int i = 0; i < nBands; i++)
            {
                if (m_poExternalDS->GetRasterBand(i + 1)->Fill(dfNoData) !=
                    CE_None)
                {
                    return false;
                }
            }
            m_poExternalDS->FlushCache(false);

            // Check that blocks are effectively written in expected order.
            GIntBig nLastOffset = 0;
            for (int i = 0; i < nBands; i++)
            {
                for (int y = 0; y < l_nBlocksPerColumn; y++)
                {
                    const char *pszBlockOffset =
                        m_poExternalDS->GetRasterBand(i + 1)->GetMetadataItem(
                            CPLSPrintf("BLOCK_OFFSET_%d_%d", 0, y), "TIFF");
                    if (pszBlockOffset)
                    {
                        GIntBig nOffset = CPLAtoGIntBig(pszBlockOffset);
                        if (i != 0 || y != 0)
                        {
                            if (nOffset != nLastOffset + nBlockSizeBytes)
                            {
                                CPLError(CE_Warning, CPLE_AppDefined,
                                         "Block %d,%d band %d not at expected "
                                         "offset",
                                         0, y, i + 1);
                                return false;
                            }
                        }
                        nLastOffset = nOffset;
                    }
                    else
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Block %d,%d band %d not at expected "
                                 "offset",
                                 0, y, i + 1);
                        return false;
                    }
                }
            }
        }
        else
        {
            void *pBlockData = VSI_MALLOC_VERBOSE(nBlockSizeBytes);
            if (pBlockData == nullptr)
                return false;
            GDALCopyWords(&dfNoData, GDT_Float64, 0, pBlockData, eDT, nDTSize,
                          nBlockXSize * nBlockYSize);
            for (int y = 0; y < l_nBlocksPerColumn; y++)
            {
                for (int i = 0; i < nBands; i++)
                {
                    if (m_poExternalDS->GetRasterBand(i + 1)->WriteBlock(
                            0, y, pBlockData) != CE_None)
                    {
                        VSIFree(pBlockData);
                        return false;
                    }
                }
            }
            VSIFree(pBlockData);
            m_poExternalDS->FlushCache(false);

            // Check that blocks are effectively written in expected order.
            GIntBig nLastOffset = 0;
            for (int y = 0; y < l_nBlocksPerColumn; y++)
            {
                const char *pszBlockOffset =
                    m_poExternalDS->GetRasterBand(1)->GetMetadataItem(
                        CPLSPrintf("BLOCK_OFFSET_%d_%d", 0, y), "TIFF");
                if (pszBlockOffset)
                {
                    GIntBig nOffset = CPLAtoGIntBig(pszBlockOffset);
                    if (y != 0)
                    {
                        if (nOffset !=
                            nLastOffset +
                                static_cast<GIntBig>(nBlockSizeBytes) * nBands)
                        {
                            CPLError(CE_Warning, CPLE_AppDefined,
                                     "Block %d,%d not at expected "
                                     "offset",
                                     0, y);
                            return false;
                        }
                    }
                    nLastOffset = nOffset;
                }
                else
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Block %d,%d not at expected "
                             "offset",
                             0, y);
                    return false;
                }
            }
        }

        return true;
    }

    int bHasNoData = FALSE;
    const double dfNoData = GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    const GDALDataType eDT = GetRasterBand(1)->GetRasterDataType();
    const int nDTSize = GDALGetDataTypeSizeBytes(eDT);
    const vsi_l_offset nFileSize = static_cast<vsi_l_offset>(nRasterXSize) *
                                   nRasterYSize * nBands * nDTSize;
    if (dfNoData == 0 || !bHasNoData)
    {
        if (VSIFTruncateL(m_fpImage, nFileSize) != 0)
        {
            CPLError(CE_Failure, CPLE_FileIO,
                     "Cannot create file of size " CPL_FRMT_GUIB " bytes",
                     nFileSize);
            return false;
        }
    }
    else
    {
        size_t nLineSize = static_cast<size_t>(nRasterXSize) * nDTSize;
        void *pData = VSI_MALLOC_VERBOSE(nLineSize);
        if (pData == nullptr)
            return false;
        GDALCopyWords(&dfNoData, GDT_Float64, 0, pData, eDT, nDTSize,
                      nRasterXSize);
#ifdef CPL_MSB
        if (GDALDataTypeIsComplex(eDT))
        {
            GDALSwapWords(pData, nDTSize / 2, nRasterXSize * 2, nDTSize / 2);
        }
        else
        {
            GDALSwapWords(pData, nDTSize, nRasterXSize, nDTSize);
        }
#endif
        for (vsi_l_offset i = 0;
             i < static_cast<vsi_l_offset>(nRasterYSize) * nBands; i++)
        {
            size_t nBytesWritten = VSIFWriteL(pData, 1, nLineSize, m_fpImage);
            if (nBytesWritten != nLineSize)
            {
                CPLError(CE_Failure, CPLE_FileIO,
                         "Cannot create file of size " CPL_FRMT_GUIB " bytes",
                         nFileSize);
                VSIFree(pData);
                return false;
            }
        }
        VSIFree(pData);
    }
    return true;
}

/************************************************************************/
/*                          GetSpecialConstants()                       */
/************************************************************************/

static CPLXMLNode *GetSpecialConstants(const CPLString &osPrefix,
                                       CPLXMLNode *psFileAreaObservational)
{
    for (CPLXMLNode *psIter = psFileAreaObservational->psChild; psIter;
         psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Element &&
            STARTS_WITH(psIter->pszValue, (osPrefix + "Array").c_str()))
        {
            CPLXMLNode *psSC =
                CPLGetXMLNode(psIter, (osPrefix + "Special_Constants").c_str());
            if (psSC)
            {
                CPLXMLNode *psNext = psSC->psNext;
                psSC->psNext = nullptr;
                CPLXMLNode *psRet = CPLCloneXMLTree(psSC);
                psSC->psNext = psNext;
                return psRet;
            }
        }
    }
    return nullptr;
}

/************************************************************************/
/*                          WriteHeaderAppendCase()                     */
/************************************************************************/

void PDS4Dataset::WriteHeaderAppendCase()
{
    CPLXMLTreeCloser oCloser(CPLParseXMLFile(GetDescription()));
    CPLXMLNode *psRoot = oCloser.get();
    if (psRoot == nullptr)
        return;
    CPLString osPrefix;
    CPLXMLNode *psProduct = CPLGetXMLNode(psRoot, "=Product_Observational");
    if (psProduct == nullptr)
    {
        psProduct = CPLGetXMLNode(psRoot, "=pds:Product_Observational");
        if (psProduct)
            osPrefix = "pds:";
    }
    if (psProduct == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot find Product_Observational element");
        return;
    }
    CPLXMLNode *psFAO = CPLGetXMLNode(
        psProduct, (osPrefix + "File_Area_Observational").c_str());
    if (psFAO == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot find File_Area_Observational element");
        return;
    }

    WriteArray(osPrefix, psFAO, nullptr, nullptr);

    CPLSerializeXMLTreeToFile(psRoot, GetDescription());
}

/************************************************************************/
/*                              WriteArray()                            */
/************************************************************************/

void PDS4Dataset::WriteArray(const CPLString &osPrefix, CPLXMLNode *psFAO,
                             const char *pszLocalIdentifierDefault,
                             CPLXMLNode *psTemplateSpecialConstants)
{
    const char *pszArrayType = CSLFetchNameValueDef(
        m_papszCreationOptions, "ARRAY_TYPE", "Array_3D_Image");
    const bool bIsArray2D = STARTS_WITH(pszArrayType, "Array_2D");
    CPLXMLNode *psArray =
        CPLCreateXMLNode(psFAO, CXT_Element, (osPrefix + pszArrayType).c_str());

    const char *pszLocalIdentifier = CSLFetchNameValueDef(
        m_papszCreationOptions, "ARRAY_IDENTIFIER", pszLocalIdentifierDefault);
    if (pszLocalIdentifier)
    {
        CPLCreateXMLElementAndValue(psArray,
                                    (osPrefix + "local_identifier").c_str(),
                                    pszLocalIdentifier);
    }

    GUIntBig nOffset = m_nBaseOffset;
    if (m_poExternalDS)
    {
        const char *pszOffset =
            m_poExternalDS->GetRasterBand(1)->GetMetadataItem(
                "BLOCK_OFFSET_0_0", "TIFF");
        if (pszOffset)
            nOffset = CPLAtoGIntBig(pszOffset);
    }
    CPLAddXMLAttributeAndValue(
        CPLCreateXMLElementAndValue(psArray, (osPrefix + "offset").c_str(),
                                    CPLSPrintf(CPL_FRMT_GUIB, nOffset)),
        "unit", "byte");
    CPLCreateXMLElementAndValue(psArray, (osPrefix + "axes").c_str(),
                                (bIsArray2D) ? "2" : "3");
    CPLCreateXMLElementAndValue(
        psArray, (osPrefix + "axis_index_order").c_str(), "Last Index Fastest");
    CPLXMLNode *psElementArray = CPLCreateXMLNode(
        psArray, CXT_Element, (osPrefix + "Element_Array").c_str());
    GDALDataType eDT = GetRasterBand(1)->GetRasterDataType();
    const char *pszDataType =
        (eDT == GDT_Byte)     ? "UnsignedByte"
        : (eDT == GDT_Int8)   ? "SignedByte"
        : (eDT == GDT_UInt16) ? "UnsignedLSB2"
        : (eDT == GDT_Int16)  ? (m_bIsLSB ? "SignedLSB2" : "SignedMSB2")
        : (eDT == GDT_UInt32) ? (m_bIsLSB ? "UnsignedLSB4" : "UnsignedMSB4")
        : (eDT == GDT_Int32)  ? (m_bIsLSB ? "SignedLSB4" : "SignedMSB4")
        : (eDT == GDT_Float32)
            ? (m_bIsLSB ? "IEEE754LSBSingle" : "IEEE754MSBSingle")
        : (eDT == GDT_Float64)
            ? (m_bIsLSB ? "IEEE754LSBDouble" : "IEEE754MSBDouble")
        : (eDT == GDT_CFloat32) ? (m_bIsLSB ? "ComplexLSB8" : "ComplexMSB8")
        : (eDT == GDT_CFloat64) ? (m_bIsLSB ? "ComplexLSB16" : "ComplexMSB16")
                                : "should not happen";
    CPLCreateXMLElementAndValue(psElementArray,
                                (osPrefix + "data_type").c_str(), pszDataType);

    const char *pszUnits = GetRasterBand(1)->GetUnitType();
    const char *pszUnitsCO = CSLFetchNameValue(m_papszCreationOptions, "UNIT");
    if (pszUnitsCO)
    {
        pszUnits = pszUnitsCO;
    }
    if (pszUnits && pszUnits[0] != 0)
    {
        CPLCreateXMLElementAndValue(psElementArray, (osPrefix + "unit").c_str(),
                                    pszUnits);
    }

    int bHasScale = FALSE;
    double dfScale = GetRasterBand(1)->GetScale(&bHasScale);
    if (bHasScale && dfScale != 1.0)
    {
        CPLCreateXMLElementAndValue(psElementArray,
                                    (osPrefix + "scaling_factor").c_str(),
                                    CPLSPrintf("%.17g", dfScale));
    }

    int bHasOffset = FALSE;
    double dfOffset = GetRasterBand(1)->GetOffset(&bHasOffset);
    if (bHasOffset && dfOffset != 1.0)
    {
        CPLCreateXMLElementAndValue(psElementArray,
                                    (osPrefix + "value_offset").c_str(),
                                    CPLSPrintf("%.17g", dfOffset));
    }

    // Axis definitions
    {
        CPLXMLNode *psAxis = CPLCreateXMLNode(
            psArray, CXT_Element, (osPrefix + "Axis_Array").c_str());
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "axis_name").c_str(),
            EQUAL(m_osInterleave, "BSQ")
                ? "Band"
                :
                /* EQUAL(m_osInterleave, "BIL") ? "Line" : */
                "Line");
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "elements").c_str(),
            CPLSPrintf("%d",
                       EQUAL(m_osInterleave, "BSQ")
                           ? nBands
                           :
                           /* EQUAL(m_osInterleave, "BIL") ? nRasterYSize : */
                           nRasterYSize));
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "sequence_number").c_str(), "1");
    }
    {
        CPLXMLNode *psAxis = CPLCreateXMLNode(
            psArray, CXT_Element, (osPrefix + "Axis_Array").c_str());
        CPLCreateXMLElementAndValue(psAxis, (osPrefix + "axis_name").c_str(),
                                    EQUAL(m_osInterleave, "BSQ")   ? "Line"
                                    : EQUAL(m_osInterleave, "BIL") ? "Band"
                                                                   : "Sample");
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "elements").c_str(),
            CPLSPrintf("%d", EQUAL(m_osInterleave, "BSQ")   ? nRasterYSize
                             : EQUAL(m_osInterleave, "BIL") ? nBands
                                                            : nRasterXSize));
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "sequence_number").c_str(), "2");
    }
    if (!bIsArray2D)
    {
        CPLXMLNode *psAxis = CPLCreateXMLNode(
            psArray, CXT_Element, (osPrefix + "Axis_Array").c_str());
        CPLCreateXMLElementAndValue(psAxis, (osPrefix + "axis_name").c_str(),
                                    EQUAL(m_osInterleave, "BSQ")   ? "Sample"
                                    : EQUAL(m_osInterleave, "BIL") ? "Sample"
                                                                   : "Band");
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "elements").c_str(),
            CPLSPrintf("%d", EQUAL(m_osInterleave, "BSQ")   ? nRasterXSize
                             : EQUAL(m_osInterleave, "BIL") ? nRasterXSize
                                                            : nBands));
        CPLCreateXMLElementAndValue(
            psAxis, (osPrefix + "sequence_number").c_str(), "3");
    }

    int bHasNoData = FALSE;
    double dfNoData = GetRasterBand(1)->GetNoDataValue(&bHasNoData);
    if (psTemplateSpecialConstants)
    {
        CPLAddXMLChild(psArray, psTemplateSpecialConstants);
        if (bHasNoData)
        {
            CPLXMLNode *psMC =
                CPLGetXMLNode(psTemplateSpecialConstants,
                              (osPrefix + "missing_constant").c_str());
            if (psMC != nullptr)
            {
                if (psMC->psChild && psMC->psChild->eType == CXT_Text)
                {
                    CPLFree(psMC->psChild->pszValue);
                    psMC->psChild->pszValue =
                        CPLStrdup(CPLSPrintf("%.17g", dfNoData));
                }
            }
            else
            {
                CPLXMLNode *psSaturatedConstant =
                    CPLGetXMLNode(psTemplateSpecialConstants,
                                  (osPrefix + "saturated_constant").c_str());
                psMC = CPLCreateXMLElementAndValue(
                    nullptr, (osPrefix + "missing_constant").c_str(),
                    CPLSPrintf("%.17g", dfNoData));
                CPLXMLNode *psNext;
                if (psSaturatedConstant)
                {
                    psNext = psSaturatedConstant->psNext;
                    psSaturatedConstant->psNext = psMC;
                }
                else
                {
                    psNext = psTemplateSpecialConstants->psChild;
                    psTemplateSpecialConstants->psChild = psMC;
                }
                psMC->psNext = psNext;
            }
        }
    }
    else if (bHasNoData)
    {
        CPLXMLNode *psSC = CPLCreateXMLNode(
            psArray, CXT_Element, (osPrefix + "Special_Constants").c_str());
        CPLCreateXMLElementAndValue(psSC,
                                    (osPrefix + "missing_constant").c_str(),
                                    CPLSPrintf("%.17g", dfNoData));
    }
}

/************************************************************************/
/*                          WriteVectorLayers()                         */
/************************************************************************/

void PDS4Dataset::WriteVectorLayers(CPLXMLNode *psProduct)
{
    CPLString osPrefix;
    if (STARTS_WITH(psProduct->pszValue, "pds:"))
        osPrefix = "pds:";

    for (auto &poLayer : m_apoLayers)
    {
        if (!poLayer->IsDirtyHeader())
            continue;

        if (poLayer->GetFeatureCount(false) == 0)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Writing header for layer %s which has 0 features. "
                     "This is not legal in PDS4",
                     poLayer->GetName());
        }

        if (poLayer->GetRawFieldCount() == 0)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Writing header for layer %s which has 0 fields. "
                     "This is not legal in PDS4",
                     poLayer->GetName());
        }

        const std::string osRelativePath(
            CPLExtractRelativePath(CPLGetPathSafe(m_osXMLFilename).c_str(),
                                   poLayer->GetFileName(), nullptr));

        bool bFound = false;
        for (CPLXMLNode *psIter = psProduct->psChild; psIter != nullptr;
             psIter = psIter->psNext)
        {
            if (psIter->eType == CXT_Element &&
                strcmp(psIter->pszValue,
                       (osPrefix + "File_Area_Observational").c_str()) == 0)
            {
                const char *pszFilename = CPLGetXMLValue(
                    psIter,
                    (osPrefix + "File." + osPrefix + "file_name").c_str(), "");
                if (strcmp(pszFilename, osRelativePath.c_str()) == 0)
                {
                    poLayer->RefreshFileAreaObservational(psIter);
                    bFound = true;
                    break;
                }
            }
        }
        if (!bFound)
        {
            CPLXMLNode *psFAO = CPLCreateXMLNode(
                psProduct, CXT_Element,
                (osPrefix + "File_Area_Observational").c_str());
            CPLXMLNode *psFile = CPLCreateXMLNode(psFAO, CXT_Element,
                                                  (osPrefix + "File").c_str());
            CPLCreateXMLElementAndValue(psFile,
                                        (osPrefix + "file_name").c_str(),
                                        osRelativePath.c_str());
            poLayer->RefreshFileAreaObservational(psFAO);
        }
    }
}

/************************************************************************/
/*                            CreateHeader()                            */
/************************************************************************/

void PDS4Dataset::CreateHeader(CPLXMLNode *psProduct,
                               const char *pszCARTVersion)
{
    CPLString osPrefix;
    if (STARTS_WITH(psProduct->pszValue, "pds:"))
        osPrefix = "pds:";

    OGREnvelope sExtent;
    if (m_oSRS.IsEmpty() && GetLayerCount() >= 1 &&
        GetLayer(0)->GetSpatialRef() != nullptr)
    {
        const auto poSRS = GetLayer(0)->GetSpatialRef();
        if (poSRS)
            m_oSRS = *poSRS;
    }

    if (!m_oSRS.IsEmpty() &&
        CSLFetchNameValue(m_papszCreationOptions, "VAR_TARGET") == nullptr)
    {
        const char *pszTarget = nullptr;
        if (fabs(m_oSRS.GetSemiMajor() - 6378137) < 0.001 * 6378137)
        {
            pszTarget = "Earth";
            m_papszCreationOptions = CSLSetNameValue(
                m_papszCreationOptions, "VAR_TARGET_TYPE", "Planet");
        }
        else
        {
            const char *pszDatum = m_oSRS.GetAttrValue("DATUM");
            if (pszDatum && STARTS_WITH(pszDatum, "D_"))
            {
                pszTarget = pszDatum + 2;
            }
            else if (pszDatum)
            {
                pszTarget = pszDatum;
            }
        }
        if (pszTarget)
        {
            m_papszCreationOptions = CSLSetNameValue(m_papszCreationOptions,
                                                     "VAR_TARGET", pszTarget);
        }
    }
    SubstituteVariables(psProduct, m_papszCreationOptions);

    // Remove <Discipline_Area>/<disp:Display_Settings> if there is no raster
    if (GetRasterCount() == 0)
    {
        CPLXMLNode *psDisciplineArea =
            CPLGetXMLNode(psProduct, (osPrefix + "Observation_Area." +
                                      osPrefix + "Discipline_Area")
                                         .c_str());
        if (psDisciplineArea)
        {
            CPLXMLNode *psDisplaySettings =
                CPLGetXMLNode(psDisciplineArea, "disp:Display_Settings");
            if (psDisplaySettings)
            {
                CPLRemoveXMLChild(psDisciplineArea, psDisplaySettings);
                CPLDestroyXMLNode(psDisplaySettings);
            }
        }
    }

    // Depending on the version of the DISP schema, Local_Internal_Reference
    // may be in the disp: namespace or the default one.
    const auto GetLocalIdentifierReferenceFromDisciplineArea =
        [](const CPLXMLNode *psDisciplineArea, const char *pszDefault)
    {
        return CPLGetXMLValue(
            psDisciplineArea,
            "disp:Display_Settings.Local_Internal_Reference."
            "local_identifier_reference",
            CPLGetXMLValue(
                psDisciplineArea,
                "disp:Display_Settings.disp:Local_Internal_Reference."
                "local_identifier_reference",
                pszDefault));
    };

    if (GetRasterCount() || !m_oSRS.IsEmpty())
    {
        CPLXMLNode *psDisciplineArea =
            CPLGetXMLNode(psProduct, (osPrefix + "Observation_Area." +
                                      osPrefix + "Discipline_Area")
                                         .c_str());
        if (GetRasterCount() && !(m_bGotTransform && !m_oSRS.IsEmpty()))
        {
            // if we have no georeferencing, strip any existing georeferencing
            // from the template
            if (psDisciplineArea)
            {
                CPLXMLNode *psCart =
                    CPLGetXMLNode(psDisciplineArea, "cart:Cartography");
                if (psCart == nullptr)
                    psCart = CPLGetXMLNode(psDisciplineArea, "Cartography");
                if (psCart)
                {
                    CPLRemoveXMLChild(psDisciplineArea, psCart);
                    CPLDestroyXMLNode(psCart);
                }

                if (CPLGetXMLNode(psDisciplineArea,
                                  "sp:Spectral_Characteristics"))
                {
                    const char *pszArrayType =
                        CSLFetchNameValue(m_papszCreationOptions, "ARRAY_TYPE");
                    // The schematron PDS4_SP_1100.sch requires that
                    // sp:local_identifier_reference is used by
                    // Array_[2D|3D]_Spectrum/pds:local_identifier
                    if (pszArrayType == nullptr)
                    {
                        m_papszCreationOptions =
                            CSLSetNameValue(m_papszCreationOptions,
                                            "ARRAY_TYPE", "Array_3D_Spectrum");
                    }
                    else if (!EQUAL(pszArrayType, "Array_2D_Spectrum") &&
                             !EQUAL(pszArrayType, "Array_3D_Spectrum"))
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "PDS4_SP_xxxx.sch schematron requires the "
                                 "use of ARRAY_TYPE=Array_2D_Spectrum or "
                                 "Array_3D_Spectrum");
                    }
                }
            }
        }
        else
        {
            if (psDisciplineArea == nullptr)
            {
                CPLXMLNode *psTI = CPLGetXMLNode(
                    psProduct, (osPrefix + "Observation_Area." + osPrefix +
                                "Target_Identification")
                                   .c_str());
                if (psTI == nullptr)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Cannot find Target_Identification element in "
                             "template");
                    return;
                }
                psDisciplineArea =
                    CPLCreateXMLNode(nullptr, CXT_Element,
                                     (osPrefix + "Discipline_Area").c_str());
                if (psTI->psNext)
                    psDisciplineArea->psNext = psTI->psNext;
                psTI->psNext = psDisciplineArea;
            }
            CPLXMLNode *psCart =
                CPLGetXMLNode(psDisciplineArea, "cart:Cartography");
            if (psCart == nullptr)
                psCart = CPLGetXMLNode(psDisciplineArea, "Cartography");
            if (psCart == nullptr)
            {
                psCart = CPLCreateXMLNode(psDisciplineArea, CXT_Element,
                                          "cart:Cartography");
                if (CPLGetXMLNode(psProduct, "xmlns:cart") == nullptr)
                {
                    CPLXMLNode *psNS =
                        CPLCreateXMLNode(nullptr, CXT_Attribute, "xmlns:cart");
                    CPLCreateXMLNode(psNS, CXT_Text,
                                     "http://pds.nasa.gov/pds4/cart/v1");
                    CPLAddXMLChild(psProduct, psNS);
                    CPLXMLNode *psSchemaLoc =
                        CPLGetXMLNode(psProduct, "xsi:schemaLocation");
                    if (psSchemaLoc != nullptr &&
                        psSchemaLoc->psChild != nullptr &&
                        psSchemaLoc->psChild->pszValue != nullptr)
                    {
                        CPLString osCartSchema;
                        if (strstr(psSchemaLoc->psChild->pszValue,
                                   "PDS4_PDS_1800.xsd"))
                        {
                            // GDAL 2.4
                            osCartSchema = "https://pds.nasa.gov/pds4/cart/v1/"
                                           "PDS4_CART_1700.xsd";
                            pszCARTVersion = "1700";
                        }
                        else if (strstr(psSchemaLoc->psChild->pszValue,
                                        "PDS4_PDS_1B00.xsd"))
                        {
                            // GDAL 3.0
                            osCartSchema =
                                "https://raw.githubusercontent.com/"
                                "nasa-pds-data-dictionaries/ldd-cart/master/"
                                "build/1.B.0.0/PDS4_CART_1B00.xsd";
                            pszCARTVersion = "1B00";
                        }
                        else if (strstr(psSchemaLoc->psChild->pszValue,
                                        "PDS4_PDS_1D00.xsd"))
                        {
                            // GDAL 3.1
                            osCartSchema = "https://pds.nasa.gov/pds4/cart/v1/"
                                           "PDS4_CART_1D00_1933.xsd";
                            pszCARTVersion = "1D00_1933";
                        }
                        else
                        {
                            // GDAL 3.4
                            osCartSchema =
                                "https://pds.nasa.gov/pds4/cart/v1/"
                                "PDS4_CART_" CURRENT_CART_VERSION ".xsd";
                            pszCARTVersion = CURRENT_CART_VERSION;
                        }
                        CPLString osNewVal(psSchemaLoc->psChild->pszValue);
                        osNewVal +=
                            " http://pds.nasa.gov/pds4/cart/v1 " + osCartSchema;
                        CPLFree(psSchemaLoc->psChild->pszValue);
                        psSchemaLoc->psChild->pszValue = CPLStrdup(osNewVal);
                    }
                }
            }
            else
            {
                if (psCart->psChild)
                {
                    CPLDestroyXMLNode(psCart->psChild);
                    psCart->psChild = nullptr;
                }
            }

            if (IsCARTVersionGTE(pszCARTVersion, "1900"))
            {
                const char *pszLocalIdentifier =
                    GetLocalIdentifierReferenceFromDisciplineArea(
                        psDisciplineArea,
                        GetRasterCount() == 0 && GetLayerCount() > 0
                            ? GetLayer(0)->GetName()
                            : "image");
                CPLXMLNode *psLIR = CPLCreateXMLNode(
                    psCart, CXT_Element,
                    (osPrefix + "Local_Internal_Reference").c_str());
                CPLCreateXMLElementAndValue(
                    psLIR, (osPrefix + "local_identifier_reference").c_str(),
                    pszLocalIdentifier);
                CPLCreateXMLElementAndValue(
                    psLIR, (osPrefix + "local_reference_type").c_str(),
                    "cartography_parameters_to_image_object");
            }

            WriteGeoreferencing(psCart, pszCARTVersion);
        }

        const char *pszVertDir = CSLFetchNameValue(
            m_papszCreationOptions, "VAR_VERTICAL_DISPLAY_DIRECTION");
        if (pszVertDir)
        {
            CPLXMLNode *psVertDirNode =
                CPLGetXMLNode(psDisciplineArea,
                              "disp:Display_Settings.disp:Display_Direction."
                              "disp:vertical_display_direction");
            if (psVertDirNode == nullptr)
            {
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "PDS4 template lacks a disp:vertical_display_direction "
                    "element where to write %s",
                    pszVertDir);
            }
            else
            {
                CPLDestroyXMLNode(psVertDirNode->psChild);
                psVertDirNode->psChild =
                    CPLCreateXMLNode(nullptr, CXT_Text, pszVertDir);
            }
        }
    }
    else
    {
        // Remove Observation_Area.Discipline_Area if it contains only
        // <disp:Display_Settings> or is empty
        CPLXMLNode *psObservationArea =
            CPLGetXMLNode(psProduct, (osPrefix + "Observation_Area").c_str());
        if (psObservationArea)
        {
            CPLXMLNode *psDisciplineArea = CPLGetXMLNode(
                psObservationArea, (osPrefix + "Discipline_Area").c_str());
            if (psDisciplineArea &&
                (psDisciplineArea->psChild == nullptr ||
                 (psDisciplineArea->psChild->eType == CXT_Element &&
                  psDisciplineArea->psChild->psNext == nullptr &&
                  strcmp(psDisciplineArea->psChild->pszValue,
                         "disp:Display_Settings") == 0)))
            {
                CPLRemoveXMLChild(psObservationArea, psDisciplineArea);
                CPLDestroyXMLNode(psDisciplineArea);
            }
        }
    }

    if (m_bStripFileAreaObservationalFromTemplate)
    {
        m_bStripFileAreaObservationalFromTemplate = false;
        CPLXMLNode *psObservationArea = nullptr;
        CPLXMLNode *psPrev = nullptr;
        CPLXMLNode *psTemplateSpecialConstants = nullptr;
        for (CPLXMLNode *psIter = psProduct->psChild; psIter != nullptr;)
        {
            if (psIter->eType == CXT_Element &&
                psIter->pszValue == osPrefix + "Observation_Area")
            {
                psObservationArea = psIter;
                psPrev = psIter;
                psIter = psIter->psNext;
            }
            else if (psIter->eType == CXT_Element &&
                     (psIter->pszValue ==
                          osPrefix + "File_Area_Observational" ||
                      psIter->pszValue ==
                          osPrefix + "File_Area_Observational_Supplemental"))
            {
                if (psIter->pszValue == osPrefix + "File_Area_Observational")
                {
                    psTemplateSpecialConstants =
                        GetSpecialConstants(osPrefix, psIter);
                }
                if (psPrev)
                    psPrev->psNext = psIter->psNext;
                else
                {
                    CPLAssert(psProduct->psChild == psIter);
                    psProduct->psChild = psIter->psNext;
                }
                CPLXMLNode *psNext = psIter->psNext;
                psIter->psNext = nullptr;
                CPLDestroyXMLNode(psIter);
                psIter = psNext;
            }
            else
            {
                psPrev = psIter;
                psIter = psIter->psNext;
            }
        }
        if (psObservationArea == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find Observation_Area in template");
            CPLDestroyXMLNode(psTemplateSpecialConstants);
            return;
        }

        if (GetRasterCount())
        {
            CPLXMLNode *psFAOPrev = psObservationArea;
            while (psFAOPrev->psNext != nullptr &&
                   psFAOPrev->psNext->eType == CXT_Comment)
            {
                psFAOPrev = psFAOPrev->psNext;
            }
            if (psFAOPrev->psNext != nullptr)
            {
                // There may be an optional Reference_List element between
                // Observation_Area and File_Area_Observational
                if (!(psFAOPrev->psNext->eType == CXT_Element &&
                      psFAOPrev->psNext->pszValue ==
                          osPrefix + "Reference_List"))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unexpected content found after Observation_Area "
                             "in template");
                    CPLDestroyXMLNode(psTemplateSpecialConstants);
                    return;
                }
                psFAOPrev = psFAOPrev->psNext;
                while (psFAOPrev->psNext != nullptr &&
                       psFAOPrev->psNext->eType == CXT_Comment)
                {
                    psFAOPrev = psFAOPrev->psNext;
                }
                if (psFAOPrev->psNext != nullptr)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unexpected content found after Reference_List in "
                             "template");
                    CPLDestroyXMLNode(psTemplateSpecialConstants);
                    return;
                }
            }

            CPLXMLNode *psFAO = CPLCreateXMLNode(
                nullptr, CXT_Element,
                (osPrefix + "File_Area_Observational").c_str());
            psFAOPrev->psNext = psFAO;

            CPLXMLNode *psFile = CPLCreateXMLNode(psFAO, CXT_Element,
                                                  (osPrefix + "File").c_str());
            CPLCreateXMLElementAndValue(psFile,
                                        (osPrefix + "file_name").c_str(),
                                        CPLGetFilename(m_osImageFilename));
            if (m_bCreatedFromExistingBinaryFile)
            {
                CPLCreateXMLNode(psFile, CXT_Comment, PREEXISTING_BINARY_FILE);
            }
            CPLXMLNode *psDisciplineArea =
                CPLGetXMLNode(psProduct, (osPrefix + "Observation_Area." +
                                          osPrefix + "Discipline_Area")
                                             .c_str());
            const char *pszLocalIdentifier =
                GetLocalIdentifierReferenceFromDisciplineArea(psDisciplineArea,
                                                              "image");

            if (m_poExternalDS && m_poExternalDS->GetDriver() &&
                EQUAL(m_poExternalDS->GetDriver()->GetDescription(), "GTiff"))
            {
                VSILFILE *fpTemp =
                    VSIFOpenL(m_poExternalDS->GetDescription(), "rb");
                if (fpTemp)
                {
                    GByte abySignature[4] = {0};
                    VSIFReadL(abySignature, 1, 4, fpTemp);
                    VSIFCloseL(fpTemp);
                    const bool bBigTIFF =
                        abySignature[2] == 43 || abySignature[3] == 43;
                    m_osHeaderParsingStandard =
                        bBigTIFF ? BIGTIFF_GEOTIFF_STRING : TIFF_GEOTIFF_STRING;
                    const char *pszOffset =
                        m_poExternalDS->GetRasterBand(1)->GetMetadataItem(
                            "BLOCK_OFFSET_0_0", "TIFF");
                    if (pszOffset)
                        m_nBaseOffset = CPLAtoGIntBig(pszOffset);
                }
            }

            if (!m_osHeaderParsingStandard.empty() && m_nBaseOffset > 0)
            {
                CPLXMLNode *psHeader = CPLCreateXMLNode(
                    psFAO, CXT_Element, (osPrefix + "Header").c_str());
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psHeader, (osPrefix + "offset").c_str(), "0"),
                    "unit", "byte");
                CPLAddXMLAttributeAndValue(
                    CPLCreateXMLElementAndValue(
                        psHeader, (osPrefix + "object_length").c_str(),
                        CPLSPrintf(CPL_FRMT_GUIB,
                                   static_cast<GUIntBig>(m_nBaseOffset))),
                    "unit", "byte");
                CPLCreateXMLElementAndValue(
                    psHeader, (osPrefix + "parsing_standard_id").c_str(),
                    m_osHeaderParsingStandard.c_str());
                if (m_osHeaderParsingStandard == TIFF_GEOTIFF_STRING)
                {
                    CPLCreateXMLElementAndValue(
                        psHeader, (osPrefix + "description").c_str(),
                        "TIFF/GeoTIFF header. The TIFF/GeoTIFF format is used "
                        "throughout the geospatial and science communities "
                        "to share geographic image data. ");
                }
                else if (m_osHeaderParsingStandard == BIGTIFF_GEOTIFF_STRING)
                {
                    CPLCreateXMLElementAndValue(
                        psHeader, (osPrefix + "description").c_str(),
                        "BigTIFF/GeoTIFF header. The BigTIFF/GeoTIFF format is "
                        "used "
                        "throughout the geospatial and science communities "
                        "to share geographic image data. ");
                }
            }

            WriteArray(osPrefix, psFAO, pszLocalIdentifier,
                       psTemplateSpecialConstants);
        }
    }
}

/************************************************************************/
/*                             WriteHeader()                            */
/************************************************************************/

void PDS4Dataset::WriteHeader()
{
    const bool bAppend =
        CPLFetchBool(m_papszCreationOptions, "APPEND_SUBDATASET", false);
    if (bAppend)
    {
        WriteHeaderAppendCase();
        return;
    }

    CPLXMLNode *psRoot;
    if (m_bCreateHeader)
    {
        CPLString osTemplateFilename =
            CSLFetchNameValueDef(m_papszCreationOptions, "TEMPLATE", "");
        if (!osTemplateFilename.empty())
        {
            if (STARTS_WITH(osTemplateFilename, "http://") ||
                STARTS_WITH(osTemplateFilename, "https://"))
            {
                osTemplateFilename = "/vsicurl_streaming/" + osTemplateFilename;
            }
            psRoot = CPLParseXMLFile(osTemplateFilename);
        }
        else if (!m_osXMLPDS4.empty())
            psRoot = CPLParseXMLString(m_osXMLPDS4);
        else
        {
#ifndef USE_ONLY_EMBEDDED_RESOURCE_FILES
#ifdef EMBED_RESOURCE_FILES
            CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
#endif
            const char *pszDefaultTemplateFilename =
                CPLFindFile("gdal", "pds4_template.xml");
            if (pszDefaultTemplateFilename)
            {
                psRoot = CPLParseXMLFile(pszDefaultTemplateFilename);
            }
            else
#endif
            {
#ifdef EMBED_RESOURCE_FILES
                static const bool bOnce [[maybe_unused]] = []()
                {
                    CPLDebug("PDS4", "Using embedded pds4_template.xml");
                    return true;
                }();
                psRoot = CPLParseXMLString(PDS4GetEmbeddedTemplate());
#else
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot find pds4_template.xml and TEMPLATE "
                         "creation option not specified");
                return;
#endif
            }
        }
    }
    else
    {
        psRoot = CPLParseXMLFile(m_osXMLFilename);
    }
    CPLXMLTreeCloser oCloser(psRoot);
    psRoot = oCloser.get();
    if (psRoot == nullptr)
        return;
    CPLXMLNode *psProduct = CPLGetXMLNode(psRoot, "=Product_Observational");
    if (psProduct == nullptr)
    {
        psProduct = CPLGetXMLNode(psRoot, "=pds:Product_Observational");
    }
    if (psProduct == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot find Product_Observational element in template");
        return;
    }

    if (m_bCreateHeader)
    {
        CPLString osCARTVersion(CURRENT_CART_VERSION);
        char *pszXML = CPLSerializeXMLTree(psRoot);
        if (pszXML)
        {
            const char *pszIter = pszXML;
            while (true)
            {
                const char *pszCartSchema = strstr(pszIter, "PDS4_CART_");
                if (pszCartSchema)
                {
                    const char *pszXSDExtension = strstr(pszCartSchema, ".xsd");
                    if (pszXSDExtension &&
                        pszXSDExtension - pszCartSchema <= 20)
                    {
                        osCARTVersion = pszCartSchema + strlen("PDS4_CART_");
                        osCARTVersion.resize(pszXSDExtension - pszCartSchema -
                                             strlen("PDS4_CART_"));
                        break;
                    }
                    else
                    {
                        pszIter = pszCartSchema + 1;
                    }
                }
                else
                {
                    break;
                }
            }

            CPLFree(pszXML);
        }

        CreateHeader(psProduct, osCARTVersion.c_str());
    }

    WriteVectorLayers(psProduct);

    CPLSerializeXMLTreeToFile(psRoot, GetDescription());
}

/************************************************************************/
/*                            ICreateLayer()                            */
/************************************************************************/

OGRLayer *PDS4Dataset::ICreateLayer(const char *pszName,
                                    const OGRGeomFieldDefn *poGeomFieldDefn,
                                    CSLConstList papszOptions)
{
    const char *pszTableType =
        CSLFetchNameValueDef(papszOptions, "TABLE_TYPE", "DELIMITED");
    if (!EQUAL(pszTableType, "CHARACTER") && !EQUAL(pszTableType, "BINARY") &&
        !EQUAL(pszTableType, "DELIMITED"))
    {
        return nullptr;
    }

    const auto eGType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone;
    const auto poSpatialRef =
        poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr;

    const char *pszExt = EQUAL(pszTableType, "CHARACTER") ? "dat"
                         : EQUAL(pszTableType, "BINARY")  ? "bin"
                                                          : "csv";

    bool bSameDirectory =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "SAME_DIRECTORY", "NO"));

    std::string osBasename(pszName);
    for (char &ch : osBasename)
    {
        if (!isalnum(static_cast<unsigned char>(ch)) &&
            static_cast<unsigned>(ch) <= 127)
            ch = '_';
    }

    CPLString osFullFilename;
    if (bSameDirectory)
    {
        osFullFilename =
            CPLFormFilenameSafe(CPLGetPathSafe(m_osXMLFilename.c_str()).c_str(),
                                osBasename.c_str(), pszExt);
        VSIStatBufL sStat;
        if (VSIStatL(osFullFilename, &sStat) == 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "%s already exists. Please delete it before, or "
                     "rename the layer",
                     osFullFilename.c_str());
            return nullptr;
        }
    }
    else
    {
        CPLString osDirectory = CPLFormFilenameSafe(
            CPLGetPathSafe(m_osXMLFilename).c_str(),
            CPLGetBasenameSafe(m_osXMLFilename).c_str(), nullptr);
        VSIStatBufL sStat;
        if (VSIStatL(osDirectory, &sStat) != 0 &&
            VSIMkdir(osDirectory, 0755) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot create directory %s",
                     osDirectory.c_str());
            return nullptr;
        }
        osFullFilename =
            CPLFormFilenameSafe(osDirectory, osBasename.c_str(), pszExt);
    }

    if (EQUAL(pszTableType, "DELIMITED"))
    {
        std::unique_ptr<PDS4DelimitedTable> poLayer(
            new PDS4DelimitedTable(this, pszName, osFullFilename));
        if (!poLayer->InitializeNewLayer(poSpatialRef, false, eGType,
                                         papszOptions))
        {
            return nullptr;
        }
        std::unique_ptr<PDS4EditableLayer> poEditableLayer(
            new PDS4EditableLayer(poLayer.release()));
        m_apoLayers.push_back(std::move(poEditableLayer));
    }
    else
    {
        std::unique_ptr<PDS4FixedWidthTable> poLayer(
            EQUAL(pszTableType, "CHARACTER")
                ? static_cast<PDS4FixedWidthTable *>(
                      new PDS4TableCharacter(this, pszName, osFullFilename))
                : static_cast<PDS4FixedWidthTable *>(
                      new PDS4TableBinary(this, pszName, osFullFilename)));
        if (!poLayer->InitializeNewLayer(poSpatialRef, false, eGType,
                                         papszOptions))
        {
            return nullptr;
        }
        std::unique_ptr<PDS4EditableLayer> poEditableLayer(
            new PDS4EditableLayer(poLayer.release()));
        m_apoLayers.push_back(std::move(poEditableLayer));
    }
    return m_apoLayers.back().get();
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int PDS4Dataset::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return eAccess == GA_Update;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return TRUE;
    else
        return FALSE;
}

/************************************************************************/
/*                             Create()                                 */
/************************************************************************/

GDALDataset *PDS4Dataset::Create(const char *pszFilename, int nXSize,
                                 int nYSize, int nBandsIn, GDALDataType eType,
                                 char **papszOptions)
{
    return CreateInternal(pszFilename, nullptr, nXSize, nYSize, nBandsIn, eType,
                          papszOptions);
}

/************************************************************************/
/*                           CreateInternal()                           */
/************************************************************************/

PDS4Dataset *PDS4Dataset::CreateInternal(const char *pszFilename,
                                         GDALDataset *poSrcDS, int nXSize,
                                         int nYSize, int nBandsIn,
                                         GDALDataType eType,
                                         const char *const *papszOptionsIn)
{
    CPLStringList aosOptions(papszOptionsIn);

    if (nXSize == 0 && nYSize == 0 && nBandsIn == 0 && eType == GDT_Unknown)
    {
        // Vector file creation
        PDS4Dataset *poDS = new PDS4Dataset();
        poDS->SetDescription(pszFilename);
        poDS->nRasterXSize = 0;
        poDS->nRasterYSize = 0;
        poDS->eAccess = GA_Update;
        poDS->m_osXMLFilename = pszFilename;
        poDS->m_bCreateHeader = true;
        poDS->m_bStripFileAreaObservationalFromTemplate = true;
        poDS->m_papszCreationOptions = CSLDuplicate(aosOptions.List());
        poDS->m_bUseSrcLabel = aosOptions.FetchBool("USE_SRC_LABEL", true);
        return poDS;
    }

    if (nXSize == 0)
        return nullptr;

    if (!(eType == GDT_Byte || eType == GDT_Int8 || eType == GDT_Int16 ||
          eType == GDT_UInt16 || eType == GDT_Int32 || eType == GDT_UInt32 ||
          eType == GDT_Float32 || eType == GDT_Float64 ||
          eType == GDT_CFloat32 || eType == GDT_CFloat64))
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "The PDS4 driver does not supporting creating files of type %s.",
            GDALGetDataTypeName(eType));
        return nullptr;
    }

    if (nBandsIn == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid number of bands");
        return nullptr;
    }

    const char *pszArrayType =
        aosOptions.FetchNameValueDef("ARRAY_TYPE", "Array_3D_Image");
    const bool bIsArray2D = STARTS_WITH(pszArrayType, "Array_2D");
    if (nBandsIn > 1 && bIsArray2D)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "ARRAY_TYPE=%s is not supported for a multi-band raster",
                 pszArrayType);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Compute pixel, line and band offsets                            */
    /* -------------------------------------------------------------------- */
    const int nItemSize = GDALGetDataTypeSizeBytes(eType);
    int nLineOffset, nPixelOffset;
    vsi_l_offset nBandOffset;

    const char *pszInterleave =
        aosOptions.FetchNameValueDef("INTERLEAVE", "BSQ");
    if (bIsArray2D)
        pszInterleave = "BIP";

    if (EQUAL(pszInterleave, "BIP"))
    {
        nPixelOffset = nItemSize * nBandsIn;
        if (nPixelOffset > INT_MAX / nBandsIn)
        {
            return nullptr;
        }
        nLineOffset = nPixelOffset * nXSize;
        nBandOffset = nItemSize;
    }
    else if (EQUAL(pszInterleave, "BSQ"))
    {
        nPixelOffset = nItemSize;
        if (nPixelOffset > INT_MAX / nXSize)
        {
            return nullptr;
        }
        nLineOffset = nPixelOffset * nXSize;
        nBandOffset = static_cast<vsi_l_offset>(nLineOffset) * nYSize;
    }
    else if (EQUAL(pszInterleave, "BIL"))
    {
        nPixelOffset = nItemSize;
        if (nPixelOffset > INT_MAX / nBandsIn ||
            nPixelOffset * nBandsIn > INT_MAX / nXSize)
        {
            return nullptr;
        }
        nLineOffset = nItemSize * nBandsIn * nXSize;
        nBandOffset = static_cast<vsi_l_offset>(nItemSize) * nXSize;
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid value for INTERLEAVE");
        return nullptr;
    }

    const char *pszImageFormat =
        aosOptions.FetchNameValueDef("IMAGE_FORMAT", "RAW");
    const char *pszImageExtension = aosOptions.FetchNameValueDef(
        "IMAGE_EXTENSION", EQUAL(pszImageFormat, "RAW") ? "img" : "tif");
    CPLString osImageFilename(aosOptions.FetchNameValueDef(
        "IMAGE_FILENAME",
        CPLResetExtensionSafe(pszFilename, pszImageExtension).c_str()));

    const bool bAppend = aosOptions.FetchBool("APPEND_SUBDATASET", false);
    if (bAppend)
    {
        GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
        auto poExistingPDS4 = static_cast<PDS4Dataset *>(Open(&oOpenInfo));
        if (!poExistingPDS4)
        {
            return nullptr;
        }
        osImageFilename = poExistingPDS4->m_osImageFilename;
        delete poExistingPDS4;

        auto poImageDS = GDALDataset::FromHandle(GDALOpenEx(
            osImageFilename, GDAL_OF_RASTER, nullptr, nullptr, nullptr));
        if (poImageDS && poImageDS->GetDriver() &&
            EQUAL(poImageDS->GetDriver()->GetDescription(), "GTiff"))
        {
            pszImageFormat = "GEOTIFF";
        }
        delete poImageDS;
    }

    GDALDataset *poExternalDS = nullptr;
    VSILFILE *fpImage = nullptr;
    vsi_l_offset nBaseOffset = 0;
    bool bIsLSB = true;
    CPLString osHeaderParsingStandard;
    const bool bCreateLabelOnly =
        aosOptions.FetchBool("CREATE_LABEL_ONLY", false);
    if (bCreateLabelOnly)
    {
        if (poSrcDS == nullptr)
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "CREATE_LABEL_ONLY is only compatible of CreateCopy() mode");
            return nullptr;
        }
        RawBinaryLayout sLayout;
        if (!poSrcDS->GetRawBinaryLayout(sLayout))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Source dataset is not compatible of a raw binary format");
            return nullptr;
        }
        if ((nBandsIn > 1 &&
             sLayout.eInterleaving == RawBinaryLayout::Interleaving::UNKNOWN) ||
            (nBandsIn == 1 &&
             !(sLayout.nPixelOffset == nItemSize &&
               sLayout.nLineOffset == sLayout.nPixelOffset * nXSize)))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Source dataset has an interleaving not handled in PDS4");
            return nullptr;
        }
        fpImage = VSIFOpenL(sLayout.osRawFilename.c_str(), "rb");
        if (fpImage == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot open raw image %s",
                     sLayout.osRawFilename.c_str());
            return nullptr;
        }
        osImageFilename = sLayout.osRawFilename;
        if (nBandsIn == 1 ||
            sLayout.eInterleaving == RawBinaryLayout::Interleaving::BIP)
            pszInterleave = "BIP";
        else if (sLayout.eInterleaving == RawBinaryLayout::Interleaving::BIL)
            pszInterleave = "BIL";
        else
            pszInterleave = "BSQ";
        nBaseOffset = sLayout.nImageOffset;
        nPixelOffset = static_cast<int>(sLayout.nPixelOffset);
        nLineOffset = static_cast<int>(sLayout.nLineOffset);
        nBandOffset = static_cast<vsi_l_offset>(sLayout.nBandOffset);
        bIsLSB = sLayout.bLittleEndianOrder;
        auto poSrcDriver = poSrcDS->GetDriver();
        if (poSrcDriver)
        {
            auto pszDriverName = poSrcDriver->GetDescription();
            if (EQUAL(pszDriverName, "GTiff"))
            {
                GByte abySignature[4] = {0};
                VSIFReadL(abySignature, 1, 4, fpImage);
                const bool bBigTIFF =
                    abySignature[2] == 43 || abySignature[3] == 43;
                osHeaderParsingStandard =
                    bBigTIFF ? BIGTIFF_GEOTIFF_STRING : TIFF_GEOTIFF_STRING;
            }
            else if (EQUAL(pszDriverName, "ISIS3"))
            {
                osHeaderParsingStandard = "ISIS3";
            }
            else if (EQUAL(pszDriverName, "VICAR"))
            {
                osHeaderParsingStandard = "VICAR2";
            }
            else if (EQUAL(pszDriverName, "PDS"))
            {
                osHeaderParsingStandard = "PDS3";
            }
            else if (EQUAL(pszDriverName, "FITS"))
            {
                osHeaderParsingStandard = "FITS 3.0";
                aosOptions.SetNameValue("VAR_VERTICAL_DISPLAY_DIRECTION",
                                        "Bottom to Top");
            }
        }
    }
    else if (EQUAL(pszImageFormat, "GEOTIFF"))
    {
        if (EQUAL(pszInterleave, "BIL"))
        {
            if (aosOptions.FetchBool("@INTERLEAVE_ADDED_AUTOMATICALLY", false))
            {
                pszInterleave = "BSQ";
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "INTERLEAVE=BIL not supported for GeoTIFF in PDS4");
                return nullptr;
            }
        }
        GDALDriver *poDrv =
            static_cast<GDALDriver *>(GDALGetDriverByName("GTiff"));
        if (poDrv == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot find GTiff driver");
            return nullptr;
        }
        char **papszGTiffOptions = nullptr;
#ifdef notdef
        // In practice I can't see which option we can really use
        const char *pszGTiffOptions =
            CSLFetchNameValueDef(papszOptions, "GEOTIFF_OPTIONS", "");
        char **papszTokens = CSLTokenizeString2(pszGTiffOptions, ",", 0);
        if (CPLFetchBool(papszTokens, "TILED", false))
        {
            CSLDestroy(papszTokens);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Tiled GeoTIFF is not supported for PDS4");
            return NULL;
        }
        if (!EQUAL(CSLFetchNameValueDef(papszTokens, "COMPRESS", "NONE"),
                   "NONE"))
        {
            CSLDestroy(papszTokens);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Compressed GeoTIFF is not supported for PDS4");
            return NULL;
        }
        papszGTiffOptions =
            CSLSetNameValue(papszGTiffOptions, "ENDIANNESS", "LITTLE");
        for (int i = 0; papszTokens[i] != NULL; i++)
        {
            papszGTiffOptions = CSLAddString(papszGTiffOptions, papszTokens[i]);
        }
        CSLDestroy(papszTokens);
#endif

        papszGTiffOptions =
            CSLSetNameValue(papszGTiffOptions, "INTERLEAVE",
                            EQUAL(pszInterleave, "BSQ") ? "BAND" : "PIXEL");
        // Will make sure that our blocks at nodata are not optimized
        // away but indeed well written
        papszGTiffOptions = CSLSetNameValue(
            papszGTiffOptions, "@WRITE_EMPTY_TILES_SYNCHRONOUSLY", "YES");
        if (nBandsIn > 1 && EQUAL(pszInterleave, "BSQ"))
        {
            papszGTiffOptions =
                CSLSetNameValue(papszGTiffOptions, "BLOCKYSIZE", "1");
        }

        if (bAppend)
        {
            papszGTiffOptions =
                CSLAddString(papszGTiffOptions, "APPEND_SUBDATASET=YES");
        }

        poExternalDS = poDrv->Create(osImageFilename, nXSize, nYSize, nBandsIn,
                                     eType, papszGTiffOptions);
        CSLDestroy(papszGTiffOptions);
        if (poExternalDS == nullptr)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s",
                     osImageFilename.c_str());
            return nullptr;
        }
    }
    else
    {
        fpImage = VSIFOpenL(
            osImageFilename,
            bAppend                                                 ? "rb+"
            : VSISupportsRandomWrite(osImageFilename.c_str(), true) ? "wb+"
                                                                    : "wb");
        if (fpImage == nullptr)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s",
                     osImageFilename.c_str());
            return nullptr;
        }
        if (bAppend)
        {
            VSIFSeekL(fpImage, 0, SEEK_END);
            nBaseOffset = VSIFTellL(fpImage);
        }
    }

    auto poDS = std::make_unique<PDS4Dataset>();
    poDS->SetDescription(pszFilename);
    poDS->m_bMustInitImageFile = true;
    poDS->m_fpImage = fpImage;
    poDS->m_nBaseOffset = nBaseOffset;
    poDS->m_poExternalDS = poExternalDS;
    poDS->nRasterXSize = nXSize;
    poDS->nRasterYSize = nYSize;
    poDS->eAccess = GA_Update;
    poDS->m_osImageFilename = std::move(osImageFilename);
    poDS->m_bCreateHeader = true;
    poDS->m_bStripFileAreaObservationalFromTemplate = true;
    poDS->m_osInterleave = pszInterleave;
    poDS->m_papszCreationOptions = CSLDuplicate(aosOptions.List());
    poDS->m_bUseSrcLabel = aosOptions.FetchBool("USE_SRC_LABEL", true);
    poDS->m_bIsLSB = bIsLSB;
    poDS->m_osHeaderParsingStandard = std::move(osHeaderParsingStandard);
    poDS->m_bCreatedFromExistingBinaryFile = bCreateLabelOnly;

    if (EQUAL(pszInterleave, "BIP"))
    {
        poDS->GDALDataset::SetMetadataItem("INTERLEAVE", "PIXEL",
                                           "IMAGE_STRUCTURE");
    }
    else if (EQUAL(pszInterleave, "BSQ"))
    {
        poDS->GDALDataset::SetMetadataItem("INTERLEAVE", "BAND",
                                           "IMAGE_STRUCTURE");
    }

    for (int i = 0; i < nBandsIn; i++)
    {
        if (poDS->m_poExternalDS != nullptr)
        {
            auto poBand = std::make_unique<PDS4WrapperRasterBand>(
                poDS->m_poExternalDS->GetRasterBand(i + 1));
            poDS->SetBand(i + 1, std::move(poBand));
        }
        else
        {
            auto poBand = std::make_unique<PDS4RawRasterBand>(
                poDS.get(), i + 1, poDS->m_fpImage,
                poDS->m_nBaseOffset + nBandOffset * i, nPixelOffset,
                nLineOffset, eType,
                bIsLSB ? RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN
                       : RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN);
            poDS->SetBand(i + 1, std::move(poBand));
        }
    }

    return poDS.release();
}

/************************************************************************/
/*                      PDS4GetUnderlyingDataset()                      */
/************************************************************************/

static GDALDataset *PDS4GetUnderlyingDataset(GDALDataset *poSrcDS)
{
    if (poSrcDS->GetDriver() != nullptr &&
        poSrcDS->GetDriver() == GDALGetDriverByName("VRT"))
    {
        VRTDataset *poVRTDS = cpl::down_cast<VRTDataset *>(poSrcDS);
        poSrcDS = poVRTDS->GetSingleSimpleSource();
    }

    return poSrcDS;
}

/************************************************************************/
/*                            CreateCopy()                              */
/************************************************************************/

GDALDataset *PDS4Dataset::CreateCopy(const char *pszFilename,
                                     GDALDataset *poSrcDS, int bStrict,
                                     char **papszOptions,
                                     GDALProgressFunc pfnProgress,
                                     void *pProgressData)
{
    const char *pszImageFormat =
        CSLFetchNameValueDef(papszOptions, "IMAGE_FORMAT", "RAW");
    GDALDataset *poSrcUnderlyingDS = PDS4GetUnderlyingDataset(poSrcDS);
    if (poSrcUnderlyingDS == nullptr)
        poSrcUnderlyingDS = poSrcDS;
    if (EQUAL(pszImageFormat, "GEOTIFF") &&
        strcmp(poSrcUnderlyingDS->GetDescription(),
               CSLFetchNameValueDef(
                   papszOptions, "IMAGE_FILENAME",
                   CPLResetExtensionSafe(pszFilename, "tif").c_str())) == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Output file has same name as input file");
        return nullptr;
    }
    if (poSrcDS->GetRasterCount() == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Unsupported band count");
        return nullptr;
    }

    const bool bAppend = CPLFetchBool(papszOptions, "APPEND_SUBDATASET", false);
    if (bAppend)
    {
        GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
        GDALDataset *poExistingDS = Open(&oOpenInfo);
        if (poExistingDS)
        {
            GDALGeoTransform existingGT;
            const bool bExistingHasGT =
                poExistingDS->GetGeoTransform(existingGT) == CE_None;
            GDALGeoTransform gt;
            const bool bSrcHasGT = poSrcDS->GetGeoTransform(gt) == CE_None;

            OGRSpatialReference oExistingSRS;
            OGRSpatialReference oSrcSRS;
            const char *pszExistingSRS = poExistingDS->GetProjectionRef();
            const char *pszSrcSRS = poSrcDS->GetProjectionRef();
            CPLString osExistingProj4;
            if (pszExistingSRS && pszExistingSRS[0])
            {
                oExistingSRS.SetFromUserInput(
                    pszExistingSRS,
                    OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
                char *pszExistingProj4 = nullptr;
                oExistingSRS.exportToProj4(&pszExistingProj4);
                if (pszExistingProj4)
                    osExistingProj4 = pszExistingProj4;
                CPLFree(pszExistingProj4);
            }
            CPLString osSrcProj4;
            if (pszSrcSRS && pszSrcSRS[0])
            {
                oSrcSRS.SetFromUserInput(
                    pszSrcSRS,
                    OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
                char *pszSrcProj4 = nullptr;
                oSrcSRS.exportToProj4(&pszSrcProj4);
                if (pszSrcProj4)
                    osSrcProj4 = pszSrcProj4;
                CPLFree(pszSrcProj4);
            }

            delete poExistingDS;

            const auto maxRelErrorGT =
                [](const GDALGeoTransform &gt1, const GDALGeoTransform &gt2)
            {
                double maxRelError = 0.0;
                for (int i = 0; i < 6; i++)
                {
                    if (gt1[i] == 0.0)
                    {
                        maxRelError = std::max(maxRelError, std::abs(gt2[i]));
                    }
                    else
                    {
                        maxRelError =
                            std::max(maxRelError, std::abs(gt2[i] - gt1[i]) /
                                                      std::abs(gt1[i]));
                    }
                }
                return maxRelError;
            };

            if ((bExistingHasGT && !bSrcHasGT) ||
                (!bExistingHasGT && bSrcHasGT) ||
                (bExistingHasGT && bSrcHasGT &&
                 maxRelErrorGT(existingGT, gt) > 1e-10))
            {
                CPLError(bStrict ? CE_Failure : CE_Warning, CPLE_NotSupported,
                         "Appending to a dataset with a different "
                         "geotransform is not supported");
                if (bStrict)
                    return nullptr;
            }
            // Do proj string comparison, as it is unlikely that
            // OGRSpatialReference::IsSame() will lead to identical reasons due
            // to PDS changing CRS names, etc...
            if (osExistingProj4 != osSrcProj4)
            {
                CPLError(bStrict ? CE_Failure : CE_Warning, CPLE_NotSupported,
                         "Appending to a dataset with a different "
                         "coordinate reference system is not supported");
                if (bStrict)
                    return nullptr;
            }
        }
    }

    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();
    const int nBands = poSrcDS->GetRasterCount();
    GDALDataType eType = poSrcDS->GetRasterBand(1)->GetRasterDataType();
    PDS4Dataset *poDS = CreateInternal(pszFilename, poSrcDS, nXSize, nYSize,
                                       nBands, eType, papszOptions);
    if (poDS == nullptr)
        return nullptr;

    GDALGeoTransform gt;
    if (poSrcDS->GetGeoTransform(gt) == CE_None && gt != GDALGeoTransform())
    {
        poDS->SetGeoTransform(gt);
    }

    if (poSrcDS->GetProjectionRef() != nullptr &&
        strlen(poSrcDS->GetProjectionRef()) > 0)
    {
        poDS->SetProjection(poSrcDS->GetProjectionRef());
    }

    for (int i = 1; i <= nBands; i++)
    {
        int bHasNoData = false;
        const double dfNoData =
            poSrcDS->GetRasterBand(i)->GetNoDataValue(&bHasNoData);
        if (bHasNoData)
            poDS->GetRasterBand(i)->SetNoDataValue(dfNoData);

        const double dfOffset = poSrcDS->GetRasterBand(i)->GetOffset();
        if (dfOffset != 0.0)
            poDS->GetRasterBand(i)->SetOffset(dfOffset);

        const double dfScale = poSrcDS->GetRasterBand(i)->GetScale();
        if (dfScale != 1.0)
            poDS->GetRasterBand(i)->SetScale(dfScale);

        poDS->GetRasterBand(i)->SetUnitType(
            poSrcDS->GetRasterBand(i)->GetUnitType());
    }

    if (poDS->m_bUseSrcLabel)
    {
        char **papszMD_PDS4 = poSrcDS->GetMetadata("xml:PDS4");
        if (papszMD_PDS4 != nullptr)
        {
            poDS->SetMetadata(papszMD_PDS4, "xml:PDS4");
        }
    }

    if (poDS->m_poExternalDS == nullptr)
    {
        // We don't need to initialize the imagery as we are going to copy it
        // completely
        poDS->m_bMustInitImageFile = false;
    }

    if (!CPLFetchBool(papszOptions, "CREATE_LABEL_ONLY", false))
    {
        CPLErr eErr = GDALDatasetCopyWholeRaster(poSrcDS, poDS, nullptr,
                                                 pfnProgress, pProgressData);
        poDS->FlushCache(false);
        if (eErr != CE_None)
        {
            delete poDS;
            return nullptr;
        }

        char **papszISIS3MD = poSrcDS->GetMetadata("json:ISIS3");
        if (papszISIS3MD)
        {
            poDS->SetMetadata(papszISIS3MD, "json:ISIS3");
        }
    }

    return poDS;
}

/************************************************************************/
/*                             Delete()                                 */
/************************************************************************/

CPLErr PDS4Dataset::Delete(const char *pszFilename)

{
    /* -------------------------------------------------------------------- */
    /*      Collect file list.                                              */
    /* -------------------------------------------------------------------- */
    GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
    auto poDS =
        std::unique_ptr<PDS4Dataset>(PDS4Dataset::OpenInternal(&oOpenInfo));
    if (poDS == nullptr)
    {
        if (CPLGetLastErrorNo() == 0)
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Unable to open %s to obtain file list.", pszFilename);

        return CE_Failure;
    }

    char **papszFileList = poDS->GetFileList();
    CPLString osImageFilename = poDS->m_osImageFilename;
    bool bCreatedFromExistingBinaryFile =
        poDS->m_bCreatedFromExistingBinaryFile;

    poDS.reset();

    if (CSLCount(papszFileList) == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unable to determine files associated with %s, "
                 "delete fails.",
                 pszFilename);
        CSLDestroy(papszFileList);
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Delete all files.                                               */
    /* -------------------------------------------------------------------- */
    CPLErr eErr = CE_None;
    for (int i = 0; papszFileList[i] != nullptr; ++i)
    {
        if (bCreatedFromExistingBinaryFile &&
            EQUAL(papszFileList[i], osImageFilename))
        {
            continue;
        }
        if (VSIUnlink(papszFileList[i]) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Deleting %s failed:\n%s",
                     papszFileList[i], VSIStrerror(errno));
            eErr = CE_Failure;
        }
    }

    CSLDestroy(papszFileList);

    return eErr;
}

/************************************************************************/
/*                         GDALRegister_PDS4()                          */
/************************************************************************/

void GDALRegister_PDS4()

{
    if (GDALGetDriverByName(PDS4_DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();
    PDS4DriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = PDS4Dataset::Open;
    poDriver->pfnCreate = PDS4Dataset::Create;
    poDriver->pfnCreateCopy = PDS4Dataset::CreateCopy;
    poDriver->pfnDelete = PDS4Dataset::Delete;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
