/******************************************************************************
 *
 * Project:  DXF Translator
 * Purpose:  Implements OGRDXFWriterDS - the OGRDataSource class used for
 *           writing a DXF file.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2009, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2010-2012, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"

#include <cmath>
#include <cstdlib>
#include <limits>

#include "ogr_dxf.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_vsi_error.h"

#ifdef EMBED_RESOURCE_FILES
#include "embedded_resources.h"
#endif

/************************************************************************/
/*                          OGRDXFWriterDS()                          */
/************************************************************************/

OGRDXFWriterDS::OGRDXFWriterDS()
    : nNextFID(80), poLayer(nullptr), poBlocksLayer(nullptr), fp(nullptr),
      fpTemp(nullptr), papszLayersToCreate(nullptr), nHANDSEEDOffset(0)
{
}

/************************************************************************/
/*                         ~OGRDXFWriterDS()                          */
/************************************************************************/

OGRDXFWriterDS::~OGRDXFWriterDS()

{
    if (fp != nullptr)
    {
        /* --------------------------------------------------------------------
         */
        /*      Transfer over the header into the destination file with any */
        /*      adjustments or insertions needed. */
        /* --------------------------------------------------------------------
         */
        CPLDebug("DXF", "Compose final DXF file from components.");

        if (IsMarkedSuppressOnClose() && fpTemp != nullptr)
        {
            CPLDebug("DXF", "Do not copy final DXF when 'suppress on close'.");
            VSIFCloseL(fpTemp);
            VSIUnlink(osTempFilename);
            fpTemp = nullptr;
        }

        TransferUpdateHeader(fp);

        if (fpTemp != nullptr)
        {
            /* --------------------------------------------------------------------
             */
            /*      Copy in the temporary file contents. */
            /* --------------------------------------------------------------------
             */
            VSIFCloseL(fpTemp);
            fpTemp = VSIFOpenL(osTempFilename, "r");

            const char *pszLine = nullptr;
            while ((pszLine = CPLReadLineL(fpTemp)) != nullptr)
            {
                VSIFWriteL(pszLine, 1, strlen(pszLine), fp);
                VSIFWriteL("\n", 1, 1, fp);
            }

            /* --------------------------------------------------------------------
             */
            /*      Cleanup temporary file. */
            /* --------------------------------------------------------------------
             */
            VSIFCloseL(fpTemp);
            VSIUnlink(osTempFilename);
        }

        /* --------------------------------------------------------------------
         */
        /*      Write trailer. */
        /* --------------------------------------------------------------------
         */
        if (osTrailerFile != "")
            TransferUpdateTrailer(fp);

        /* --------------------------------------------------------------------
         */
        /*      Fixup the HANDSEED value now that we know all the entity ids */
        /*      created. */
        /* --------------------------------------------------------------------
         */
        FixupHANDSEED(fp);

        /* --------------------------------------------------------------------
         */
        /*      Close file. */
        /* --------------------------------------------------------------------
         */

        VSIFCloseL(fp);
        fp = nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Destroy layers.                                                 */
    /* -------------------------------------------------------------------- */
    delete poLayer;
    delete poBlocksLayer;

    CSLDestroy(papszLayersToCreate);

    if (m_bHeaderFileIsTemp)
        VSIUnlink(osHeaderFile.c_str());
    if (m_bTrailerFileIsTemp)
        VSIUnlink(osTrailerFile.c_str());
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRDXFWriterDS::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer))
        // Unable to have more than one OGR entities layer in a DXF file, with
        // one options blocks layer.
        return poBlocksLayer == nullptr || poLayer == nullptr;
    else
        return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRDXFWriterDS::GetLayer(int iLayer)

{
    if (iLayer == 0)
        return poLayer;
    else
        return nullptr;
}

/************************************************************************/
/*                           GetLayerCount()                            */
/************************************************************************/

int OGRDXFWriterDS::GetLayerCount()

