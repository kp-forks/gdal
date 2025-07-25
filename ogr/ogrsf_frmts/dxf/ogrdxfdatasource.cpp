/******************************************************************************
 *
 * Project:  DXF Translator
 * Purpose:  Implements OGRDXFDataSource class
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2009, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_dxf.h"
#include "cpl_conv.h"
#include "cpl_string.h"

#include <algorithm>

/************************************************************************/
/*                          OGRDXFDataSource()                          */
/************************************************************************/

OGRDXFDataSource::OGRDXFDataSource()
    : fp(nullptr), iEntitiesOffset(0), iEntitiesLineNumber(0),
      bInlineBlocks(false), bMergeBlockGeometries(false),
      bTranslateEscapeSequences(false), bIncludeRawCodeValues(false),
      b3DExtensibleMode(false), bHaveReadSolidData(false),
      poReader(std::make_unique<OGRDXFReaderASCII>())
{
}

/************************************************************************/
/*                         ~OGRDXFDataSource()                          */
/************************************************************************/

OGRDXFDataSource::~OGRDXFDataSource()

{
    /* -------------------------------------------------------------------- */
    /*      Destroy layers.                                                 */
    /* -------------------------------------------------------------------- */
    while (!apoLayers.empty())
    {
        delete apoLayers.back();
        apoLayers.pop_back();
    }

    /* -------------------------------------------------------------------- */
    /*      Close file.                                                     */
    /* -------------------------------------------------------------------- */
    if (fp != nullptr)
    {
        VSIFCloseL(fp);
        fp = nullptr;
    }
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRDXFDataSource::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, ODsCZGeometries))
        return true;

    return false;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRDXFDataSource::GetLayer(int iLayer)

