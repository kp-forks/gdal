/******************************************************************************
 *
 * Project:  Oracle Spatial Driver
 * Purpose:  Implementation of the OGROCIWritableLayer class.  This provides
 *           some services for converting OGRGeometries into Oracle structures
 *           that is shared between OGROCITableLayer and OGROCILoaderLayer.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_oci.h"
#include "cpl_conv.h"
#include "cpl_string.h"

/************************************************************************/
/*                        OGROCIWritableLayer()                         */
/************************************************************************/

OGROCIWritableLayer::OGROCIWritableLayer(OGROCIDataSource *poDSIn)
    : OGROCILayer(poDSIn)

{
    nDimension =
        MAX(2, MIN(3, atoi(CPLGetConfigOption("OCI_DEFAULT_DIM", "3"))));
    nSRID = -1;

    nOrdinalCount = 0;
    nOrdinalMax = 0;
    padfOrdinals = nullptr;

    nElemInfoCount = 0;
    nElemInfoMax = 0;
    panElemInfo = nullptr;

    bLaunderColumnNames = TRUE;
    bPreservePrecision = FALSE;
    nDefaultStringSize = DEFAULT_STRING_SIZE;
    bTruncationReported = FALSE;
    poSRS = nullptr;

    papszOptions = nullptr;
}

/************************************************************************/
/*                        ~OGROCIWritableLayer()                        */
/************************************************************************/

OGROCIWritableLayer::~OGROCIWritableLayer()

{
    CPLFree(padfOrdinals);
    CPLFree(panElemInfo);

    CSLDestroy(papszOptions);
}

/************************************************************************/
/*                            PushOrdinal()                             */
/************************************************************************/

void OGROCIWritableLayer::PushOrdinal(double dfOrd)

{
    if (nOrdinalCount == nOrdinalMax)
    {
        nOrdinalMax = nOrdinalMax * 2 + 100;
        padfOrdinals =
            (double *)CPLRealloc(padfOrdinals, sizeof(double) * nOrdinalMax);
    }

    padfOrdinals[nOrdinalCount++] = dfOrd;
}

/************************************************************************/
/*                            PushElemInfo()                            */
/************************************************************************/

void OGROCIWritableLayer::PushElemInfo(int nOffset, int nEType, int nInterp)

{
    if (nElemInfoCount + 3 >= nElemInfoMax)
    {
        nElemInfoMax = nElemInfoMax * 2 + 100;
        panElemInfo =
            (int *)CPLRealloc(panElemInfo, sizeof(int) * nElemInfoMax);
    }

    panElemInfo[nElemInfoCount++] = nOffset;
    panElemInfo[nElemInfoCount++] = nEType;
    panElemInfo[nElemInfoCount++] = nInterp;
}

/************************************************************************/
/*                       TranslateElementGroup()                        */
/*                                                                      */
/*      Append one or more element groups to the existing element       */
/*      info and ordinates lists for the passed geometry.               */
/************************************************************************/

OGRErr OGROCIWritableLayer::TranslateElementGroup(OGRGeometry *poGeometry)

{
    switch (wkbFlatten(poGeometry->getGeometryType()))
    {
        case wkbPoint:
        {
            OGRPoint *poPoint = poGeometry->toPoint();

            PushElemInfo(nOrdinalCount + 1, 1, 1);

            PushOrdinal(poPoint->getX());
            PushOrdinal(poPoint->getY());
            if (nDimension == 3)
                PushOrdinal(poPoint->getZ());

            return OGRERR_NONE;
        }

        case wkbLineString:
        {
            OGRLineString *poLine = poGeometry->toLineString();
            int iVert;

            PushElemInfo(nOrdinalCount + 1, 2, 1);

            for (iVert = 0; iVert < poLine->getNumPoints(); iVert++)
            {
                PushOrdinal(poLine->getX(iVert));
                PushOrdinal(poLine->getY(iVert));
                if (nDimension == 3)
                    PushOrdinal(poLine->getZ(iVert));
            }
            return OGRERR_NONE;
        }

        case wkbPolygon:
        {
            OGRPolygon *poPoly = poGeometry->toPolygon();
            int iRing;

            for (iRing = -1; iRing < poPoly->getNumInteriorRings(); iRing++)
            {
                OGRLinearRing *poRing;
                int iVert;

                if (iRing == -1)
                    poRing = poPoly->getExteriorRing();
                else
                    poRing = poPoly->getInteriorRing(iRing);

                if (iRing == -1)
                    PushElemInfo(nOrdinalCount + 1, 1003, 1);
                else
                    PushElemInfo(nOrdinalCount + 1, 2003, 1);

                if ((iRing == -1 && poRing->isClockwise()) ||
                    (iRing != -1 && !poRing->isClockwise()))
                {
                    for (iVert = poRing->getNumPoints() - 1; iVert >= 0;
                         iVert--)
                    {
                        PushOrdinal(poRing->getX(iVert));
                        PushOrdinal(poRing->getY(iVert));
                        if (nDimension == 3)
                            PushOrdinal(poRing->getZ(iVert));
                    }
                }
                else
                {
                    for (iVert = 0; iVert < poRing->getNumPoints(); iVert++)
                    {
                        PushOrdinal(poRing->getX(iVert));
                        PushOrdinal(poRing->getY(iVert));
                        if (nDimension == 3)
                            PushOrdinal(poRing->getZ(iVert));
                    }
                }
            }

            return OGRERR_NONE;
        }

        default:
        {
            return OGRERR_FAILURE;
        }
    }
}