{
    if (poLayer)
        return 1;
    else
        return 0;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRDXFWriterDS::Open(const char *pszFilename, char **papszOptions)

{
    /* -------------------------------------------------------------------- */
    /*      Open the standard header, or a user provided header.            */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszOptions, "HEADER") != nullptr)
        osHeaderFile = CSLFetchNameValue(papszOptions, "HEADER");
    else
    {
#ifdef EMBED_RESOURCE_FILES
        CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
#endif
#ifdef USE_ONLY_EMBEDDED_RESOURCE_FILES
        const char *pszValue = nullptr;
#else
        const char *pszValue = CPLFindFile("gdal", "header.dxf");
        if (pszValue == nullptr)
#endif
        {
#ifdef EMBED_RESOURCE_FILES
            static const bool bOnce [[maybe_unused]] = []()
            {
                CPLDebug("DXF", "Using embedded header.dxf");
                return true;
            }();
            pszValue = VSIMemGenerateHiddenFilename("header.dxf");
            VSIFCloseL(VSIFileFromMemBuffer(
                pszValue,
                const_cast<GByte *>(
                    reinterpret_cast<const GByte *>(OGRDXFGetHEADER())),
                static_cast<int>(strlen(OGRDXFGetHEADER())),
                /* bTakeOwnership = */ false));
            m_bHeaderFileIsTemp = true;
#else
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to find template header file header.dxf for "
                     "reading,\nis GDAL_DATA set properly?");
            return FALSE;
#endif
        }
        osHeaderFile = pszValue;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish the name for our trailer file.                        */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszOptions, "TRAILER") != nullptr)
        osTrailerFile = CSLFetchNameValue(papszOptions, "TRAILER");
    else
    {
#ifdef EMBED_RESOURCE_FILES
        CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
#endif
#ifdef USE_ONLY_EMBEDDED_RESOURCE_FILES
        const char *pszValue = nullptr;
#else
        const char *pszValue = CPLFindFile("gdal", "trailer.dxf");
        if (pszValue != nullptr)
            osTrailerFile = pszValue;
#endif
#ifdef EMBED_RESOURCE_FILES
        if (!pszValue)
        {
            static const bool bOnce [[maybe_unused]] = []()
            {
                CPLDebug("DXF", "Using embedded trailer.dxf");
                return true;
            }();
            osTrailerFile = VSIMemGenerateHiddenFilename("trailer.dxf");
            m_bTrailerFileIsTemp = false;
            VSIFCloseL(VSIFileFromMemBuffer(
                osTrailerFile.c_str(),
                const_cast<GByte *>(
                    reinterpret_cast<const GByte *>(OGRDXFGetTRAILER())),
                static_cast<int>(strlen(OGRDXFGetTRAILER())),
                /* bTakeOwnership = */ false));
        }
#endif
    }

/* -------------------------------------------------------------------- */
/*      What entity id do we want to start with when writing?  Small    */
/*      values run a risk of overlapping some undetected entity in      */
/*      the header or trailer despite the prescanning we do.            */
/* -------------------------------------------------------------------- */
#ifdef DEBUG
    nNextFID = 80;
#else
    nNextFID = 131072;
#endif

    if (CSLFetchNameValue(papszOptions, "FIRST_ENTITY") != nullptr)
        nNextFID = atoi(CSLFetchNameValue(papszOptions, "FIRST_ENTITY"));

    m_osINSUNITS =
        CSLFetchNameValueDef(papszOptions, "INSUNITS", m_osINSUNITS.c_str());
    m_osMEASUREMENT = CSLFetchNameValueDef(papszOptions, "MEASUREMENT",
                                           m_osMEASUREMENT.c_str());

    /* -------------------------------------------------------------------- */
    /*      Prescan the header and trailer for entity codes.                */
    /* -------------------------------------------------------------------- */
    ScanForEntities(osHeaderFile, "HEADER");
    ScanForEntities(osTrailerFile, "TRAILER");

    /* -------------------------------------------------------------------- */
    /*      Attempt to read the template header file so we have a list      */
    /*      of layers, linestyles and blocks.                               */
    /* -------------------------------------------------------------------- */
    if (!oHeaderDS.Open(osHeaderFile, nullptr, true, nullptr))
        return FALSE;

    /* -------------------------------------------------------------------- */
    /*      Create the output file.                                         */
    /* -------------------------------------------------------------------- */
    fp = VSIFOpenExL(pszFilename, "w+", true);

    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open '%s' for writing: %s", pszFilename,
                 VSIGetLastErrorMsg());
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish the temporary file.                                   */
    /* -------------------------------------------------------------------- */
    osTempFilename = pszFilename;
    osTempFilename += ".tmp";

    fpTemp = VSIFOpenL(osTempFilename, "w");
    if (fpTemp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open '%s' for writing.", osTempFilename.c_str());
        return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *OGRDXFWriterDS::ICreateLayer(const char *pszName,
                                       const OGRGeomFieldDefn *poGeomFieldDefn,
                                       CSLConstList /*papszOptions*/)

{
    if (poGeomFieldDefn)
    {
        const auto poSRS = poGeomFieldDefn->GetSpatialRef();
        if (poSRS)
            m_oSRS = *poSRS;
    }
    if (EQUAL(pszName, "blocks") && poBlocksLayer == nullptr)
    {
        poBlocksLayer = new OGRDXFBlocksWriterLayer(this);
        return poBlocksLayer;
    }
    else if (poLayer == nullptr)
    {
        poLayer = new OGRDXFWriterLayer(this, fpTemp);
        return poLayer;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to have more than one OGR entities layer in a DXF "
                 "file, with one options blocks layer.");
        return nullptr;
    }
}

/************************************************************************/
/*                             WriteValue()                             */
/************************************************************************/

