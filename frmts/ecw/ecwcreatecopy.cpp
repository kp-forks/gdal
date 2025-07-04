/******************************************************************************
 *
 * Project:  GDAL ECW Driver
 * Purpose:  ECW CreateCopy method implementation.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2001, 2004, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

// ncsjpcbuffer.h needs the min and max macros.
#undef NOMINMAX

#include "gdal_ecw.h"
#include "gdaljp2metadata.h"
#include "ogr_spatialref.h"

#if defined(HAVE_COMPRESS)

#define OPTIMIZED_FOR_GDALWARP

#if ECWSDK_VERSION >= 50
static CPLString GetCompressionSoftwareName()
{
    CPLString osRet;
    char szProcessName[2048];

    /* For privacy reason, allow the user to not write the software name in the
     * ECW */
    if (!CPLTestBool(
            CPLGetConfigOption("GDAL_ECW_WRITE_COMPRESSION_SOFTWARE", "YES")))
        return osRet;

    if (CPLGetExecPath(szProcessName, sizeof(szProcessName) - 1))
    {
        szProcessName[sizeof(szProcessName) - 1] = 0;
#ifdef _WIN32
        char *szLastSlash = strrchr(szProcessName, '\\');
#else
        char *szLastSlash = strrchr(szProcessName, '/');
#endif
        if (szLastSlash != nullptr)
            memmove(szProcessName, szLastSlash + 1,
                    strlen(szLastSlash + 1) + 1);
    }
    else
        strcpy(szProcessName, "Unknown");

    osRet.Printf("%s/GDAL v%d.%d.%d.%d/ECWJP2 SDK v%s", szProcessName,
                 GDAL_VERSION_MAJOR, GDAL_VERSION_MINOR, GDAL_VERSION_REV,
                 GDAL_VERSION_BUILD, NCS_ECWJP2_FULL_VERSION_STRING_DOT_DEL);
    return osRet;
}
#endif

class GDALECWCompressor final : public CNCSFile
{

  public:
    GDALECWCompressor();
    virtual ~GDALECWCompressor();
    virtual CNCSError WriteReadLine(UINT32 nNextLine,
                                    void **ppInputArray) override;
#if ECWSDK_VERSION >= 50
    virtual void WriteStatus(IEEE4 fPercentComplete,
                             const NCS::CString &sStatusText,
                             const CompressionCounters &Counters) override;
#else
    virtual void WriteStatus(UINT32 nCurrentLine) override;
#endif

    virtual bool WriteCancel() override;

    CPLErr Initialize(const char *pszFilename, char **papszOptions, int nXSize,
                      int nYSize, int nBands,
                      const char *const *papszBandDescriptions,
                      int bRGBColorSpace, GDALDataType eType,
                      const OGRSpatialReference *poSRS,
                      const GDALGeoTransform &gt, int nGCPCount,
                      const GDAL_GCP *pasGCPList, int bIsJPEG2000,
                      int bPixelIsPoint, char **papszRPCMD,
                      GDALDataset *poSrcDS = nullptr);
    CPLErr CloseDown();

    CPLErr WriteJP2Box(GDALJP2Box *);
    void WriteXMLBoxes();
    CPLErr ourWriteLineBIL(UINT16 nBands, void **ppOutputLine,
                           UINT32 *pLineSteps = nullptr);
#if ECWSDK_VERSION >= 50
    virtual NCSEcwCellType WriteReadLineGetCellType() override
    {
        return sFileInfo.eCellType;
    }
#endif
#ifdef ECW_FW
    CNCSJP2File::CNCSJPXAssocBox m_oGMLAssoc;
#endif

    // Data

    GDALDataset *m_poSrcDS;

    std::shared_ptr<VSIIOStream> m_OStream;
    int m_nPercentComplete;

    int m_bCanceled;

    GDALProgressFunc pfnProgress;
    void *pProgressData;

    GDALDataType eWorkDT;
    int m_nSwathLines;
    UINT32 m_nSwathOffset;
    GByte *m_pabySwathBuf;
    JP2UserBox **papoJP2UserBox;
    int nJP2UserBox;
    std::vector<int> m_anBandMap{};

  private:
    NCSFileViewFileInfoEx sFileInfo;

    /* To fix 'warning: ‘virtual NCS::CView& NCS::CView::operator=(const
     * NCS::CView&)’ was hidden ' with SDK 5 */
#if ECWSDK_VERSION >= 50
    using CNCSFile::operator=;
#endif
    CPL_DISALLOW_COPY_ASSIGN(GDALECWCompressor)
};

/************************************************************************/
/*                         GDALECWCompressor()                          */
/************************************************************************/

GDALECWCompressor::GDALECWCompressor()
    : m_OStream(std::make_shared<VSIIOStream>()), eWorkDT(GDT_Unknown),
      m_nSwathLines(0), m_nSwathOffset(0), m_pabySwathBuf(nullptr)
{
    m_poSrcDS = nullptr;
    m_nPercentComplete = -1;
    m_bCanceled = FALSE;
    pfnProgress = GDALDummyProgress;
    pProgressData = nullptr;
    papoJP2UserBox = nullptr;
    nJP2UserBox = 0;
#if ECWSDK_VERSION >= 50
    NCSInitFileInfo(&sFileInfo);
#else
    NCSInitFileInfoEx(&sFileInfo);
#endif
    m_anBandMap.resize(sFileInfo.nBands);
    for (int iBand = 0; iBand < sFileInfo.nBands; iBand++)
        m_anBandMap[iBand] = iBand + 1;
}

/************************************************************************/
/*                         ~GDALECWCompressor()                         */
/************************************************************************/

GDALECWCompressor::~GDALECWCompressor()

{
    int i;
    for (i = 0; i < nJP2UserBox; i++)
        delete papoJP2UserBox[i];
    CPLFree(papoJP2UserBox);

#if ECWSDK_VERSION >= 50
    NCSFreeFileInfo(&sFileInfo);
#else
    NCSFreeFileInfoEx(&sFileInfo);
#endif
    CPLFree(m_pabySwathBuf);
}

/************************************************************************/
/*                             CloseDown()                              */
/************************************************************************/

CPLErr GDALECWCompressor::CloseDown()

{
    Close(true);
    m_OStream->Close();

    return CE_None;
}

/************************************************************************/
/*                           WriteReadLine()                            */
/************************************************************************/

CNCSError GDALECWCompressor::WriteReadLine(UINT32 nNextLine,
                                           void **ppInputArray)

{
    CPLErr eErr;

#ifdef DEBUG_VERBOSE
    CPLDebug("ECW", "nNextLine = %d", nNextLine);
#endif

    if (m_poSrcDS == nullptr || m_poSrcDS->GetRasterBand(1) == nullptr)
    {
        return GetCNCSError(NCS_FILEIO_ERROR);
    }
    if (m_nSwathLines <= 0)
    {
        int nBlockX;
        constexpr int MIN_SWATH_LINES = 256;
        m_poSrcDS->GetRasterBand(1)->GetBlockSize(&nBlockX, &m_nSwathLines);
        if (m_nSwathLines < MIN_SWATH_LINES)
            m_nSwathLines = MIN_SWATH_LINES;
    }

    const GSpacing nPixelSpace = GDALGetDataTypeSizeBytes(eWorkDT);
    const GSpacing nLineSpace = sFileInfo.nSizeX * nPixelSpace;
    const GSpacing nBandSpace = nLineSpace * m_nSwathLines;

    if (m_pabySwathBuf == nullptr)
    {
        size_t nBufSize = static_cast<size_t>(nBandSpace * sFileInfo.nBands);
        m_pabySwathBuf = (GByte *)VSI_MALLOC_VERBOSE(nBufSize);
    }
    if (m_pabySwathBuf == nullptr)
    {
        return GetCNCSError(NCS_FILE_NO_MEMORY);
    }

    if (nNextLine == 0 || nNextLine >= m_nSwathOffset + m_nSwathLines)
    {
        int nSwathLines = m_nSwathLines;
        if (nNextLine + nSwathLines > sFileInfo.nSizeY)
        {
            nSwathLines = sFileInfo.nSizeY - nNextLine;
        }
        eErr = m_poSrcDS->RasterIO(
            GF_Read, 0, nNextLine, sFileInfo.nSizeX, nSwathLines,
            m_pabySwathBuf, sFileInfo.nSizeX, nSwathLines, eWorkDT,
            sFileInfo.nBands, &m_anBandMap[0], nPixelSpace, nLineSpace,
            nBandSpace, nullptr);
        m_nSwathOffset = nNextLine;
        UINT32 nNextSwathLine = nNextLine + nSwathLines;
        if (nNextSwathLine < sFileInfo.nSizeY)
        {
            if (nNextSwathLine + nSwathLines > sFileInfo.nSizeY)
            {
                nSwathLines = sFileInfo.nSizeY - nNextSwathLine;
            }
            m_poSrcDS->AdviseRead(0, nNextSwathLine, sFileInfo.nSizeX,
                                  nSwathLines, sFileInfo.nSizeX, nSwathLines,
                                  eWorkDT, sFileInfo.nBands, &m_anBandMap[0],
                                  nullptr);
        }
    }
    else
    {
        eErr = CE_None;
    }

    for (int iBand = 0; iBand < (int)sFileInfo.nBands; iBand++)
    {
        memcpy(ppInputArray[iBand],
               m_pabySwathBuf + nLineSpace * (nNextLine - m_nSwathOffset) +
                   nBandSpace * iBand,
               static_cast<size_t>(nPixelSpace * sFileInfo.nSizeX));
    }

    if (eErr == CE_None)
        return GetCNCSError(NCS_SUCCESS);
    else
        return GetCNCSError(NCS_FILEIO_ERROR);
}

