/******************************************************************************
 *
 * Project:  NITF Read/Write Library
 * Purpose:  Module responsible for opening NITF file, populating NITFFile
 *           structure, and instantiating segment specific access objects.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 **********************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "nitflib.h"
#include "cpl_vsi.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include <stdbool.h>

#ifdef EMBED_RESOURCE_FILES
#include "embedded_resources.h"
#endif

#ifndef CPL_IGNORE_RET_VAL_INT_defined
#define CPL_IGNORE_RET_VAL_INT_defined

CPL_INLINE static void CPL_IGNORE_RET_VAL_INT(CPL_UNUSED int unused)
{
}
#endif

static int NITFWriteBLOCKA(VSILFILE *fp, vsi_l_offset nOffsetUDIDL,
                           int *pnOffset, char **papszOptions);
static int NITFWriteTREsFromOptions(VSILFILE *fp, vsi_l_offset nOffsetUDIDL,
                                    int *pnOffset, char **papszOptions,
                                    const char *pszTREPrefix);

static int NITFCollectSegmentInfo(NITFFile *psFile, int nFileHeaderLenSize,
                                  int nOffset, const char szType[3],
                                  int nHeaderLenSize, int nDataLenSize,
                                  GUIntBig *pnNextData);

static void NITFExtractAndRecodeMetadata(char ***ppapszMetadata,
                                         const char *pachHeader, int nStart,
                                         int nLength, const char *pszName,
                                         const char *pszSrcEncoding);

static int NITFWriteOption(VSILFILE *fp, char **papszOptions, size_t nWidth,
                           GUIntBig nLocation, const char *pszName,
                           const char *pszText);

/************************************************************************/
/*                              NITFOpen()                              */
/************************************************************************/

NITFFile *NITFOpen(const char *pszFilename, int bUpdatable)

{
    VSILFILE *fp;

    /* -------------------------------------------------------------------- */
    /*      Open the file.                                                  */
    /* -------------------------------------------------------------------- */
    if (bUpdatable)
        fp = VSIFOpenL(pszFilename, "r+b");
    else
        fp = VSIFOpenL(pszFilename, "rb");

    if (fp == NULL)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Failed to open file %s.",
                 pszFilename);
        return NULL;
    }

    return NITFOpenEx(fp, pszFilename);
}

/************************************************************************/
/*                             NITFOpenEx()                             */
/************************************************************************/

NITFFile *NITFOpenEx(VSILFILE *fp, const char *pszFilename)

{
    char *pachHeader;
    NITFFile *psFile;
    int nHeaderLen, nOffset, nHeaderLenOffset;
    GUIntBig nNextData;
    char szTemp[128], achFSDWNG[6];
    GIntBig currentPos;
    int bTriedStreamingFileHeader = FALSE;

    /* -------------------------------------------------------------------- */
    /*      Check file type.                                                */
    /* -------------------------------------------------------------------- */
    if (VSIFSeekL(fp, 0, SEEK_SET) != 0 || VSIFReadL(szTemp, 1, 9, fp) != 9 ||
        (!STARTS_WITH_CI(szTemp, "NITF") && !STARTS_WITH_CI(szTemp, "NSIF")))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The file %s is not an NITF file.", pszFilename);
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        return NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Read the FSDWNG field.                                          */
    /* -------------------------------------------------------------------- */
    if (VSIFSeekL(fp, 280, SEEK_SET) != 0 ||
        VSIFReadL(achFSDWNG, 1, 6, fp) != 6)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unable to read FSDWNG field from NITF file.  File is either "
                 "corrupt\n"
                 "or empty.");
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        return NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Get header length.                                              */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(szTemp, "NITF01.") ||
        STARTS_WITH_CI(achFSDWNG, "999998"))
        nHeaderLenOffset = 394;
    else
        nHeaderLenOffset = 354;

    if (VSIFSeekL(fp, nHeaderLenOffset, SEEK_SET) != 0 ||
        VSIFReadL(szTemp, 1, 6, fp) != 6)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Unable to read header length from NITF file.  File is either "
                 "corrupt\n"
                 "or empty.");
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        return NULL;
    }

    szTemp[6] = '\0';
    nHeaderLen = atoi(szTemp);

    if (VSIFSeekL(fp, nHeaderLen, SEEK_SET) != 0)
        currentPos = 0;
    else
        currentPos = VSIFTellL(fp);
    if (nHeaderLen < nHeaderLenOffset || nHeaderLen > currentPos)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "NITF Header Length (%d) seems to be corrupt.", nHeaderLen);
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        return NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Read the whole file header.                                     */
    /* -------------------------------------------------------------------- */
    pachHeader = (char *)VSI_MALLOC_VERBOSE(nHeaderLen);
    if (pachHeader == NULL)
    {
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        return NULL;
    }
    if (VSIFSeekL(fp, 0, SEEK_SET) != 0 ||
        (int)VSIFReadL(pachHeader, 1, nHeaderLen, fp) != nHeaderLen)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Cannot read %d bytes for NITF header", (nHeaderLen));
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        CPLFree(pachHeader);
        return NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Create and initialize info structure about file.                */
    /* -------------------------------------------------------------------- */
    psFile = (NITFFile *)CPLCalloc(sizeof(NITFFile), 1);
    psFile->fp = fp;
    psFile->pachHeader = pachHeader;

retry_read_header:
    /* -------------------------------------------------------------------- */
    /*      Get version.                                                    */
    /* -------------------------------------------------------------------- */
    NITFGetField(psFile->szVersion, pachHeader, 0, 9);

/* -------------------------------------------------------------------- */
/*      Collect a variety of information as metadata.                   */
/* -------------------------------------------------------------------- */
#define GetMD(target, hdr, start, length, name)                                \
    NITFExtractMetadata(&(target->papszMetadata), hdr, start, length,          \
                        "NITF_" #name);

    if (EQUAL(psFile->szVersion, "NITF02.10") ||
        EQUAL(psFile->szVersion, "NSIF01.00"))
    {
        char szWork[100];

        GetMD(psFile, pachHeader, 0, 9, FHDR);
        GetMD(psFile, pachHeader, 9, 2, CLEVEL);
        GetMD(psFile, pachHeader, 11, 4, STYPE);
        GetMD(psFile, pachHeader, 15, 10, OSTAID);
        GetMD(psFile, pachHeader, 25, 14, FDT);
        GetMD(psFile, pachHeader, 39, 80, FTITLE);
        GetMD(psFile, pachHeader, 119, 1, FSCLAS);
        GetMD(psFile, pachHeader, 120, 2, FSCLSY);
        GetMD(psFile, pachHeader, 122, 11, FSCODE);
        GetMD(psFile, pachHeader, 133, 2, FSCTLH);
        GetMD(psFile, pachHeader, 135, 20, FSREL);
        GetMD(psFile, pachHeader, 155, 2, FSDCTP);
        GetMD(psFile, pachHeader, 157, 8, FSDCDT);
        GetMD(psFile, pachHeader, 165, 4, FSDCXM);
        GetMD(psFile, pachHeader, 169, 1, FSDG);
        GetMD(psFile, pachHeader, 170, 8, FSDGDT);
        GetMD(psFile, pachHeader, 178, 43, FSCLTX);
        GetMD(psFile, pachHeader, 221, 1, FSCATP);
        GetMD(psFile, pachHeader, 222, 40, FSCAUT);
        GetMD(psFile, pachHeader, 262, 1, FSCRSN);
        GetMD(psFile, pachHeader, 263, 8, FSSRDT);
        GetMD(psFile, pachHeader, 271, 15, FSCTLN);
        GetMD(psFile, pachHeader, 286, 5, FSCOP);
        GetMD(psFile, pachHeader, 291, 5, FSCPYS);
        GetMD(psFile, pachHeader, 296, 1, ENCRYP);
        snprintf(szWork, sizeof(szWork), "%3d,%3d,%3d",
                 ((GByte *)pachHeader)[297], ((GByte *)pachHeader)[298],
                 ((GByte *)pachHeader)[299]);
        GetMD(psFile, szWork, 0, 11, FBKGC);
        GetMD(psFile, pachHeader, 300, 24, ONAME);
        GetMD(psFile, pachHeader, 324, 18, OPHONE);
        NITFGetField(szTemp, pachHeader, 342, 12);
    }
    else if (EQUAL(psFile->szVersion, "NITF02.00"))
    {
        int nCOff = 0;

        GetMD(psFile, pachHeader, 0, 9, FHDR);
        GetMD(psFile, pachHeader, 9, 2, CLEVEL);
        GetMD(psFile, pachHeader, 11, 4, STYPE);
        GetMD(psFile, pachHeader, 15, 10, OSTAID);
        GetMD(psFile, pachHeader, 25, 14, FDT);
        GetMD(psFile, pachHeader, 39, 80, FTITLE);
        GetMD(psFile, pachHeader, 119, 1, FSCLAS);
        GetMD(psFile, pachHeader, 120, 40, FSCODE);
        GetMD(psFile, pachHeader, 160, 40, FSCTLH);
        GetMD(psFile, pachHeader, 200, 40, FSREL);
        GetMD(psFile, pachHeader, 240, 20, FSCAUT);
        GetMD(psFile, pachHeader, 260, 20, FSCTLN);
        GetMD(psFile, pachHeader, 280, 6, FSDWNG);
        if (STARTS_WITH_CI(pachHeader + 280, "999998"))
        {
            GetMD(psFile, pachHeader, 286, 40, FSDEVT);
            nCOff += 40;
        }
        GetMD(psFile, pachHeader, 286 + nCOff, 5, FSCOP);
        GetMD(psFile, pachHeader, 291 + nCOff, 5, FSCPYS);
        GetMD(psFile, pachHeader, 296 + nCOff, 1, ENCRYP);
        GetMD(psFile, pachHeader, 297 + nCOff, 27, ONAME);
        GetMD(psFile, pachHeader, 324 + nCOff, 18, OPHONE);
        NITFGetField(szTemp, pachHeader, 342 + nCOff, 12);
    }
#undef GetMD

    if (!bTriedStreamingFileHeader && EQUAL(szTemp, "999999999999"))
    {
        GUIntBig nFileSize;
        GByte abyDELIM2_L2[12] = {0};
        GByte abyL1_DELIM1[11] = {0};
        int bOK;

        bTriedStreamingFileHeader = TRUE;
        CPLDebug("NITF",
                 "Total file unknown. Trying to get a STREAMING_FILE_HEADER");

        bOK = VSIFSeekL(fp, 0, SEEK_END) == 0;
        nFileSize = VSIFTellL(fp);

        bOK &= VSIFSeekL(fp, nFileSize - 11, SEEK_SET) == 0;
        abyDELIM2_L2[11] = '\0';

        if (bOK && VSIFReadL(abyDELIM2_L2, 1, 11, fp) == 11 &&
            abyDELIM2_L2[0] == 0x0E && abyDELIM2_L2[1] == 0xCA &&
            abyDELIM2_L2[2] == 0x14 && abyDELIM2_L2[3] == 0xBF)
        {
            int SFHL2 = atoi((const char *)(abyDELIM2_L2 + 4));
            if (SFHL2 > 0 && (nFileSize > (size_t)(11 + SFHL2 + 11)))
            {
                bOK &=
                    VSIFSeekL(fp, nFileSize - 11 - SFHL2 - 11, SEEK_SET) == 0;

                if (bOK && VSIFReadL(abyL1_DELIM1, 1, 11, fp) == 11 &&
                    abyL1_DELIM1[7] == 0x0A && abyL1_DELIM1[8] == 0x6E &&
                    abyL1_DELIM1[9] == 0x1D && abyL1_DELIM1[10] == 0x97 &&
                    memcmp(abyL1_DELIM1, abyDELIM2_L2 + 4, 7) == 0)
                {
                    if (SFHL2 == nHeaderLen)
                    {
                        CSLDestroy(psFile->papszMetadata);
                        psFile->papszMetadata = NULL;

                        if ((int)VSIFReadL(pachHeader, 1, SFHL2, fp) != SFHL2)
                        {
                            CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
                            CPLFree(pachHeader);
                            CPLFree(psFile);
                            return NULL;
                        }

                        goto retry_read_header;
                    }
                }
            }
        }
        if (!bOK)
        {
            NITFClose(psFile);
            return NULL;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Collect segment info for the types we care about.               */
    /* -------------------------------------------------------------------- */
    nNextData = nHeaderLen;

    nOffset = nHeaderLenOffset + 6;

    nOffset = NITFCollectSegmentInfo(psFile, nHeaderLen, nOffset, "IM", 6, 10,
                                     &nNextData);

    if (nOffset != -1)
        nOffset = NITFCollectSegmentInfo(psFile, nHeaderLen, nOffset, "GR", 4,
                                         6, &nNextData);

    /* LA Called NUMX in NITF 2.1 */
    if (nOffset != -1)
        nOffset = NITFCollectSegmentInfo(psFile, nHeaderLen, nOffset, "LA", 4,
                                         3, &nNextData);

    if (nOffset != -1)
        nOffset = NITFCollectSegmentInfo(psFile, nHeaderLen, nOffset, "TX", 4,
                                         5, &nNextData);

    if (nOffset != -1)
        nOffset = NITFCollectSegmentInfo(psFile, nHeaderLen, nOffset, "DE", 4,
                                         9, &nNextData);

    if (nOffset != -1)
        nOffset = NITFCollectSegmentInfo(psFile, nHeaderLen, nOffset, "RE", 4,
                                         7, &nNextData);

    if (nOffset < 0)
    {
        NITFClose(psFile);
        return NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Is there User Define Header Data? (TREs)                        */
    /* -------------------------------------------------------------------- */
    if (nHeaderLen < nOffset + 5)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "NITF header too small");
        NITFClose(psFile);
        return NULL;
    }

    psFile->nTREBytes = atoi(NITFGetField(szTemp, pachHeader, nOffset, 5));
    if (psFile->nTREBytes < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Invalid TRE size : %d",
                 psFile->nTREBytes);
        NITFClose(psFile);
        return NULL;
    }
    nOffset += 5;

    if (psFile->nTREBytes == 3)
    {
        nOffset += 3; /* UDHOFL */
        psFile->nTREBytes = 0;
    }
    else if (psFile->nTREBytes > 3)
    {
        nOffset += 3; /* UDHOFL */
        psFile->nTREBytes -= 3;

        if (nHeaderLen < nOffset + psFile->nTREBytes)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "NITF header too small");
            NITFClose(psFile);
            return NULL;
        }

        psFile->pachTRE = (char *)VSI_MALLOC_VERBOSE(psFile->nTREBytes);
        if (psFile->pachTRE == NULL)
        {
            NITFClose(psFile);
            return NULL;
        }
        memcpy(psFile->pachTRE, pachHeader + nOffset, psFile->nTREBytes);
    }

    /* -------------------------------------------------------------------- */
    /*      Is there Extended Header Data?  (More TREs)                     */
    /* -------------------------------------------------------------------- */
    if (nHeaderLen > nOffset + 8)
    {
        int nXHDL = atoi(NITFGetField(szTemp, pachHeader, nOffset, 5));
        if (nXHDL < 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid XHDL value : %d",
                     nXHDL);
            NITFClose(psFile);
            return NULL;
        }

        nOffset += 5; /* XHDL */

        if (nXHDL > 3)
        {
            char *pachNewTRE;

            nOffset += 3; /* XHDLOFL */
            nXHDL -= 3;

            if (nHeaderLen < nOffset + nXHDL)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "NITF header too small");
                NITFClose(psFile);
                return NULL;
            }

            pachNewTRE = (char *)VSI_REALLOC_VERBOSE(psFile->pachTRE,
                                                     psFile->nTREBytes + nXHDL);
            if (pachNewTRE == NULL)
            {
                NITFClose(psFile);
                return NULL;
            }
            psFile->pachTRE = pachNewTRE;
            memcpy(psFile->pachTRE, pachHeader + nOffset, nXHDL);
            psFile->nTREBytes += nXHDL;
        }
    }

    return psFile;
}

/************************************************************************/
/*                             NITFClose()                              */
/************************************************************************/

void NITFClose(NITFFile *psFile)

{
    int iSegment;

    for (iSegment = 0; iSegment < psFile->nSegmentCount; iSegment++)
    {
        NITFSegmentInfo *psSegInfo = psFile->pasSegmentInfo + iSegment;

        if (psSegInfo->hAccess == NULL)
            continue;

        if (EQUAL(psSegInfo->szSegmentType, "IM"))
            NITFImageDeaccess((NITFImage *)psSegInfo->hAccess);
        else if (EQUAL(psSegInfo->szSegmentType, "DE"))
            NITFDESDeaccess((NITFDES *)psSegInfo->hAccess);
        else
        {
            CPLAssert(FALSE);
        }
    }

    CPLFree(psFile->pasSegmentInfo);
    if (psFile->fp != NULL)
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(psFile->fp));
    CPLFree(psFile->pachHeader);
    CSLDestroy(psFile->papszMetadata);
    CPLFree(psFile->pachTRE);

    if (psFile->psNITFSpecNode)
        CPLDestroyXMLNode(psFile->psNITFSpecNode);

    CPLFree(psFile);
}