{
    if (iLayer < 0 || iLayer >= (int)apoLayers.size())
        return nullptr;
    else
        return apoLayers[iLayer];
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

bool OGRDXFDataSource::Open(const char *pszFilename, VSILFILE *fpIn,
                            bool bHeaderOnly, CSLConstList papszOptionsIn)

{
    SetDescription(pszFilename);

    osEncoding = CPL_ENC_ISO8859_1;

    bInlineBlocks = CPLTestBool(
        CSLFetchNameValueDef(papszOptionsIn, "INLINE_BLOCKS",
                             CPLGetConfigOption("DXF_INLINE_BLOCKS", "TRUE")));
    bMergeBlockGeometries = CPLTestBool(CSLFetchNameValueDef(
        papszOptionsIn, "MERGE_BLOCK_GEOMETRIES",
        CPLGetConfigOption("DXF_MERGE_BLOCK_GEOMETRIES", "TRUE")));

    bTranslateEscapeSequences = CPLTestBool(CSLFetchNameValueDef(
        papszOptionsIn, "TRANSLATE_ESCAPE_SEQUENCES",
        CPLGetConfigOption("DXF_TRANSLATE_ESCAPE_SEQUENCES", "TRUE")));

    bIncludeRawCodeValues = CPLTestBool(CSLFetchNameValueDef(
        papszOptionsIn, "INCLUDE_RAW_CODE_VALUES",
        CPLGetConfigOption("DXF_INCLUDE_RAW_CODE_VALUES", "FALSE")));

    b3DExtensibleMode = CPLTestBool(CSLFetchNameValueDef(
        papszOptionsIn, "3D_EXTENSIBLE_MODE",
        CPLGetConfigOption("DXF_3D_EXTENSIBLE_MODE", "FALSE")));

    m_bClosedLineAsPolygon = CPLTestBool(CSLFetchNameValueDef(
        papszOptionsIn, "CLOSED_LINE_AS_POLYGON",
        CPLGetConfigOption("DXF_CLOSED_LINE_AS_POLYGON", "FALSE")));

    m_dfHatchTolerance = CPLAtof(
        CSLFetchNameValueDef(papszOptionsIn, "HATCH_TOLERANCE",
                             CPLGetConfigOption("DXF_HATCH_TOLERANCE", "-1")));

    // Only for debugging
    if (CPLTestBool(CPLGetConfigOption("DXF_HEADER_ONLY", "FALSE")))
        bHeaderOnly = true;

    /* -------------------------------------------------------------------- */
    /*      Open the file.                                                  */
    /* -------------------------------------------------------------------- */
    fp = fpIn;
    if (fp == nullptr)
        fp = VSIFOpenL(pszFilename, "rb");
    if (fp == nullptr)
        return false;

    VSIFSeekL(fp, 0, SEEK_SET);
    std::string osBuffer;
    constexpr size_t nBinarySignatureLen = AUTOCAD_BINARY_DXF_SIGNATURE.size();
    osBuffer.resize(nBinarySignatureLen);
    VSIFReadL(osBuffer.data(), 1, osBuffer.size(), fp);
    if (memcmp(osBuffer.data(), AUTOCAD_BINARY_DXF_SIGNATURE.data(),
               nBinarySignatureLen) == 0)
    {
        poReader = std::make_unique<OGRDXFReaderBinary>();
    }
    else
    {
        VSIFSeekL(fp, 0, SEEK_SET);
    }

    poReader->Initialize(fp);

    /* -------------------------------------------------------------------- */
    /*      Confirm we have a header section.                               */
    /* -------------------------------------------------------------------- */
    char szLineBuf[257];
    bool bEntitiesOnly = false;

    if (ReadValue(szLineBuf) != 0 || !EQUAL(szLineBuf, "SECTION"))
        return false;

    if (ReadValue(szLineBuf) != 2 ||
        (!EQUAL(szLineBuf, "HEADER") && !EQUAL(szLineBuf, "ENTITIES") &&
         !EQUAL(szLineBuf, "TABLES")))
        return false;

    if (EQUAL(szLineBuf, "ENTITIES"))
        bEntitiesOnly = true;

    /* Some files might have no header but begin directly with a TABLES section
     */
    else if (EQUAL(szLineBuf, "TABLES"))
    {
        osEncoding = CSLFetchNameValueDef(
            papszOptionsIn, "ENCODING",
            CPLGetConfigOption("DXF_ENCODING", osEncoding));

        if (!ReadTablesSection())
            return false;
        if (ReadValue(szLineBuf) < 0)
        {
            DXF_READER_ERROR();
            return false;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Process the header, picking up a few useful pieces of           */
    /*      information.                                                    */
    /* -------------------------------------------------------------------- */
    else /* if( EQUAL(szLineBuf,"HEADER") ) */
    {
        if (!ReadHeaderSection())
            return false;
        if (ReadValue(szLineBuf) < 0)
        {
            DXF_READER_ERROR();
            return false;
        }

        /* --------------------------------------------------------------------
         */
        /*      Process the CLASSES section, if present. */
        /* --------------------------------------------------------------------
         */
        if (EQUAL(szLineBuf, "ENDSEC"))
        {
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }

        if (EQUAL(szLineBuf, "SECTION"))
        {
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }

        if (EQUAL(szLineBuf, "CLASSES"))
        {
            // int nCode = 0;
            while ((/* nCode = */ ReadValue(szLineBuf, sizeof(szLineBuf))) >
                       -1 &&
                   !EQUAL(szLineBuf, "ENDSEC"))
            {
                // printf("C:%d/%s\n", nCode, szLineBuf );
            }
        }

        /* --------------------------------------------------------------------
         */
        /*      Process the TABLES section, if present. */
        /* --------------------------------------------------------------------
         */
        if (EQUAL(szLineBuf, "ENDSEC"))
        {
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }

        if (EQUAL(szLineBuf, "SECTION"))
        {
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }

        if (EQUAL(szLineBuf, "TABLES"))
        {
            if (!ReadTablesSection())
                return false;
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create a blocks layer if we are not in inlining mode.           */
    /* -------------------------------------------------------------------- */
    if (!bInlineBlocks)
        apoLayers.push_back(new OGRDXFBlocksLayer(this));

    /* -------------------------------------------------------------------- */
    /*      Create out layer object - we will need it when interpreting     */
    /*      blocks.                                                         */
    /* -------------------------------------------------------------------- */
    apoLayers.push_back(new OGRDXFLayer(this));

    /* -------------------------------------------------------------------- */
    /*      Process the BLOCKS section if present.                          */
    /* -------------------------------------------------------------------- */
    if (!bEntitiesOnly)
    {
        if (EQUAL(szLineBuf, "ENDSEC"))
        {
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }

        if (EQUAL(szLineBuf, "SECTION"))
        {
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }

        if (EQUAL(szLineBuf, "BLOCKS"))
        {
            if (!ReadBlocksSection())
                return false;
            if (ReadValue(szLineBuf) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }
        }
    }

    if (bHeaderOnly)
        return true;

    /* -------------------------------------------------------------------- */
    /*      Now we are at the entities section, hopefully.  Confirm.        */
    /* -------------------------------------------------------------------- */
    if (EQUAL(szLineBuf, "SECTION"))
    {
        if (ReadValue(szLineBuf) < 0)
        {
            DXF_READER_ERROR();
            return false;
        }
    }

    if (!EQUAL(szLineBuf, "ENTITIES"))
    {
        DXF_READER_ERROR();
        return false;
    }

    iEntitiesOffset = poReader->GetCurrentFilePos();
    iEntitiesLineNumber = poReader->nLineNumber;
    apoLayers[0]->ResetReading();

    return true;
}

/************************************************************************/
/*                         ReadTablesSection()                          */
/************************************************************************/

bool OGRDXFDataSource::ReadTablesSection()

{
    char szLineBuf[257];
    int nCode = 0;

    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > -1 &&
           !EQUAL(szLineBuf, "ENDSEC"))
    {
        // We are only interested in extracting tables.
        if (nCode != 0 || !EQUAL(szLineBuf, "TABLE"))
            continue;

        nCode = ReadValue(szLineBuf, sizeof(szLineBuf));
        if (nCode < 0)
        {
            DXF_READER_ERROR();
            return false;
        }

        if (nCode != 2)
            continue;

        // CPLDebug( "DXF", "Found table %s.", szLineBuf );

        while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > -1 &&
               !EQUAL(szLineBuf, "ENDTAB"))
        {
            if (nCode == 0 && EQUAL(szLineBuf, "LAYER"))
            {
                if (!ReadLayerDefinition())
                    return false;
            }
            if (nCode == 0 && EQUAL(szLineBuf, "LTYPE"))
            {
                if (!ReadLineTypeDefinition())
                    return false;
            }
            if (nCode == 0 && EQUAL(szLineBuf, "STYLE"))
            {
                if (!ReadTextStyleDefinition())
                    return false;
            }
            if (nCode == 0 && EQUAL(szLineBuf, "DIMSTYLE"))
            {
                if (!ReadDimStyleDefinition())
                    return false;
            }
        }
    }
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }

    CPLDebug("DXF", "Read %d layer definitions.", (int)oLayerTable.size());
    return true;
}

/************************************************************************/
/*                        ReadLayerDefinition()                         */
/************************************************************************/

bool OGRDXFDataSource::ReadLayerDefinition()

{
    char szLineBuf[257];
    int nCode = 0;
    std::map<CPLString, CPLString> oLayerProperties;
    CPLString osLayerName = "";

    oLayerProperties["Hidden"] = "0";

    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > 0)
    {
        switch (nCode)
        {
            case 2:
                osLayerName =
                    CPLString(szLineBuf).Recode(GetEncoding(), CPL_ENC_UTF8);
                oLayerProperties["Exists"] = "1";
                break;

            case 6:
                oLayerProperties["Linetype"] =
                    CPLString(szLineBuf).Recode(GetEncoding(), CPL_ENC_UTF8);
                break;

            case 62:
                oLayerProperties["Color"] = szLineBuf;

                // Is layer off?
                if (atoi(szLineBuf) < 0 && oLayerProperties["Hidden"] != "2")
                    oLayerProperties["Hidden"] = "1";
                break;

            case 420:
                oLayerProperties["TrueColor"] = szLineBuf;
                break;

            case 440:
                oLayerProperties["Transparency"] = szLineBuf;
                break;

            case 70:
                oLayerProperties["Flags"] = szLineBuf;

                // Is layer frozen?
                if (atoi(szLineBuf) & 0x01)
                    oLayerProperties["Hidden"] = "2";
                break;

            case 370:
            case 39:
                oLayerProperties["LineWeight"] = szLineBuf;
                break;

            default:
                break;
        }
    }
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }

    if (!oLayerProperties.empty())
        oLayerTable[osLayerName] = std::move(oLayerProperties);

    if (nCode == 0)
        UnreadValue();
    return true;
}