static bool WriteValue(VSILFILE *fp, int nCode, const char *pszLine)

{
    char szLinePair[300];

    snprintf(szLinePair, sizeof(szLinePair), "%3d\n%s\n", nCode, pszLine);
    size_t nLen = strlen(szLinePair);
    if (VSIFWriteL(szLinePair, 1, nLen, fp) != nLen)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Attempt to write line to DXF file failed, disk full?.");
        return false;
    }

    return true;
}

/************************************************************************/
/*                             WriteValue()                             */
/************************************************************************/

static bool WriteValue(VSILFILE *fp, int nCode, double dfValue)

{
    char szLinePair[64];

    CPLsnprintf(szLinePair, sizeof(szLinePair), "%3d\n%.15g\n", nCode, dfValue);
    size_t nLen = strlen(szLinePair);
    if (VSIFWriteL(szLinePair, 1, nLen, fp) != nLen)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Attempt to write line to DXF file failed, disk full?.");
        return false;
    }

    return true;
}

/************************************************************************/
/*                        TransferUpdateHeader()                        */
/************************************************************************/

bool OGRDXFWriterDS::TransferUpdateHeader(VSILFILE *fpOut)

{
    oHeaderDS.ResetReadPointer(0);

    // We don't like non-finite extents. In this case, just write a generic
    // bounding box. Most CAD programs probably ignore this anyway.
    if (!std::isfinite(oGlobalEnvelope.MinX) ||
        !std::isfinite(oGlobalEnvelope.MinY) ||
        !std::isfinite(oGlobalEnvelope.MaxX) ||
        !std::isfinite(oGlobalEnvelope.MaxY))
    {
        oGlobalEnvelope.MinX = 0.0;
        oGlobalEnvelope.MinY = 0.0;
        oGlobalEnvelope.MaxX = 10.0;
        oGlobalEnvelope.MaxY = 10.0;
    }

    /* -------------------------------------------------------------------- */
    /*      Copy header, inserting in new objects as needed.                */
    /* -------------------------------------------------------------------- */
    char szLineBuf[257];
    int nCode = 0;
    CPLString osSection;
    CPLString osTable;
    CPLString osEntity;

    while ((nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf))) != -1 &&
           osSection != "ENTITIES")
    {
        if (nCode == 0 && EQUAL(szLineBuf, "ENDTAB"))
        {
            // If we are at the end of the LAYER TABLE consider inserting
            // missing definitions.
            if (osTable == "LAYER")
            {
                if (!WriteNewLayerDefinitions(fp))
                    return false;
            }

            // If at the end of the BLOCK_RECORD TABLE consider inserting
            // missing definitions.
            if (osTable == "BLOCK_RECORD" && poBlocksLayer)
            {
                if (!WriteNewBlockRecords(fp))
                    return false;
            }

            // If at the end of the LTYPE TABLE consider inserting
            // missing layer type definitions.
            if (osTable == "LTYPE")
            {
                if (!WriteNewLineTypeRecords(fp))
                    return false;
            }

            // If at the end of the STYLE TABLE consider inserting
            // missing layer type definitions.
            if (osTable == "STYLE")
            {
                if (!WriteNewTextStyleRecords(fp))
                    return false;
            }

            osTable = "";
        }

        // If we are at the end of the BLOCKS section, consider inserting
        // supplementary blocks.
        if (nCode == 0 && osSection == "BLOCKS" && EQUAL(szLineBuf, "ENDSEC") &&
            poBlocksLayer != nullptr)
        {
            if (!WriteNewBlockDefinitions(fp))
                return false;
        }

        // We need to keep track of where $HANDSEED is so that we can
        // come back and fix it up when we have generated all entity ids.
        if (nCode == 9 && EQUAL(szLineBuf, "$HANDSEED"))
        {
            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;

            nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));

            // ensure we have room to overwrite with a longer value.
            while (strlen(szLineBuf) < 8)
            {
                memmove(szLineBuf + 1, szLineBuf, strlen(szLineBuf) + 1);
                szLineBuf[0] = '0';
            }

            nHANDSEEDOffset = VSIFTellL(fpOut);
        }

        // Patch EXTMIN with minx and miny
        if (nCode == 9 && EQUAL(szLineBuf, "$EXTMIN"))
        {
            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;

            nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));
            if (nCode == 10)
            {
                if (!WriteValue(fpOut, nCode, oGlobalEnvelope.MinX))
                    return false;

                nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));
                if (nCode == 20)
                {
                    if (!WriteValue(fpOut, nCode, oGlobalEnvelope.MinY))
                        return false;

                    continue;
                }
            }
        }

        // Patch EXTMAX with maxx and maxy
        if (nCode == 9 && EQUAL(szLineBuf, "$EXTMAX"))
        {
            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;

            nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));
            if (nCode == 10)
            {
                if (!WriteValue(fpOut, nCode, oGlobalEnvelope.MaxX))
                    return false;

                nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));
                if (nCode == 20)
                {
                    if (!WriteValue(fpOut, nCode, oGlobalEnvelope.MaxY))
                        return false;

                    continue;
                }
            }
        }

        // Patch INSUNITS
        if (nCode == 9 && EQUAL(szLineBuf, "$INSUNITS") &&
            m_osINSUNITS != "HEADER_VALUE")
        {
            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;
            nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));
            if (nCode == 70)
            {
                int nVal = -1;
                if (m_osINSUNITS == "AUTO" && m_oSRS.IsProjected())
                {
                    const char *pszUnits = nullptr;
                    const double dfUnits = m_oSRS.GetLinearUnits(&pszUnits);
                    const auto IsAlmostEqual = [](double x, double y)
                    { return std::fabs(x - y) <= 1e-10; };
                    if (IsAlmostEqual(dfUnits, 1)) /* METERS */
                    {
                        nVal = 6;
                    }
                    else if (IsAlmostEqual(dfUnits, CPLAtof(SRS_UL_FOOT_CONV)))
                    {
                        nVal = 2;
                    }
                    else if (IsAlmostEqual(dfUnits,
                                           CPLAtof(SRS_UL_US_FOOT_CONV)))
                    {
                        nVal = 21;
                    }
                    else
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Could not translate CRS unit %s to "
                                 "$INSUNIT. Using default value from "
                                 "template header file",
                                 pszUnits);
                    }
                }
                else if (m_osINSUNITS != "AUTO")
                {
                    static const struct
                    {
                        const char *pszValue;
                        int nValue;
                    } INSUNITSMap[] = {
                        {"UNITLESS", 0},
                        {"INCHES", 1},
                        {"FEET", 2},
                        {"MILLIMETERS", 4},
                        {"CENTIMETERS", 5},
                        {"METERS", 6},
                        {"US_SURVEY_FEET", 21},
                    };

                    for (const auto &sTuple : INSUNITSMap)
                    {
                        if (m_osINSUNITS == sTuple.pszValue ||
                            m_osINSUNITS == CPLSPrintf("%d", sTuple.nValue))
                        {
                            nVal = sTuple.nValue;
                            break;
                        }
                    }
                    if (nVal < 0)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Could not translate $INSUNITS=%s. "
                                 "Using default value from template header "
                                 "file",
                                 m_osINSUNITS.c_str());
                    }
                }

                if (nVal >= 0)
                {
                    if (!WriteValue(fpOut, nCode, CPLSPrintf("%d", nVal)))
                        return false;

                    continue;
                }
            }
        }

        // Patch MEASUREMENT
        if (nCode == 9 && EQUAL(szLineBuf, "$MEASUREMENT") &&
            m_osMEASUREMENT != "HEADER_VALUE")
        {
            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;
            nCode = oHeaderDS.ReadValue(szLineBuf, sizeof(szLineBuf));
            if (nCode == 70)
            {
                int nVal = -1;

                static const struct
                {
                    const char *pszValue;
                    int nValue;
                } MEASUREMENTMap[] = {
                    {"IMPERIAL", 0},
                    {"METRIC", 1},
                };

                for (const auto &sTuple : MEASUREMENTMap)
                {
                    if (m_osMEASUREMENT == sTuple.pszValue ||
                        m_osMEASUREMENT == CPLSPrintf("%d", sTuple.nValue))
                    {
                        nVal = sTuple.nValue;
                        break;
                    }
                }
                if (nVal < 0)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Could not translate $MEASUREMENT=%s. "
                             "Using default value from template header file",
                             m_osMEASUREMENT.c_str());
                }
                else
                {
                    if (!WriteValue(fpOut, nCode, CPLSPrintf("%d", nVal)))
                        return false;

                    continue;
                }
            }
        }

        // Copy over the source line.
        if (!WriteValue(fpOut, nCode, szLineBuf))
            return false;

        // Track what entity we are in - that is the last "code 0" object.
        if (nCode == 0)
            osEntity = szLineBuf;

        // Track what section we are in.
        if (nCode == 0 && EQUAL(szLineBuf, "SECTION"))
        {
            nCode = oHeaderDS.ReadValue(szLineBuf);
            if (nCode == -1)
                break;

            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;

            osSection = szLineBuf;
        }

        // Track what TABLE we are in.
        if (nCode == 0 && EQUAL(szLineBuf, "TABLE"))
        {
            nCode = oHeaderDS.ReadValue(szLineBuf);
            if (!WriteValue(fpOut, nCode, szLineBuf))
                return false;

            osTable = szLineBuf;
        }

        // If we are starting the first layer, then capture
        // the layer contents while copying so we can duplicate
        // it for any new layer definitions.
        if (nCode == 0 && EQUAL(szLineBuf, "LAYER") && osTable == "LAYER" &&
            aosDefaultLayerText.empty())
        {
            do
            {
                anDefaultLayerCode.push_back(nCode);
                aosDefaultLayerText.push_back(szLineBuf);

                if (nCode != 0 && !WriteValue(fpOut, nCode, szLineBuf))
                    return false;

                nCode = oHeaderDS.ReadValue(szLineBuf);

                if (nCode == 2 && !EQUAL(szLineBuf, "0"))
                {
                    anDefaultLayerCode.resize(0);
                    aosDefaultLayerText.resize(0);
                    break;
                }
            } while (nCode != 0);

            oHeaderDS.UnreadValue();
        }
    }

    return true;
}