/************************************************************************/
/*                            WriteStatus()                             */
/************************************************************************/
#if ECWSDK_VERSION >= 50
void GDALECWCompressor::WriteStatus(IEEE4 fPercentComplete,
                                    const NCS::CString &sStatusText,
                                    const CompressionCounters &Counters)
{
    std::string sStatusUTF8;
    sStatusText.utf8_str(sStatusUTF8);

    m_bCanceled = !pfnProgress(fPercentComplete / 100.0, sStatusUTF8.c_str(),
                               pProgressData);
}
#else

void GDALECWCompressor::WriteStatus(UINT32 nCurrentLine)

{
    m_bCanceled = !pfnProgress(nCurrentLine / (float)sFileInfo.nSizeY, nullptr,
                               pProgressData);
}
#endif
/************************************************************************/
/*                            WriteCancel()                             */
/************************************************************************/

bool GDALECWCompressor::WriteCancel()

{
    return (bool)m_bCanceled;
}

/************************************************************************/
/*                            WriteJP2Box()                             */
/************************************************************************/

CPLErr GDALECWCompressor::WriteJP2Box(GDALJP2Box *poBox)

{
    JP2UserBox *poECWBox;

    if (poBox == nullptr)
        return CE_None;

    poECWBox = new JP2UserBox();
    memcpy(&(poECWBox->m_nTBox), poBox->GetType(), 4);
    CPL_MSBPTR32(&(poECWBox->m_nTBox));

    poECWBox->SetData((int)poBox->GetDataLength(), poBox->GetWritableData());

    AddBox(poECWBox);

    delete poBox;

    papoJP2UserBox = (JP2UserBox **)CPLRealloc(
        papoJP2UserBox, (nJP2UserBox + 1) * sizeof(JP2UserBox *));
    papoJP2UserBox[nJP2UserBox] = poECWBox;
    nJP2UserBox++;

    return CE_None;
}

/************************************************************************/
/*                         WriteXMLBoxes()                              */
/************************************************************************/

void GDALECWCompressor::WriteXMLBoxes()
{
    int nBoxes = 0;
    GDALJP2Box **papoBoxes =
        GDALJP2Metadata::CreateXMLBoxes(m_poSrcDS, &nBoxes);
    for (int i = 0; i < nBoxes; i++)
    {
        WriteJP2Box(papoBoxes[i]);
    }
    CPLFree(papoBoxes);
}

/************************************************************************/
/*                          ourWriteLineBIL()                           */
/************************************************************************/

CPLErr GDALECWCompressor::ourWriteLineBIL(UINT16 nBands, void **ppOutputLine,
                                          UINT32 *pLineSteps)
{

    CNCSError oError = CNCSFile::WriteLineBIL(sFileInfo.eCellType, nBands,
                                              ppOutputLine, pLineSteps);

    if (oError.GetErrorNumber() != NCS_SUCCESS)
    {
        ECWReportError(oError, "Scanline write write failed.\n");
        return CE_Failure;
    }
    return CE_None;
}

/************************************************************************/
/*                             Initialize()                             */
/*                                                                      */
/*      Initialize compressor output.                                   */
/************************************************************************/

CPLErr GDALECWCompressor::Initialize(
    const char *pszFilename, char **papszOptions, int nXSize, int nYSize,
    int nBands, const char *const *papszBandDescriptions, int bRGBColorSpace,
    GDALDataType eType, const OGRSpatialReference *poSRS,
    const GDALGeoTransform &gt, int nGCPCount, const GDAL_GCP *pasGCPList,
    int bIsJPEG2000, int bPixelIsPoint, char **papszRPCMD, GDALDataset *poSrcDS)

{
/* -------------------------------------------------------------------- */
/*      For 4.x and beyond you need a license key to compress data.     */
/*      Check for it as a configuration option or a creation option.    */
/* -------------------------------------------------------------------- */
#if ECWSDK_VERSION >= 40
    const char *pszECWKey = CSLFetchNameValue(papszOptions, "ECW_ENCODE_KEY");
    if (pszECWKey == nullptr)
        pszECWKey = CPLGetConfigOption("ECW_ENCODE_KEY", nullptr);

    const char *pszECWCompany =
        CSLFetchNameValue(papszOptions, "ECW_ENCODE_COMPANY");
    if (pszECWCompany == nullptr)
        pszECWCompany = CPLGetConfigOption("ECW_ENCODE_COMPANY", nullptr);

    if (pszECWKey && pszECWCompany)
    {
        CPLDebug("ECW", "SetOEMKey(%s,%s)", pszECWCompany, pszECWKey);
        CNCSFile::SetOEMKey((char *)pszECWCompany, (char *)pszECWKey);
    }
    else if (pszECWKey || pszECWCompany)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Only one of ECW_ENCODE_KEY and ECW_ENCODE_COMPANY were "
                 "provided.\nBoth are required.");
        return CE_Failure;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "None of ECW_ENCODE_KEY and ECW_ENCODE_COMPANY were "
                 "provided.\nBoth are required.");
        return CE_Failure;
    }

#endif /* ECWSDK_VERSION >= 40 */

    /* -------------------------------------------------------------------- */
    /*      Do some rudimentary checking in input.                          */
    /* -------------------------------------------------------------------- */
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "ECW driver requires at least one band.");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Parse out some known options.                                   */
    /* -------------------------------------------------------------------- */
    float fTargetCompression;

    // Default compression based on image type per request from Paul Beaty.
    if (nBands > 1)
        fTargetCompression = 95.0;
    else
        fTargetCompression = 90.0;

    if (CSLFetchNameValue(papszOptions, "TARGET") != nullptr)
    {
        fTargetCompression =
            (float)CPLAtof(CSLFetchNameValue(papszOptions, "TARGET"));

        /* The max allowed value should be 100 - 100 / 65535 = 99.9984740978 */
        /* so that nCompressionRate fits on a uint16 (see below) */
        /* No need to be so pedantic, so we will limit to 99.99 % */
        /* (compression rate = 10 000) */
        if (fTargetCompression < 0.0 || fTargetCompression > 99.99)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "TARGET compression of %.3f invalid, should be a\n"
                     "value between 0 and 99.99 percent.\n",
                     (double)fTargetCompression);
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create and initialize compressor.                               */
    /* -------------------------------------------------------------------- */
    NCSFileViewFileInfoEx *psClient = &(sFileInfo);
    const char *pszOption = nullptr;
#if ECWSDK_VERSION >= 50
    if (bIsJPEG2000 == FALSE)
    {
        bool bECWV3 = false;
        pszOption = CSLFetchNameValue(papszOptions, "ECW_FORMAT_VERSION");
        if (pszOption != nullptr)
        {
            bECWV3 = (3 == atoi(pszOption));
        }
        psClient->nFormatVersion = (bECWV3) ? 3 : 2;
    }
    else
    {
        psClient->nFormatVersion = 1;
    }
#endif
    psClient->nBands = (UINT16)nBands;
    psClient->nSizeX = nXSize;
    psClient->nSizeY = nYSize;
    psClient->nCompressionRate =
        (UINT16)MAX(1, 100 / (100 - fTargetCompression));
    psClient->eCellSizeUnits = ECW_CELL_UNITS_METERS;

    if (nBands == 1)
        psClient->eColorSpace = NCSCS_GREYSCALE;
    else if (nBands == 3 && bRGBColorSpace)
        psClient->eColorSpace = NCSCS_sRGB;