/************************************************************************/
/*                        LookupLayerProperty()                         */
/************************************************************************/

std::optional<CPLString>
OGRDXFDataSource::LookupLayerProperty(const char *pszLayer,
                                      const char *pszProperty) const

{
    if (pszLayer)
    {
        const auto oIterLayer = oLayerTable.find(pszLayer);
        if (oIterLayer != oLayerTable.end())
        {
            const auto &oTable = oIterLayer->second;
            const auto oIterProperty = oTable.find(pszProperty);
            if (oIterProperty != oTable.end())
                return oIterProperty->second;
        }
    }
    return std::nullopt;
}

/************************************************************************/
/*                       ReadLineTypeDefinition()                       */
/************************************************************************/

bool OGRDXFDataSource::ReadLineTypeDefinition()

{
    char szLineBuf[257];
    int nCode = 0;
    CPLString osLineTypeName;
    std::vector<double> oLineTypeDef;
    double dfThisValue;

    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > 0)
    {
        switch (nCode)
        {
            case 2:
                osLineTypeName =
                    CPLString(szLineBuf).Recode(GetEncoding(), CPL_ENC_UTF8);
                break;

            case 49:
                dfThisValue = CPLAtof(szLineBuf);

                // Same sign as the previous entry? Continue the previous dash
                // or gap by appending this length
                if (oLineTypeDef.size() > 0 &&
                    (dfThisValue < 0) == (oLineTypeDef.back() < 0))
                {
                    oLineTypeDef.back() += dfThisValue;
                }
                // Otherwise, add a new entry
                else
                {
                    oLineTypeDef.push_back(dfThisValue);
                }

                break;

            default:
                break;
        }
    }
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }

    // Deal with an odd number of elements by adding the last element
    // onto the first
    if (oLineTypeDef.size() % 2 == 1)
    {
        oLineTypeDef.front() += oLineTypeDef.back();
        oLineTypeDef.pop_back();
    }

    if (oLineTypeDef.size())
    {
        // If the first element is a gap, rotate the elements so the first
        // element is a dash
        if (oLineTypeDef.front() < 0)
        {
            std::rotate(oLineTypeDef.begin(), oLineTypeDef.begin() + 1,
                        oLineTypeDef.end());
        }

        oLineTypeTable[osLineTypeName] = std::move(oLineTypeDef);
    }

    if (nCode == 0)
        UnreadValue();
    return true;
}

