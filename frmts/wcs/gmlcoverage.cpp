/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Generic support for GML Coverage descriptions.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2006, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal_priv.h"

#include <cstdlib>
#include <cstring>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minixml.h"
#include "cpl_string.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "gmlcoverage.h"

/************************************************************************/
/*                        ParseGMLCoverageDesc()                        */
/************************************************************************/

CPLErr WCSParseGMLCoverage(CPLXMLNode *psXML, int *pnXSize, int *pnYSize,
                           GDALGeoTransform &gt, char **ppszProjection)

{
    CPLStripXMLNamespace(psXML, nullptr, TRUE);

    /* -------------------------------------------------------------------- */
    /*      Isolate RectifiedGrid.  Eventually we will need to support      */
    /*      other georeferencing objects.                                   */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psRG = CPLSearchXMLNode(psXML, "=RectifiedGrid");
    CPLXMLNode *psOriginPoint = nullptr;
    const char *pszOffset1 = nullptr;
    const char *pszOffset2 = nullptr;

    if (psRG != nullptr)
    {
        psOriginPoint = CPLGetXMLNode(psRG, "origin.Point");
        if (psOriginPoint == nullptr)
            psOriginPoint = CPLGetXMLNode(psRG, "origin");

        CPLXMLNode *psOffset1 = CPLGetXMLNode(psRG, "offsetVector");
        if (psOffset1 != nullptr)
        {
            pszOffset1 = CPLGetXMLValue(psOffset1, "", nullptr);
            pszOffset2 =
                CPLGetXMLValue(psOffset1->psNext, "=offsetVector", nullptr);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      If we are missing any of the origin or 2 offsets then give up.  */
    /* -------------------------------------------------------------------- */
    if (psRG == nullptr || psOriginPoint == nullptr || pszOffset1 == nullptr ||
        pszOffset2 == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to find GML RectifiedGrid, origin or offset vectors");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Search for the GridEnvelope and derive the raster size.         */
    /* -------------------------------------------------------------------- */
    char **papszLow =
        CSLTokenizeString(CPLGetXMLValue(psRG, "limits.GridEnvelope.low", ""));
    char **papszHigh =
        CSLTokenizeString(CPLGetXMLValue(psRG, "limits.GridEnvelope.high", ""));

    if (CSLCount(papszLow) < 2 || CSLCount(papszHigh) < 2)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to find or parse GridEnvelope.low/high.");
        CSLDestroy(papszLow);
        CSLDestroy(papszHigh);
        return CE_Failure;
    }

    if (pnXSize != nullptr)
        *pnXSize = atoi(papszHigh[0]) - atoi(papszLow[0]) + 1;
    if (pnYSize != nullptr)
        *pnYSize = atoi(papszHigh[1]) - atoi(papszLow[1]) + 1;

    CSLDestroy(papszLow);
    CSLDestroy(papszHigh);

    /* -------------------------------------------------------------------- */
    /*      Extract origin location.                                        */
    /* -------------------------------------------------------------------- */
    OGRPoint *poOriginGeometry = nullptr;
    std::unique_ptr<OGRGeometry> poGeom;
    const char *pszSRSName = nullptr;

    {
        bool bOldWrap = false;

        // Old coverages (i.e. WCS) just have <pos> under <origin>, so we
        // may need to temporarily force <origin> to <Point>.
        if (psOriginPoint->eType == CXT_Element &&
            EQUAL(psOriginPoint->pszValue, "origin"))
        {
            strcpy(psOriginPoint->pszValue, "Point");
            bOldWrap = true;
        }
        poGeom.reset(
            OGRGeometry::FromHandle(OGR_G_CreateFromGMLTree(psOriginPoint)));

        if (poGeom != nullptr &&
            wkbFlatten(poGeom->getGeometryType()) == wkbPoint)
        {
            poOriginGeometry = poGeom->toPoint();
        }

        if (bOldWrap)
            strcpy(psOriginPoint->pszValue, "origin");

        // SRS?
        pszSRSName = CPLGetXMLValue(psOriginPoint, "srsName", nullptr);
    }

    /* -------------------------------------------------------------------- */
    /*      Extract offset(s)                                               */
    /* -------------------------------------------------------------------- */
    bool bSuccess = false;

    char **papszOffset1Tokens =
        CSLTokenizeStringComplex(pszOffset1, " ,", FALSE, FALSE);
    char **papszOffset2Tokens =
        CSLTokenizeStringComplex(pszOffset2, " ,", FALSE, FALSE);

    if (CSLCount(papszOffset1Tokens) >= 2 &&
        CSLCount(papszOffset2Tokens) >= 2 && poOriginGeometry != nullptr)
    {
        gt[0] = poOriginGeometry->getX();
        gt[1] = CPLAtof(papszOffset1Tokens[0]);
        gt[2] = CPLAtof(papszOffset1Tokens[1]);
        gt[3] = poOriginGeometry->getY();
        gt[4] = CPLAtof(papszOffset2Tokens[0]);
        gt[5] = CPLAtof(papszOffset2Tokens[1]);

        // offset from center of pixel.
        gt[0] -= gt[1] * 0.5;
        gt[0] -= gt[2] * 0.5;
        gt[3] -= gt[4] * 0.5;
        gt[3] -= gt[5] * 0.5;

        bSuccess = true;
    }

    CSLDestroy(papszOffset1Tokens);
    CSLDestroy(papszOffset2Tokens);

    /* -------------------------------------------------------------------- */
    /*      If we have gotten a geotransform, then try to interpret the     */
    /*      srsName.                                                        */
    /* -------------------------------------------------------------------- */
    if (bSuccess && pszSRSName != nullptr &&
        (*ppszProjection == nullptr || strlen(*ppszProjection) == 0))
    {
        if (STARTS_WITH_CI(pszSRSName, "epsg:"))
        {
            OGRSpatialReference oSRS;
            if (oSRS.SetFromUserInput(pszSRSName) == OGRERR_NONE)
                oSRS.exportToWkt(ppszProjection);
        }
        else if (STARTS_WITH_CI(pszSRSName, "urn:ogc:def:crs:"))
        {
            OGRSpatialReference oSRS;
            if (oSRS.importFromURN(pszSRSName) == OGRERR_NONE)
                oSRS.exportToWkt(ppszProjection);
        }
        else
            *ppszProjection = CPLStrdup(pszSRSName);
    }

    if (*ppszProjection)
        CPLDebug("GDALJP2Metadata", "Got projection from GML box: %s",
                 *ppszProjection);

    return CE_None;
}