static int NITFGotoOffset(VSILFILE *fp, GUIntBig nLocation)
{
    int bOK = TRUE;
    GUIntBig nCurrentLocation = VSIFTellL(fp);
    if (nLocation > nCurrentLocation)
    {
        GUIntBig nFileSize;
        size_t iFill;
        char cSpace = ' ';

        bOK &= VSIFSeekL(fp, 0, SEEK_END) == 0;
        nFileSize = VSIFTellL(fp);
        if (bOK && nLocation > nFileSize)
        {
            for (iFill = 0; bOK && iFill < nLocation - nFileSize; iFill++)
                bOK &= VSIFWriteL(&cSpace, 1, 1, fp) == 1;
        }
        else
            bOK &= VSIFSeekL(fp, nLocation, SEEK_SET) == 0;
    }
    else if (nLocation < nCurrentLocation)
    {
        bOK &= VSIFSeekL(fp, nLocation, SEEK_SET) == 0;
    }
    if (!bOK)
    {
        CPLError(CE_Failure, CPLE_FileIO, "I/O error");
    }
    return bOK;
}

/************************************************************************/
/*                             NITFCreate()                             */
/*                                                                      */
/*      Create a new uncompressed NITF file.                            */
/************************************************************************/

int NITFCreate(const char *pszFilename, int nPixels, int nLines, int nBands,
               int nBitsPerSample, const char *pszPVType, char **papszOptions)

{
    return NITFCreateEx(pszFilename, nPixels, nLines, nBands, nBitsPerSample,
                        pszPVType, papszOptions, NULL, NULL, NULL, NULL);
}

int NITFCreateEx(const char *pszFilename, int nPixels, int nLines, int nBands,
                 int nBitsPerSample, const char *pszPVType, char **papszOptions,
                 int *pnIndex, int *pnImageCount, vsi_l_offset *pnImageOffset,
                 vsi_l_offset *pnICOffset)

{
    VSILFILE *fp;
    GUIntBig nCur = 0;
    int nOffset = 0, iBand, nIHSize, nNPPBH, nNPPBV;
    GUIntBig nImageSize = 0;
    int nNBPR, nNBPC;
    const char *pszIREP;
    const char *pszIC = CSLFetchNameValue(papszOptions, "IC");
    int nCLevel;
    const char *pszNUMT;
    int nNUMT = 0;
    vsi_l_offset nOffsetUDIDL;
    const char *pszVersion;
    int iIM, nIM = 1;
    const char *pszNUMI;
    int iGS, nGS = 0;     // number of graphic segment
    const char *pszNUMS;  // graphic segment option string
    int iDES, nDES = 0;
    int bOK;

    if (pnIndex)
        *pnIndex = 0;

    if (nBands <= 0 || nBands > 99999)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid band number : %d",
                 nBands);
        return FALSE;
    }

    if (pszIC == NULL)
        pszIC = "NC";

    /* -------------------------------------------------------------------- */
    /*      Fetch some parameter overrides.                                 */
    /* -------------------------------------------------------------------- */
    pszIREP = CSLFetchNameValue(papszOptions, "IREP");
    if (pszIREP == NULL)
        pszIREP = "MONO";

    pszNUMT = CSLFetchNameValue(papszOptions, "NUMT");
    if (pszNUMT != NULL)
    {
        nNUMT = atoi(pszNUMT);
        if (nNUMT < 0 || nNUMT > 999)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid NUMT value : %s",
                     pszNUMT);
            return FALSE;
        }
    }

    const bool bAppendSubdataset =
        CSLTestBoolean(CSLFetchNameValueDef(papszOptions, "APPEND_SUBDATASET",
                                            "NO")) == TRUE;
    const bool bWriteAllImages =
        CSLTestBoolean(CSLFetchNameValueDef(papszOptions, "WRITE_ALL_IMAGES",
                                            "NO")) == TRUE;
    pszNUMI = CSLFetchNameValue(papszOptions, "NUMI");
    if (pszNUMI != NULL)
    {
        if (bAppendSubdataset)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "NUMI not supported with APPEND_SUBDATASET");
            return FALSE;
        }
        nIM = atoi(pszNUMI);
        if (nIM == 0)
        {
            if (pnIndex)
                *pnIndex = -1;
        }
        else if (nIM < 0 || nIM > 999)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid NUMI value : %s",
                     pszNUMI);
            return FALSE;
        }
        if (nIM != 1 && !EQUAL(pszIC, "NC") && bWriteAllImages)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create file with multiple images and "
                     "compression at the same time");
            return FALSE;
        }
    }
    else if (bAppendSubdataset && bWriteAllImages)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "WRITE_ALL_IMAGES=YES only supported for first image");
    }

    if (pnImageCount)
        *pnImageCount = nIM;

    // Reads and validates graphics segment number option
    pszNUMS = CSLFetchNameValue(papszOptions, "NUMS");
    if (pszNUMS != NULL)
    {
        nGS = atoi(pszNUMS);
        if (nGS < 0 || nGS > 999)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid NUMS value : %s",
                     pszNUMS);
            return FALSE;
        }
    }

    const char *pszNUMDES = CSLFetchNameValue(papszOptions, "NUMDES");
    if (pszNUMDES)
        nDES = atoi(pszNUMDES);
    else
    {
        char **papszSubList = CSLFetchNameValueMultiple(papszOptions, "DES");
        nDES = CSLCount(papszSubList);
        CSLDestroy(papszSubList);
    }

    /* -------------------------------------------------------------------- */
    /*      Compute raw image size, blocking factors and so forth.          */
    /* -------------------------------------------------------------------- */
    nNPPBH = nPixels;
    nNPPBV = nLines;

    if (CSLFetchNameValue(papszOptions, "BLOCKXSIZE") != NULL)
        nNPPBH = atoi(CSLFetchNameValue(papszOptions, "BLOCKXSIZE"));

    if (CSLFetchNameValue(papszOptions, "BLOCKYSIZE") != NULL)
        nNPPBV = atoi(CSLFetchNameValue(papszOptions, "BLOCKYSIZE"));

    if (CSLFetchNameValue(papszOptions, "NPPBH") != NULL)
        nNPPBH = atoi(CSLFetchNameValue(papszOptions, "NPPBH"));

    if (CSLFetchNameValue(papszOptions, "NPPBV") != NULL)
        nNPPBV = atoi(CSLFetchNameValue(papszOptions, "NPPBV"));

    if ((EQUAL(pszIC, "NC") || EQUAL(pszIC, "C8")) &&
        (nPixels > 8192 || nLines > 8192) && nNPPBH == nPixels &&
        nNPPBV == nLines)
    {
        /* See MIL-STD-2500-C, paragraph 5.4.2.2-d (#3263) */
        nNBPR = 1;
        nNBPC = 1;
        nNPPBH = 0;
        nNPPBV = 0;

        if (EQUAL(pszIC, "NC"))
        {
            nImageSize =
                ((nBitsPerSample) / 8) * ((GUIntBig)nPixels * nLines) * nBands;
        }
    }
    else if ((EQUAL(pszIC, "NC") || EQUAL(pszIC, "C8")) && nPixels > 8192 &&
             nNPPBH == nPixels)
    {
        if (nNPPBV <= 0)
            nNPPBV = 256;

        /* See MIL-STD-2500-C, paragraph 5.4.2.2-d */
        nNBPR = 1;
        nNPPBH = 0;
        nNBPC = nLines / nNPPBV + ((nLines % nNPPBV) == 0 ? 0 : 1);

        if (nNBPC > 9999)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create file %s,\n"
                     "Too many blocks : %d x %d",
                     pszFilename, nNBPR, nNBPC);
            return FALSE;
        }

        if (EQUAL(pszIC, "NC"))
        {
            nImageSize = ((nBitsPerSample) / 8) *
                         ((GUIntBig)nPixels * (nNBPC * nNPPBV)) * nBands;
        }
    }
    else if ((EQUAL(pszIC, "NC") || EQUAL(pszIC, "C8")) && nLines > 8192 &&
             nNPPBV == nLines)
    {
        if (nNPPBH <= 0)
            nNPPBH = 256;

        /* See MIL-STD-2500-C, paragraph 5.4.2.2-d */
        nNBPC = 1;
        nNPPBV = 0;
        nNBPR = nPixels / nNPPBH + ((nPixels % nNPPBH) == 0 ? 0 : 1);

        if (nNBPR > 9999)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create file %s,\n"
                     "Too many blocks : %d x %d",
                     pszFilename, nNBPR, nNBPC);
            return FALSE;
        }

        if (EQUAL(pszIC, "NC"))
        {
            nImageSize = ((nBitsPerSample) / 8) *
                         ((GUIntBig)nLines * (nNBPR * nNPPBH)) * nBands;
        }
    }
    else
    {
        if (nNPPBH <= 0 || nNPPBV <= 0 || nNPPBH > 9999 || nNPPBV > 9999)
            nNPPBH = nNPPBV = 256;

        nNBPR = nPixels / nNPPBH + ((nPixels % nNPPBH) == 0 ? 0 : 1);
        nNBPC = nLines / nNPPBV + ((nLines % nNPPBV) == 0 ? 0 : 1);
        if (nNBPR > 9999 || nNBPC > 9999)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create file %s,\n"
                     "Too many blocks : %d x %d",
                     pszFilename, nNBPR, nNBPC);
            return FALSE;
        }

        if (EQUAL(pszIC, "NC"))
        {
            nImageSize = ((nBitsPerSample) / 8) * ((GUIntBig)nNBPR * nNBPC) *
                         nNPPBH * nNPPBV * nBands;
        }
    }

    if (EQUAL(pszIC, "NC"))
    {
        if (nImageSize >= NITF_MAX_IMAGE_SIZE)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create file %s,\n"
                     "Too big image size : " CPL_FRMT_GUIB,
                     pszFilename, nImageSize);
            return FALSE;
        }
        if (nImageSize * nIM >= NITF_MAX_FILE_SIZE)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to create file %s,\n"
                     "Too big file size : " CPL_FRMT_GUIB,
                     pszFilename, nImageSize * nIM);
            return FALSE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Open new file.                                                  */
    /* -------------------------------------------------------------------- */
    fp = VSIFOpenL(pszFilename, bAppendSubdataset ? "rb+" : "wb+");
    if (fp == NULL)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Unable to create file %s,\n"
                 "check path and permissions.",
                 pszFilename);
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Work out the version we are producing.  For now we really       */
    /*      only support creating NITF02.10 or the nato analog              */
    /*      NSIF01.00.                                                      */
    /* -------------------------------------------------------------------- */
    pszVersion = CSLFetchNameValue(papszOptions, "FHDR");
    if (pszVersion == NULL)
        pszVersion = "NITF02.10";
    else if (!EQUAL(pszVersion, "NITF02.10") && !EQUAL(pszVersion, "NSIF01.00"))
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "FHDR=%s not supported, switching to NITF02.10.", pszVersion);
        pszVersion = "NITF02.10";
    }

    /* -------------------------------------------------------------------- */
    /*      Prepare the file header.                                        */
    /* -------------------------------------------------------------------- */

#define PLACE(location, name, text)                                            \
    do                                                                         \
    {                                                                          \
        const char *_text = text;                                              \
        bOK &= NITFGotoOffset(fp, location);                                   \
        bOK &= VSIFWriteL(_text, 1, strlen(_text), fp) == strlen(_text);       \
    } while (0)