/************************************************************************/
/*                           LookupLineType()                           */
/************************************************************************/

std::vector<double> OGRDXFDataSource::LookupLineType(const char *pszName)

{
    if (pszName && oLineTypeTable.count(pszName) > 0)
        return oLineTypeTable[pszName];
    else
        return std::vector<double>();  // empty, represents a continuous line
}

/************************************************************************/
/*                       ReadTextStyleDefinition()                      */
/************************************************************************/

bool OGRDXFDataSource::ReadTextStyleDefinition()

{
    char szLineBuf[257];
    int nCode = 0;

    CPLString osStyleHandle;
    CPLString osStyleName;
    bool bInsideAcadSection = false;

    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > 0)
    {
        switch (nCode)
        {
            case 5:
                osStyleHandle = szLineBuf;
                break;

            case 2:
                osStyleName = CPLString(szLineBuf)
                                  .Recode(GetEncoding(), CPL_ENC_UTF8)
                                  .toupper();
                break;

            case 70:
                // If the LSB is set, this is not a text style
                if (atoi(szLineBuf) & 1)
                    return true;
                break;

                // Note: 40 and 41 group codes do not propagate from a text
                // style down to TEXT objects. However, 41 does propagate down
                // for MTEXT.

            case 41:
                oTextStyleTable[osStyleName]["Width"] = szLineBuf;
                break;

            case 1001:
                bInsideAcadSection = EQUAL(szLineBuf, "ACAD");
                break;

            case 1000:
                if (bInsideAcadSection)
                    oTextStyleTable[osStyleName]["Font"] = szLineBuf;
                break;

            case 1071:
                // bold and italic are kept in this undocumented bitfield
                if (bInsideAcadSection)
                {
                    const int nFontFlags = atoi(szLineBuf);
                    oTextStyleTable[osStyleName]["Bold"] =
                        (nFontFlags & 0x2000000) ? "1" : "0";
                    oTextStyleTable[osStyleName]["Italic"] =
                        (nFontFlags & 0x1000000) ? "1" : "0";
                }
                break;

            default:
                break;
        }
    }
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }

    if (nCode == 0)
        UnreadValue();

    if (osStyleHandle != "")
        oTextStyleHandles[osStyleHandle] = std::move(osStyleName);

    return true;
}