#if ECWSDK_VERSION >= 40
    else if (nBands == 4 && bRGBColorSpace)
        psClient->eColorSpace = NCSCS_sRGB;
#endif
    else
        psClient->eColorSpace = NCSCS_MULTIBAND;

    /* -------------------------------------------------------------------- */
    /*      Figure out the data type.                                       */
    /* -------------------------------------------------------------------- */
    int bSigned = FALSE;
    int nBits = 8;
    eWorkDT = eType;

    switch (eWorkDT)
    {
        case GDT_Byte:
#if ECWSDK_VERSION >= 50
            psClient->nCellBitDepth = 8;
#endif
            psClient->eCellType = NCSCT_UINT8;
            nBits = 8;
            bSigned = FALSE;
            break;

        case GDT_UInt16:
#if ECWSDK_VERSION >= 50
            psClient->nCellBitDepth = 16;
#endif
            psClient->eCellType = NCSCT_UINT16;
            nBits = 16;
            bSigned = FALSE;
            break;

        case GDT_UInt32:
#if ECWSDK_VERSION >= 50
            psClient->nCellBitDepth = 32;
#endif
            psClient->eCellType = NCSCT_UINT32;
            nBits = 32;
            bSigned = FALSE;
            break;

        case GDT_Int16:
#if ECWSDK_VERSION >= 50
            psClient->nCellBitDepth = 16;
#endif
            psClient->eCellType = NCSCT_INT16;
            nBits = 16;
            bSigned = TRUE;
            break;

        case GDT_Int32:
#if ECWSDK_VERSION >= 50
            psClient->nCellBitDepth = 32;
#endif
            psClient->eCellType = NCSCT_INT32;
            nBits = 32;
            bSigned = TRUE;
            break;

        case GDT_Float32:
            psClient->eCellType = NCSCT_IEEE4;
            nBits = 32;
            bSigned = TRUE;
            break;

#if ECWSDK_VERSION >= 40
        case GDT_Float64:
            psClient->eCellType = NCSCT_IEEE8;
            nBits = 64;
            bSigned = TRUE;
            break;
#endif

        default:
            // We treat complex types as float.
            psClient->eCellType = NCSCT_IEEE4;
            nBits = 32;
            bSigned = TRUE;
            eWorkDT = GDT_Float32;
            break;
    }

    /* -------------------------------------------------------------------- */
    /*      Create band information structures.                             */
    /* -------------------------------------------------------------------- */
    int iBand;

    psClient->pBands =
        (NCSFileBandInfo *)NCSMalloc(sizeof(NCSFileBandInfo) * nBands, true);
    for (iBand = 0; iBand < nBands; iBand++)
    {
        const char *pszNBITS = CSLFetchNameValue(papszOptions, "NBITS");
        if (pszNBITS && atoi(pszNBITS) > 0)
            psClient->pBands[iBand].nBits = (UINT8)atoi(pszNBITS);
        else
            psClient->pBands[iBand].nBits = (UINT8)nBits;
        psClient->pBands[iBand].bSigned = (BOOLEAN)bSigned;
#if ECWSDK_VERSION >= 50
        psClient->pBands[iBand].szDesc =
            NCSStrDup(papszBandDescriptions[iBand]);
#else
        psClient->pBands[iBand].szDesc =
            NCSStrDup((char *)papszBandDescriptions[iBand]);
#endif
    }

    /* -------------------------------------------------------------------- */
    /*      Allow CNCSFile::SetParameter() requests.                        */
    /* -------------------------------------------------------------------- */

    if (bIsJPEG2000)
    {
        pszOption = CSLFetchNameValue(papszOptions, "PROFILE");
        if (pszOption != nullptr && EQUAL(pszOption, "BASELINE_0"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROFILE_BASELINE_0);
        else if (pszOption != nullptr && EQUAL(pszOption, "BASELINE_1"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROFILE_BASELINE_1);
        else if (pszOption != nullptr && EQUAL(pszOption, "BASELINE_2"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROFILE_BASELINE_2);
        else if (pszOption != nullptr && EQUAL(pszOption, "NPJE"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROFILE_NITF_BIIF_NPJE);
        else if (pszOption != nullptr && EQUAL(pszOption, "EPJE"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROFILE_NITF_BIIF_EPJE);

        pszOption = CSLFetchNameValue(papszOptions, "CODESTREAM_ONLY");
        if (pszOption == nullptr &&
            EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "j2k"))
            pszOption = "YES";
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_CODESTREAM_ONLY,
                         CPLTestBool(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "LEVELS");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_LEVELS,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "LAYERS");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_LAYERS,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "PRECINCT_WIDTH");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PRECINCT_WIDTH,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "PRECINCT_HEIGHT");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PRECINCT_HEIGHT,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "TILE_WIDTH");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_TILE_WIDTH,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "TILE_HEIGHT");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_TILE_HEIGHT,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "INCLUDE_SOP");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_INCLUDE_SOP,
                         CPLTestBool(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "INCLUDE_EPH");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_INCLUDE_EPH,
                         CPLTestBool(pszOption));

        pszOption = CSLFetchNameValue(papszOptions, "PROGRESSION");
        if (pszOption != nullptr && EQUAL(pszOption, "LRCP"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROGRESSION_LRCP);

        else if (pszOption != nullptr && EQUAL(pszOption, "RLCP"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROGRESSION_RLCP);

        else if (pszOption != nullptr && EQUAL(pszOption, "RPCL"))
            SetParameter(CNCSJP2FileView::JP2_COMPRESS_PROGRESSION_RPCL);

        pszOption = CSLFetchNameValue(papszOptions, "GEODATA_USAGE");
        if (pszOption == nullptr)
            // Default to suppressing ECW SDK geodata, just use our own stuff.
            SetGeodataUsage(JP2_GEODATA_USE_NONE);
        else if (EQUAL(pszOption, "NONE"))
            SetGeodataUsage(JP2_GEODATA_USE_NONE);
        else if (EQUAL(pszOption, "PCS_ONLY"))
            SetGeodataUsage(JP2_GEODATA_USE_PCS_ONLY);
        else if (EQUAL(pszOption, "GML_ONLY"))
            SetGeodataUsage(JP2_GEODATA_USE_GML_ONLY);
        else if (EQUAL(pszOption, "PCS_GML"))
            SetGeodataUsage(JP2_GEODATA_USE_PCS_GML);
        else if (EQUAL(pszOption, "GML_PCS"))
            SetGeodataUsage(JP2_GEODATA_USE_GML_PCS);
        else if (EQUAL(pszOption, "ALL"))
            SetGeodataUsage(JP2_GEODATA_USE_GML_PCS_WLD);

        pszOption = CSLFetchNameValue(papszOptions, "DECOMPRESS_LAYERS");
        if (pszOption != nullptr)
            SetParameter(CNCSJP2FileView::JP2_DECOMPRESS_LAYERS,
                         (UINT32)atoi(pszOption));

        pszOption = CSLFetchNameValue(papszOptions,
                                      "DECOMPRESS_RECONSTRUCTION_PARAMETER");
        if (pszOption != nullptr)
            SetParameter(
                CNCSJP2FileView::JPC_DECOMPRESS_RECONSTRUCTION_PARAMETER,
                (IEEE4)CPLAtof(pszOption));
    }

    /* -------------------------------------------------------------------- */
    /*      Georeferencing.                                                 */
    /* -------------------------------------------------------------------- */

    psClient->fOriginX = 0.0;
    psClient->fOriginY = psClient->nSizeY;
    psClient->fCellIncrementX = 1.0;
    psClient->fCellIncrementY = -1.0;
    psClient->fCWRotationDegrees = 0.0;

    if (gt[2] != 0.0 || gt[4] != 0.0)
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Rotational coefficients ignored, georeferencing of\n"
                 "output ECW file will be incorrect.\n");
    else
    {
        psClient->fOriginX = gt[0];
        psClient->fOriginY = gt[3];
        psClient->fCellIncrementX = gt[1];
        psClient->fCellIncrementY = gt[5];
    }

    /* -------------------------------------------------------------------- */
    /*      Projection.                                                     */
    /* -------------------------------------------------------------------- */
    char szProjection[128];
    char szDatum[128];
    char szUnits[128];

    strcpy(szProjection, "RAW");
    strcpy(szDatum, "RAW");

    if (CSLFetchNameValue(papszOptions, "PROJ") != nullptr)
    {
        strncpy(szProjection, CSLFetchNameValue(papszOptions, "PROJ"),
                sizeof(szProjection));
        szProjection[sizeof(szProjection) - 1] = 0;
    }

    if (CSLFetchNameValue(papszOptions, "DATUM") != nullptr)
    {
        strncpy(szDatum, CSLFetchNameValue(papszOptions, "DATUM"),
                sizeof(szDatum));
        szDatum[sizeof(szDatum) - 1] = 0;
        if (EQUAL(szProjection, "RAW"))
            strcpy(szProjection, "GEODETIC");
    }

    const char *pszUnits = CSLFetchNameValue(papszOptions, "UNITS");
    if (pszUnits != nullptr)
    {
        psClient->eCellSizeUnits = ECWTranslateToCellSizeUnits(pszUnits);
    }

    if (EQUAL(szProjection, "RAW") && poSRS != nullptr && !poSRS->IsEmpty())
    {
        ECWTranslateFromWKT(poSRS, szProjection, sizeof(szProjection), szDatum,
                            sizeof(szDatum), szUnits);
        psClient->eCellSizeUnits = ECWTranslateToCellSizeUnits(szUnits);
    }

    NCSFree(psClient->szDatum);
    psClient->szDatum = NCSStrDup(szDatum);
    NCSFree(psClient->szProjection);
    psClient->szProjection = NCSStrDup(szProjection);

    CPLDebug("ECW", "Writing with PROJ=%s, DATUM=%s, UNITS=%s", szProjection,
             szDatum, ECWTranslateFromCellSizeUnits(psClient->eCellSizeUnits));

    /* -------------------------------------------------------------------- */
    /*      Setup GML and GeoTIFF information.                              */
    /* -------------------------------------------------------------------- */
    if ((poSRS != nullptr && !poSRS->IsEmpty()) || gt != GDALGeoTransform() ||
        nGCPCount > 0 || papszRPCMD != nullptr)
    {
        GDALJP2Metadata oJP2MD;

        oJP2MD.SetSpatialRef(poSRS);
        oJP2MD.SetGeoTransform(gt);
        oJP2MD.SetGCPs(nGCPCount, pasGCPList);
        oJP2MD.bPixelIsPoint = bPixelIsPoint;
        oJP2MD.SetRPCMD(papszRPCMD);

        if (bIsJPEG2000)
        {
            if (CPLFetchBool(papszOptions, "WRITE_METADATA", false))
            {
                if (!CPLFetchBool(papszOptions, "MAIN_MD_DOMAIN_ONLY", false))
                {
                    WriteXMLBoxes();
                }
                WriteJP2Box(
                    GDALJP2Metadata::CreateGDALMultiDomainMetadataXMLBox(
                        m_poSrcDS, CPLFetchBool(papszOptions,
                                                "MAIN_MD_DOMAIN_ONLY", false)));
            }
            if (CPLFetchBool(papszOptions, "GMLJP2", true))
            {
                const char *pszGMLJP2V2Def =
                    CSLFetchNameValue(papszOptions, "GMLJP2V2_DEF");
                if (pszGMLJP2V2Def != nullptr)
                {
                    WriteJP2Box(oJP2MD.CreateGMLJP2V2(nXSize, nYSize,
                                                      pszGMLJP2V2Def, poSrcDS));
                }
                else
                {
                    if (!poSRS || poSRS->IsEmpty() ||
                        GDALJP2Metadata::IsSRSCompatible(poSRS))
                    {
                        WriteJP2Box(oJP2MD.CreateGMLJP2(nXSize, nYSize));
                    }
                    else if (CSLFetchNameValue(papszOptions, "GMLJP2"))
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "GMLJP2 box was explicitly required but "
                                 "cannot be written due "
                                 "to lack of georeferencing and/or unsupported "
                                 "georeferencing "
                                 "for GMLJP2");
                    }
                    else
                    {
                        CPLDebug(
                            "JP2ECW",
                            "Cannot write GMLJP2 box due to unsupported SRS");
                    }
                }
            }
            if (CPLFetchBool(papszOptions, "GeoJP2", true))
                WriteJP2Box(oJP2MD.CreateJP2GeoTIFF());
            if (CPLFetchBool(papszOptions, "WRITE_METADATA", false) &&
                !CPLFetchBool(papszOptions, "MAIN_MD_DOMAIN_ONLY", false))
            {
                WriteJP2Box(GDALJP2Metadata::CreateXMPBox(m_poSrcDS));
            }
        }
    }
    /* -------------------------------------------------------------------- */
    /*      We handle all jpeg2000 files via the VSIIOStream, but ECW       */
    /*      files cannot be done this way for some reason.                  */
    /* -------------------------------------------------------------------- */
    VSILFILE *fpVSIL = nullptr;

    if (bIsJPEG2000)
    {
        int bSeekable = !(STARTS_WITH(pszFilename, "/vsistdout/") ||
                          STARTS_WITH(pszFilename, "/vsizip/") ||
                          STARTS_WITH(pszFilename, "/vsigzip/"));
        fpVSIL = VSIFOpenL(pszFilename, (bSeekable) ? "wb+" : "wb");
        if (fpVSIL == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed, "Failed to open/create %s.",
                     pszFilename);
            return CE_Failure;
        }

        m_OStream->Access(fpVSIL, TRUE, (BOOLEAN)bSeekable, pszFilename, 0, -1);
    }
    else
    {
        if (!STARTS_WITH(pszFilename, "/vsi"))
        {
            // Try now to create the file to avoid memory leaks if it is
            // the SDK that fails to do it.
            fpVSIL = VSIFOpenL(pszFilename, "wb");
            if (fpVSIL == nullptr)
            {
                CPLError(CE_Failure, CPLE_OpenFailed,
                         "Failed to open/create %s.", pszFilename);
                return CE_Failure;
            }
            VSIFCloseL(fpVSIL);
            VSIUnlink(pszFilename);
            fpVSIL = nullptr;
        }
    }