#define OVR(width, location, name, text)                                       \
    bOK &= NITFWriteOption(fp, papszOptions, width, location, #name, text);

#define WRITE_BYTE(location, val)                                              \
    do                                                                         \
    {                                                                          \
        char cVal = val;                                                       \
        bOK &= NITFGotoOffset(fp, location);                                   \
        bOK &= VSIFWriteL(&cVal, 1, 1, fp) == 1;                               \
    } while (0)

    bOK = VSIFSeekL(fp, 0, SEEK_SET) == 0;

    if (!bAppendSubdataset)
    {
        PLACE(0, FDHR_FVER, pszVersion);
        OVR(2, 9, CLEVEL, "03"); /* Patched at the end */
        PLACE(11, STYPE, "BF01");
        OVR(10, 15, OSTAID, "GDAL");
        OVR(14, 25, FDT, "20021216151629");
        OVR(80, 39, FTITLE, "");
        OVR(1, 119, FSCLAS, "U");
        OVR(2, 120, FSCLSY, "");
        OVR(11, 122, FSCODE, "");
        OVR(2, 133, FSCTLH, "");
        OVR(20, 135, FSREL, "");
        OVR(2, 155, FSDCTP, "");
        OVR(8, 157, FSDCDT, "");
        OVR(4, 165, FSDCXM, "");
        OVR(1, 169, FSDG, "");
        OVR(8, 170, FSDGDT, "");
        OVR(43, 178, FSCLTX, "");
        OVR(1, 221, FSCATP, "");
        OVR(40, 222, FSCAUT, "");
        OVR(1, 262, FSCRSN, "");
        OVR(8, 263, FSSRDT, "");
        OVR(15, 271, FSCTLN, "");
        OVR(5, 286, FSCOP, "00000");
        OVR(5, 291, FSCPYS, "00000");
        PLACE(296, ENCRYP, "0");
        WRITE_BYTE(297, 0x00); /* FBKGC */
        WRITE_BYTE(298, 0x00);
        WRITE_BYTE(299, 0x00);
        OVR(24, 300, ONAME, "");
        OVR(18, 324, OPHONE, "");
        PLACE(342, FL, "????????????");
        PLACE(354, HL, "??????");
        PLACE(360, NUMI, CPLSPrintf("%03d", nIM));

        int nHL = 363;
        for (iIM = 0; iIM < nIM; iIM++)
        {
            /* Patched when image segments are written. */
            PLACE(nHL, LISHi, "??????");
            PLACE(nHL + 6, LIi, "??????????");
            nHL += 6 + 10;
        }

        // Creates Header entries for graphic segment
        //    NUMS: number of segment
        // For each segment:
        // 	  LSSH[i]: subheader length (4 byte), set to be 258, the size for
        //				minimal amount of information.
        //    LS[i] data length (6 byte)
        PLACE(nHL, NUMS, CPLSPrintf("%03d", nGS));
        nHL += 3;  // Move three characters
        for (iGS = 0; iGS < nGS; iGS++)
        {
            /* Patched when graphic segments are written. */
            PLACE(nHL, LSSHi, "????");
            nHL += 4;
            PLACE(nHL, LSi, "??????");
            nHL += 6;
        }

        PLACE(nHL, NUMX, "000");
        PLACE(nHL + 3, NUMT, CPLSPrintf("%03d", nNUMT));

        /* Patched when text segments are written. */
        PLACE(nHL + 6, LTSHnLTn, "");

        nHL += 6 + (4 + 5) * nNUMT;

        PLACE(nHL, NUMDES, CPLSPrintf("%03d", nDES));
        nHL += 3;

        for (iDES = 0; iDES < nDES; iDES++)
        {
            /* Patched when DESs are written. */
            PLACE(nHL, LDSH, "????");
            nHL += 4;
            PLACE(nHL, LD, "?????????");
            nHL += 9;
        }

        PLACE(nHL, NUMRES, "000");
        nHL += 3;
        PLACE(nHL, UDHDL, "00000");
        nHL += 5;
        PLACE(nHL, XHDL, "00000");
        nHL += 5;

        if (CSLFetchNameValue(papszOptions, "FILE_TRE") != NULL)
        {
            bOK &= NITFWriteTREsFromOptions(fp, nHL - 10, &nHL, papszOptions,
                                            "FILE_TRE=");
        }

        if (nHL > 999999)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Too big file header length : %d", nHL);
            CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
            return FALSE;
        }

        // update header length
        PLACE(354, HL, CPLSPrintf("%06d", nHL));

        nCur = nHL;
        iIM = 0;
    }
    else
    {
        // Append subdataset
        NITFFile *psFile = NITFOpenEx(fp, pszFilename);
        if (psFile == NULL)
            return FALSE;

        iIM = -1;
        nIM = 0;
        for (int i = 0; i < psFile->nSegmentCount; i++)
        {
            if (strcmp(psFile->pasSegmentInfo[i].szSegmentType, "IM") == 0)
            {
                nIM++;
                if (psFile->pasSegmentInfo[i].nSegmentHeaderSize == 0 &&
                    iIM < 0)
                {
                    iIM = i;
                    if (pnIndex)
                        *pnIndex = i;
                }
            }
        }
        if (pnImageCount)
            *pnImageCount = nIM;

        psFile->fp = NULL;
        NITFClose(psFile);

        if (iIM < 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Did not find free image segment");
            CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
            return FALSE;
        }
        nIM = iIM + 1;

        bOK &= VSIFSeekL(fp, 0, SEEK_END) == 0;
        nCur = VSIFTellL(fp);
    }

    /* -------------------------------------------------------------------- */
    /*      Prepare the image header.                                       */
    /* -------------------------------------------------------------------- */
    for (; iIM < nIM; iIM++)
    {
        char **papszIREPBANDTokens = NULL;
        char **papszISUBCATTokens = NULL;

        if (CSLFetchNameValue(papszOptions, "IREPBAND") != NULL)
        {
            papszIREPBANDTokens = CSLTokenizeStringComplex(
                CSLFetchNameValue(papszOptions, "IREPBAND"), ",", 0, 0);
            if (papszIREPBANDTokens != NULL &&
                CSLCount(papszIREPBANDTokens) != nBands)
            {
                CSLDestroy(papszIREPBANDTokens);
                papszIREPBANDTokens = NULL;
            }
        }
        if (CSLFetchNameValue(papszOptions, "ISUBCAT") != NULL)
        {
            papszISUBCATTokens = CSLTokenizeStringComplex(
                CSLFetchNameValue(papszOptions, "ISUBCAT"), ",", 0, 0);
            if (papszISUBCATTokens != NULL &&
                CSLCount(papszISUBCATTokens) != nBands)
            {
                CSLDestroy(papszISUBCATTokens);
                papszISUBCATTokens = NULL;
            }
        }

        bOK &= VSIFSeekL(fp, nCur, SEEK_SET) == 0;

        PLACE(nCur + 0, IM, "IM");
        OVR(10, nCur + 2, IID1, "Missing");
        OVR(14, nCur + 12, IDATIM, "20021216151629");
        OVR(17, nCur + 26, TGTID, "");
        OVR(80, nCur + 43, IID2, "");
        OVR(1, nCur + 123, ISCLAS, "U");
        OVR(2, nCur + 124, ISCLSY, "");
        OVR(11, nCur + 126, ISCODE, "");
        OVR(2, nCur + 137, ISCTLH, "");
        OVR(20, nCur + 139, ISREL, "");
        OVR(2, nCur + 159, ISDCTP, "");
        OVR(8, nCur + 161, ISDCDT, "");
        OVR(4, nCur + 169, ISDCXM, "");
        OVR(1, nCur + 173, ISDG, "");
        OVR(8, nCur + 174, ISDGDT, "");
        OVR(43, nCur + 182, ISCLTX, "");
        OVR(1, nCur + 225, ISCATP, "");
        OVR(40, nCur + 226, ISCAUT, "");
        OVR(1, nCur + 266, ISCRSN, "");
        OVR(8, nCur + 267, ISSRDT, "");
        OVR(15, nCur + 275, ISCTLN, "");
        PLACE(nCur + 290, ENCRYP, "0");
        OVR(42, nCur + 291, ISORCE, "Unknown");
        PLACE(nCur + 333, NROWS, CPLSPrintf("%08d", nLines));
        PLACE(nCur + 341, NCOLS, CPLSPrintf("%08d", nPixels));
        PLACE(nCur + 349, PVTYPE, pszPVType);
        PLACE(nCur + 352, IREP, pszIREP);
        OVR(8, nCur + 360, ICAT, "VIS");
        {
            const char *pszParamValue = CSLFetchNameValue(papszOptions, "ABPP");
            PLACE(nCur + 368, ABPP,
                  CPLSPrintf("%02d", pszParamValue ? atoi(pszParamValue)
                                                   : nBitsPerSample));
        }
        OVR(1, nCur + 370, PJUST, "R");
        OVR(1, nCur + 371, ICORDS, " ");

        nOffset = 372;

        {
            const char *pszParamValue;
            pszParamValue = CSLFetchNameValue(papszOptions, "ICORDS");
            if (pszParamValue == NULL)
                pszParamValue = " ";
            if (*pszParamValue != ' ')
            {
                OVR(60, nCur + nOffset, IGEOLO, "");
                nOffset += 60;
            }
        }

        {
            const char *pszICOM = CSLFetchNameValue(papszOptions, "ICOM");
            if (pszICOM != NULL)
            {
                char *pszRecodedICOM =
                    CPLRecode(pszICOM, CPL_ENC_UTF8, CPL_ENC_ISO8859_1);
                int nLenICOM = (int)strlen(pszRecodedICOM);
                int nICOM = (79 + nLenICOM) / 80;
                size_t nToWrite;
                if (nICOM > 9)
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "ICOM will be truncated");
                    nICOM = 9;
                }
                PLACE(nCur + nOffset, NICOM, CPLSPrintf("%01d", nICOM));
                nToWrite = MIN(nICOM * 80, nLenICOM);
                bOK &= VSIFWriteL(pszRecodedICOM, 1, nToWrite, fp) == nToWrite;
                nOffset += nICOM * 80;
                CPLFree(pszRecodedICOM);
            }
            else
            {
                PLACE(nCur + nOffset, NICOM, "0");
            }
        }

        if (pnICOffset)
        {
            if (iIM == 0 || bAppendSubdataset)
                *pnICOffset = nCur + nOffset + 1;
        }
        OVR(2, nCur + nOffset + 1, IC, "NC");

        if (pszIC[0] != 'N')
        {
            OVR(4, nCur + nOffset + 3, COMRAT, "    ");
            nOffset += 4;
        }

        if (nBands <= 9)
        {
            PLACE(nCur + nOffset + 3, NBANDS, CPLSPrintf("%d", nBands));
        }
        else
        {
            PLACE(nCur + nOffset + 3, NBANDS, "0");
            PLACE(nCur + nOffset + 4, XBANDS, CPLSPrintf("%05d", nBands));
            nOffset += 5;
        }

        nOffset += 4;

        /* --------------------------------------------------------------------
         */
        /*      Per band info */
        /* --------------------------------------------------------------------
         */
        for (iBand = 0; iBand < nBands; iBand++)
        {
            const char *pszIREPBAND = "M";

            if (papszIREPBANDTokens != NULL)
            {
                if (strlen(papszIREPBANDTokens[iBand]) > 2)
                {
                    papszIREPBANDTokens[iBand][2] = '\0';
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Truncating IREPBAND[%d] to '%s'", iBand + 1,
                             papszIREPBANDTokens[iBand]);
                }
                pszIREPBAND = papszIREPBANDTokens[iBand];
            }
            else if (EQUAL(pszIREP, "RGB/LUT"))
                pszIREPBAND = "LU";
            else if (EQUAL(pszIREP, "RGB"))
            {
                if (iBand == 0)
                    pszIREPBAND = "R";
                else if (iBand == 1)
                    pszIREPBAND = "G";
                else if (iBand == 2)
                    pszIREPBAND = "B";
            }
            else if (STARTS_WITH_CI(pszIREP, "YCbCr"))
            {
                if (iBand == 0)
                    pszIREPBAND = "Y";
                else if (iBand == 1)
                    pszIREPBAND = "Cb";
                else if (iBand == 2)
                    pszIREPBAND = "Cr";
            }

            PLACE(nCur + nOffset + 0, IREPBANDn, pszIREPBAND);

            if (papszISUBCATTokens != NULL)
            {
                if (strlen(papszISUBCATTokens[iBand]) > 6)
                {
                    papszISUBCATTokens[iBand][6] = '\0';
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Truncating ISUBCAT[%d] to '%s'", iBand + 1,
                             papszISUBCATTokens[iBand]);
                }
                PLACE(nCur + nOffset + 2, ISUBCATn, papszISUBCATTokens[iBand]);
            }
            //      else
            //          PLACE(nCur+nOffset+ 2, ISUBCATn, "" );

            PLACE(nCur + nOffset + 8, IFCn, "N");
            //      PLACE(nCur+nOffset+ 9, IMFLTn, "" );

            if (!EQUAL(pszIREP, "RGB/LUT"))
            {
                PLACE(nCur + nOffset + 12, NLUTSn, "0");
                nOffset += 13;
            }
            else
            {
                int iC, nCount = 256;

                if (CSLFetchNameValue(papszOptions, "LUT_SIZE") != NULL)
                    nCount = atoi(CSLFetchNameValue(papszOptions, "LUT_SIZE"));

                if (!(nCount >= 0 && nCount <= 99999))
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Invalid LUT value : %d. Defaulting to 256",
                             nCount);
                    nCount = 256;
                }
                PLACE(nCur + nOffset + 12, NLUTSn, "3");
                PLACE(nCur + nOffset + 13, NELUTn, CPLSPrintf("%05d", nCount));

                for (iC = 0; iC < nCount; iC++)
                {
                    WRITE_BYTE(nCur + nOffset + 18 + iC + 0, (char)iC);
                    WRITE_BYTE(nCur + nOffset + 18 + iC + nCount * 1, (char)iC);
                    WRITE_BYTE(nCur + nOffset + 18 + iC + nCount * 2, (char)iC);
                }
                nOffset += 18 + nCount * 3;
            }
        }

        CSLDestroy(papszIREPBANDTokens);
        CSLDestroy(papszISUBCATTokens);

        /* --------------------------------------------------------------------
         */
        /*      Remainder of image header info. */
        /* --------------------------------------------------------------------
         */
        PLACE(nCur + nOffset + 0, ISYNC, "0");

        /* RGB JPEG compressed NITF requires IMODE=P (see #3345) */
        if (nBands >= 3 && (EQUAL(pszIC, "C3") || EQUAL(pszIC, "M3")))
        {
            PLACE(nCur + nOffset + 1, IMODE, "P");
        }
        else
        {
            PLACE(nCur + nOffset + 1, IMODE, "B");
        }
        PLACE(nCur + nOffset + 2, NBPR, CPLSPrintf("%04d", nNBPR));
        PLACE(nCur + nOffset + 6, NBPC, CPLSPrintf("%04d", nNBPC));
        PLACE(nCur + nOffset + 10, NPPBH, CPLSPrintf("%04d", nNPPBH));
        PLACE(nCur + nOffset + 14, NPPBV, CPLSPrintf("%04d", nNPPBV));
        PLACE(nCur + nOffset + 18, NBPP, CPLSPrintf("%02d", nBitsPerSample));
        PLACE(nCur + nOffset + 20, IDLVL,
              CPLSPrintf("%03d", atoi(CSLFetchNameValueDef(papszOptions,
                                                           "IDLVL", "1"))));
        PLACE(nCur + nOffset + 23, IALVL,
              CPLSPrintf("%03d", atoi(CSLFetchNameValueDef(papszOptions,
                                                           "IALVL", "0"))));
        PLACE(nCur + nOffset + 26, ILOCROW,
              CPLSPrintf("%05d", atoi(CSLFetchNameValueDef(papszOptions,
                                                           "ILOCROW", "0"))));
        PLACE(nCur + nOffset + 31, ILOCCOL,
              CPLSPrintf("%05d", atoi(CSLFetchNameValueDef(papszOptions,
                                                           "ILOCCOL", "0"))));
        PLACE(nCur + nOffset + 36, IMAG, "1.0 ");
        PLACE(nCur + nOffset + 40, UDIDL, "00000");
        PLACE(nCur + nOffset + 45, IXSHDL, "00000");

        nOffsetUDIDL = nCur + nOffset + 40;
        nOffset += 50;

        /* --------------------------------------------------------------------
         */
        /*      Add BLOCKA TRE if requested. */
        /* --------------------------------------------------------------------
         */
        if (CSLFetchNameValue(papszOptions, "BLOCKA_BLOCK_COUNT") != NULL)
        {
            NITFWriteBLOCKA(fp, nOffsetUDIDL, &nOffset, papszOptions);
        }

        if (CSLFetchNameValue(papszOptions, "TRE") != NULL ||
            CSLFetchNameValue(papszOptions, "RESERVE_SPACE_FOR_TRE_OVERFLOW") !=
                NULL)
        {
            bOK &= NITFWriteTREsFromOptions(fp, nOffsetUDIDL, &nOffset,
                                            papszOptions, "TRE=");
        }

        /* --------------------------------------------------------------------
         */
        /*      Update the image header length in the file header. */
        /* --------------------------------------------------------------------
         */
        nIHSize = nOffset;

        if (nIHSize > 999999)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Too big image header length : %d", nIHSize);
            CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
            return FALSE;
        }

        PLACE(363 + iIM * 16, LISH1, CPLSPrintf("%06d", nIHSize));
        if (EQUAL(pszIC, "NC"))
        {
            PLACE(
                369 + iIM * 16, LIi,
                CPLSPrintf("%010" CPL_FRMT_GB_WITHOUT_PREFIX "u", nImageSize));
        }

        nCur += nIHSize;
        if (pnImageOffset)
        {
            if (iIM == 0 || bAppendSubdataset)
                *pnImageOffset = nCur;
        }
        nCur += nImageSize;

        if (!bWriteAllImages)
            break;
    }

    /* -------------------------------------------------------------------- */
    /*      Fill in image data by writing one byte at the end               */
    /* -------------------------------------------------------------------- */
    if (EQUAL(pszIC, "NC"))
    {
        char cNul = 0;
        bOK &= VSIFSeekL(fp, nCur - 1, SEEK_SET) == 0;
        bOK &= VSIFWriteL(&cNul, 1, 1, fp) == 1;
    }

    /* -------------------------------------------------------------------- */
    /*      Compute and update CLEVEL ("complexity" level).                 */
    /*      See: http://164.214.2.51/ntb/baseline/docs/2500b/2500b_not2.pdf */
    /*            page 96u                                                  */
    /* -------------------------------------------------------------------- */
    nCLevel = 3;
    if (bAppendSubdataset)
    {
        // Get existing CLEVEL
        bOK &= VSIFSeekL(fp, 9, SEEK_SET) == 0;
        char szCLEVEL[3] = {0};
        bOK &= VSIFReadL(szCLEVEL, 1, 2, fp) != 0;
        nCLevel = atoi(szCLEVEL);
    }
    if (nBands > 9 || nIM > 20 || nPixels > 2048 || nLines > 2048 ||
        nNPPBH > 2048 || nNPPBV > 2048 || nCur > 52428799)
    {
        nCLevel = MAX(nCLevel, 5);
    }
    if (nPixels > 8192 || nLines > 8192 || nNPPBH > 8192 || nNPPBV > 8192 ||
        nCur > 1073741833 || nDES > 10)
    {
        nCLevel = MAX(nCLevel, 6);
    }
    if (nBands > 256 || nPixels > 65536 || nLines > 65536 ||
        nCur > 2147483647 || nDES > 50)
    {
        nCLevel = MAX(nCLevel, 7);
    }
    OVR(2, 9, CLEVEL, CPLSPrintf("%02d", nCLevel));

    /* -------------------------------------------------------------------- */
    /*      Update total file length                                        */
    /* -------------------------------------------------------------------- */

    /* According to the spec, CLEVEL 7 supports up to 10,737,418,330 bytes */
    /* but we can support technically much more */
    if (EQUAL(pszIC, "NC") && nCur >= 999999999999ULL)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Too big file : " CPL_FRMT_GUIB,
                 nCur);
        CPL_IGNORE_RET_VAL_INT(VSIFCloseL(fp));
        return FALSE;
    }

    PLACE(342, FL, CPLSPrintf("%012" CPL_FRMT_GB_WITHOUT_PREFIX "d", nCur));

    if (VSIFCloseL(fp) != 0)
        bOK = FALSE;

    return bOK;
}

static int NITFWriteOption(VSILFILE *psFile, char **papszOptions, size_t nWidth,
                           GUIntBig nLocation, const char *pszName,
                           const char *pszText)
{
    const char *pszParamValue;
    char *pszRecodedValue;
    size_t nToWrite;
    int bOK = TRUE;

    pszParamValue = CSLFetchNameValue(papszOptions, pszName);
    if (pszParamValue == NULL)
    {
        pszRecodedValue = CPLRecode(pszText, CPL_ENC_UTF8, CPL_ENC_ISO8859_1);
    }
    else
    {
        pszRecodedValue =
            CPLRecode(pszParamValue, CPL_ENC_UTF8, CPL_ENC_ISO8859_1);
    }

    bOK &= NITFGotoOffset(psFile, nLocation);
    nToWrite = MIN(nWidth, strlen(pszRecodedValue));
    bOK &= VSIFWriteL(pszRecodedValue, 1, nToWrite, psFile) == nToWrite;
    CPLFree(pszRecodedValue);
    return bOK;
}

/************************************************************************/
/*                            NITFWriteTRE()                            */
/************************************************************************/

static int NITFWriteTRE(VSILFILE *fp, vsi_l_offset nOffsetUDIDL, int *pnOffset,
                        const char *pszTREName, char *pabyTREData,
                        int nTREDataSize)

{
    char szTemp[12];
    int nOldOffset;
    int bOK = TRUE;

    /* -------------------------------------------------------------------- */
    /*      Update IXSHDL.                                                  */
    /* -------------------------------------------------------------------- */
    bOK &= VSIFSeekL(fp, nOffsetUDIDL + 5, SEEK_SET) == 0;
    bOK &= VSIFReadL(szTemp, 1, 5, fp) == 5;
    szTemp[5] = 0;
    nOldOffset = atoi(szTemp);

    if (nOldOffset == 0)
    {
        nOldOffset = 3;
        PLACE(nOffsetUDIDL + 10, IXSOFL, "000");
        *pnOffset += 3;
    }

    if (nOldOffset + 11 + nTREDataSize > 99999 || nTREDataSize < 0 ||
        nTREDataSize > 99999)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Too big TRE to be written");
        return FALSE;
    }

    snprintf(szTemp, sizeof(szTemp), "%05d", nOldOffset + 11 + nTREDataSize);
    PLACE(nOffsetUDIDL + 5, IXSHDL, szTemp);

    /* -------------------------------------------------------------------- */
    /*      Create TRE prefix.                                              */
    /* -------------------------------------------------------------------- */
    snprintf(szTemp, sizeof(szTemp), "%-6s%05d", pszTREName, nTREDataSize);
    bOK &= VSIFSeekL(fp, nOffsetUDIDL + 10 + nOldOffset, SEEK_SET) == 0;
    bOK &= VSIFWriteL(szTemp, 11, 1, fp) == 1;
    bOK &= (int)VSIFWriteL(pabyTREData, 1, nTREDataSize, fp) == nTREDataSize;

    /* -------------------------------------------------------------------- */
    /*      Increment values.                                               */
    /* -------------------------------------------------------------------- */
    *pnOffset += nTREDataSize + 11;

    return bOK;
}