/************************************************************************/
/*                           TextStyleExists()                          */
/************************************************************************/

bool OGRDXFDataSource::TextStyleExists(const char *pszTextStyle)

{
    if (!pszTextStyle)
        return false;

    CPLString osTextStyleUpper = pszTextStyle;
    osTextStyleUpper.toupper();

    return oTextStyleTable.count(osTextStyleUpper) > 0;
}

/************************************************************************/
/*                       LookupTextStyleProperty()                      */
/************************************************************************/

const char *OGRDXFDataSource::LookupTextStyleProperty(const char *pszTextStyle,
                                                      const char *pszProperty,
                                                      const char *pszDefault)

{
    if (!pszTextStyle)
        return pszDefault;

    CPLString osTextStyleUpper = pszTextStyle;
    osTextStyleUpper.toupper();

    if (pszProperty && oTextStyleTable.count(osTextStyleUpper) > 0 &&
        oTextStyleTable[osTextStyleUpper].count(pszProperty) > 0)
    {
        return (oTextStyleTable[osTextStyleUpper])[pszProperty];
    }
    else
    {
        return pszDefault;
    }
}

/************************************************************************/
/*                      GetTextStyleNameByHandle()                      */
/*                                                                      */
/*      Find the name of the text style with the given STYLE table      */
/*      handle. If there is no such style, an empty string is returned. */
/************************************************************************/

CPLString OGRDXFDataSource::GetTextStyleNameByHandle(const char *pszID)

{
    CPLString l_osID = pszID;

    if (oTextStyleHandles.count(l_osID) == 0)
        return "";
    else
        return oTextStyleHandles[l_osID];
}

/************************************************************************/
/*                  PopulateDefaultDimStyleProperties()                 */
/************************************************************************/

void OGRDXFDataSource::PopulateDefaultDimStyleProperties(
    std::map<CPLString, CPLString> &oDimStyleProperties)

{
    const int *piCode = ACGetKnownDimStyleCodes();
    do
    {
        const char *pszProperty = ACGetDimStylePropertyName(*piCode);
        oDimStyleProperties[pszProperty] =
            ACGetDimStylePropertyDefault(*piCode);
    } while (*(++piCode));
}

/************************************************************************/
/*                       ReadDimStyleDefinition()                       */
/************************************************************************/

bool OGRDXFDataSource::ReadDimStyleDefinition()

{
    char szLineBuf[257];
    int nCode = 0;
    std::map<CPLString, CPLString> oDimStyleProperties;
    CPLString osDimStyleName = "";

    PopulateDefaultDimStyleProperties(oDimStyleProperties);

    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > 0)
    {
        switch (nCode)
        {
            case 2:
                osDimStyleName =
                    CPLString(szLineBuf).Recode(GetEncoding(), CPL_ENC_UTF8);
                break;

            default:
                const char *pszProperty = ACGetDimStylePropertyName(nCode);
                if (pszProperty)
                    oDimStyleProperties[pszProperty] = szLineBuf;
                break;
        }
    }
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }

    if (!oDimStyleProperties.empty())
        oDimStyleTable[osDimStyleName] = std::move(oDimStyleProperties);

    if (nCode == 0)
        UnreadValue();
    return true;
}