/* -------------------------------------------------------------------- */
/*      Check if we can enable large files.  This option should only    */
/*      be set when the application is adhering to one of the           */
/*      ERMapper options for licensing larger than 500MB input          */
/*      files.  See Bug 767.  This option no longer exists with         */
/*      version 4+.                                                     */
/* -------------------------------------------------------------------- */
#if ECWSDK_VERSION < 40
    const char *pszLargeOK = CSLFetchNameValue(papszOptions, "LARGE_OK");
    if (pszLargeOK == nullptr)
        pszLargeOK = "NO";

    pszLargeOK = CPLGetConfigOption("ECW_LARGE_OK", pszLargeOK);

    if (CPLTestBool(pszLargeOK))
    {
        CNCSFile::SetKeySize();
        CPLDebug("ECW", "Large file generation enabled.");
    }
#endif /* ECWSDK_VERSION < 40 */
/* -------------------------------------------------------------------- */
/*      Infer metadata information from source dataset if possible      */
/* -------------------------------------------------------------------- */
#if ECWSDK_VERSION >= 50
    if (psClient->nFormatVersion > 2)
    {
        if (psClient->pFileMetaData == nullptr)
        {
            NCSEcwInitMetaData(&psClient->pFileMetaData);
        }
        if (m_poSrcDS && m_poSrcDS->GetMetadataItem(
                             "FILE_METADATA_ACQUISITION_DATE") != nullptr)
        {
            psClient->pFileMetaData->sAcquisitionDate =
                NCSStrDupT(NCS::CString(m_poSrcDS->GetMetadataItem(
                                            "FILE_METADATA_ACQUISITION_DATE"))
                               .c_str());
        }

        if (m_poSrcDS &&
            m_poSrcDS->GetMetadataItem(
                "FILE_METADATA_ACQUISITION_SENSOR_NAME") != nullptr)
        {
            psClient->pFileMetaData->sAcquisitionSensorName = NCSStrDupT(
                NCS::CString(m_poSrcDS->GetMetadataItem(
                                 "FILE_METADATA_ACQUISITION_SENSOR_NAME"))
                    .c_str());
        }
        if (m_poSrcDS &&
            m_poSrcDS->GetMetadataItem("FILE_METADATA_ADDRESS") != nullptr)
        {
            psClient->pFileMetaData->sAddress =
                NCSStrDupT(NCS::CString(m_poSrcDS->GetMetadataItem(
                                            "FILE_METADATA_ADDRESS"))
                               .c_str());
        }
        if (m_poSrcDS &&
            m_poSrcDS->GetMetadataItem("FILE_METADATA_AUTHOR") != nullptr)
        {
            psClient->pFileMetaData->sAuthor = NCSStrDupT(
                NCS::CString(m_poSrcDS->GetMetadataItem("FILE_METADATA_AUTHOR"))
                    .c_str());
        }
        if (m_poSrcDS && m_poSrcDS->GetMetadataItem(
                             "FILE_METADATA_CLASSIFICATION") != nullptr)
        {
            psClient->pFileMetaData->sClassification =
                NCSStrDupT(NCS::CString(m_poSrcDS->GetMetadataItem(
                                            "FILE_METADATA_CLASSIFICATION"))
                               .c_str());
        }
        if (pszECWCompany != nullptr &&
            CPLTestBool(CPLGetConfigOption("GDAL_ECW_WRITE_COMPANY", "YES")))
        {
            psClient->pFileMetaData->sCompany =
                NCSStrDupT(NCS::CString(pszECWCompany).c_str());
        }
        CPLString osCompressionSoftware = GetCompressionSoftwareName();
        if (!osCompressionSoftware.empty())
        {
            psClient->pFileMetaData->sCompressionSoftware =
                NCSStrDupT(NCS::CString(osCompressionSoftware.c_str()).c_str());
        }
        if (m_poSrcDS &&
            m_poSrcDS->GetMetadataItem("FILE_METADATA_COPYRIGHT") != nullptr)
        {
            psClient->pFileMetaData->sCopyright =
                NCSStrDupT(NCS::CString(m_poSrcDS->GetMetadataItem(
                                            "FILE_METADATA_COPYRIGHT"))
                               .c_str());
        }
        if (m_poSrcDS &&
            m_poSrcDS->GetMetadataItem("FILE_METADATA_EMAIL") != nullptr)
        {
            psClient->pFileMetaData->sEmail = NCSStrDupT(
                NCS::CString(m_poSrcDS->GetMetadataItem("FILE_METADATA_EMAIL"))
                    .c_str());
        }
        if (m_poSrcDS &&
            m_poSrcDS->GetMetadataItem("FILE_METADATA_TELEPHONE") != nullptr)
        {
            psClient->pFileMetaData->sTelephone =
                NCSStrDupT(NCS::CString(m_poSrcDS->GetMetadataItem(
                                            "FILE_METADATA_TELEPHONE"))
                               .c_str());
        }
    }