/************************************************************************/
/*                   NITFWriteTREsFromOptions()                         */
/************************************************************************/

static int NITFWriteTREsFromOptions(VSILFILE *fp, vsi_l_offset nOffsetUDIDL,
                                    int *pnOffset, char **papszOptions,
                                    const char *pszTREPrefix)

{
    int bIgnoreBLOCKA =
        CSLFetchNameValue(papszOptions, "BLOCKA_BLOCK_COUNT") != NULL;
    int iOption;
    const bool bReserveSpaceForTREOverflow =
        CSLFetchNameValue(papszOptions, "RESERVE_SPACE_FOR_TRE_OVERFLOW") !=
        NULL;

    if (papszOptions == NULL)
        return TRUE;

    for (iOption = 0; papszOptions[iOption] != NULL; iOption++)
    {
        const char *pszEscapedContents;
        char *pszUnescapedContents;
        char *pszTREName;
        int nContentLength;
        const char *pszSpace;
        int bIsHex = FALSE;
        int nTREPrefixLen = (int)strlen(pszTREPrefix);

        if (!EQUALN(papszOptions[iOption], pszTREPrefix, nTREPrefixLen))
            continue;

        if (STARTS_WITH_CI(papszOptions[iOption] + nTREPrefixLen, "BLOCKA=") &&
            bIgnoreBLOCKA)
            continue;

        if (STARTS_WITH_CI(papszOptions[iOption] + nTREPrefixLen, "HEX/"))
        {
            bIsHex = TRUE;
            nTREPrefixLen += 4;
        }

        /* We do no longer use CPLParseNameValue() as it removes leading spaces
         */
        /* from the value (see #3088) */
        pszSpace = strchr(papszOptions[iOption] + nTREPrefixLen, '=');
        if (pszSpace == NULL)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Could not parse creation options %s",
                     papszOptions[iOption] + nTREPrefixLen);
            return FALSE;
        }

        pszTREName = CPLStrdup(papszOptions[iOption] + nTREPrefixLen);
        pszTREName[MIN(6, pszSpace - (papszOptions[iOption] + nTREPrefixLen))] =
            '\0';
        pszEscapedContents = pszSpace + 1;

        pszUnescapedContents = CPLUnescapeString(
            pszEscapedContents, &nContentLength, CPLES_BackslashQuotable);

        if (bIsHex)
        {
            int i;
            char pszSubStr[3];

            if (nContentLength % 2)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "Could not parse creation options %s: invalid hex data",
                    papszOptions[iOption] + nTREPrefixLen);
                CPLFree(pszTREName);
                CPLFree(pszUnescapedContents);
                return FALSE;
            }

            nContentLength = nContentLength / 2;
            for (i = 0; i < nContentLength; i++)
            {
                CPLStrlcpy(pszSubStr, pszUnescapedContents + 2 * i, 3);
                pszUnescapedContents[i] = (char)strtoul(pszSubStr, NULL, 16);
            }
            pszUnescapedContents[nContentLength] = '\0';
        }

        if (!NITFWriteTRE(fp, nOffsetUDIDL, pnOffset, pszTREName,
                          pszUnescapedContents, nContentLength))
        {
            CPLFree(pszTREName);
            CPLFree(pszUnescapedContents);
            return FALSE;
        }

        CPLFree(pszTREName);
        CPLFree(pszUnescapedContents);
    }

    if (bReserveSpaceForTREOverflow)
    {
        /* --------------------------------------------------------------------
         */
        /*      Update IXSHDL. */
        /* --------------------------------------------------------------------
         */
        int nOldOffset;
        char szTemp[6];
        bool bOK = VSIFSeekL(fp, nOffsetUDIDL + 5, SEEK_SET) == 0;
        bOK &= VSIFReadL(szTemp, 1, 5, fp) == 5;
        szTemp[5] = 0;
        nOldOffset = atoi(szTemp);

        if (nOldOffset == 0)
        {
            PLACE(nOffsetUDIDL + 5, IXSHDL, "00003");

            PLACE(nOffsetUDIDL + 10, IXSOFL, "000");
            *pnOffset += 3;
        }

        return bOK;
    }

    return TRUE;
}

/************************************************************************/
/*                          NITFWriteBLOCKA()                           */
/************************************************************************/

static int NITFWriteBLOCKA(VSILFILE *fp, vsi_l_offset nOffsetUDIDL,
                           int *pnOffset, char **papszOptions)

{
    static const char *const apszFields[] = {"BLOCK_INSTANCE",
                                             "0",
                                             "2",
                                             "N_GRAY",
                                             "2",
                                             "5",
                                             "L_LINES",
                                             "7",
                                             "5",
                                             "LAYOVER_ANGLE",
                                             "12",
                                             "3",
                                             "SHADOW_ANGLE",
                                             "15",
                                             "3",
                                             "BLANKS",
                                             "18",
                                             "16",
                                             "FRLC_LOC",
                                             "34",
                                             "21",
                                             "LRLC_LOC",
                                             "55",
                                             "21",
                                             "LRFC_LOC",
                                             "76",
                                             "21",
                                             "FRFC_LOC",
                                             "97",
                                             "21",
                                             NULL,
                                             NULL,
                                             NULL};
    int nBlockCount =
        atoi(CSLFetchNameValue(papszOptions, "BLOCKA_BLOCK_COUNT"));
    int iBlock;

    /* ==================================================================== */
    /*      Loop over all the blocks we have metadata for.                  */
    /* ==================================================================== */
    for (iBlock = 1; iBlock <= nBlockCount; iBlock++)
    {
        char szBLOCKA[123];
        int iField;

        /* --------------------------------------------------------------------
         */
        /*      Write all fields. */
        /* --------------------------------------------------------------------
         */
        for (iField = 0; apszFields[iField * 3] != NULL; iField++)
        {
            char szFullFieldName[64];
            int iStart = atoi(apszFields[iField * 3 + 1]);
            int iSize = atoi(apszFields[iField * 3 + 2]);
            const char *pszValue;

            snprintf(szFullFieldName, sizeof(szFullFieldName), "BLOCKA_%s_%02d",
                     apszFields[iField * 3 + 0], iBlock);

            pszValue = CSLFetchNameValue(papszOptions, szFullFieldName);
            if (pszValue == NULL)
                pszValue = "";

            if (iSize - (int)strlen(pszValue) < 0)
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "Too much data for %s. Got %d bytes, max allowed is %d",
                    szFullFieldName, (int)strlen(pszValue), iSize);
                return FALSE;
            }

            /* Right align value and left pad with spaces */
            memset(szBLOCKA + iStart, ' ', iSize);
            /* unsigned is always >= 0 */
            /* memcpy( szBLOCKA + iStart +
             * MAX((size_t)0,iSize-strlen(pszValue)), */
            memcpy(szBLOCKA + iStart + (iSize - (int)strlen(pszValue)),
                   pszValue, strlen(pszValue));
        }

        // required field - semantics unknown.
        memcpy(szBLOCKA + 118, "010.0", 5);

        if (!NITFWriteTRE(fp, nOffsetUDIDL, pnOffset, "BLOCKA", szBLOCKA, 123))
            return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/*                       NITFCollectSegmentInfo()                       */
/*                                                                      */
/*      Collect the information about a set of segments of a            */
/*      particular type from the NITF file header, and add them to      */
/*      the segment list in the NITFFile object.                        */
/************************************************************************/

static int NITFCollectSegmentInfo(NITFFile *psFile, int nFileHeaderLen,
                                  int nOffset, const char szType[3],
                                  int nHeaderLenSize, int nDataLenSize,
                                  GUIntBig *pnNextData)

{
    char szTemp[12];
    int nCount, nSegDefSize, iSegment;

    /* -------------------------------------------------------------------- */
    /*      Get the segment count, and grow the segmentinfo array           */
    /*      accordingly.                                                    */
    /* -------------------------------------------------------------------- */
    if (nFileHeaderLen < nOffset + 3)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Not enough bytes to read segment count");
        return -1;
    }

    NITFGetField(szTemp, psFile->pachHeader, nOffset, 3);
    nCount = atoi(szTemp);

    if (nCount <= 0)
        return nOffset + 3;

    nSegDefSize = nCount * (nHeaderLenSize + nDataLenSize);
    if (nFileHeaderLen < nOffset + 3 + nSegDefSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Not enough bytes to read segment info");
        return -1;
    }

    if (psFile->pasSegmentInfo == NULL)
        psFile->pasSegmentInfo =
            (NITFSegmentInfo *)CPLMalloc(sizeof(NITFSegmentInfo) * nCount);
    else
        psFile->pasSegmentInfo = (NITFSegmentInfo *)CPLRealloc(
            psFile->pasSegmentInfo,
            sizeof(NITFSegmentInfo) * (psFile->nSegmentCount + nCount));

    /* -------------------------------------------------------------------- */
    /*      Collect detailed about segment.                                 */
    /* -------------------------------------------------------------------- */
    for (iSegment = 0; iSegment < nCount; iSegment++)
    {
        NITFSegmentInfo *psInfo =
            psFile->pasSegmentInfo + psFile->nSegmentCount;

        psInfo->nDLVL = -1;
        psInfo->nALVL = -1;
        psInfo->nLOC_R = -1;
        psInfo->nLOC_C = -1;
        psInfo->nCCS_R = -1;
        psInfo->nCCS_C = -1;

        psInfo->hAccess = NULL;
        strncpy(psInfo->szSegmentType, szType, sizeof(psInfo->szSegmentType));
        psInfo->szSegmentType[sizeof(psInfo->szSegmentType) - 1] = '\0';

        psInfo->nSegmentHeaderSize = atoi(NITFGetField(
            szTemp, psFile->pachHeader,
            nOffset + 3 + iSegment * (nHeaderLenSize + nDataLenSize),
            nHeaderLenSize));
        if (strchr(szTemp, '-') != NULL) /* Avoid negative values being mapped
                                            to huge unsigned values */
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid segment header size : %s", szTemp);
            return -1;
        }

        if (strcmp(szType, "DE") == 0 && psInfo->nSegmentHeaderSize == 207)
        {
            /* DMAAC A.TOC files have a wrong header size. It says 207 but it is
             * 209 really */
            psInfo->nSegmentHeaderSize = 209;
        }

        psInfo->nSegmentSize = CPLScanUIntBig(
            NITFGetField(szTemp, psFile->pachHeader,
                         nOffset + 3 +
                             iSegment * (nHeaderLenSize + nDataLenSize) +
                             nHeaderLenSize,
                         nDataLenSize),
            nDataLenSize);
        if (strchr(szTemp, '-') != NULL) /* Avoid negative values being mapped
                                            to huge unsigned values */
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid segment size : %s",
                     szTemp);
            return -1;
        }

        psInfo->nSegmentHeaderStart = *pnNextData;
        psInfo->nSegmentStart = *pnNextData + psInfo->nSegmentHeaderSize;

        *pnNextData += (psInfo->nSegmentHeaderSize + psInfo->nSegmentSize);
        psFile->nSegmentCount++;
    }

    return nOffset + nSegDefSize + 3;
}

/************************************************************************/
/*                            NITFGetField()                            */
/*                                                                      */
/*      Copy a field from a passed in header buffer into a temporary    */
/*      buffer and zero terminate it.                                   */
/************************************************************************/

char *NITFGetField(char *pszTarget, const char *pszSource, int nStart,
                   int nLength)

{
    memcpy(pszTarget, pszSource + nStart, nLength);
    pszTarget[nLength] = '\0';

    return pszTarget;
}

/************************************************************************/
/*                            NITFFindTRE()                             */
/************************************************************************/

const char *NITFFindTRE(const char *pszTREData, int nTREBytes,
                        const char *pszTag, int *pnFoundTRESize)

{
    char szTemp[100];

    while (nTREBytes >= 11)
    {
        int nThisTRESize = atoi(NITFGetField(szTemp, pszTREData, 6, 5));
        if (nThisTRESize < 0)
        {
            NITFGetField(szTemp, pszTREData, 0, 6);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid size (%d) for TRE %s", nThisTRESize, szTemp);
            return NULL;
        }
        if (nTREBytes - 11 < nThisTRESize)
        {
            NITFGetField(szTemp, pszTREData, 0, 6);
            if (STARTS_WITH_CI(szTemp, "RPFIMG"))
            {
                /* See #3848 */
                CPLDebug("NITF",
                         "Adjusting RPFIMG TRE size from %d to %d, which is "
                         "the remaining size",
                         nThisTRESize, nTREBytes - 11);
                nThisTRESize = nTREBytes - 11;
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot read %s TRE. Not enough bytes : remaining %d, "
                         "expected %d",
                         szTemp, nTREBytes - 11, nThisTRESize);
                return NULL;
            }
        }

        if (EQUALN(pszTREData, pszTag, 6))
        {
            if (pnFoundTRESize != NULL)
                *pnFoundTRESize = nThisTRESize;

            return pszTREData + 11;
        }

        nTREBytes -= (nThisTRESize + 11);
        pszTREData += (nThisTRESize + 11);
    }

    return NULL;
}

/************************************************************************/
/*                     NITFFindTREByIndex()                             */
/************************************************************************/

const char *NITFFindTREByIndex(const char *pszTREData, int nTREBytes,
                               const char *pszTag, int nTreIndex,
                               int *pnFoundTRESize)

{
    char szTemp[100];

    while (nTREBytes >= 11)
    {
        int nThisTRESize = atoi(NITFGetField(szTemp, pszTREData, 6, 5));
        if (nThisTRESize < 0)
        {
            NITFGetField(szTemp, pszTREData, 0, 6);
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid size (%d) for TRE %s", nThisTRESize, szTemp);
            return NULL;
        }
        if (nTREBytes - 11 < nThisTRESize)
        {
            NITFGetField(szTemp, pszTREData, 0, 6);
            if (STARTS_WITH_CI(szTemp, "RPFIMG"))
            {
                /* See #3848 */
                CPLDebug("NITF",
                         "Adjusting RPFIMG TRE size from %d to %d, which is "
                         "the remaining size",
                         nThisTRESize, nTREBytes - 11);
                nThisTRESize = nTREBytes - 11;
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot read %s TRE. Not enough bytes : remaining %d, "
                         "expected %d",
                         szTemp, nTREBytes - 11, nThisTRESize);
                return NULL;
            }
        }

        if (EQUALN(pszTREData, pszTag, 6))
        {
            if (nTreIndex <= 0)
            {
                if (pnFoundTRESize != NULL)
                    *pnFoundTRESize = nThisTRESize;

                return pszTREData + 11;
            }

            /* Found a previous one - skip it ... */
            nTreIndex--;
        }

        nTREBytes -= (nThisTRESize + 11);
        pszTREData += (nThisTRESize + 11);
    }

    return NULL;
}

/************************************************************************/
/*                        NITFExtractMetadata()                         */
/************************************************************************/

static void NITFExtractAndRecodeMetadata(char ***ppapszMetadata,
                                         const char *pachHeader, int nStart,
                                         int nLength, const char *pszName,
                                         const char *pszSrcEncoding)

{
    char szWork[400];
    char *pszWork;
    char *pszRecode;

    if (nLength <= 0)
        return;

    if (nLength >= (int)(sizeof(szWork) - 1))
        pszWork = (char *)CPLMalloc(nLength + 1);
    else
        pszWork = szWork;

    /* trim white space */
    while (nLength > 0 && pachHeader[nStart + nLength - 1] == ' ')
        nLength--;

    memcpy(pszWork, pachHeader + nStart, nLength);
    pszWork[nLength] = '\0';

    if (strcmp(pszSrcEncoding, CPL_ENC_UTF8) != 0)
    {
        pszRecode = CPLRecode(pszWork, pszSrcEncoding, CPL_ENC_UTF8);
        *ppapszMetadata = CSLSetNameValue(*ppapszMetadata, pszName, pszRecode);
        CPLFree(pszRecode);
    }
    else
    {
        *ppapszMetadata = CSLSetNameValue(*ppapszMetadata, pszName, pszWork);
    }

    if (szWork != pszWork)
        CPLFree(pszWork);
}

void NITFExtractMetadata(char ***ppapszMetadata, const char *pachHeader,
                         int nStart, int nLength, const char *pszName)

{
    NITFExtractAndRecodeMetadata(ppapszMetadata, pachHeader, nStart, nLength,
                                 pszName, CPL_ENC_ISO8859_1);
}

/************************************************************************/
/*        NITF_WGS84_Geocentric_Latitude_To_Geodetic_Latitude()         */
/*                                                                      */
/*      The input is a geocentric latitude in degrees.  The output      */
/*      is a geodetic latitude in degrees.                              */
/************************************************************************/