/************************************************************************/
/*                           LookupDimStyle()                           */
/*                                                                      */
/*      If the specified DIMSTYLE does not exist, a default set of      */
/*      of style properties are copied into oDimStyleProperties and     */
/*      false is returned.  Otherwise true is returned.                 */
/************************************************************************/

bool OGRDXFDataSource::LookupDimStyle(
    const char *pszDimStyle,
    std::map<CPLString, CPLString> &oDimStyleProperties)

{
    if (pszDimStyle == nullptr || !oDimStyleTable.count(pszDimStyle))
    {
        PopulateDefaultDimStyleProperties(oDimStyleProperties);
        return false;
    }

    // make a copy of the DIMSTYLE properties, so no-one can mess around
    // with our original copy
    oDimStyleProperties = oDimStyleTable[pszDimStyle];
    return true;
}

/************************************************************************/
/*                         ReadHeaderSection()                          */
/************************************************************************/

bool OGRDXFDataSource::ReadHeaderSection()

{
    char szLineBuf[257];
    int nCode = 0;

    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > -1 &&
           !EQUAL(szLineBuf, "ENDSEC"))
    {
        if (nCode != 9)
            continue;

        CPLString l_osName = szLineBuf;

        if (ReadValue(szLineBuf, sizeof(szLineBuf)) < 0)
        {
            DXF_READER_ERROR();
            return false;
        }

        oHeaderVariables[l_osName] = szLineBuf;
        GDALDataset::SetMetadataItem(l_osName.c_str(), szLineBuf,
                                     "DXF_HEADER_VARIABLES");
    }
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }

    nCode = ReadValue(szLineBuf, sizeof(szLineBuf));
    if (nCode < 0)
    {
        DXF_READER_ERROR();
        return false;
    }
    UnreadValue();

    /* Unusual DXF files produced by dxflib */
    /* such as http://www.ribbonsoft.com/library/architecture/plants/decd5.dxf
     */
    /* where there is a spurious ENDSEC in the middle of the header variables */
    if (nCode == 9 && STARTS_WITH_CI(szLineBuf, "$"))
    {
        while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) > -1 &&
               !EQUAL(szLineBuf, "ENDSEC"))
        {
            if (nCode != 9)
                continue;

            CPLString l_osName = szLineBuf;

            if (ReadValue(szLineBuf, sizeof(szLineBuf)) < 0)
            {
                DXF_READER_ERROR();
                return false;
            }

            oHeaderVariables[l_osName] = szLineBuf;
            GDALDataset::SetMetadataItem(l_osName.c_str(), szLineBuf,
                                         "DXF_HEADER_VARIABLES");
        }
        if (nCode < 0)
        {
            DXF_READER_ERROR();
            return false;
        }
    }

    CPLDebug("DXF", "Read %d header variables.", (int)oHeaderVariables.size());

    /* -------------------------------------------------------------------- */
    /*      Decide on what CPLRecode() name to use for the files            */
    /*      encoding or allow the encoding to be overridden.                */
    /* -------------------------------------------------------------------- */
    CPLString osCodepage = GetVariable("$DWGCODEPAGE", "ANSI_1252");

    // not strictly accurate but works even without iconv.
    if (osCodepage == "ANSI_1252")
        osEncoding = CPL_ENC_ISO8859_1;
    else if (STARTS_WITH_CI(osCodepage, "ANSI_"))
    {
        osEncoding = "CP";
        osEncoding += osCodepage + 5;
    }
    else
    {
        // fallback to the default
        osEncoding = CPL_ENC_ISO8859_1;
    }

    const char *pszEncoding = CPLGetConfigOption("DXF_ENCODING", nullptr);
    if (pszEncoding != nullptr)
        osEncoding = pszEncoding;

    if (osEncoding != CPL_ENC_ISO8859_1)
        CPLDebug("DXF", "Treating DXF as encoding '%s', $DWGCODEPAGE='%s'",
                 osEncoding.c_str(), osCodepage.c_str());
    return true;
}