/************************************************************************/
/*                          ReportTruncation()                          */
/************************************************************************/

void OGROCIWritableLayer::ReportTruncation(OGRFieldDefn *psFldDefn)

{
    if (bTruncationReported)
        return;

    CPLError(CE_Warning, CPLE_AppDefined,
             "The value for the field %s is being truncated to fit the\n"
             "declared width/precision of the field.  No more truncations\n"
             "for table %s will be reported.",
             psFldDefn->GetNameRef(), poFeatureDefn->GetName());

    bTruncationReported = TRUE;
}

/************************************************************************/
/*                             SetOptions()                             */
/*                                                                      */
/*      Set layer creation or other options.                            */
/************************************************************************/

void OGROCIWritableLayer::SetOptions(CSLConstList papszOptionsIn)

{
    CSLDestroy(papszOptions);
    papszOptions = CSLDuplicate(papszOptionsIn);
}

/************************************************************************/
/*                            CreateField()                             */
/************************************************************************/

OGRErr OGROCIWritableLayer::CreateField(const OGRFieldDefn *poFieldIn,
                                        int bApproxOK)

{
    OGROCISession *poSession = poDS->GetSession();
    char szFieldType[256];
    char szFieldName[128];  // 12.2 max identifier name
    OGRFieldDefn oField(poFieldIn);

    /* -------------------------------------------------------------------- */
    /*      Do we want to "launder" the column names into Oracle            */
    /*      friendly format?                                                */
    /* -------------------------------------------------------------------- */
    if (bLaunderColumnNames)
    {
        char *pszSafeName = CPLStrdup(oField.GetNameRef());

        poSession->CleanName(pszSafeName);
        oField.SetName(pszSafeName);
        CPLFree(pszSafeName);
    }

    /* -------------------------------------------------------------------- */
    /*      Work out the Oracle type.                                       */
    /* -------------------------------------------------------------------- */
    if (oField.GetType() == OFTInteger)
    {
        if (bPreservePrecision && oField.GetWidth() != 0)
            snprintf(szFieldType, sizeof(szFieldType), "NUMBER(%d)",
                     oField.GetWidth());
        else
            strcpy(szFieldType, "INTEGER");
    }
    else if (oField.GetType() == OFTInteger64)
    {
        if (bPreservePrecision && oField.GetWidth() != 0)
            snprintf(szFieldType, sizeof(szFieldType), "NUMBER(%d)",
                     oField.GetWidth());
        else
            strcpy(szFieldType, "NUMBER(20)");
    }
    else if (oField.GetType() == OFTReal)
    {
        if (bPreservePrecision && oField.GetWidth() != 0)
            snprintf(szFieldType, sizeof(szFieldType), "NUMBER(%d,%d)",
                     oField.GetWidth(), oField.GetPrecision());
        else
            strcpy(szFieldType, "FLOAT(126)");
    }
    else if (oField.GetType() == OFTString)
    {
        if (oField.GetWidth() == 0 || !bPreservePrecision)
            snprintf(szFieldType, sizeof(szFieldType), "VARCHAR2(%d)",
                     nDefaultStringSize);
        else
            snprintf(szFieldType, sizeof(szFieldType), "VARCHAR2(%d CHAR)",
                     oField.GetWidth());
    }
    else if (oField.GetType() == OFTDate)
    {
        snprintf(szFieldType, sizeof(szFieldType), "DATE");
    }
    else if (oField.GetType() == OFTDateTime)
    {
        const char *pszTIMESTAMP_WITH_TIME_ZONE =
            CSLFetchNameValue(papszOptions, "TIMESTAMP_WITH_TIME_ZONE");
        if ((!pszTIMESTAMP_WITH_TIME_ZONE &&
             oField.GetTZFlag() >= OGR_TZFLAG_MIXED_TZ) ||
            (pszTIMESTAMP_WITH_TIME_ZONE &&
             CPLTestBool(pszTIMESTAMP_WITH_TIME_ZONE)))
        {
            setFieldIndexWithTimeStampWithTZ.insert(
                poFeatureDefn->GetFieldCount());
            snprintf(szFieldType, sizeof(szFieldType),
                     "TIMESTAMP(3) WITH TIME ZONE");
        }
        else
            snprintf(szFieldType, sizeof(szFieldType), "TIMESTAMP(3)");
    }
    else if (bApproxOK)
    {
        oField.SetDefault(nullptr);
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Can't create field %s with type %s on Oracle layers.  "
                 "Creating as VARCHAR.",
                 oField.GetNameRef(),
                 OGRFieldDefn::GetFieldTypeName(oField.GetType()));
        snprintf(szFieldType, sizeof(szFieldType), "VARCHAR2(%d)",
                 nDefaultStringSize);
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Can't create field %s with type %s on Oracle layers.",
                 oField.GetNameRef(),
                 OGRFieldDefn::GetFieldTypeName(oField.GetType()));

        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Create the new field.                                           */
    /* -------------------------------------------------------------------- */
    OGROCIStringBuf oCommand;
    OGROCIStatement oAddField(poSession);

    const int nCommandSize = static_cast<int>(
        70 + strlen(poFeatureDefn->GetName()) + strlen(oField.GetNameRef()) +
        strlen(szFieldType) +
        (oField.GetDefault() ? strlen(oField.GetDefault()) : 0));
    oCommand.MakeRoomFor(nCommandSize);

    snprintf(szFieldName, sizeof(szFieldName), "%s", oField.GetNameRef());
    szFieldName[sizeof(szFieldName) - 1] = '\0';
    if (strlen(oField.GetNameRef()) > sizeof(szFieldName))
    {
        szFieldName[sizeof(szFieldName) - 1] = '_';
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Column %s is too long (at most 30 characters). Using %s.",
                 oField.GetNameRef(), szFieldName);
        oField.SetName(szFieldName);
    }
    snprintf(oCommand.GetString(), nCommandSize, "ALTER TABLE %s ADD \"%s\" %s",
             poFeatureDefn->GetName(), szFieldName, szFieldType);
    if (oField.GetDefault() != nullptr && !oField.IsDefaultDriverSpecific())
    {
        snprintf(oCommand.GetString() + strlen(oCommand.GetString()),
                 nCommandSize - strlen(oCommand.GetString()), " DEFAULT %s",
                 oField.GetDefault());
    }
    if (!oField.IsNullable())
        strcat(oCommand.GetString(), " NOT NULL");

    if (oAddField.Execute(oCommand.GetString()) != CE_None)
        return OGRERR_FAILURE;

    poFeatureDefn->AddFieldDefn(&oField);

    return OGRERR_NONE;
}