/*
 * "The angle L' is called "geocentric latitude" and is defined as the
 * angle between the equatorial plane and the radius from the geocenter.
 *
 * The angle L is called "geodetic latitude" and is defined as the angle
 * between the equatorial plane and the normal to the surface of the
 * ellipsoid.  The word "latitude" usually means geodetic latitude.  This
 * is the basis for most of the maps and charts we use.  The normal to the
 * surface is the direction that a plumb bob would hang were it not for
 * local anomalies in the earth's gravitational field."
 */

double NITF_WGS84_Geocentric_Latitude_To_Geodetic_Latitude(double dfLat)

{
    /* WGS84 Ellipsoid */
    const double a = 6378137.0;
    const double b = 6356752.3142;

    /* convert to radians */
    dfLat = dfLat * M_PI / 180.0;

    /* convert to geodetic */
    dfLat = atan(((a * a) / (b * b)) * tan(dfLat));

    /* convert back to degrees */
    dfLat = dfLat * 180.0 / M_PI;

    return dfLat;
}

/************************************************************************/
/*                        NITFGetSeriesInfo()                           */
/************************************************************************/

/* From
 * http://trac.osgeo.org/gdal/attachment/ticket/5353/MIL-STD-2411_1_CHG-3.pdf */
static const NITFSeries nitfSeries[] = {
    {"A1", "CM", "1:10K", "Combat Charts (1:10K)", "CADRG"},
    {"A2", "CM", "1:25K", "Combat Charts (1:25K)", "CADRG"},
    {"A3", "CM", "1:50K", "Combat Charts (1:50K)", "CADRG"},
    {"A4", "CM", "1:100K", "Combat Charts (1:100K)", "CADRG"},
    {"AT", "ATC", "1:200K", "Series 200 Air Target Chart", "CADRG"},
    {"C1", "CG", "1:10000", "City Graphics", "CADRG"},
    {"C2", "CG", "1:10560", "City Graphics", "CADRG"},
    {"C3", "CG", "1:11000", "City Graphics", "CADRG"},
    {"C4", "CG", "1:11800", "City Graphics", "CADRG"},
    {"C5", "CG", "1:12000", "City Graphics", "CADRG"},
    {"C6", "CG", "1:12500", "City Graphics", "CADRG"},
    {"C7", "CG", "1:12800", "City Graphics", "CADRG"},
    {"C8", "CG", "1:14000", "City Graphics", "CADRG"},
    {"C9", "CG", "1:14700", "City Graphics", "CADRG"},
    {"CA", "CG", "1:15000", "City Graphics", "CADRG"},
    {"CB", "CG", "1:15500", "City Graphics", "CADRG"},
    {"CC", "CG", "1:16000", "City Graphics", "CADRG"},
    {"CD", "CG", "1:16666", "City Graphics", "CADRG"},
    {"CE", "CG", "1:17000", "City Graphics", "CADRG"},
    {"CF", "CG", "1:17500", "City Graphics", "CADRG"},
    {"CG", "CG", "Various", "City Graphics", "CADRG"},
    {"CH", "CG", "1:18000", "City Graphics", "CADRG"},
    {"CJ", "CG", "1:20000", "City Graphics", "CADRG"},
    {"CK", "CG", "1:21000", "City Graphics", "CADRG"},
    {"CL", "CG", "1:21120", "City Graphics", "CADRG"},
    {"CM", "CM", "Various", "Combat Charts", "CADRG"},
    {"CN", "CG", "1:22000", "City Graphics", "CADRG"},
    {"CO", "CO", "Various", "Coastal Charts", "CADRG"},
    {"CP", "CG", "1:23000", "City Graphics", "CADRG"},
    {"CQ", "CG", "1:25000", "City Graphics", "CADRG"},
    {"CR", "CG", "1:26000", "City Graphics", "CADRG"},
    {"CS", "CG", "1:35000", "City Graphics", "CADRG"},
    {"CT", "CG", "1:36000", "City Graphics", "CADRG"},
    {"D1", "", "100m", "Elevation Data from DTED level 1", "CDTED"},
    {"D2", "", "30m", "Elevation Data from DTED level 2", "CDTED"},
    {"EG", "NARC", "1:11,000,000", "North Atlantic Route Chart", "CADRG"},
    {"ES", "SEC", "1:500K", "VFR Sectional", "CADRG"},
    {"ET", "SEC", "1:250K", "VFR Sectional Inserts", "CADRG"},
    {"F1", "TFC-1", "1:250K", "Transit Flying Chart (TBD #1)", "CADRG"},
    {"F2", "TFC-2", "1:250K", "Transit Flying Chart (TBD #2)", "CADRG"},
    {"F3", "TFC-3", "1:250K", "Transit Flying Chart (TBD #3)", "CADRG"},
    {"F4", "TFC-4", "1:250K", "Transit Flying Chart (TBD #4)", "CADRG"},
    {"F5", "TFC-5", "1:250K", "Transit Flying Chart (TBD #5)", "CADRG"},
    {"GN", "GNC", "1:5M", "Global Navigation Chart", "CADRG"},
    {"HA", "HA", "Various", "Harbor and Approach Charts", "CADRG"},
    {"I1", "", "10m", "Imagery, 10 meter resolution", "CIB"},
    {"I2", "", "5m", "Imagery, 5 meter resolution", "CIB"},
    {"I3", "", "2m", "Imagery, 2 meter resolution", "CIB"},
    {"I4", "", "1m", "Imagery, 1 meter resolution", "CIB"},
    {"I5", "", ".5m", "Imagery, .5 (half) meter resolution", "CIB"},
    {"IV", "", "Various > 10m", "Imagery, greater than 10 meter resolution",
     "CIB"},
    {"JA", "JOG-A", "1:250K", "Joint Operation Graphic - Air", "CADRG"},
    {"JG", "JOG", "1:250K", "Joint Operation Graphic", "CADRG"},
    {"JN", "JNC", "1:2M", "Jet Navigation Chart", "CADRG"},
    {"JO", "OPG", "1:250K", "Operational Planning Graphic", "CADRG"},
    {"JR", "JOG-R", "1:250K", "Joint Operation Graphic - Radar", "CADRG"},
    {"K1", "ICM", "1:8K", "Image City Maps", "CADRG"},
    {"K2", "ICM", "1:10K", "Image City Maps", "CADRG"},
    {"K3", "ICM", "1:10560", "Image City Maps", "CADRG"},
    {"K7", "ICM", "1:12500", "Image City Maps", "CADRG"},
    {"K8", "ICM", "1:12800", "Image City Maps", "CADRG"},
    {"KB", "ICM", "1:15K", "Image City Maps", "CADRG"},
    {"KE", "ICM", "1:16666", "Image City Maps", "CADRG"},
    {"KM", "ICM", "1:21120", "Image City Maps", "CADRG"},
    {"KR", "ICM", "1:25K", "Image City Maps", "CADRG"},
    {"KS", "ICM", "1:26K", "Image City Maps", "CADRG"},
    {"KU", "ICM", "1:36K", "Image City Maps", "CADRG"},
    {"L1", "LFC-1", "1:500K", "Low Flying Chart (TBD #1)", "CADRG"},
    {"L2", "LFC-2", "1:500K", "Low Flying Chart (TBD #2)", "CADRG"},
    {"L3", "LFC-3", "1:500K", "Low Flying Chart (TBD #3)", "CADRG"},
    {"L4", "LFC-4", "1:500K", "Low Flying Chart (TBD #4)", "CADRG"},
    {"L5", "LFC-5", "1:500K", "Low Flying Chart (TBD #5)", "CADRG"},
    {"LF", "LFC-FR (Day)", "1:500K", "Low Flying Chart (Day) - Host Nation",
     "CADRG"},
    {"LN", "LN (Night)", "1:500K", "Low Flying Chart (Night) - Host Nation",
     "CADRG"},
    {"M1", "MIM", "Various", "Military Installation Maps (TBD #1)", "CADRG"},
    {"M2", "MIM", "Various", "Military Installation Maps (TBD #2)", "CADRG"},
    {"MH", "MIM", "1:25K", "Military Installation Maps", "CADRG"},
    {"MI", "MIM", "1:50K", "Military Installation Maps", "CADRG"},
    {"MJ", "MIM", "1:100K", "Military Installation Maps", "CADRG"},
    {"MM", "", "Various", "(Miscellaneous Maps & Charts)", "CADRG"},
    {"OA", "OPAREA", "Various", "Naval Range Operation Area Chart", "CADRG"},
    {"OH", "VHRC", "1:1M", "VFR Helicopter Route Chart", "CADRG"},
    {"ON", "ONC", "1:1M", "Operational Navigation Chart", "CADRG"},
    {"OW", "WAC", "1:1M", "High Flying Chart - Host Nation", "CADRG"},
    {"P1", "", "1:25K", "Special Military Map - Overlay", "CADRG"},
    {"P2", "", "1:25K", "Special Military Purpose", "CADRG"},
    {"P3", "", "1:25K", "Special Military Purpose", "CADRG"},
    {"P4", "", "1:25K", "Special Military Purpose", "CADRG"},
    {"P5", "", "1:50K", "Special Military Map - Overlay", "CADRG"},
    {"P6", "", "1:50K", "Special Military Purpose", "CADRG"},
    {"P7", "", "1:50K", "Special Military Purpose", "CADRG"},
    {"P8", "", "1:50K", "Special Military Purpose", "CADRG"},
    {"P9", "", "1:100K", "Special Military Map - Overlay", "CADRG"},
    {"PA", "", "1:100K", "Special Military Purpose", "CADRG"},
    {"PB", "", "1:100K", "Special Military Purpose", "CADRG"},
    {"PC", "", "1:100K", "Special Military Purpose", "CADRG"},
    {"PD", "", "1:250K", "Special Military Map - Overlay", "CADRG"},
    {"PE", "", "1:250K", "Special Military Purpose", "CADRG"},
    {"PF", "", "1:250K", "Special Military Purpose", "CADRG"},
    {"PG", "", "1:250K", "Special Military Purpose", "CADRG"},
    {"PH", "", "1:500K", "Special Military Map - Overlay", "CADRG"},
    {"PI", "", "1:500K", "Special Military Purpose", "CADRG"},
    {"PJ", "", "1:500K", "Special Military Purpose", "CADRG"},
    {"PK", "", "1:500K", "Special Military Purpose", "CADRG"},
    {"PL", "", "1:1M", "Special Military Map - Overlay", "CADRG"},
    {"PM", "", "1:1M", "Special Military Purpose", "CADRG"},
    {"PN", "", "1:1M", "Special Military Purpose", "CADRG"},
    {"PO", "", "1:1M", "Special Military Purpose", "CADRG"},
    {"PP", "", "1:2M", "Special Military Map - Overlay", "CADRG"},
    {"PQ", "", "1:2M", "Special Military Purpose", "CADRG"},
    {"PR", "", "1:2M", "Special Military Purpose", "CADRG"},
    {"PS", "", "1:5M", "Special Military Map - Overlay", "CADRG"},
    {"PT", "", "1:5M", "Special Military Purpose", "CADRG"},
    {"PU", "", "1:5M", "Special Military Purpose", "CADRG"},
    {"PV", "", "1:5M", "Special Military Purpose", "CADRG"},
    {"R1", "", "1:50K", "Range Charts", "CADRG"},
    {"R2", "", "1:100K", "Range Charts", "CADRG"},
    {"R3", "", "1:250K", "Range Charts", "CADRG"},
    {"R4", "", "1:500K", "Range Charts", "CADRG"},
    {"R5", "", "1:1M", "Range Charts", "CADRG"},
    {"RC", "RGS-100", "1:100K", "Russian General Staff Maps", "CADRG"},
    {"RL", "RGS-50", "1:50K", "Russian General Staff Maps", "CADRG"},
    {"RR", "RGS-200", "1:200K", "Russian General Staff Maps", "CADRG"},
    {"RV", "Riverine", "1:50K", "Riverine Map 1:50,000 scale", "CADRG"},
    {"TC", "TLM 100", "1:100K", "Topographic Line Map 1:100,000 scale",
     "CADRG"},
    {"TF", "TFC (Day)", "1:250K", "Transit Flying Chart (Day)", "CADRG"},
    {"TL", "TLM50", "1:50K", "Topographic Line Map", "CADRG"},
    {"TN", "TFC (Night)", "1:250K",
     "Transit Flying Chart (Night) - Host Nation", "CADRG"},
    {"TP", "TPC", "1:500K", "Tactical Pilotage Chart", "CADRG"},
    {"TQ", "TLM24", "1:24K", "Topographic Line Map 1:24,000 scale", "CADRG"},
    {"TR", "TLM200", "1:200K", "Topographic Line Map 1:200,000 scale", "CADRG"},
    {"TT", "TLM25", "1:25K", "Topographic Line Map 1:25,000 scale", "CADRG"},
    {"UL", "TLM50 - Other", "1:50K",
     "Topographic Line Map (other 1:50,000 scale)", "CADRG"},
    {"V1", "Inset HRC", "1:50", "Helicopter Route Chart Inset", "CADRG"},
    {"V2", "Inset HRC", "1:62500", "Helicopter Route Chart Inset", "CADRG"},
    {"V3", "Inset HRC", "1:90K", "Helicopter Route Chart Inset", "CADRG"},
    {"V4", "Inset HRC", "1:250K", "Helicopter Route Chart Inset", "CADRG"},
    {"VH", "HRC", "1:125K", "Helicopter Route Chart", "CADRG"},
    {"VN", "VNC", "1:500K", "Visual Navigation Charts", "CADRG"},
    {"VT", "VTAC", "1:250K", "VFR Terminal Area Chart", "CADRG"},
    {"WA", "", "1:250K", "IFR Enroute Low", "CADRG"},
    {"WB", "", "1:500K", "IFR Enroute Low", "CADRG"},
    {"WC", "", "1:750K", "IFR Enroute Low", "CADRG"},
    {"WD", "", "1:1M", "IFR Enroute Low", "CADRG"},
    {"WE", "", "1:1.5M", "IFR Enroute Low", "CADRG"},
    {"WF", "", "1:2M", "IFR Enroute Low", "CADRG"},
    {"WG", "", "1:2.5M", "IFR Enroute Low", "CADRG"},
    {"WH", "", "1:3M", "IFR Enroute Low", "CADRG"},
    {"WI", "", "1:3.5M", "IFR Enroute Low", "CADRG"},
    {"WK", "", "1:4M", "IFR Enroute Low", "CADRG"},
    {"XD", "", "1:1M", "IFR Enroute High", "CADRG"},
    {"XE", "", "1:1.5M", "IFR Enroute High", "CADRG"},
    {"XF", "", "1:2M", "IFR Enroute High", "CADRG"},
    {"XG", "", "1:2.5M", "IFR Enroute High", "CADRG"},
    {"XH", "", "1:3M", "IFR Enroute High", "CADRG"},
    {"XI", "", "1:3.5M", "IFR Enroute High", "CADRG"},
    {"XJ", "", "1:4M", "IFR Enroute High", "CADRG"},
    {"XK", "", "1:4.5M", "IFR Enroute High", "CADRG"},
    {"Y9", "", "1:16.5M", "IFR Enroute Area", "CADRG"},
    {"YA", "", "1:250K", "IFR Enroute Area", "CADRG"},
    {"YB", "", "1:500K", "IFR Enroute Area", "CADRG"},
    {"YC", "", "1:750K", "IFR Enroute Area", "CADRG"},
    {"YD", "", "1:1M", "IFR Enroute Area", "CADRG"},
    {"YE", "", "1:1.5M", "IFR Enroute Area", "CADRG"},
    {"YF", "", "1:2M", "IFR Enroute Area", "CADRG"},
    {"YI", "", "1:3.5M", "IFR Enroute Area", "CADRG"},
    {"YJ", "", "1:4M", "IFR Enroute Area", "CADRG"},
    {"YZ", "", "1:12M", "IFR Enroute Area", "CADRG"},
    {"ZA", "", "1:250K", "IFR Enroute High/Low", "CADRG"},
    {"ZB", "", "1:500K", "IFR Enroute High/Low", "CADRG"},
    {"ZC", "", "1:750K", "IFR Enroute High/Low", "CADRG"},
    {"ZD", "", "1:1M", "IFR Enroute High/Low", "CADRG"},
    {"ZE", "", "1:1.5M", "IFR Enroute High/Low", "CADRG"},
    {"ZF", "", "1:2M", "IFR Enroute High/Low", "CADRG"},
    {"ZG", "", "1:2.5M", "IFR Enroute High/Low", "CADRG"},
    {"ZH", "", "1:3M", "IFR Enroute High/Low", "CADRG"},
    {"ZI", "", "1:3.5M", "IFR Enroute High/Low", "CADRG"},
    {"ZJ", "", "1:4M", "IFR Enroute High/Low", "CADRG"},
    {"ZK", "", "1:4.5M", "IFR Enroute High/Low", "CADRG"},
    {"ZT", "", "1:9M", "IFR Enroute High/Low", "CADRG"},
    {"ZV", "", "1:10M", "IFR Enroute High/Low", "CADRG"},
    {"ZZ", "", "1:12M", "IFR Enroute High/Low", "CADRG"}};