/************************************************************************/
/*                            GetVariable()                             */
/*                                                                      */
/*      Fetch a variable that came from the HEADER section.             */
/************************************************************************/

const char *OGRDXFDataSource::GetVariable(const char *pszName,
                                          const char *pszDefault)

{
    if (oHeaderVariables.count(pszName) == 0)
        return pszDefault;
    else
        return oHeaderVariables[pszName];
}

/************************************************************************/
/*                         AddStandardFields()                          */
/************************************************************************/

void OGRDXFDataSource::AddStandardFields(OGRFeatureDefn *poFeatureDefn,
                                         const int nFieldModes)
{
    OGRFieldDefn oLayerField("Layer", OFTString);
    poFeatureDefn->AddFieldDefn(&oLayerField);

    OGRFieldDefn oPaperSpaceField("PaperSpace", OFTInteger);
    oPaperSpaceField.SetSubType(OFSTBoolean);
    poFeatureDefn->AddFieldDefn(&oPaperSpaceField);

    OGRFieldDefn oClassField("SubClasses", OFTString);
    poFeatureDefn->AddFieldDefn(&oClassField);

    if (nFieldModes & ODFM_IncludeRawCodeValues)
    {
        OGRFieldDefn oRawCodeField("RawCodeValues", OFTStringList);
        poFeatureDefn->AddFieldDefn(&oRawCodeField);
    }

    OGRFieldDefn oLinetypeField("Linetype", OFTString);
    poFeatureDefn->AddFieldDefn(&oLinetypeField);

    OGRFieldDefn oEntityHandleField("EntityHandle", OFTString);
    poFeatureDefn->AddFieldDefn(&oEntityHandleField);

    OGRFieldDefn oTextField("Text", OFTString);
    poFeatureDefn->AddFieldDefn(&oTextField);

    if (nFieldModes & ODFM_Include3DModeFields)
    {
        OGRFieldDefn oASMBinaryField("ASMData", OFTBinary);
        poFeatureDefn->AddFieldDefn(&oASMBinaryField);

        OGRFieldDefn oASMTransformField("ASMTransform", OFTRealList);
        poFeatureDefn->AddFieldDefn(&oASMTransformField);
    }

    if (nFieldModes & ODFM_IncludeBlockFields)
    {
        OGRFieldDefn oBlockNameField("BlockName", OFTString);
        poFeatureDefn->AddFieldDefn(&oBlockNameField);

        OGRFieldDefn oScaleField("BlockScale", OFTRealList);
        poFeatureDefn->AddFieldDefn(&oScaleField);

        OGRFieldDefn oBlockAngleField("BlockAngle", OFTReal);
        poFeatureDefn->AddFieldDefn(&oBlockAngleField);

        OGRFieldDefn oBlockOCSNormalField("BlockOCSNormal", OFTRealList);
        poFeatureDefn->AddFieldDefn(&oBlockOCSNormalField);

        OGRFieldDefn oBlockOCSCoordsField("BlockOCSCoords", OFTRealList);
        poFeatureDefn->AddFieldDefn(&oBlockOCSCoordsField);

        OGRFieldDefn oBlockAttribsField("BlockAttributes", OFTStringList);
        poFeatureDefn->AddFieldDefn(&oBlockAttribsField);

        // This field holds the name of the block on which the entity lies.
        // The BlockName field was previously used for this purpose; this
        // was changed because of the ambiguity with the BlockName field
        // used by INSERT entities.
        OGRFieldDefn oBlockField("Block", OFTString);
        poFeatureDefn->AddFieldDefn(&oBlockField);

        // Extra field to use with ATTDEF entities
        OGRFieldDefn oAttributeTagField("AttributeTag", OFTString);
        poFeatureDefn->AddFieldDefn(&oAttributeTagField);
    }
}