/************************************************************************/
/*                       TransferUpdateTrailer()                        */
/************************************************************************/

bool OGRDXFWriterDS::TransferUpdateTrailer(VSILFILE *fpOut)
{
    /* -------------------------------------------------------------------- */
    /*      Open the file and setup a reader.                               */
    /* -------------------------------------------------------------------- */
    VSILFILE *l_fp = VSIFOpenL(osTrailerFile, "r");

    if (l_fp == nullptr)
        return false;

    OGRDXFReaderASCII oReader;
    oReader.Initialize(l_fp);

    /* -------------------------------------------------------------------- */
    /*      Scan ahead to find the OBJECTS section.                         */
    /* -------------------------------------------------------------------- */
    char szLineBuf[257];
    int nCode = 0;

    while ((nCode = oReader.ReadValue(szLineBuf, sizeof(szLineBuf))) != -1)
    {
        if (nCode == 0 && EQUAL(szLineBuf, "SECTION"))
        {
            nCode = oReader.ReadValue(szLineBuf, sizeof(szLineBuf));
            if (nCode == 2 && EQUAL(szLineBuf, "OBJECTS"))
                break;
        }
    }

    if (nCode == -1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to find OBJECTS section in trailer file '%s'.",
                 osTrailerFile.c_str());
        return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Insert the "end of section" for ENTITIES, and the start of      */
    /*      the OBJECTS section.                                            */
    /* -------------------------------------------------------------------- */
    WriteValue(fpOut, 0, "ENDSEC");
    WriteValue(fpOut, 0, "SECTION");
    WriteValue(fpOut, 2, "OBJECTS");

    /* -------------------------------------------------------------------- */
    /*      Copy the remainder of the file.                                 */
    /* -------------------------------------------------------------------- */
    bool ret = true;
    while ((nCode = oReader.ReadValue(szLineBuf, sizeof(szLineBuf))) != -1)
    {
        if (!WriteValue(fpOut, nCode, szLineBuf))
        {
            ret = false;
            break;
        }
    }

    VSIFCloseL(l_fp);

    return ret;
}