/* See 24111CN1.pdf paragraph 5.1.4 */
const NITFSeries *NITFGetSeriesInfo(const char *pszFilename)
{
    int i;
    char seriesCode[3] = {0, 0, 0};
    if (pszFilename == NULL)
        return NULL;
    for (i = (int)strlen(pszFilename) - 1; i >= 0; i--)
    {
        if (pszFilename[i] == '.')
        {
            if (i < (int)strlen(pszFilename) - 3)
            {
                seriesCode[0] = pszFilename[i + 1];
                seriesCode[1] = pszFilename[i + 2];
                for (i = 0;
                     i < (int)(sizeof(nitfSeries) / sizeof(nitfSeries[0])); i++)
                {
                    if (EQUAL(seriesCode, nitfSeries[i].code))
                    {
                        return &nitfSeries[i];
                    }
                }
                return NULL;
            }
        }
    }
    return NULL;
}

/************************************************************************/
/*                       NITFCollectAttachments()                       */
/*                                                                      */
/*      Collect attachment, display level and location info into the    */
/*      segmentinfo structures.                                         */
/************************************************************************/

int NITFCollectAttachments(NITFFile *psFile)

{
    int iSegment;

    /* ==================================================================== */
    /*      Loop over all segments.                                         */
    /* ==================================================================== */
    for (iSegment = 0; iSegment < psFile->nSegmentCount; iSegment++)
    {
        NITFSegmentInfo *psSegInfo = psFile->pasSegmentInfo + iSegment;

        /* --------------------------------------------------------------------
         */
        /*      For image segments, we use the normal image access stuff. */
        /* --------------------------------------------------------------------
         */
        if (EQUAL(psSegInfo->szSegmentType, "IM"))
        {
            NITFImage *psImage = NITFImageAccess(psFile, iSegment);
            if (psImage == NULL)
                return FALSE;

            psSegInfo->nDLVL = psImage->nIDLVL;
            psSegInfo->nALVL = psImage->nIALVL;
            psSegInfo->nLOC_R = psImage->nILOCRow;
            psSegInfo->nLOC_C = psImage->nILOCColumn;
        }
        /* --------------------------------------------------------------------
         */
        /*      For graphic file we need to process the header. */
        /* --------------------------------------------------------------------
         */
        else if (EQUAL(psSegInfo->szSegmentType, "SY") ||
                 EQUAL(psSegInfo->szSegmentType, "GR"))
        {
            char achSubheader[298];
            int nSTYPEOffset;
            char szTemp[100];

            /* --------------------------------------------------------------------
             */
            /*      Load the graphic subheader. */
            /* --------------------------------------------------------------------
             */
            if (VSIFSeekL(psFile->fp, psSegInfo->nSegmentHeaderStart,
                          SEEK_SET) != 0 ||
                VSIFReadL(achSubheader, 1, sizeof(achSubheader), psFile->fp) <
                    258)
            {
                CPLError(CE_Warning, CPLE_FileIO,
                         "Failed to read graphic subheader at " CPL_FRMT_GUIB
                         ".",
                         psSegInfo->nSegmentHeaderStart);
                continue;
            }

            // NITF 2.0. (also works for NITF 2.1)
            nSTYPEOffset = 200;
            if (STARTS_WITH_CI(achSubheader + 193, "999998"))
                nSTYPEOffset += 40;

            /* --------------------------------------------------------------------
             */
            /*      Report some standard info. */
            /* --------------------------------------------------------------------
             */
            psSegInfo->nDLVL =
                atoi(NITFGetField(szTemp, achSubheader, nSTYPEOffset + 14, 3));
            psSegInfo->nALVL =
                atoi(NITFGetField(szTemp, achSubheader, nSTYPEOffset + 17, 3));
            psSegInfo->nLOC_R =
                atoi(NITFGetField(szTemp, achSubheader, nSTYPEOffset + 20, 5));
            psSegInfo->nLOC_C =
                atoi(NITFGetField(szTemp, achSubheader, nSTYPEOffset + 25, 5));
        }
    }

    return TRUE;
}

/************************************************************************/
/*                      NITFReconcileAttachments()                      */
/*                                                                      */
/*      Generate the CCS location information for all the segments      */
/*      if possible.                                                    */
/************************************************************************/

int NITFReconcileAttachments(NITFFile *psFile)

{
    int iSegment;
    int bSuccess = TRUE;
    int bMadeProgress = FALSE;

    for (iSegment = 0; iSegment < psFile->nSegmentCount; iSegment++)
    {
        NITFSegmentInfo *psSegInfo = psFile->pasSegmentInfo + iSegment;
        int iOther;

        // already processed?
        if (psSegInfo->nCCS_R != -1)
            continue;

        // unattached segments are straight forward.
        if (psSegInfo->nALVL < 1)
        {
            psSegInfo->nCCS_R = psSegInfo->nLOC_R;
            psSegInfo->nCCS_C = psSegInfo->nLOC_C;
            if (psSegInfo->nCCS_R != -1)
                bMadeProgress = TRUE;
            continue;
        }

        // Loc for segment to which we are attached.
        for (iOther = 0; iOther < psFile->nSegmentCount; iOther++)
        {
            NITFSegmentInfo *psOtherSegInfo = psFile->pasSegmentInfo + iOther;

            if (psSegInfo->nALVL == psOtherSegInfo->nDLVL)
            {
                if (psOtherSegInfo->nCCS_R != -1)
                {
                    psSegInfo->nCCS_R =
                        psOtherSegInfo->nLOC_R + psSegInfo->nLOC_R;
                    psSegInfo->nCCS_C =
                        psOtherSegInfo->nLOC_C + psSegInfo->nLOC_C;
                    if (psSegInfo->nCCS_R != -1)
                        bMadeProgress = TRUE;
                }
                else
                {
                    bSuccess = FALSE;
                }
                break;
            }
        }

        if (iOther == psFile->nSegmentCount)
            bSuccess = FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      If succeeded or made no progress then return our success        */
    /*      flag.  Otherwise make another pass, hopefully filling in        */
    /*      more values.                                                    */
    /* -------------------------------------------------------------------- */
    if (bSuccess || !bMadeProgress)
        return bSuccess;
    else
        return NITFReconcileAttachments(psFile);
}

/************************************************************************/
/*                        NITFFindValFromEnd()                          */
/************************************************************************/

static const char *NITFFindValFromEnd(char **papszMD, int nMDSize,
                                      const char *pszVar,
                                      CPL_UNUSED const char *pszDefault)
{
    int nVarLen = (int)strlen(pszVar);
    int nIter = nMDSize - 1;
    for (; nIter >= 0; nIter--)
    {
        if (strncmp(papszMD[nIter], pszVar, nVarLen) == 0 &&
            papszMD[nIter][nVarLen] == '=')
            return papszMD[nIter] + nVarLen + 1;
    }
    return NULL;
}

/************************************************************************/
/*                  NITFFindValRecursive()                              */
/************************************************************************/

static const char *NITFFindValRecursive(char **papszMD, int nMDSize,
                                        const char *pszMDPrefix,
                                        const char *pszVar)
{
    char *pszMDItemName = CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, pszVar));
    const char *pszCondVal =
        NITFFindValFromEnd(papszMD, nMDSize, pszMDItemName, NULL);

    if (pszCondVal == NULL)
    {
        /* Needed for SENSRB */
        /* See https://github.com/OSGeo/gdal/issues/1520 */
        /* If the condition variable is not found at this level, */
        /* try to research it at upper levels by shortening on _ */
        /* separators */
        char *pszMDPrefixShortened = CPLStrdup(pszMDPrefix);
        char *pszLastUnderscore = strrchr(pszMDPrefixShortened, '_');
        if (pszLastUnderscore)
        {
            *pszLastUnderscore = 0;
            pszLastUnderscore = strrchr(pszMDPrefixShortened, '_');
        }
        while (pszLastUnderscore)
        {
            pszLastUnderscore[1] = 0;
            CPLFree(pszMDItemName);
            pszMDItemName =
                CPLStrdup(CPLSPrintf("%s%s", pszMDPrefixShortened, pszVar));
            pszCondVal =
                NITFFindValFromEnd(papszMD, nMDSize, pszMDItemName, NULL);
            if (pszCondVal)
                break;
            *pszLastUnderscore = 0;
            pszLastUnderscore = strrchr(pszMDPrefixShortened, '_');
        }
        CPLFree(pszMDPrefixShortened);

        if (!pszCondVal)
            pszCondVal = NITFFindValFromEnd(papszMD, nMDSize, pszVar, NULL);
    }
    CPLFree(pszMDItemName);

    return pszCondVal;
}

/************************************************************************/
/*                              CSLSplit()                              */
/************************************************************************/

static char **CSLSplit(const char *pszStr, const char *pszSplitter)
{
    char **papszRet = NULL;
    const char *pszIter = pszStr;
    while (TRUE)
    {
        const char *pszNextSplitter = strstr(pszIter, pszSplitter);
        if (pszNextSplitter == NULL)
        {
            papszRet = CSLAddString(papszRet, pszIter);
            break;
        }
        size_t nLen = (size_t)(pszNextSplitter - pszIter);
        char *pszToken = (char *)CPLMalloc(nLen + 1);
        memcpy(pszToken, pszIter, nLen);
        pszToken[nLen] = 0;
        papszRet = CSLAddString(papszRet, pszToken);
        CPLFree(pszToken);
        pszIter = pszNextSplitter + strlen(pszSplitter);
    }
    return papszRet;
}

/************************************************************************/
/*                          NITFEvaluateCond()                          */
/************************************************************************/

static int NITFEvaluateCond(const char *pszCond, char **papszMD, int *pnMDSize,
                            const char *pszMDPrefix,
                            const char *pszDESOrTREKind,
                            const char *pszDESOrTREName)
{
    const char *pszAnd = strstr(pszCond, " AND ");
    const char *pszOr = strstr(pszCond, " OR ");
    if (pszAnd && pszOr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Unsupported if condition in %s %s in XML resource: %s. "
                 "AND and OR conditions cannot be used at the same time",
                 pszDESOrTREName, pszDESOrTREKind, pszCond);
        return -1;
    }

    int nRet = 0;
    const char *pszOperator;
    if (pszAnd)
    {
        char **papszTokens = CSLSplit(pszCond, " AND ");
        for (char **papszIter = papszTokens; *papszIter; ++papszIter)
        {
            nRet = NITFEvaluateCond(*papszIter, papszMD, pnMDSize, pszMDPrefix,
                                    pszDESOrTREKind, pszDESOrTREName);
            // exit early as soon as we have a negative evaluation (or error)
            if (nRet != 1)
                break;
        }
        CSLDestroy(papszTokens);
    }
    else if (pszOr)
    {
        char **papszTokens = CSLSplit(pszCond, " OR ");
        for (char **papszIter = papszTokens; *papszIter; ++papszIter)
        {
            nRet = NITFEvaluateCond(*papszIter, papszMD, pnMDSize, pszMDPrefix,
                                    pszDESOrTREKind, pszDESOrTREName);
            // exit early as soon as we have a positive evaluation (or error)
            if (nRet != 0)
                break;
        }
        CSLDestroy(papszTokens);
    }
    else if ((pszOperator = strchr(pszCond, '=')) != NULL)
    {
        char *pszCondVar = (char *)CPLMalloc(pszOperator - pszCond + 1);
        const char *pszCondExpectedVal = pszOperator + 1;
        const char *pszCondVal;
        int bTestEqual = FALSE;
        int bTestNotEqual = FALSE;
        int bTestGreaterOrEqual = FALSE;
        memcpy(pszCondVar, pszCond, pszOperator - pszCond);
        if (pszOperator - pszCond > 1 &&
            pszCondVar[pszOperator - pszCond - 1] == '!')
        {
            bTestNotEqual = TRUE;
            pszCondVar[pszOperator - pszCond - 1] = '\0';
        }
        else if (pszOperator - pszCond > 1 &&
                 pszCondVar[pszOperator - pszCond - 1] == '>')
        {
            bTestGreaterOrEqual = TRUE;
            pszCondVar[pszOperator - pszCond - 1] = '\0';
        }
        else
        {
            bTestEqual = TRUE;
        }
        pszCondVar[pszOperator - pszCond] = '\0';
        pszCondVal =
            NITFFindValRecursive(papszMD, *pnMDSize, pszMDPrefix, pszCondVar);
        if (pszCondVal == NULL)
        {
            CPLDebug("NITF", "Cannot find if cond variable %s", pszCondVar);
        }
        else if ((bTestEqual && strcmp(pszCondVal, pszCondExpectedVal) == 0) ||
                 (bTestNotEqual &&
                  strcmp(pszCondVal, pszCondExpectedVal) != 0) ||
                 (bTestGreaterOrEqual &&
                  strcmp(pszCondVal, pszCondExpectedVal) >= 0))
        {
            nRet = 1;
        }
        CPLFree(pszCondVar);
    }
    else if ((pszOperator = strchr(pszCond, ':')) != NULL)
    {
        char *pszCondVar = (char *)CPLMalloc(pszOperator - pszCond + 1);
        const char *pszCondTestBit = pszOperator + 1;
        const char *pszCondVal;
        memcpy(pszCondVar, pszCond, pszOperator - pszCond);
        pszCondVar[pszOperator - pszCond] = '\0';
        pszCondVal =
            NITFFindValRecursive(papszMD, *pnMDSize, pszMDPrefix, pszCondVar);
        if (pszCondVal == NULL)
        {
            CPLDebug("NITF", "Cannot find if cond variable %s", pszCondVar);
        }
        else if (strtoul(pszCondVal, CPL_NULLPTR, 10) &
                 (1U << (unsigned)atoi(pszCondTestBit)))
        {
            nRet = 1;
        }
        CPLFree(pszCondVar);
    }
    else
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Invalid if construct in %s %s in XML resource: %s. "
                 "invalid 'cond' attribute",
                 pszDESOrTREName, pszDESOrTREKind, pszCond);
        return -1;
    }
    return nRet;
}

/************************************************************************/
/*                  NITFGenericMetadataReadTREInternal()                */
/************************************************************************/