#endif
    /* -------------------------------------------------------------------- */
    /*      Set the file info.                                              */
    /* -------------------------------------------------------------------- */
    CNCSError oError = SetFileInfo(sFileInfo);

    if (oError.GetErrorNumber() == NCS_SUCCESS)
    {
        if (fpVSIL == nullptr)
        {
#if ECWSDK_VERSION >= 40 && defined(_WIN32)
            if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
            {
                wchar_t *pwszFilename =
                    CPLRecodeToWChar(pszFilename, CPL_ENC_UTF8, CPL_ENC_UCS2);
                oError = GetCNCSError(Open(pwszFilename, false, true));
                CPLFree(pwszFilename);
            }
            else
#endif
            {
                oError = GetCNCSError(Open((char *)pszFilename, false, true));
            }
        }
        else
        {
#if ECWSDK_VERSION >= 55
            oError = CNCSJP2FileView::Open(m_OStream);
#else
            oError = CNCSJP2FileView::Open(m_OStream.get());
#endif
        }
    }

    if (oError.GetErrorNumber() == NCS_SUCCESS)
        return CE_None;
    else if (oError.GetErrorNumber() == NCS_INPUT_SIZE_EXCEEDED)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ECW SDK compress limit exceeded.");
        return CE_Failure;
    }
    else
    {
        ECWReportError(oError);

        return CE_Failure;
    }
}

/************************************************************************/
/*                      ECWIsInputRGBColorSpace()                       */
/************************************************************************/

static int ECWIsInputRGBColorSpace(GDALDataset *poSrcDS)
{
    int nBands = poSrcDS->GetRasterCount();

    /* -------------------------------------------------------------------- */
    /*      Is the input RGB or RGBA?                                       */
    /* -------------------------------------------------------------------- */
    int bRGBColorSpace = FALSE;
    int bRGB = FALSE;
    if (nBands >= 3)
    {
        bRGB = (poSrcDS->GetRasterBand(1)->GetColorInterpretation() ==
                GCI_RedBand);
        bRGB &= (poSrcDS->GetRasterBand(2)->GetColorInterpretation() ==
                 GCI_GreenBand);
        bRGB &= (poSrcDS->GetRasterBand(3)->GetColorInterpretation() ==
                 GCI_BlueBand);
    }
    if (nBands == 3)
    {
        bRGBColorSpace = bRGB;
    }
    else if (nBands == 4 && bRGB)
    {
        bRGBColorSpace = (poSrcDS->GetRasterBand(4)->GetColorInterpretation() ==
                          GCI_AlphaBand);
    }

    return bRGBColorSpace;
}

/************************************************************************/
/*                           ECWCreateCopy()                            */
/************************************************************************/

static GDALDataset *ECWCreateCopy(const char *pszFilename, GDALDataset *poSrcDS,
                                  int bStrict, char **papszOptions,
                                  GDALProgressFunc pfnProgress,
                                  void *pProgressData, int bIsJPEG2000)

{
    ECWInitialize();

    /* -------------------------------------------------------------------- */
    /*      Get various values from the source dataset.                     */
    /* -------------------------------------------------------------------- */
    int nBands = poSrcDS->GetRasterCount();
    int nXSize = poSrcDS->GetRasterXSize();
    int nYSize = poSrcDS->GetRasterYSize();

    if (nBands == 0)
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "ECW driver does not support source dataset with zero band.\n");
        return nullptr;
    }

    GDALDataType eType = poSrcDS->GetRasterBand(1)->GetRasterDataType();

    const OGRSpatialReference *poSRS = poSrcDS->GetSpatialRef();
    GDALGeoTransform gt;
    poSrcDS->GetGeoTransform(gt);

    if (poSrcDS->GetGCPCount() > 0)
        poSRS = poSrcDS->GetGCPSpatialRef();

        /* --------------------------------------------------------------------
         */
        /*      For ECW, confirm the datatype is 8bit (or uint16 for ECW v3) */
        /* --------------------------------------------------------------------
         */
#if ECWSDK_VERSION >= 50
    bool bECWV3 = false;
    if (bIsJPEG2000 == FALSE)
    {
        const char *pszOption =
            CSLFetchNameValue(papszOptions, "ECW_FORMAT_VERSION");
        if (pszOption != nullptr)
        {
            bECWV3 = (3 == atoi(pszOption));
        }
    }
#endif
    if (!(eType == GDT_Byte ||
#if ECWSDK_VERSION >= 50
          (bECWV3 && eType == GDT_UInt16) ||
#endif
          bIsJPEG2000))
    {
        if (bStrict)
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "Attempt to create ECW file with pixel data type %s failed.\n"
                "Only Byte data type supported for ECW version 2 files."
#if ECWSDK_VERSION >= 50
                " ECW version 3 files supports UInt16 as well."
                " Specify ECW_FORMAT_VERSION=3 creation option to write "
                "version 3 file. \n"
#else
                ". \n"
#endif
                ,
                GDALGetDataTypeName(eType));
        }
        else
        {
#if ECWSDK_VERSION >= 50
            if (eType == GDT_UInt16)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "ECW version 2 does not support UInt16 data type, "
                         "truncating to Byte."
                         " Consider specifying ECW_FORMAT_VERSION=3 for full "
                         "UInt16 support available in ECW version 3. \n");
            }
            else