/************************************************************************/
/*                            SetDimension()                            */
/************************************************************************/

void OGROCIWritableLayer::SetDimension(int nNewDim)

{
    nDimension = nNewDim;
}

/************************************************************************/
/*                            ParseDIMINFO()                            */
/************************************************************************/

void OGROCIWritableLayer::ParseDIMINFO(const char *pszOptionName,
                                       double *pdfMin, double *pdfMax,
                                       double *pdfRes)

{
    const char *pszUserDIMINFO;
    char **papszTokens;

    pszUserDIMINFO = CSLFetchNameValue(papszOptions, pszOptionName);
    if (pszUserDIMINFO == nullptr)
        return;

    papszTokens = CSLTokenizeStringComplex(pszUserDIMINFO, ",", FALSE, FALSE);
    if (CSLCount(papszTokens) != 3)
    {
        CSLDestroy(papszTokens);
        CPLError(
            CE_Warning, CPLE_AppDefined,
            "Ignoring %s, it does not contain three comma separated values.",
            pszOptionName);
        return;
    }

    *pdfMin = CPLAtof(papszTokens[0]);
    *pdfMax = CPLAtof(papszTokens[1]);
    *pdfRes = CPLAtof(papszTokens[2]);

    CSLDestroy(papszTokens);
}

/************************************************************************/
/*                       TranslateToSDOGeometry()                       */
/************************************************************************/