static char **NITFGenericMetadataReadTREInternal(
    char **papszMD, int *pnMDSize, int *pnMDAlloc, CPLXMLNode *psOutXMLNode,
    const char *pszDESOrTREKind, const char *pszDESOrTREName,
    const char *pachTRE, int nTRESize, CPLXMLNode *psTreNode, int *pnTreOffset,
    const char *pszMDPrefix, bool bValidate, int *pbError)
{
    CPLXMLNode *psIter;
    for (psIter = psTreNode->psChild; psIter != NULL && *pbError == FALSE;
         psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
            strcmp(psIter->pszValue, "field") == 0)
        {
            const char *pszName = CPLGetXMLValue(psIter, "name", NULL);
            const char *pszLongName = CPLGetXMLValue(psIter, "longname", NULL);
            const char *pszLength = CPLGetXMLValue(psIter, "length", NULL);
            const char *pszType = CPLGetXMLValue(psIter, "type", "string");
            const char *pszMinVal = CPLGetXMLValue(psIter, "minval", NULL);
            const char *pszMaxVal = CPLGetXMLValue(psIter, "maxval", NULL);
            int nLength = -1;
            if (pszLength != NULL)
                nLength = atoi(pszLength);
            else
            {
                const char *pszLengthVar =
                    CPLGetXMLValue(psIter, "length_var", NULL);
                if (pszLengthVar != NULL)
                {
                    // Preferably look for item at the same level as ours.
                    const char *pszLengthValue = CSLFetchNameValue(
                        papszMD, CPLSPrintf("%s%s", pszMDPrefix, pszLengthVar));
                    if (pszLengthValue != NULL)
                    {
                        nLength = atoi(pszLengthValue);
                    }
                    else
                    {
                        char **papszMDIter = papszMD;
                        while (papszMDIter != NULL && *papszMDIter != NULL)
                        {
                            if (strstr(*papszMDIter, pszLengthVar) != NULL)
                            {
                                const char *pszEqual =
                                    strchr(*papszMDIter, '=');
                                if (pszEqual != NULL)
                                {
                                    nLength = atoi(pszEqual + 1);
                                    // Voluntary missing break so as to find the
                                    // "closest" item to ours in case it is not
                                    // defined in the same level
                                }
                            }
                            papszMDIter++;
                        }
                    }
                }
            }
            if (pszName != NULL && nLength > 0)
            {
                char *pszMDItemName;
                char **papszTmp = NULL;
                char *pszValue = NULL;

                if (*pnTreOffset + nLength > nTRESize)
                {
                    *pbError = TRUE;
                    CPLError(bValidate ? CE_Failure : CE_Warning,
                             CPLE_AppDefined,
                             "Not enough bytes when reading %s %s "
                             "(at least %d needed, only %d available)",
                             pszDESOrTREName, pszDESOrTREKind,
                             *pnTreOffset + nLength, nTRESize);
                    break;
                }

                pszMDItemName =
                    CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, pszName));

                if (strcmp(pszType, "IEEE754_Float32_BigEndian") == 0)
                {
                    if (nLength == 4)
                    {
                        const size_t nBufferSize = 128;
                        float f;
                        memcpy(&f, pachTRE + *pnTreOffset, 4);
                        CPL_MSBPTR32(&f);
                        pszValue = (char *)CPLMalloc(nBufferSize);
                        CPLsnprintf(pszValue, nBufferSize, "%f", f);
                        papszTmp =
                            CSLSetNameValue(papszTmp, pszMDItemName, pszValue);
                    }
                    else
                    {
                        *pbError = TRUE;
                        CPLError(bValidate ? CE_Failure : CE_Warning,
                                 CPLE_AppDefined,
                                 "IEEE754_Float32_BigEndian field must be 4 "
                                 "bytes in %s %s",
                                 pszDESOrTREName, pszDESOrTREKind);
                        break;
                    }
                }
                else if (strcmp(pszType, "UnsignedInt_BigEndian") == 0 ||
                         strcmp(pszType, "bitmask") == 0)
                {
                    if (nLength <= 8)
                    {
                        const size_t nBufferSize = 21;
                        unsigned long long nVal = 0;
                        GByte byData;

                        int i;
                        for (i = 0; i < nLength; ++i)
                        {
                            memcpy(&byData, pachTRE + *pnTreOffset + i, 1);
                            nVal += (unsigned long long)byData
                                    << 8 * (nLength - i - 1);
                        }

                        pszValue = (char *)CPLMalloc(nBufferSize);
                        CPLsnprintf(pszValue, nBufferSize, CPL_FRMT_GUIB,
                                    (GUIntBig)nVal);
                        papszTmp =
                            CSLSetNameValue(papszTmp, pszMDItemName, pszValue);
                    }
                    else
                    {
                        *pbError = TRUE;
                        CPLError(bValidate ? CE_Failure : CE_Warning,
                                 CPLE_AppDefined,
                                 "UnsignedInt/bitmask field must be <= 8 bytes "
                                 "in %s %s",
                                 pszDESOrTREName, pszDESOrTREKind);
                        break;
                    }
                }
                else if (strcmp(pszType, "ISO8859-1") == 0)
                {
                    NITFExtractMetadata(&papszTmp, pachTRE, *pnTreOffset,
                                        nLength, pszMDItemName);

                    pszValue =
                        CPLStrdup(CSLFetchNameValue(papszTmp, pszMDItemName));
                }
                else
                {
                    NITFExtractAndRecodeMetadata(&papszTmp, pachTRE,
                                                 *pnTreOffset, nLength,
                                                 pszMDItemName, CPL_ENC_UTF8);

                    pszValue = CPLStrdup(strchr(papszTmp[0], '=') + 1);
                }

                if (papszTmp)
                {
                    if (*pnMDSize + 1 >= *pnMDAlloc)
                    {
                        *pnMDAlloc = (*pnMDAlloc * 4 / 3) + 32;
                        papszMD = (char **)CPLRealloc(
                            papszMD, *pnMDAlloc * sizeof(char *));
                    }
                    papszMD[*pnMDSize] = papszTmp[0];
                    papszMD[(*pnMDSize) + 1] = NULL;
                    (*pnMDSize)++;
                    papszTmp[0] = NULL;
                    CSLDestroy(papszTmp);
                }

                CPLXMLNode *psFieldNode = NULL;
                if (pszValue != NULL && psOutXMLNode != NULL)
                {
                    CPLXMLNode *psNameNode;
                    CPLXMLNode *psValueNode;

                    psFieldNode =
                        CPLCreateXMLNode(psOutXMLNode, CXT_Element, "field");
                    psNameNode =
                        CPLCreateXMLNode(psFieldNode, CXT_Attribute, "name");
                    psValueNode =
                        CPLCreateXMLNode(psFieldNode, CXT_Attribute, "value");
                    CPLCreateXMLNode(psNameNode, CXT_Text,
                                     (pszName[0] || pszLongName == NULL)
                                         ? pszName
                                         : pszLongName);
                    CPLCreateXMLNode(psValueNode, CXT_Text, pszValue);
                }

                if (pszValue != NULL)
                {
                    if (pszMinVal != NULL)
                    {
                        bool bMinValConstraintOK = true;
                        if (strcmp(pszType, "real") == 0)
                        {
                            bMinValConstraintOK =
                                CPLAtof(pszValue) >= CPLAtof(pszMinVal);
                        }
                        else if (strcmp(pszType, "integer") == 0)
                        {
                            bMinValConstraintOK = CPLAtoGIntBig(pszValue) >=
                                                  CPLAtoGIntBig(pszMinVal);
                        }
                        if (!bMinValConstraintOK)
                        {
                            if (bValidate)
                            {
                                CPLError(CE_Failure, CPLE_AppDefined,
                                         "%s %s: minimum value constraint of "
                                         "%s for %s=%s not met",
                                         pszDESOrTREKind, pszDESOrTREName,
                                         pszMinVal, pszName, pszValue);
                            }
                            if (psFieldNode)
                            {
                                CPLCreateXMLElementAndValue(
                                    psFieldNode,
                                    bValidate ? "error" : "warning",
                                    CPLSPrintf("Minimum value constraint of %s "
                                               "not met",
                                               pszMinVal));
                            }
                        }
                    }
                    if (pszMaxVal != NULL)
                    {
                        bool bMinValConstraintOK = true;
                        if (strcmp(pszType, "real") == 0)
                        {
                            bMinValConstraintOK =
                                CPLAtof(pszValue) <= CPLAtof(pszMaxVal);
                        }
                        else if (strcmp(pszType, "integer") == 0)
                        {
                            bMinValConstraintOK = CPLAtoGIntBig(pszValue) <=
                                                  CPLAtoGIntBig(pszMaxVal);
                        }
                        if (!bMinValConstraintOK)
                        {
                            if (bValidate)
                            {
                                CPLError(CE_Failure, CPLE_AppDefined,
                                         "%s %s: maximum value constraint of "
                                         "%s for %s=%s not met",
                                         pszDESOrTREKind, pszDESOrTREName,
                                         pszMaxVal, pszName, pszValue);
                            }
                            if (psFieldNode)
                            {
                                CPLCreateXMLElementAndValue(
                                    psFieldNode,
                                    bValidate ? "error" : "warning",
                                    CPLSPrintf("Maximum value constraint of %s "
                                               "not met",
                                               pszMaxVal));
                            }
                        }
                    }
                }

                CPLFree(pszMDItemName);
                CPLFree(pszValue);

                *pnTreOffset += nLength;
            }
            else if (nLength > 0)
            {
                *pnTreOffset += nLength;
            }
            else
            {
                *pbError = TRUE;
                CPLError(bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                         "Invalid item construct in %s %s in XML resource",
                         pszDESOrTREName, pszDESOrTREKind);
                break;
            }
        }
        else if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
                 strcmp(psIter->pszValue, "loop") == 0)
        {
            const char *pszCounter = CPLGetXMLValue(psIter, "counter", NULL);
            const char *pszIterations =
                CPLGetXMLValue(psIter, "iterations", NULL);
            const char *pszFormula = CPLGetXMLValue(psIter, "formula", NULL);
            const char *pszMDSubPrefix =
                CPLGetXMLValue(psIter, "md_prefix", NULL);
            int nIterations = -1;

            if (pszCounter != NULL)
            {
                const char *pszIterationsVal = NITFFindValRecursive(
                    papszMD, *pnMDSize, pszMDPrefix, pszCounter);
                if (pszIterationsVal == NULL ||
                    (nIterations = atoi(pszIterationsVal)) < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "invalid 'counter' %s",
                        pszDESOrTREName, pszDESOrTREKind, pszCounter);
                    *pbError = TRUE;
                    break;
                }
            }
            else if (pszIterations != NULL)
            {
                nIterations = atoi(pszIterations);
            }
            else if (pszFormula != NULL &&
                     strcmp(pszFormula, "NPAR*NPARO") == 0)
            {
                char *pszMDNPARName =
                    CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, "NPAR"));
                int NPAR = atoi(NITFFindValFromEnd(papszMD, *pnMDSize,
                                                   pszMDNPARName, "-1"));
                char *pszMDNPAROName =
                    CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, "NPARO"));
                int NPARO = atoi(NITFFindValFromEnd(papszMD, *pnMDSize,
                                                    pszMDNPAROName, "-1"));
                CPLFree(pszMDNPARName);
                CPLFree(pszMDNPAROName);
                if (NPAR < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "invalid 'counter' %s",
                        pszDESOrTREName, pszDESOrTREKind, "NPAR");
                    *pbError = TRUE;
                    break;
                }
                if (NPARO < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "invalid 'counter' %s",
                        pszDESOrTREName, pszDESOrTREKind, "NPAR0");
                    *pbError = TRUE;
                    break;
                }
                nIterations = NPAR * NPARO;
            }
            else if (pszFormula != NULL && strcmp(pszFormula, "NPLN-1") == 0)
            {
                char *pszMDItemName =
                    CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, "NPLN"));
                int NPLN = atoi(NITFFindValFromEnd(papszMD, *pnMDSize,
                                                   pszMDItemName, "-1"));
                CPLFree(pszMDItemName);
                if (NPLN < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "invalid 'counter' %s",
                        pszDESOrTREName, pszDESOrTREKind, "NPLN");
                    *pbError = TRUE;
                    break;
                }
                nIterations = NPLN - 1;
            }
            else if (pszFormula != NULL &&
                     strcmp(pszFormula, "NXPTS*NYPTS") == 0)
            {
                char *pszMDNPARName =
                    CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, "NXPTS"));
                int NXPTS = atoi(NITFFindValFromEnd(papszMD, *pnMDSize,
                                                    pszMDNPARName, "-1"));
                char *pszMDNPAROName =
                    CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, "NYPTS"));
                int NYPTS = atoi(NITFFindValFromEnd(papszMD, *pnMDSize,
                                                    pszMDNPAROName, "-1"));
                CPLFree(pszMDNPARName);
                CPLFree(pszMDNPAROName);
                if (NXPTS < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "invalid 'counter' %s",
                        pszDESOrTREName, pszDESOrTREKind, "NXPTS");
                    *pbError = TRUE;
                    break;
                }
                if (NYPTS < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "invalid 'counter' %s",
                        pszDESOrTREName, pszDESOrTREKind, "NYPTS");
                    *pbError = TRUE;
                    break;
                }
                nIterations = NXPTS * NYPTS;
            }
            else if (pszFormula)
            {
                const char *const apszVarAndFormulaNp1NDiv2[] = {
                    "NPAR",         "(NPART+1)*(NPART)/2",
                    "NUMOPG",       "(NUMOPG+1)*(NUMOPG)/2",
                    "NUM_ADJ_PARM", "(NUM_ADJ_PARM+1)*(NUM_ADJ_PARM)/2",
                    "N1_CAL",       "(N1_CAL+1)*(N1_CAL)/2",
                    "NUM_PARA",     "(NUM_PARA+1)*(NUM_PARA)/2",
                    NULL,           NULL};

                for (int i = 0; apszVarAndFormulaNp1NDiv2[i]; i += 2)
                {
                    if (strcmp(pszFormula, apszVarAndFormulaNp1NDiv2[i + 1]) ==
                        0)
                    {
                        const char *pszVar = apszVarAndFormulaNp1NDiv2[i];
                        char *pszMDItemName =
                            CPLStrdup(CPLSPrintf("%s%s", pszMDPrefix, pszVar));
                        int var = atoi(NITFFindValFromEnd(papszMD, *pnMDSize,
                                                          pszMDItemName, "-1"));
                        CPLFree(pszMDItemName);
                        if (var < 0)
                        {
                            CPLError(bValidate ? CE_Failure : CE_Warning,
                                     CPLE_AppDefined,
                                     "Invalid loop construct in %s %s in XML "
                                     "resource : "
                                     "invalid 'counter' %s",
                                     pszDESOrTREName, pszDESOrTREKind, pszVar);
                            *pbError = TRUE;
                            return papszMD;
                        }
                        nIterations = var * (var + 1) / 2;
                        break;
                    }
                }

                if (nIterations < 0)
                {
                    CPLError(
                        bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                        "Invalid loop construct in %s %s in XML resource : "
                        "missing or invalid 'counter' or 'iterations' or "
                        "'formula'",
                        pszDESOrTREName, pszDESOrTREKind);
                    *pbError = TRUE;
                    break;
                }
            }

            if (nIterations > 0)
            {
                int iIter;
                const char *pszPercent;
                int bHasValidPercentD = FALSE;
                CPLXMLNode *psRepeatedNode = NULL;
                CPLXMLNode *psLastChild = NULL;

                /* Check that md_prefix has one and only %XXXXd pattern */
                if (pszMDSubPrefix != NULL &&
                    (pszPercent = strchr(pszMDSubPrefix, '%')) != NULL &&
                    strchr(pszPercent + 1, '%') == NULL)
                {
                    const char *pszIter = pszPercent + 1;
                    while (*pszIter != '\0')
                    {
                        if (*pszIter >= '0' && *pszIter <= '9')
                            pszIter++;
                        else if (*pszIter == 'd')
                        {
                            bHasValidPercentD = atoi(pszPercent + 1) <= 10;
                            break;
                        }
                        else
                            break;
                    }
                }

                if (psOutXMLNode != NULL)
                {
                    CPLXMLNode *psNumberNode;
                    CPLXMLNode *psNameNode;
                    const char *pszName = CPLGetXMLValue(psIter, "name", NULL);
                    psRepeatedNode =
                        CPLCreateXMLNode(psOutXMLNode, CXT_Element, "repeated");
                    if (pszName)
                    {
                        psNameNode = CPLCreateXMLNode(psRepeatedNode,
                                                      CXT_Attribute, "name");
                        CPLCreateXMLNode(psNameNode, CXT_Text, pszName);
                    }
                    psNumberNode = CPLCreateXMLNode(psRepeatedNode,
                                                    CXT_Attribute, "number");
                    CPLCreateXMLNode(psNumberNode, CXT_Text,
                                     CPLSPrintf("%d", nIterations));

                    psLastChild = psRepeatedNode->psChild;
                    while (psLastChild->psNext != NULL)
                        psLastChild = psLastChild->psNext;
                }

                for (iIter = 0; iIter < nIterations && *pbError == FALSE;
                     iIter++)
                {
                    char *pszMDNewPrefix = NULL;
                    CPLXMLNode *psGroupNode = NULL;
                    if (pszMDSubPrefix != NULL)
                    {
                        if (bHasValidPercentD)
                        {
                            const size_t nTmpLen =
                                strlen(pszMDSubPrefix) + 10 + 1;
                            char *szTmp = (char *)CPLMalloc(nTmpLen);
                            snprintf(szTmp, nTmpLen, pszMDSubPrefix, iIter + 1);
                            pszMDNewPrefix = CPLStrdup(
                                CPLSPrintf("%s%s", pszMDPrefix, szTmp));
                            CPLFree(szTmp);
                        }
                        else
                            pszMDNewPrefix = CPLStrdup(
                                CPLSPrintf("%s%s%04d_", pszMDPrefix,
                                           pszMDSubPrefix, iIter + 1));
                    }
                    else
                        pszMDNewPrefix = CPLStrdup(
                            CPLSPrintf("%s%04d_", pszMDPrefix, iIter + 1));

                    if (psRepeatedNode != NULL)
                    {
                        CPLXMLNode *psIndexNode;
                        psGroupNode =
                            CPLCreateXMLNode(NULL, CXT_Element, "group");
                        CPLAssert(psLastChild->psNext == NULL);
                        psLastChild->psNext = psGroupNode;
                        psLastChild = psGroupNode;
                        psIndexNode = CPLCreateXMLNode(psGroupNode,
                                                       CXT_Attribute, "index");
                        CPLCreateXMLNode(psIndexNode, CXT_Text,
                                         CPLSPrintf("%d", iIter));
                    }

                    papszMD = NITFGenericMetadataReadTREInternal(
                        papszMD, pnMDSize, pnMDAlloc, psGroupNode,
                        pszDESOrTREKind, pszDESOrTREName, pachTRE, nTRESize,
                        psIter, pnTreOffset, pszMDNewPrefix, bValidate,
                        pbError);
                    CPLFree(pszMDNewPrefix);
                }
            }
        }
        else if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
                 strcmp(psIter->pszValue, "if") == 0)
        {
            const char *pszCond = CPLGetXMLValue(psIter, "cond", NULL);
            if (pszCond == NULL)
            {
                CPLError(bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                         "Invalid if construct in %s %s in XML resource : "
                         "missing 'cond' attribute",
                         pszDESOrTREName, pszDESOrTREKind);
                *pbError = TRUE;
                break;
            }

            int nRet = NITFEvaluateCond(pszCond, papszMD, pnMDSize, pszMDPrefix,
                                        pszDESOrTREKind, pszDESOrTREName);
            if (nRet < 0)
            {
                *pbError = TRUE;
                break;
            }
            if (nRet > 0)
            {
                papszMD = NITFGenericMetadataReadTREInternal(
                    papszMD, pnMDSize, pnMDAlloc, psOutXMLNode, pszDESOrTREKind,
                    pszDESOrTREName, pachTRE, nTRESize, psIter, pnTreOffset,
                    pszMDPrefix, bValidate, pbError);
            }
        }
        else if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
                 strcmp(psIter->pszValue, "if_remaining_bytes") == 0)
        {
            if (*pnTreOffset < nTRESize)
            {
                papszMD = NITFGenericMetadataReadTREInternal(
                    papszMD, pnMDSize, pnMDAlloc, psOutXMLNode, pszDESOrTREKind,
                    pszDESOrTREName, pachTRE, nTRESize, psIter, pnTreOffset,
                    pszMDPrefix, bValidate, pbError);
            }
        }
        else
        {
            // CPLDebug("NITF", "Unknown element : %s", psIter->pszValue ?
            // psIter->pszValue : "null");
        }
    }
    return papszMD;
}