#endif
                CPLError(CE_Warning, CPLE_AppDefined,
                         "ECW v2 does not support data type, ignoring request "
                         "for %s. \n",
                         GDALGetDataTypeName(eType));

            eType = GDT_Byte;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Is the input RGB or RGBA?                                       */
    /* -------------------------------------------------------------------- */
    int bRGBColorSpace = ECWIsInputRGBColorSpace(poSrcDS);

    /* -------------------------------------------------------------------- */
    /*      Setup the compressor.                                           */
    /* -------------------------------------------------------------------- */
    GDALECWCompressor oCompressor;

    oCompressor.pfnProgress = pfnProgress;
    oCompressor.pProgressData = pProgressData;
    oCompressor.m_poSrcDS = poSrcDS;

    CPLStringList aosBandDescriptions;
    for (int i = 0; i < nBands; i++)
    {
        /* Make a copy since ECWGetColorInterpretationName() can return a string
         * generated */
        /* by CPLSPrintf(), which has just a few rotating entries. */
        aosBandDescriptions.AddString(ECWGetColorInterpretationName(
            poSrcDS->GetRasterBand(i + 1)->GetColorInterpretation(), i));
    }

    const char *pszAreaOrPoint = poSrcDS->GetMetadataItem(GDALMD_AREA_OR_POINT);
    int bPixelIsPoint =
        pszAreaOrPoint != nullptr && EQUAL(pszAreaOrPoint, GDALMD_AOP_POINT);

    if (oCompressor.Initialize(
            pszFilename, papszOptions, nXSize, nYSize, nBands,
            aosBandDescriptions.List(), bRGBColorSpace, eType, poSRS, gt,
            poSrcDS->GetGCPCount(), poSrcDS->GetGCPs(), bIsJPEG2000,
            bPixelIsPoint, poSrcDS->GetMetadata("RPC"), poSrcDS) != CE_None)
    {
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Start the compression.                                          */
    /* -------------------------------------------------------------------- */

    if (!pfnProgress(0.0, nullptr, pProgressData))
        return nullptr;

    CNCSError oErr = oCompressor.Write();

    if (oErr.GetErrorNumber() != NCS_SUCCESS)
    {
        ECWReportError(oErr);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup, and return read-only handle.                           */
    /* -------------------------------------------------------------------- */
    oCompressor.CloseDown();
    pfnProgress(1.0, nullptr, pProgressData);

    /* -------------------------------------------------------------------- */
    /*      Re-open dataset, and copy any auxiliary pam information.         */
    /* -------------------------------------------------------------------- */
    GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
    GDALPamDataset *poDS = nullptr;

    if (bIsJPEG2000)
        poDS = cpl::down_cast<GDALPamDataset *>(
            ECWDatasetOpenJPEG2000(&oOpenInfo));
    else
        poDS =
            cpl::down_cast<GDALPamDataset *>(ECWDataset::OpenECW(&oOpenInfo));

    if (poDS)
    {
#if ECWSDK_VERSION >= 50
        for (int i = 1; i <= poSrcDS->GetRasterCount(); i++)
        {
            double dMin, dMax, dMean, dStdDev;
            if (poSrcDS->GetRasterBand(i)->GetStatistics(
                    FALSE, FALSE, &dMin, &dMax, &dMean, &dStdDev) == CE_None)
            {
                poDS->GetRasterBand(i)->SetStatistics(dMin, dMax, dMean,
                                                      dStdDev);
            }
            double dHistMin, dHistMax;
            int nBuckets;
            GUIntBig *pHistogram = nullptr;
            if (poSrcDS->GetRasterBand(i)->GetDefaultHistogram(
                    &dHistMin, &dHistMax, &nBuckets, &pHistogram, FALSE,
                    nullptr, nullptr) == CE_None)
            {
                poDS->GetRasterBand(i)->SetDefaultHistogram(
                    dHistMin, dHistMax, nBuckets, pHistogram);
                VSIFree(pHistogram);
            }
        }
#endif

        cpl::down_cast<ECWDataset *>(poDS)->SetPreventCopyingSomeMetadata(TRUE);
        int nFlags = GCIF_PAM_DEFAULT;
        if (bIsJPEG2000 && !CPLFetchBool(papszOptions, "WRITE_METADATA", false))
            nFlags &= ~GCIF_METADATA;
        poDS->CloneInfo(poSrcDS, nFlags);
        cpl::down_cast<ECWDataset *>(poDS)->SetPreventCopyingSomeMetadata(
            FALSE);
    }

    return poDS;
}

/************************************************************************/
/*                          ECWCreateCopyECW()                          */
/************************************************************************/

GDALDataset *ECWCreateCopyECW(const char *pszFilename, GDALDataset *poSrcDS,
                              int bStrict, char **papszOptions,
                              GDALProgressFunc pfnProgress, void *pProgressData)

{
    int nBands = poSrcDS->GetRasterCount();
    if (nBands == 0)
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "ECW driver does not support source dataset with zero band.\n");
        return nullptr;
    }

    if (!EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "ecw"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ECW driver does not support creating ECW files\n"
                 "with an extension other than .ecw");
        return nullptr;
    }

#if ECWSDK_VERSION >= 50
    bool bECWV3 = false;
    const char *pszOption =
        CSLFetchNameValue(papszOptions, "ECW_FORMAT_VERSION");
    if (pszOption != nullptr)
    {
        bECWV3 = (3 == atoi(pszOption));
    }

#endif

    GDALDataType eDataType = poSrcDS->GetRasterBand(1)->GetRasterDataType();
    if (eDataType != GDT_Byte
#if ECWSDK_VERSION >= 50
        && !(bECWV3 && (eDataType == GDT_UInt16))
#endif
        && bStrict)
    {
#if ECWSDK_VERSION >= 50
        if (eDataType == GDT_UInt16)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "ECW v2 does not support UInt16 data type. Consider "
                     " specifying ECW_FORMAT_VERSION=3 for full UInt16 support "
                     "available in ECW v3. \n");
        }
        else
#endif
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "ECW driver doesn't support data type %s. "
                     "Only unsigned eight "
#if ECWSDK_VERSION >= 50
                     "or sixteen "
#endif
                     "bit bands supported. \n",
                     GDALGetDataTypeName(eDataType));
        }

        return nullptr;
    }

    if (poSrcDS->GetRasterXSize() < 128 || poSrcDS->GetRasterYSize() < 128)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "ECW driver requires image to be at least 128x128,\n"
                 "the source image is %dx%d.\n",
                 poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize());

        return nullptr;
    }

    if (poSrcDS->GetRasterBand(1)->GetColorTable() != nullptr)
    {
        CPLError((bStrict) ? CE_Failure : CE_Warning, CPLE_NotSupported,
                 "ECW driver ignores color table. "
                 "The source raster band will be considered as grey level.\n"
                 "Consider using color table expansion (-expand option in "
                 "gdal_translate)\n");
        if (bStrict)
            return nullptr;
    }

    return ECWCreateCopy(pszFilename, poSrcDS, bStrict, papszOptions,
                         pfnProgress, pProgressData, FALSE);
}

/************************************************************************/
/*                       ECWCreateCopyJPEG2000()                        */
/************************************************************************/

GDALDataset *ECWCreateCopyJPEG2000(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData)

{
    int nBands = poSrcDS->GetRasterCount();
    if (nBands == 0)
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "JP2ECW driver does not support source dataset with zero band.\n");
        return nullptr;
    }

    if (EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "ecw"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "JP2ECW driver does not support creating JPEG2000 files\n"
                 "with a .ecw extension.  Please use anything else.");
        return nullptr;
    }

    GDALDataType eDataType = poSrcDS->GetRasterBand(1)->GetRasterDataType();
    if (eDataType != GDT_Byte && eDataType != GDT_Int16 &&
        eDataType != GDT_UInt16 && eDataType != GDT_Int32 &&
        eDataType != GDT_UInt32 && eDataType != GDT_Float32
#if ECWSDK_VERSION >= 40
        && eDataType != GDT_Float64
#endif
        && bStrict)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "JP2ECW driver doesn't support data type %s. ",
                 GDALGetDataTypeName(eDataType));

        return nullptr;
    }

    if (poSrcDS->GetRasterBand(1)->GetColorTable() != nullptr)
    {
        CPLError((bStrict) ? CE_Failure : CE_Warning, CPLE_NotSupported,
                 "JP2ECW driver ignores color table. "
                 "The source raster band will be considered as grey level.\n"
                 "Consider using color table expansion (-expand option in "
                 "gdal_translate)\n");
        if (bStrict)
            return nullptr;
    }

    return ECWCreateCopy(pszFilename, poSrcDS, bStrict, papszOptions,
                         pfnProgress, pProgressData, TRUE);
}

/************************************************************************/
/************************************************************************

               ECW/JPEG200 Create() Support
               ----------------------------

  The remainder of the file is code to implement the Create() method.
  New dataset and raster band classes are defined specifically for the
  purpose of being write-only.  In particular, you cannot read back data
  from these datasets, and writing must occur in a pretty specific order.

  That is, you need to write all metadata (projection, georef, etc) first
  and then write the image data.  All bands data for the first scanline
  should be written followed by all bands for the second scanline and so on.

  Creation supports the same virtual subfile names as CreateCopy() supports.

 ************************************************************************/
/************************************************************************/

/************************************************************************/
/* ==================================================================== */
/*                              ECWWriteDataset                         */
/* ==================================================================== */
/************************************************************************/

class ECWWriteRasterBand;