OGRErr OGROCIWritableLayer::TranslateToSDOGeometry(OGRGeometry *poGeometry,
                                                   int *pnGType)

{
    nOrdinalCount = 0;
    nElemInfoCount = 0;

    if (poGeometry == nullptr)
        return OGRERR_FAILURE;

    /* ==================================================================== */
    /*      Handle a point geometry.                                        */
    /* ==================================================================== */
    if (wkbFlatten(poGeometry->getGeometryType()) == wkbPoint)
    {
#ifdef notdef
        char szResult[1024];
        OGRPoint *poPoint = poGeometry->toPoint();

        if (nDimension == 2)
            CPLsprintf(
                szResult,
                "%s(%d,%s,MDSYS.SDO_POINT_TYPE(%.16g,%.16g,0.0),NULL,NULL)",
                SDO_GEOMETRY, 2001, szSRID, poPoint->getX(), poPoint->getY());
        else
            CPLsprintf(
                szResult,
                "%s(%d,%s,MDSYS.SDO_POINT_TYPE(%.16g,%.16g,%.16g),NULL,NULL)",
                SDO_GEOMETRY, 3001, szSRID, poPoint->getX(), poPoint->getY(),
                poPoint->getZ());

        return CPLStrdup(szResult);
#endif
    }

    /* ==================================================================== */
    /*      Handle a line string geometry.                                  */
    /* ==================================================================== */
    else if (wkbFlatten(poGeometry->getGeometryType()) == wkbLineString)
    {
        *pnGType = nDimension * 1000 + 2;
        TranslateElementGroup(poGeometry);
        return OGRERR_NONE;
    }

    /* ==================================================================== */
    /*      Handle a polygon geometry.                                      */
    /* ==================================================================== */
    else if (wkbFlatten(poGeometry->getGeometryType()) == wkbPolygon)
    {
        *pnGType = nDimension == 2 ? 2003 : 3003;
        TranslateElementGroup(poGeometry);
        return OGRERR_NONE;
    }

    /* ==================================================================== */
    /*      Handle a multi point geometry.                                  */
    /* ==================================================================== */
    else if (wkbFlatten(poGeometry->getGeometryType()) == wkbMultiPoint)
    {
        OGRMultiPoint *poMP = poGeometry->toMultiPoint();

        *pnGType = nDimension * 1000 + 5;
        PushElemInfo(1, 1, poMP->getNumGeometries());

        for (auto &&poPoint : *poMP)
        {
            PushOrdinal(poPoint->getX());
            PushOrdinal(poPoint->getY());
            if (nDimension == 3)
                PushOrdinal(poPoint->getZ());
        }

        return OGRERR_NONE;
    }

    /* ==================================================================== */
    /*      Handle other geometry collections.                              */
    /* ==================================================================== */
    else
    {
        /* --------------------------------------------------------------------
         */
        /*      Identify the GType. */
        /* --------------------------------------------------------------------
         */
        if (wkbFlatten(poGeometry->getGeometryType()) == wkbMultiLineString)
            *pnGType = nDimension * 1000 + 6;
        else if (wkbFlatten(poGeometry->getGeometryType()) == wkbMultiPolygon)
            *pnGType = nDimension * 1000 + 7;
        else if (wkbFlatten(poGeometry->getGeometryType()) ==
                 wkbGeometryCollection)
            *pnGType = nDimension * 1000 + 4;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unexpected geometry type (%d/%s) in "
                     "OGROCIWritableLayer::TranslateToSDOGeometry()",
                     poGeometry->getGeometryType(),
                     poGeometry->getGeometryName());
            return OGRERR_FAILURE;
        }

        /* --------------------------------------------------------------------
         */
        /*      Translate each child in turn. */
        /* --------------------------------------------------------------------
         */
        OGRGeometryCollection *poGC = poGeometry->toGeometryCollection();
        for (auto &&poMember : *poGC)
            TranslateElementGroup(poMember);

        return OGRERR_NONE;
    }

    return OGRERR_FAILURE;
}

int OGROCIWritableLayer::FindFieldIndex(const char *pszFieldName,
                                        int bExactMatch)
{
    int iField = GetLayerDefn()->GetFieldIndex(pszFieldName);

    if (!bExactMatch && iField < 0)
    {
        // try laundered version
        OGROCISession *poSession = poDS->GetSession();
        char *pszSafeName = CPLStrdup(pszFieldName);

        poSession->CleanName(pszSafeName);

        iField = GetLayerDefn()->GetFieldIndex(pszSafeName);

        CPLFree(pszSafeName);
    }

    return iField;
}