/************************************************************************/
/*                      NITFGenericMetadataReadTRE()                    */
/************************************************************************/

static char **NITFGenericMetadataReadTRE(char **papszMD, const char *pszTREName,
                                         const char *pachTRE, int nTRESize,
                                         CPLXMLNode *psTreNode)
{
    int bError = FALSE;
    int nTreOffset = 0;
    const char *pszMDPrefix;
    int nMDSize, nMDAlloc;

    int nTreLength = atoi(CPLGetXMLValue(psTreNode, "length", "-1"));
    int nTreMinLength = atoi(CPLGetXMLValue(psTreNode, "minlength", "-1"));
    /* int nTreMaxLength = atoi(CPLGetXMLValue(psTreNode, "maxlength", "-1"));
     */

    if (nTreLength > 0 && nTRESize != nTreLength)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "%s TRE wrong size (%d). Expected %d.", pszTREName, nTRESize,
                 nTreLength);
    }

    if (nTreMinLength > 0 && nTRESize < nTreMinLength)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "%s TRE wrong size (%d). Expected >= %d.", pszTREName,
                 nTRESize, nTreMinLength);
    }

    pszMDPrefix = CPLGetXMLValue(psTreNode, "md_prefix", "");

    nMDSize = nMDAlloc = CSLCount(papszMD);

    papszMD = NITFGenericMetadataReadTREInternal(
        papszMD, &nMDSize, &nMDAlloc, NULL, "TRE", pszTREName, pachTRE,
        nTRESize, psTreNode, &nTreOffset, pszMDPrefix,
        false,  // bValidate
        &bError);

    if (bError == FALSE && nTreLength > 0 && nTreOffset != nTreLength)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Inconsistent declaration of %s TRE", pszTREName);
    }
    if (nTreOffset < nTRESize)
        CPLDebug("NITF", "%d remaining bytes at end of %s TRE",
                 nTRESize - nTreOffset, pszTREName);

    return papszMD;
}

/************************************************************************/
/*                           NITFLoadXMLSpec()                          */
/************************************************************************/

#define NITF_SPEC_FILE "nitf_spec.xml"

static CPLXMLNode *NITFLoadXMLSpec(NITFFile *psFile)
{

    if (psFile->psNITFSpecNode == NULL)
    {
#ifndef USE_ONLY_EMBEDDED_RESOURCE_FILES
#ifdef EMBED_RESOURCE_FILES
        CPLPushErrorHandler(CPLQuietErrorHandler);
#endif
        const char *pszXMLDescFilename = CPLFindFile("gdal", NITF_SPEC_FILE);
#ifdef EMBED_RESOURCE_FILES
        CPLPopErrorHandler();
        CPLErrorReset();
#endif
        if (pszXMLDescFilename == NULL)
#endif
        {
#ifdef EMBED_RESOURCE_FILES
            CPLDebug("NITF", "Using embedded %s", NITF_SPEC_FILE);
            psFile->psNITFSpecNode = CPLParseXMLString(NITFGetSpecFile());
            CPLAssert(psFile->psNITFSpecNode);
            return psFile->psNITFSpecNode;
#else
            CPLDebug("NITF", "Cannot find XML file : %s", NITF_SPEC_FILE);
            return NULL;
#endif
        }
#ifndef USE_ONLY_EMBEDDED_RESOURCE_FILES
        psFile->psNITFSpecNode = CPLParseXMLFile(pszXMLDescFilename);
        if (psFile->psNITFSpecNode == NULL)
        {
            CPLDebug("NITF", "Invalid XML file : %s", pszXMLDescFilename);
            return NULL;
        }
#endif
    }

    return psFile->psNITFSpecNode;
}

/************************************************************************/
/*                      NITFFindTREXMLDescFromName()                    */
/************************************************************************/

static CPLXMLNode *NITFFindTREXMLDescFromName(NITFFile *psFile,
                                              const char *pszTREName)
{
    CPLXMLNode *psTreeNode;
    CPLXMLNode *psTresNode;
    CPLXMLNode *psIter;

    psTreeNode = NITFLoadXMLSpec(psFile);
    if (psTreeNode == NULL)
        return NULL;

    psTresNode = CPLGetXMLNode(psTreeNode, "=root.tres");
    if (psTresNode == NULL)
    {
        CPLDebug("NITF", "Cannot find <root><tres> root element");
        return NULL;
    }

    for (psIter = psTresNode->psChild; psIter != NULL; psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
            strcmp(psIter->pszValue, "tre") == 0)
        {
            const char *pszName = CPLGetXMLValue(psIter, "name", NULL);
            if (pszName != NULL && strcmp(pszName, pszTREName) == 0)
            {
                return psIter;
            }
        }
    }

    return NULL;
}

/************************************************************************/
/*                         NITFCreateXMLTre()                           */
/************************************************************************/

CPLXMLNode *NITFCreateXMLTre(NITFFile *psFile, const char *pszTREName,
                             const char *pachTRE, int nTRESize, bool bValidate,
                             bool *pbGotError)
{
    int nTreLength, nTreMinLength = -1 /* , nTreMaxLength = -1 */;
    int bError = FALSE;
    int nTreOffset = 0;
    CPLXMLNode *psTreNode;
    CPLXMLNode *psOutXMLNode = NULL;
    int nMDSize = 0, nMDAlloc = 0;
    const char *pszMDPrefix;

    psTreNode = NITFFindTREXMLDescFromName(psFile, pszTREName);
    if (psTreNode == NULL)
    {
        if (!(STARTS_WITH_CI(pszTREName, "RPF") ||
              strcmp(pszTREName, "XXXXXX") == 0))
        {
            CPLDebug("NITF", "Cannot find definition of TRE %s in %s",
                     pszTREName, NITF_SPEC_FILE);
        }
        return NULL;
    }

    nTreLength = atoi(CPLGetXMLValue(psTreNode, "length", "-1"));
    nTreMinLength = atoi(CPLGetXMLValue(psTreNode, "minlength", "-1"));
    /* nTreMaxLength = atoi(CPLGetXMLValue(psTreNode, "maxlength", "-1")); */

    psOutXMLNode = CPLCreateXMLNode(NULL, CXT_Element, "tre");
    CPLCreateXMLNode(CPLCreateXMLNode(psOutXMLNode, CXT_Attribute, "name"),
                     CXT_Text, pszTREName);

    if (nTreLength > 0 && nTRESize != nTreLength)
    {
        CPLError(bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                 "%s TRE wrong size (%d). Expected %d.", pszTREName, nTRESize,
                 nTreLength);
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf("%s TRE wrong size (%d). Expected %d.", pszTREName,
                       nTRESize, nTreLength));
        if (pbGotError)
            *pbGotError = true;
    }

    if (nTreMinLength > 0 && nTRESize < nTreMinLength)
    {
        CPLError(bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                 "%s TRE wrong size (%d). Expected >= %d.", pszTREName,
                 nTRESize, nTreMinLength);
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf("%s TRE wrong size (%d). Expected >= %d.", pszTREName,
                       nTRESize, nTreMinLength));
        if (pbGotError)
            *pbGotError = true;
    }

    pszMDPrefix = CPLGetXMLValue(psTreNode, "md_prefix", "");
    CSLDestroy(NITFGenericMetadataReadTREInternal(
        NULL, &nMDSize, &nMDAlloc, psOutXMLNode, "TRE", pszTREName, pachTRE,
        nTRESize, psTreNode, &nTreOffset, pszMDPrefix, bValidate, &bError));

    if (bError == FALSE && nTreLength > 0 && nTreOffset != nTreLength)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Inconsistent declaration of %s TRE", pszTREName);
    }
    if (nTreOffset < nTRESize)
    {
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf("%d remaining bytes at end of %s TRE",
                       nTRESize - nTreOffset, pszTREName));
    }
    if (pbGotError && bError)
        *pbGotError = true;

    return psOutXMLNode;
}

/************************************************************************/
/*                      NITFFindTREXMLDescFromName()                    */
/************************************************************************/

static CPLXMLNode *NITFFindDESXMLDescFromName(NITFFile *psFile,
                                              const char *pszDESName)
{
    CPLXMLNode *psTreeNode;
    CPLXMLNode *psTresNode;
    CPLXMLNode *psIter;

    psTreeNode = NITFLoadXMLSpec(psFile);
    if (psTreeNode == NULL)
        return NULL;

    psTresNode = CPLGetXMLNode(psTreeNode, "=root.des_list");
    if (psTresNode == NULL)
    {
        CPLDebug("NITF", "Cannot find <root><des_list> root element");
        return NULL;
    }

    for (psIter = psTresNode->psChild; psIter != NULL; psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
            strcmp(psIter->pszValue, "des") == 0)
        {
            const char *pszName = CPLGetXMLValue(psIter, "name", NULL);
            if (pszName != NULL && strcmp(pszName, pszDESName) == 0)
            {
                return psIter;
            }
        }
    }

    return NULL;
}

/************************************************************************/
/*                 NITFCreateXMLDesUserDefinedSubHeader()               */
/************************************************************************/

CPLXMLNode *NITFCreateXMLDesUserDefinedSubHeader(NITFFile *psFile,
                                                 const NITFDES *psDES,
                                                 bool bValidate,
                                                 bool *pbGotError)
{
    const char *pszDESID = CSLFetchNameValue(psDES->papszMetadata, "DESID");
    CPLXMLNode *psDESDef = NITFFindDESXMLDescFromName(psFile, pszDESID);
    if (psDESDef == NULL)
    {
        CPLDebug("NITF", "Cannot find definition of DES %s in %s", pszDESID,
                 NITF_SPEC_FILE);
        return NULL;
    }
    CPLXMLNode *psUserDefinedFields =
        CPLGetXMLNode(psDESDef, "subheader_fields");
    if (psUserDefinedFields == NULL)
    {
        return NULL;
    }

    CPLXMLNode *psOutXMLNode =
        CPLCreateXMLNode(NULL, CXT_Element, "user_defined_fields");

    int bError = FALSE;
    int nOffset = 200;
    char **papszMD = CSLDuplicate(psDES->papszMetadata);
    int nMDSize = CSLCount(papszMD);
    int nMDAlloc = nMDSize;
    const int nDESSize =
        psFile->pasSegmentInfo[psDES->iSegment].nSegmentHeaderSize;
    CSLDestroy(NITFGenericMetadataReadTREInternal(
        papszMD, &nMDSize, &nMDAlloc, psOutXMLNode, "DES", pszDESID,
        psDES->pachHeader, nDESSize, psUserDefinedFields, &nOffset,
        "", /* pszMDPrefix, */
        bValidate, &bError));
    int nDESSHL =
        atoi(CSLFetchNameValueDef(psDES->papszMetadata, "DESSHL", "0"));

    const int nLength =
        atoi(CPLGetXMLValue(psUserDefinedFields, "length", "-1"));
    const int nMinLength =
        atoi(CPLGetXMLValue(psUserDefinedFields, "minlength", "-1"));

    if (nLength > 0 && nDESSHL != nLength)
    {
        CPLError(bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                 "%s DES wrong header size (%d). Expected %d.", pszDESID,
                 nDESSHL, nLength);
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf("%s DES wrong size (%d). Expected %d.", pszDESID,
                       nDESSHL, nLength));
        if (pbGotError)
            *pbGotError = true;
    }

    if (nMinLength > 0 && nDESSHL < nMinLength)
    {
        CPLError(bValidate ? CE_Failure : CE_Warning, CPLE_AppDefined,
                 "%s DES wrong size (%d). Expected >= %d.", pszDESID, nDESSHL,
                 nMinLength);
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf("%s DES wrong size (%d). Expected >= %d.", pszDESID,
                       nDESSHL, nMinLength));
        if (pbGotError)
            *pbGotError = true;
    }

    if (nOffset < nDESSHL)
    {
        bError = TRUE;
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf(
                "%d remaining bytes at end of user defined subheader section",
                nDESSHL - nOffset));
    }
    if (pbGotError && bError)
        *pbGotError = true;

    return psOutXMLNode;
}

/************************************************************************/
/*                   NITFCreateXMLDesDataFields()                       */
/************************************************************************/

CPLXMLNode *NITFCreateXMLDesDataFields(NITFFile *psFile, const NITFDES *psDES,
                                       const GByte *pabyData, int nDataLen,
                                       bool bValidate, bool *pbGotError)
{
    const char *pszDESID = CSLFetchNameValue(psDES->papszMetadata, "DESID");
    CPLXMLNode *psDESDef = NITFFindDESXMLDescFromName(psFile, pszDESID);
    if (psDESDef == NULL)
    {
        CPLDebug("NITF", "Cannot find definition of DES %s in %s", pszDESID,
                 NITF_SPEC_FILE);
        return NULL;
    }
    CPLXMLNode *psFields = CPLGetXMLNode(psDESDef, "data_fields");
    if (psFields == NULL)
    {
        return NULL;
    }

    CPLXMLNode *psOutXMLNode =
        CPLCreateXMLNode(NULL, CXT_Element, "data_fields");

    int bError = FALSE;
    int nOffset = 0;
    char **papszMD = CSLDuplicate(psDES->papszMetadata);
    int nMDSize = CSLCount(papszMD);
    int nMDAlloc = nMDSize;
    CSLDestroy(NITFGenericMetadataReadTREInternal(
        papszMD, &nMDSize, &nMDAlloc, psOutXMLNode, "DES", pszDESID,
        (const char *)pabyData, nDataLen, psFields, &nOffset,
        "", /* pszMDPrefix, */
        bValidate, &bError));
    if (nOffset < nDataLen)
    {
        bError = TRUE;
        CPLCreateXMLElementAndValue(
            psOutXMLNode, bValidate ? "error" : "warning",
            CPLSPrintf("%d remaining bytes at end of data section",
                       nDataLen - nOffset));
    }
    if (pbGotError && bError)
        *pbGotError = true;

    return psOutXMLNode;
}

/************************************************************************/
/*                        NITFGenericMetadataRead()                     */
/*                                                                      */
/* Add metadata from TREs of file and image objects in the papszMD list */
/* pszSpecificTRE can be NULL, in which case all TREs listed in         */
/* data/nitf_resources.xml that have md_prefix defined will be looked   */
/* for. If not NULL, only the specified one will be looked for.         */
/************************************************************************/

char **NITFGenericMetadataRead(char **papszMD, NITFFile *psFile,
                               NITFImage *psImage,
                               const char *pszSpecificTREName)
{
    CPLXMLNode *psTreeNode = NULL;
    CPLXMLNode *psTresNode = NULL;
    CPLXMLNode *psIter = NULL;

    if (psFile == NULL)
    {
        if (psImage == NULL)
            return papszMD;
        psTreeNode = NITFLoadXMLSpec(psImage->psFile);
    }
    else
        psTreeNode = NITFLoadXMLSpec(psFile);

    if (psTreeNode == NULL)
        return papszMD;

    psTresNode = CPLGetXMLNode(psTreeNode, "=root.tres");
    if (psTresNode == NULL)
    {
        CPLDebug("NITF", "Cannot find <root><tres> root element");
        return papszMD;
    }

    for (psIter = psTresNode->psChild; psIter != NULL; psIter = psIter->psNext)
    {
        if (psIter->eType == CXT_Element && psIter->pszValue != NULL &&
            strcmp(psIter->pszValue, "tre") == 0)
        {
            const char *pszName = CPLGetXMLValue(psIter, "name", NULL);
            const char *pszMDPrefix = CPLGetXMLValue(psIter, "md_prefix", NULL);
            int bHasRightPrefix = FALSE;
            if (pszName == NULL)
                continue;
            if (pszSpecificTREName == NULL)
                bHasRightPrefix = (pszMDPrefix != NULL);
            else
                bHasRightPrefix = (strcmp(pszName, pszSpecificTREName) == 0);
            if (bHasRightPrefix)
            {
                if (psFile != NULL)
                {
                    const char *pachTRE = NULL;
                    int nTRESize = 0;

                    pachTRE = NITFFindTRE(psFile->pachTRE, psFile->nTREBytes,
                                          pszName, &nTRESize);
                    if (pachTRE != NULL)
                        papszMD = NITFGenericMetadataReadTRE(
                            papszMD, pszName, pachTRE, nTRESize, psIter);
                }
                if (psImage != NULL)
                {
                    const char *pachTRE = NULL;
                    int nTRESize = 0;

                    pachTRE = NITFFindTRE(psImage->pachTRE, psImage->nTREBytes,
                                          pszName, &nTRESize);
                    if (pachTRE != NULL)
                        papszMD = NITFGenericMetadataReadTRE(
                            papszMD, pszName, pachTRE, nTRESize, psIter);
                }
                if (pszSpecificTREName)
                    break;
            }
        }
    }

    return papszMD;
}