#ifdef OPTIMIZED_FOR_GDALWARP
class IRasterIORequest
{
  public:
    GDALRasterBand *poBand;
    int nXOff;
    int nYOff;
    int nXSize;
    int nYSize;
    GByte *pabyData;
    int nBufXSize;
    int nBufYSize;

    IRasterIORequest(GDALRasterBand *poBandIn, int nXOffIn, int nYOffIn,
                     int nXSizeIn, int nYSizeIn, void *pData, int nBufXSizeIn,
                     int nBufYSizeIn, GDALDataType eBufType,
                     GSpacing nPixelSpace, GSpacing nLineSpace)
        : poBand(poBandIn), nXOff(nXOffIn), nYOff(nYOffIn), nXSize(nXSizeIn),
          nYSize(nYSizeIn), pabyData(nullptr), nBufXSize(nBufXSizeIn),
          nBufYSize(nBufYSizeIn)
    {
        GDALDataType eDataType = poBand->GetRasterDataType();
        const int nDataTypeSize = GDALGetDataTypeSizeBytes(eDataType);
        pabyData = (GByte *)CPLMalloc(static_cast<size_t>(nBufXSize) *
                                      nBufYSize * nDataTypeSize);
        for (int iY = 0; iY < nBufYSize; iY++)
        {
            GDALCopyWords((GByte *)pData + iY * nLineSpace, eBufType,
                          static_cast<int>(nPixelSpace),
                          pabyData + static_cast<size_t>(iY) * nBufXSize *
                                         nDataTypeSize,
                          eDataType, nDataTypeSize, nBufXSize);
        }
    }

    ~IRasterIORequest()
    {
        CPLFree(pabyData);
    }
};
#endif

class ECWWriteDataset final : public GDALDataset
{
    friend class ECWWriteRasterBand;

    char *pszFilename;

    int bIsJPEG2000;
    GDALDataType eDataType;
    char **papszOptions;

    OGRSpatialReference m_oSRS{};
    GDALGeoTransform m_gt{};

    GDALECWCompressor oCompressor;
    int bCrystalized;  // TODO: Spelling.

    int nLoadedLine;
    GByte *pabyBILBuffer;

    int bOutOfOrderWriteOccurred;
#ifdef OPTIMIZED_FOR_GDALWARP
    int nPrevIRasterIOBand;
#endif

    CPLErr Crystalize();  // TODO: Spelling.
    CPLErr FlushLine();

  public:
    ECWWriteDataset(const char *, int, int, int, GDALDataType,
                    char **papszOptions, int);
    ~ECWWriteDataset();

    virtual CPLErr FlushCache(bool bAtClosing) override;

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    virtual CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;
    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

#ifdef OPTIMIZED_FOR_GDALWARP
    virtual CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, BANDMAP_TYPE panBandMap,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;
#endif
};

/************************************************************************/
/* ==================================================================== */
/*                         ECWWriteRasterBand                           */
/* ==================================================================== */
/************************************************************************/

class ECWWriteRasterBand final : public GDALRasterBand
{
    friend class ECWWriteDataset;

    // NOTE: poDS may be altered for NITF/JPEG2000 files!
    ECWWriteDataset *poGDS;

    GDALColorInterp eInterp;

#ifdef OPTIMIZED_FOR_GDALWARP
    IRasterIORequest *poIORequest;
#endif

  public:
    ECWWriteRasterBand(ECWWriteDataset *, int);
    ~ECWWriteRasterBand();

    virtual CPLErr SetColorInterpretation(GDALColorInterp eInterpIn) override
    {
        eInterp = eInterpIn;
        if (strlen(GetDescription()) == 0)
            SetDescription(ECWGetColorInterpretationName(eInterp, nBand - 1));
        return CE_None;
    }

    virtual GDALColorInterp GetColorInterpretation() override
    {
        return eInterp;
    }

    virtual CPLErr IReadBlock(int, int, void *) override;
    virtual CPLErr IWriteBlock(int, int, void *) override;

#ifdef OPTIMIZED_FOR_GDALWARP
    virtual CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;
#endif
};

/************************************************************************/
/*                          ECWWriteDataset()                           */
/************************************************************************/

ECWWriteDataset::ECWWriteDataset(const char *pszFilenameIn, int nXSize,
                                 int nYSize, int nBandCount, GDALDataType eType,
                                 char **papszOptionsIn, int bIsJPEG2000In)

{
    bCrystalized = FALSE;
    pabyBILBuffer = nullptr;
    nLoadedLine = -1;

    eAccess = GA_Update;

    this->bIsJPEG2000 = bIsJPEG2000In;
    this->eDataType = eType;
    this->papszOptions = CSLDuplicate(papszOptionsIn);
    this->pszFilename = CPLStrdup(pszFilenameIn);

    nRasterXSize = nXSize;
    nRasterYSize = nYSize;

    // create band objects.
    for (int iBand = 1; iBand <= nBandCount; iBand++)
    {
        SetBand(iBand, new ECWWriteRasterBand(this, iBand));
    }

    bOutOfOrderWriteOccurred = FALSE;
#ifdef OPTIMIZED_FOR_GDALWARP
    nPrevIRasterIOBand = -1;
#endif
}

/************************************************************************/
/*                          ~ECWWriteDataset()                          */
/************************************************************************/

ECWWriteDataset::~ECWWriteDataset()

{
    ECWWriteDataset::FlushCache(true);

    if (bCrystalized)
    {
        if (bOutOfOrderWriteOccurred)
        {
            /* Otherwise there's a hang-up in the destruction of the oCompressor
             * object */
            while (nLoadedLine < nRasterYSize - 1)
                FlushLine();
        }
        if (nLoadedLine == nRasterYSize - 1)
            FlushLine();
        oCompressor.CloseDown();
    }

    CPLFree(pabyBILBuffer);
    CSLDestroy(papszOptions);
    CPLFree(pszFilename);
}

/************************************************************************/
/*                             FlushCache()                             */
/************************************************************************/

CPLErr ECWWriteDataset::FlushCache(bool bAtClosing)

{
    return BlockBasedFlushCache(bAtClosing);
}

/************************************************************************/
/*                         GetSpatialRef()                              */
/************************************************************************/