/************************************************************************/
/*                           FixupHANDSEED()                            */
/*                                                                      */
/*      Fixup the next entity id information in the $HANDSEED header    */
/*      variable.                                                       */
/************************************************************************/

bool OGRDXFWriterDS::FixupHANDSEED(VSILFILE *fpIn)

{
    /* -------------------------------------------------------------------- */
    /*      What is a good next handle seed?  Scan existing values.         */
    /* -------------------------------------------------------------------- */
    unsigned int nHighestHandle = 0;
    std::set<CPLString>::iterator it;

    for (it = aosUsedEntities.begin(); it != aosUsedEntities.end(); ++it)
    {
        unsigned int nHandle = 0;
        if (sscanf((*it).c_str(), "%x", &nHandle) == 1)
        {
            if (nHandle > nHighestHandle)
                nHighestHandle = nHandle;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Read the existing handseed value, replace it, and write back.   */
    /* -------------------------------------------------------------------- */
    if (nHANDSEEDOffset == 0)
        return false;

    char szWorkBuf[30];
    VSIFSeekL(fpIn, nHANDSEEDOffset, SEEK_SET);
    VSIFReadL(szWorkBuf, 1, sizeof(szWorkBuf), fpIn);

    int i = 0;
    while (szWorkBuf[i] != '\n')
        i++;

    i++;
    if (szWorkBuf[i] == '\r')
        i++;

    CPLString osNewValue;

    osNewValue.Printf("%08X", nHighestHandle + 1);
    strncpy(szWorkBuf + i, osNewValue.c_str(), osNewValue.size());

    VSIFSeekL(fpIn, nHANDSEEDOffset, SEEK_SET);
    VSIFWriteL(szWorkBuf, 1, sizeof(szWorkBuf), fp);

    return true;
}

/************************************************************************/
/*                      WriteNewLayerDefinitions()                      */
/************************************************************************/

bool OGRDXFWriterDS::WriteNewLayerDefinitions(VSILFILE *fpOut)

{
    const int nNewLayers = CSLCount(papszLayersToCreate);

    for (int iLayer = 0; iLayer < nNewLayers; iLayer++)
    {
        bool bIsDefPoints = false;
        bool bWrote290 = false;
        for (unsigned i = 0; i < aosDefaultLayerText.size(); i++)
        {
            if (anDefaultLayerCode[i] == 2)
            {
                if (EQUAL(papszLayersToCreate[iLayer], "DEFPOINTS"))
                    bIsDefPoints = true;

                if (!WriteValue(fpOut, 2, papszLayersToCreate[iLayer]))
                    return false;
            }
            else if (anDefaultLayerCode[i] == 5)
            {
                unsigned int nIgnored;
                if (!WriteEntityID(fpOut, nIgnored))
                    return false;
            }
            else
            {
                if (anDefaultLayerCode[i] == 290)
                    bWrote290 = true;

                if (!WriteValue(fpOut, anDefaultLayerCode[i],
                                aosDefaultLayerText[i]))
                    return false;
            }
        }
        if (bIsDefPoints && !bWrote290)
        {
            // The Defpoints layer must be explicitly set to not plotted to
            // please Autocad. See https://trac.osgeo.org/gdal/ticket/7078
            if (!WriteValue(fpOut, 290, "0"))
                return false;
        }
    }

    return true;
}

/************************************************************************/
/*                      WriteNewLineTypeRecords()                       */
/************************************************************************/

bool OGRDXFWriterDS::WriteNewLineTypeRecords(VSILFILE *fpIn)

{
    if (poLayer == nullptr)
        return true;

    const std::map<CPLString, std::vector<double>> &oNewLineTypes =
        poLayer->GetNewLineTypeMap();

    bool bRet = true;
    for (const auto &oPair : oNewLineTypes)
    {
        bRet &= WriteValue(fpIn, 0, "LTYPE");
        unsigned int nIgnored;
        bRet &= WriteEntityID(fpIn, nIgnored);
        bRet &= WriteValue(fpIn, 100, "AcDbSymbolTableRecord");
        bRet &= WriteValue(fpIn, 100, "AcDbLinetypeTableRecord");
        bRet &= WriteValue(fpIn, 2, oPair.first);
        bRet &= WriteValue(fpIn, 70, "0");
        bRet &= WriteValue(fpIn, 3, "");
        bRet &= WriteValue(fpIn, 72, "65");
        bRet &= WriteValue(fpIn, 73, static_cast<int>(oPair.second.size()));

        double dfTotalLength = 0.0;
        for (const double &dfSegment : oPair.second)
            dfTotalLength += fabs(dfSegment);
        bRet &= WriteValue(fpIn, 40, dfTotalLength);

        for (const double &dfSegment : oPair.second)
        {
            bRet &= WriteValue(fpIn, 49, dfSegment);
            bRet &= WriteValue(fpIn, 74, "0");
        }
    }

    return bRet;
}

/************************************************************************/
/*                      WriteNewTextStyleRecords()                      */
/************************************************************************/

bool OGRDXFWriterDS::WriteNewTextStyleRecords(VSILFILE *fpIn)

{
    if (poLayer == nullptr)
        return true;

    auto &oNewTextStyles = poLayer->GetNewTextStyleMap();

    bool bRet = true;
    for (auto &oPair : oNewTextStyles)
    {
        bRet &= WriteValue(fpIn, 0, "STYLE");
        unsigned int nIgnored;
        bRet &= WriteEntityID(fpIn, nIgnored);
        bRet &= WriteValue(fpIn, 100, "AcDbSymbolTableRecord");
        bRet &= WriteValue(fpIn, 100, "AcDbTextStyleTableRecord");
        bRet &= WriteValue(fpIn, 2, oPair.first);
        bRet &= WriteValue(fpIn, 70, "0");
        bRet &= WriteValue(fpIn, 40, "0.0");

        if (oPair.second.count("Width"))
            bRet &= WriteValue(fpIn, 41, oPair.second["Width"]);
        else
            bRet &= WriteValue(fpIn, 41, "1.0");

        bRet &= WriteValue(fpIn, 50, "0.0");
        bRet &= WriteValue(fpIn, 71, "0");
        bRet &= WriteValue(fpIn, 1001, "ACAD");

        if (oPair.second.count("Font"))
            bRet &= WriteValue(fpIn, 1000, oPair.second["Font"]);

        int nStyleValue = 0;
        if (oPair.second.count("Italic") && oPair.second["Italic"] == "1")
            nStyleValue |= 0x1000000;
        if (oPair.second.count("Bold") && oPair.second["Bold"] == "1")
            nStyleValue |= 0x2000000;
        bRet &= WriteValue(fpIn, 1071,
                           CPLString().Printf("%d", nStyleValue).c_str());
    }

    return bRet;
}

/************************************************************************/
/*                        WriteNewBlockRecords()                        */
/************************************************************************/

bool OGRDXFWriterDS::WriteNewBlockRecords(VSILFILE *fpIn)

{
    std::set<CPLString> aosAlreadyHandled;

    /* ==================================================================== */
    /*      Loop over all block objects written via the blocks layer.       */
    /* ==================================================================== */
    bool bRet = true;
    for (size_t iBlock = 0; iBlock < poBlocksLayer->apoBlocks.size(); iBlock++)
    {
        OGRFeature *poThisBlockFeat = poBlocksLayer->apoBlocks[iBlock];

        /* --------------------------------------------------------------------
         */
        /*      Is this block already defined in the template header? */
        /* --------------------------------------------------------------------
         */
        CPLString osBlockName = poThisBlockFeat->GetFieldAsString("Block");

        if (oHeaderDS.LookupBlock(osBlockName) != nullptr)
            continue;

        /* --------------------------------------------------------------------
         */
        /*      Have we already written a BLOCK_RECORD for this block? */
        /* --------------------------------------------------------------------
         */
        if (aosAlreadyHandled.find(osBlockName) != aosAlreadyHandled.end())
            continue;

        aosAlreadyHandled.insert(std::move(osBlockName));

        /* --------------------------------------------------------------------
         */
        /*      Write the block record. */
        /* --------------------------------------------------------------------
         */
        bRet &= WriteValue(fpIn, 0, "BLOCK_RECORD");
        unsigned int nIgnored;
        bRet &= WriteEntityID(fpIn, nIgnored);
        bRet &= WriteValue(fpIn, 100, "AcDbSymbolTableRecord");
        bRet &= WriteValue(fpIn, 100, "AcDbBlockTableRecord");
        bRet &= WriteValue(fpIn, 2, poThisBlockFeat->GetFieldAsString("Block"));
        bRet &= WriteValue(fpIn, 340, "0");
    }

    return bRet;
}

/************************************************************************/
/*                      WriteNewBlockDefinitions()                      */
/************************************************************************/

bool OGRDXFWriterDS::WriteNewBlockDefinitions(VSILFILE *fpIn)

{
    if (poLayer == nullptr)
        poLayer = new OGRDXFWriterLayer(this, fpTemp);
    poLayer->ResetFP(fpIn);

    /* ==================================================================== */
    /*      Loop over all block objects written via the blocks layer.       */
    /* ==================================================================== */
    bool bRet = true;
    for (size_t iBlock = 0; iBlock < poBlocksLayer->apoBlocks.size(); iBlock++)
    {
        OGRFeature *poThisBlockFeat = poBlocksLayer->apoBlocks[iBlock];

        /* --------------------------------------------------------------------
         */
        /*      Is this block already defined in the template header? */
        /* --------------------------------------------------------------------
         */
        CPLString osBlockName = poThisBlockFeat->GetFieldAsString("Block");

        if (oHeaderDS.LookupBlock(osBlockName) != nullptr)
            continue;

        /* --------------------------------------------------------------------
         */
        /*      Write the block definition preamble. */
        /* --------------------------------------------------------------------
         */
        CPLDebug("DXF", "Writing BLOCK definition for '%s'.",
                 poThisBlockFeat->GetFieldAsString("Block"));

        bRet &= WriteValue(fpIn, 0, "BLOCK");
        unsigned int nIgnored;
        bRet &= WriteEntityID(fpIn, nIgnored);
        bRet &= WriteValue(fpIn, 100, "AcDbEntity");
        if (strlen(poThisBlockFeat->GetFieldAsString("Layer")) > 0)
            bRet &=
                WriteValue(fpIn, 8, poThisBlockFeat->GetFieldAsString("Layer"));
        else
            bRet &= WriteValue(fpIn, 8, "0");
        bRet &= WriteValue(fpIn, 100, "AcDbBlockBegin");
        bRet &= WriteValue(fpIn, 2, poThisBlockFeat->GetFieldAsString("Block"));
        bRet &= WriteValue(fpIn, 70, "0");

        // Origin
        bRet &= WriteValue(fpIn, 10, "0.0");
        bRet &= WriteValue(fpIn, 20, "0.0");
        bRet &= WriteValue(fpIn, 30, "0.0");

        bRet &= WriteValue(fpIn, 3, poThisBlockFeat->GetFieldAsString("Block"));
        bRet &= WriteValue(fpIn, 1, "");

        /* --------------------------------------------------------------------
         */
        /*      Write out the feature entities. */
        /* --------------------------------------------------------------------
         */
        if (poLayer->CreateFeature(poThisBlockFeat) != OGRERR_NONE)
            return false;

        /* --------------------------------------------------------------------
         */
        /*      Write out following features if they are the same block. */
        /* --------------------------------------------------------------------
         */
        while (iBlock < poBlocksLayer->apoBlocks.size() - 1 &&
               EQUAL(poBlocksLayer->apoBlocks[iBlock + 1]->GetFieldAsString(
                         "Block"),
                     osBlockName))
        {
            iBlock++;

            if (poLayer->CreateFeature(poBlocksLayer->apoBlocks[iBlock]) !=
                OGRERR_NONE)
                return false;
        }

        /* --------------------------------------------------------------------
         */
        /*      Write out the block definition postamble. */
        /* --------------------------------------------------------------------
         */
        bRet &= WriteValue(fpIn, 0, "ENDBLK");
        bRet &= WriteEntityID(fpIn, nIgnored);
        bRet &= WriteValue(fpIn, 100, "AcDbEntity");
        if (strlen(poThisBlockFeat->GetFieldAsString("Layer")) > 0)
            bRet &=
                WriteValue(fpIn, 8, poThisBlockFeat->GetFieldAsString("Layer"));
        else
            bRet &= WriteValue(fpIn, 8, "0");
        bRet &= WriteValue(fpIn, 100, "AcDbBlockEnd");
    }

    return bRet;
}

/************************************************************************/
/*                          ScanForEntities()                           */
/*                                                                      */
/*      Scan the indicated file for entity ids ("5" records) and        */
/*      build them up as a set so we can be careful to avoid            */
/*      creating new entities with conflicting ids.                     */
/************************************************************************/

void OGRDXFWriterDS::ScanForEntities(const char *pszFilename,
                                     const char *pszTarget)

{
    /* -------------------------------------------------------------------- */
    /*      Open the file and setup a reader.                               */
    /* -------------------------------------------------------------------- */
    VSILFILE *l_fp = VSIFOpenL(pszFilename, "r");

    if (l_fp == nullptr)
        return;

    OGRDXFReaderASCII oReader;
    oReader.Initialize(l_fp);

    /* -------------------------------------------------------------------- */
    /*      Add every code "5" line to our entities list.                   */
    /* -------------------------------------------------------------------- */
    char szLineBuf[257];
    int nCode = 0;
    const char *pszPortion = "HEADER";

    while ((nCode = oReader.ReadValue(szLineBuf, sizeof(szLineBuf))) != -1)
    {
        if ((nCode == 5 || nCode == 105) && EQUAL(pszTarget, pszPortion))
        {
            CPLString osEntity(szLineBuf);

            if (CheckEntityID(osEntity))
                CPLDebug("DXF", "Encountered entity '%s' multiple times.",
                         osEntity.c_str());
            else
                aosUsedEntities.insert(std::move(osEntity));
        }

        if (nCode == 0 && EQUAL(szLineBuf, "SECTION"))
        {
            nCode = oReader.ReadValue(szLineBuf, sizeof(szLineBuf));
            if (nCode == 2 && EQUAL(szLineBuf, "ENTITIES"))
                pszPortion = "BODY";
            if (nCode == 2 && EQUAL(szLineBuf, "OBJECTS"))
                pszPortion = "TRAILER";
        }
    }

    VSIFCloseL(l_fp);
}

/************************************************************************/
/*                           CheckEntityID()                            */
/*                                                                      */
/*      Does the mentioned entity already exist?                        */
/************************************************************************/

bool OGRDXFWriterDS::CheckEntityID(const char *pszEntityID)

{
    std::set<CPLString>::iterator it;

    it = aosUsedEntities.find(pszEntityID);
    return it != aosUsedEntities.end();
}

/************************************************************************/
/*                           WriteEntityID()                            */
/************************************************************************/

bool OGRDXFWriterDS::WriteEntityID(VSILFILE *fpIn, unsigned int &nAssignedFID,
                                   GIntBig nPreferredFID)

{
    CPLString osEntityID;

    // From https://github.com/OSGeo/gdal/issues/11299 it seems that 0 is an
    // invalid handle value.
    if (nPreferredFID > 0 &&
        nPreferredFID <=
            static_cast<GIntBig>(std::numeric_limits<unsigned int>::max()))
    {

        osEntityID.Printf("%X", static_cast<unsigned int>(nPreferredFID));
        if (!CheckEntityID(osEntityID))
        {
            aosUsedEntities.insert(osEntityID);
            if (!WriteValue(fpIn, 5, osEntityID))
                return false;
            nAssignedFID = static_cast<unsigned int>(nPreferredFID);
            return true;
        }
    }

    do
    {
        osEntityID.Printf("%X", nNextFID++);
    } while (CheckEntityID(osEntityID));

    aosUsedEntities.insert(osEntityID);
    if (!WriteValue(fpIn, 5, osEntityID))
        return false;

    nAssignedFID = nNextFID - 1;
    return true;
}

/************************************************************************/
/*                           UpdateExtent()                             */
/************************************************************************/

void OGRDXFWriterDS::UpdateExtent(OGREnvelope *psEnvelope)
{
    oGlobalEnvelope.Merge(*psEnvelope);
}