/************************************************************************/
/*                    GetEntryFromAcDsDataSection()                     */
/************************************************************************/

size_t
OGRDXFDataSource::GetEntryFromAcDsDataSection(const char *pszEntityHandle,
                                              const GByte **pabyBuffer)

{
    if (!pszEntityHandle || !pabyBuffer)
        return 0;

    if (bHaveReadSolidData)
    {
        if (oSolidBinaryData.count(pszEntityHandle) > 0)
        {
            *pabyBuffer = oSolidBinaryData[pszEntityHandle].data();
            return oSolidBinaryData[pszEntityHandle].size();
        }
        return 0;
    }

    // Keep track of our current position and line number in the file so we can
    // return here later
    uint64_t iPrevOffset = poReader->GetCurrentFilePos();
    int nPrevLineNumber = poReader->nLineNumber;

    char szLineBuf[4096];
    int nCode = 0;
    bool bFound = false;

    // Search for the ACDSDATA section
    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) >= 0)
    {
        // Check whether the ACDSDATA section starts here
        if (nCode == 0 && EQUAL(szLineBuf, "SECTION"))
        {
            if ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) < 0)
            {
                break;
            }

            if (nCode == 2 && EQUAL(szLineBuf, "ACDSDATA"))
            {
                bFound = true;
                break;
            }
        }
    }

    if (!bFound)
    {
        poReader->ResetReadPointer(iPrevOffset, nPrevLineNumber);
        return 0;
    }

    bool bInAcDsRecord = false;
    bool bGotAsmData = false;
    CPLString osThisHandle;

    // Search for the relevant ACDSRECORD and extract its binary data
    while ((nCode = ReadValue(szLineBuf, sizeof(szLineBuf))) >= 0)
    {
        if (nCode == 0 && EQUAL(szLineBuf, "ENDSEC"))
        {
            // We've reached the end of the ACDSDATA section
            break;
        }
        else if (nCode == 0)
        {
            bInAcDsRecord = EQUAL(szLineBuf, "ACDSRECORD");
            bGotAsmData = false;
            osThisHandle.clear();
        }
        else if (bInAcDsRecord && nCode == 320)
        {
            osThisHandle = szLineBuf;
        }
        else if (bInAcDsRecord && nCode == 2)
        {
            bGotAsmData = EQUAL(szLineBuf, "ASM_Data");
        }
        else if (bInAcDsRecord && bGotAsmData && nCode == 94)
        {
            // Group code 94 gives the length of the binary data that follows
            int nLen = atoi(szLineBuf);

            // Enforce some limits (the upper limit is arbitrary)
            if (nLen <= 0 || nLen > 1048576)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "ACDSRECORD data for entity %s is too long (more than "
                         "1MB in size) and was skipped.",
                         pszEntityHandle);
                continue;
            }

            oSolidBinaryData[osThisHandle].resize(nLen);

            // Read the binary data into the buffer
            int nPos = 0;
            while (ReadValue(szLineBuf, sizeof(szLineBuf)) == 310)
            {
                int nBytesRead;
                std::unique_ptr<GByte, VSIFreeReleaser> pabyHex(
                    CPLHexToBinary(szLineBuf, &nBytesRead));

                if (nPos + nBytesRead > nLen)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Too many bytes in ACDSRECORD data for entity %s. "
                             "Is the length (group code 94) correct?",
                             pszEntityHandle);
                    break;
                }
                else
                {
                    std::copy_n(pabyHex.get(), nBytesRead,
                                oSolidBinaryData[osThisHandle].begin() + nPos);
                    nPos += nBytesRead;
                }
            }
        }
    }

    poReader->ResetReadPointer(iPrevOffset, nPrevLineNumber);

    bHaveReadSolidData = true;

    if (oSolidBinaryData.count(pszEntityHandle) > 0)
    {
        *pabyBuffer = oSolidBinaryData[pszEntityHandle].data();
        return oSolidBinaryData[pszEntityHandle].size();
    }
    return 0;
}