const OGRSpatialReference *ECWWriteDataset::GetSpatialRef() const
{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr ECWWriteDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr ECWWriteDataset::SetGeoTransform(const GDALGeoTransform &gt)

{
    m_gt = gt;
    return CE_None;
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

CPLErr ECWWriteDataset::SetSpatialRef(const OGRSpatialReference *poSRS)

{
    m_oSRS.Clear();
    if (poSRS)
        m_oSRS = *poSRS;

    return CE_None;
}

/************************************************************************/
/*                             Crystalize()                             */
/************************************************************************/

CPLErr ECWWriteDataset::Crystalize()

{
    const int nWordSize = GDALGetDataTypeSizeBytes(eDataType);

    CPLErr eErr;

    if (bCrystalized)
        return CE_None;

    const char **paszBandDescriptions =
        (const char **)CPLMalloc(nBands * sizeof(char *));
    for (int i = 0; i < nBands; i++)
    {
        paszBandDescriptions[i] = GetRasterBand(i + 1)->GetDescription();
    }

    int bRGBColorSpace = ECWIsInputRGBColorSpace(this);

    eErr = oCompressor.Initialize(pszFilename, papszOptions, nRasterXSize,
                                  nRasterYSize, nBands, paszBandDescriptions,
                                  bRGBColorSpace, eDataType, &m_oSRS, m_gt, 0,
                                  nullptr, bIsJPEG2000, FALSE, nullptr);

    if (eErr == CE_None)
        bCrystalized = TRUE;

    nLoadedLine = -1;
    pabyBILBuffer = (GByte *)CPLMalloc(static_cast<size_t>(nWordSize) * nBands *
                                       nRasterXSize);

    CPLFree(paszBandDescriptions);

    return eErr;
}

/************************************************************************/
/*                             FlushLine()                              */
/************************************************************************/

CPLErr ECWWriteDataset::FlushLine()

{
    const int nWordSize = GDALGetDataTypeSizeBytes(eDataType);
    CPLErr eErr;

    /* -------------------------------------------------------------------- */
    /*      Crystallize if not already done.                                */
    /* -------------------------------------------------------------------- */
    if (!bCrystalized)
    {
        eErr = Crystalize();

        if (eErr != CE_None)
            return eErr;
    }

    /* -------------------------------------------------------------------- */
    /*      Write out the currently loaded line.                            */
    /* -------------------------------------------------------------------- */
    if (nLoadedLine != -1)
    {

        void **papOutputLine = (void **)CPLMalloc(sizeof(void *) * nBands);
        for (int i = 0; i < nBands; i++)
            papOutputLine[i] =
                (void *)(pabyBILBuffer +
                         static_cast<size_t>(i) * nWordSize * nRasterXSize);

        eErr = oCompressor.ourWriteLineBIL((UINT16)nBands, papOutputLine);
        CPLFree(papOutputLine);
        if (eErr != CE_None)
        {
            return eErr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Clear the buffer and increment the "current line" indicator.    */
    /* -------------------------------------------------------------------- */
    memset(pabyBILBuffer, 0,
           static_cast<size_t>(nWordSize) * nRasterXSize * nBands);
    nLoadedLine++;

    return CE_None;
}

#ifdef OPTIMIZED_FOR_GDALWARP
/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr ECWWriteDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                  int nXSize, int nYSize, void *pData,
                                  int nBufXSize, int nBufYSize,
                                  GDALDataType eBufType, int nBandCount,
                                  BANDMAP_TYPE panBandMap, GSpacing nPixelSpace,
                                  GSpacing nLineSpace, GSpacing nBandSpace,
                                  GDALRasterIOExtraArg *psExtraArg)
{
    ECWWriteRasterBand *po4thBand = nullptr;
    IRasterIORequest *poIORequest = nullptr;

    if (bOutOfOrderWriteOccurred)
        return CE_Failure;

    if (eRWFlag == GF_Write && nBandCount == 3 && nBands == 4)
    {
        po4thBand = cpl::down_cast<ECWWriteRasterBand *>(GetRasterBand(4));
        poIORequest = po4thBand->poIORequest;
        if (poIORequest != nullptr)
        {
            if (nXOff != poIORequest->nXOff || nYOff != poIORequest->nYOff ||
                nXSize != poIORequest->nXSize ||
                nYSize != poIORequest->nYSize ||
                nBufXSize != poIORequest->nBufXSize ||
                nBufYSize != poIORequest->nBufYSize)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Out of order write");
                bOutOfOrderWriteOccurred = TRUE;
                return CE_Failure;
            }
        }
    }

    const int nDataTypeSize = GDALGetDataTypeSizeBytes(eDataType);
    if (eRWFlag == GF_Write && nXOff == 0 && nXSize == nRasterXSize &&
        nBufXSize == nXSize && nBufYSize == nYSize && eBufType == eDataType &&
        (nBandCount == nBands ||
         (nBandCount == 3 && poIORequest != nullptr && nBands == 4)) &&
        nPixelSpace == nDataTypeSize &&
        nLineSpace == nPixelSpace * nRasterXSize)
    {
        CPLErr eErr = CE_None;
        GByte *pabyData = (GByte *)pData;
        for (int iY = 0; iY < nYSize; iY++)
        {
            for (int iBand = 0; iBand < nBandCount && eErr == CE_None; iBand++)
            {
                eErr = GetRasterBand(panBandMap[iBand])
                           ->WriteBlock(0, iY + nYOff,
                                        pabyData + iY * nLineSpace +
                                            iBand * nBandSpace);
            }

            if (poIORequest != nullptr && eErr == CE_None)
            {
                eErr = po4thBand->WriteBlock(0, iY + nYOff,
                                             poIORequest->pabyData +
                                                 iY * nDataTypeSize * nXSize);
            }
        }

        if (poIORequest != nullptr)
        {
            delete poIORequest;
            po4thBand->poIORequest = nullptr;
        }

        return eErr;
    }
    else
        return GDALDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                      pData, nBufXSize, nBufYSize, eBufType,
                                      nBandCount, panBandMap, nPixelSpace,
                                      nLineSpace, nBandSpace, psExtraArg);
}
#endif

/************************************************************************/
/* ==================================================================== */
/*                          ECWWriteRasterBand                          */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                         ECWWriteRasterBand()                         */
/************************************************************************/

ECWWriteRasterBand::ECWWriteRasterBand(ECWWriteDataset *poDSIn, int nBandIn)

{
    nBand = nBandIn;
    poDS = poDSIn;
    poGDS = poDSIn;
    nBlockXSize = poDSIn->GetRasterXSize();
    nBlockYSize = 1;
    eDataType = poDSIn->eDataType;
    eInterp = GCI_Undefined;
#ifdef OPTIMIZED_FOR_GDALWARP
    poIORequest = nullptr;
#endif
}

/************************************************************************/
/*                        ~ECWWriteRasterBand()                         */
/************************************************************************/

ECWWriteRasterBand::~ECWWriteRasterBand()

{
#ifdef OPTIMIZED_FOR_GDALWARP
    delete poIORequest;
#endif
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr ECWWriteRasterBand::IReadBlock(CPL_UNUSED int nBlockX,
                                      CPL_UNUSED int nBlockY, void *pBuffer)
{
    const int nWordSize = GDALGetDataTypeSizeBytes(eDataType);

    // We zero stuff out here, but we can't really read stuff from
    // a write only stream.

    memset(pBuffer, 0, static_cast<size_t>(nBlockXSize) * nWordSize);

    return CE_None;
}

#ifdef OPTIMIZED_FOR_GDALWARP
/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr ECWWriteRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                     int nXSize, int nYSize, void *pData,
                                     int nBufXSize, int nBufYSize,
                                     GDALDataType eBufType,
                                     GSpacing nPixelSpace, GSpacing nLineSpace,
                                     GDALRasterIOExtraArg *psExtraArg)
{
    if (eRWFlag == GF_Write && nBand == 4 && poGDS->nBands == 4 &&
        poGDS->nPrevIRasterIOBand < 0)
    {
        /* Triggered when gdalwarp outputs an alpha band */
        /* It is called before GDALDatasetRasterIO() on the 3 first bands */
        if (poIORequest != nullptr)
            return CE_Failure;
        poIORequest = new IRasterIORequest(this, nXOff, nYOff, nXSize, nYSize,
                                           pData, nBufXSize, nBufYSize,
                                           eBufType, nPixelSpace, nLineSpace);
        return CE_None;
    }

    poGDS->nPrevIRasterIOBand = nBand;
    return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nPixelSpace, nLineSpace, psExtraArg);
}
#endif

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr ECWWriteRasterBand::IWriteBlock(CPL_UNUSED int nBlockX, int nBlockY,
                                       void *pBuffer)
{
    const int nWordSize = GDALGetDataTypeSizeBytes(eDataType);
    CPLErr eErr;

    if (poGDS->bOutOfOrderWriteOccurred)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Flush previous line if needed.                                  */
    /* -------------------------------------------------------------------- */
    if (nBlockY == poGDS->nLoadedLine + 1)
    {
        eErr = poGDS->FlushLine();
        if (eErr != CE_None)
            return eErr;
    }

    /* -------------------------------------------------------------------- */
    /*      Blow a gasket if we have been asked to write something out      */
    /*      of order.                                                       */
    /* -------------------------------------------------------------------- */
    if (nBlockY != poGDS->nLoadedLine)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Apparent attempt to write to ECW non-sequentially.\n"
                 "Loaded line is %d, but %d of band %d was written to.",
                 poGDS->nLoadedLine, nBlockY, nBand);
        poGDS->bOutOfOrderWriteOccurred = TRUE;
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Copy passed data into current line buffer.                      */
    /* -------------------------------------------------------------------- */
    memcpy(poGDS->pabyBILBuffer + (nBand - 1) * nWordSize * nRasterXSize,
           pBuffer, nWordSize * nRasterXSize);

    return CE_None;
}

/************************************************************************/
/*                         ECWCreateJPEG2000()                          */
/************************************************************************/

GDALDataset *ECWCreateJPEG2000(const char *pszFilename, int nXSize, int nYSize,
                               int nBands, GDALDataType eType,
                               char **papszOptions)

{
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "0 band not supported");
        return nullptr;
    }
    ECWInitialize();

    return new ECWWriteDataset(pszFilename, nXSize, nYSize, nBands, eType,
                               papszOptions, TRUE);
}

/************************************************************************/
/*                            ECWCreateECW()                            */
/************************************************************************/

GDALDataset *ECWCreateECW(const char *pszFilename, int nXSize, int nYSize,
                          int nBands, GDALDataType eType, char **papszOptions)

{
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "0 band not supported");
        return nullptr;
    }
    ECWInitialize();

    return new ECWWriteDataset(pszFilename, nXSize, nYSize, nBands, eType,
                               papszOptions, FALSE);
}

#endif /* def HAVE_COMPRESS */
