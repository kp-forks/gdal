/******************************************************************************
 *
 * Project:  libgeotiff
 * Purpose:  Code to convert a normalized GeoTIFF definition into a PROJ.4
 *           (OGDI) compatible projection string.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cpl_serv.h"
#include "geotiff.h"
#include "geo_normalize.h"
#include "geovalues.h"
#include "geo_tiffp.h"
#include "proj.h"

/************************************************************************/
/*                    GTIFProj4AppendEllipsoid()                        */
/************************************************************************/

static void GTIFProj4AppendEllipsoid(GTIFDefn* psDefn, char* pszProjection)
{
/* ==================================================================== */
/*      Handle ellipsoid information.                                   */
/* ==================================================================== */
    if( psDefn->Ellipsoid == Ellipse_WGS_84 )
        strcat( pszProjection, "+ellps=WGS84 " );
    else if( psDefn->Ellipsoid == Ellipse_Clarke_1866 )
        strcat( pszProjection, "+ellps=clrk66 " );
    else if( psDefn->Ellipsoid == Ellipse_Clarke_1880 )
        strcat( pszProjection, "+ellps=clrk80 " );
    else if( psDefn->Ellipsoid == Ellipse_GRS_1980 )
        strcat( pszProjection, "+ellps=GRS80 " );
    else
    {
        if( psDefn->SemiMajor != 0.0 && psDefn->SemiMinor != 0.0 )
        {
            sprintf( pszProjection+strlen(pszProjection),
                     "+a=%.3f +b=%.3f ",
                     psDefn->SemiMajor,
                     psDefn->SemiMinor );
        }
    }
}

/************************************************************************/
/*                          OSRProj4Tokenize()                          */
/*                                                                      */
/*      Custom tokenizing function for PROJ.4 strings.  The main        */
/*      reason we can't just use CSLTokenizeString is to handle         */
/*      strings with a + sign in the exponents of parameter values.     */
/************************************************************************/

static char **OSRProj4Tokenize( const char *pszFull )

{
    static const int nMaxTokens = 200;

    if( pszFull == NULL )
        return NULL;

    char **papszTokens = (char **) calloc(nMaxTokens, sizeof(char*));

    char *pszFullWrk = CPLStrdup(pszFull);

    int  nTokens = 0;
    char *pszStart = NULL;
    for( int i=0; pszFullWrk[i] != '\0' && nTokens != nMaxTokens-1; i++ )
    {
        switch( pszFullWrk[i] )
        {
          case '+':
            if( i == 0 || pszFullWrk[i-1] == '\0' )
            {
                if( pszStart != NULL )
                {
                    if( strstr(pszStart,"=") != NULL )
                    {
                        papszTokens[nTokens++] = CPLStrdup(pszStart);
                    }
                    else
                    {
                        char szAsBoolean[100];
                        strncpy( szAsBoolean,pszStart, sizeof(szAsBoolean)-1-4);
                        szAsBoolean[sizeof(szAsBoolean)-1-4] = '\0';
                        strcat( szAsBoolean,"=yes" );
                        papszTokens[nTokens++] = CPLStrdup(szAsBoolean);
                    }
                }
                pszStart = pszFullWrk + i + 1;
            }
            break;

          case ' ':
          case '\t':
          case '\n':
            pszFullWrk[i] = '\0';
            break;

          default:
            break;
        }
    }

    if( pszStart != NULL && strlen(pszStart) > 0 )
    {
        if (nTokens != 199)
            papszTokens[nTokens++] = CPLStrdup(pszStart);
    }

    CPLFree( pszFullWrk );

    return papszTokens;
}


/************************************************************************/
/*                              OSR_GSV()                               */
/************************************************************************/

static const char *OSR_GSV( char **papszNV, const char * pszField )

{
    if( !papszNV )
        return NULL;

    const size_t field_len = strlen(pszField);

    for( int i = 0; papszNV[i] != NULL; i++ )
    {
        if( EQUALN(papszNV[i],pszField,field_len) )
        {
            if( papszNV[i][field_len] == '=' )
                return papszNV[i] + field_len + 1;

            if( strlen(papszNV[i]) == field_len )
                return "";
        }
    }

    return NULL;
}

/************************************************************************/
/*                              OSR_GDV()                               */
/*                                                                      */
/*      Fetch a particular parameter out of the parameter list, or      */
/*      the indicated default if it isn't available.  This is a         */
/*      helper function for importFromProj4().                          */
/************************************************************************/

static double OSR_GDV( char **papszNV, const char * pszField,
                       double dfDefaultValue )

{
    const char *pszValue = OSR_GSV( papszNV, pszField );

    /* special hack to use k_0 if available. */
    if( pszValue == NULL && EQUAL(pszField,"k") )
        return OSR_GDV( papszNV, "k_0", dfDefaultValue );

    if( pszValue == NULL )
        return dfDefaultValue;
    else
        return GTIFAtof(pszValue);
}

/************************************************************************/
/*                         OSRFreeStringList()                          */
/************************************************************************/

static void OSRFreeStringList( char ** list )

{
    for( int i = 0; list != NULL && list[i] != NULL; i++ )
        free( list[i] );
    free(list);
}


/************************************************************************/
/*                          GTIFSetFromProj4()                          */
/************************************************************************/

int GTIFSetFromProj4( GTIF *gtif, const char *proj4 )

{
    char **papszNV = OSRProj4Tokenize( proj4 );
    short nSpheroid = KvUserDefined;
    double dfSemiMajor=0.0, dfSemiMinor=0.0, dfInvFlattening=0.0;

/* -------------------------------------------------------------------- */
/*      Get the ellipsoid definition.                                   */
/* -------------------------------------------------------------------- */
    const char  *value = OSR_GSV( papszNV, "ellps" );

    if( value == NULL )
    {
        /* nothing */;
    }
    else if( EQUAL(value,"WGS84") )
        nSpheroid = Ellipse_WGS_84;
    else if( EQUAL(value,"clrk66") )
        nSpheroid = Ellipse_Clarke_1866;
    else if( EQUAL(value,"clrk80") )
        nSpheroid = Ellipse_Clarke_1880;
    else if( EQUAL(value,"GRS80") )
        nSpheroid = Ellipse_GRS_1980;

    if( nSpheroid == KvUserDefined )
    {
        dfSemiMajor = OSR_GDV(papszNV,"a",0.0);
        dfSemiMinor = OSR_GDV(papszNV,"b",0.0);
        dfInvFlattening = OSR_GDV(papszNV,"rf",0.0);
        if( dfSemiMajor != 0.0 && dfSemiMinor != 0.0 && dfInvFlattening == 0.0 )
            dfInvFlattening = -1.0 / (dfSemiMinor/dfSemiMajor - 1.0);
    }

/* -------------------------------------------------------------------- */
/*      Get the GCS/Datum code.                                         */
/* -------------------------------------------------------------------- */
    value = OSR_GSV( papszNV, "datum" );

    int nDatum = KvUserDefined;
    int nGCS = KvUserDefined;

    if( value == NULL )
    {
    }
    else if( EQUAL(value,"WGS84") )
    {
        nGCS = GCS_WGS_84;
        nDatum = Datum_WGS84;
    }
    else if( EQUAL(value,"NAD83") )
    {
        nGCS = GCS_NAD83;
        nDatum = Datum_North_American_Datum_1983;
    }
    else if( EQUAL(value,"NAD27") )
    {
        nGCS = GCS_NAD27;
        nDatum = Datum_North_American_Datum_1927;
    }

/* -------------------------------------------------------------------- */
/*      Operate on the basis of the projection name.                    */
/* -------------------------------------------------------------------- */
    value = OSR_GSV(papszNV,"proj");

    if( value == NULL )
    {
        OSRFreeStringList( papszNV );
        return FALSE;
    }

    else if( EQUAL(value,"longlat") || EQUAL(value,"latlong") )
    {
    }

    else if( EQUAL(value,"tmerc") )
    {
	GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1,
                   ModelTypeProjected);
	GTIFKeySet(gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );
	GTIFKeySet(gtif, ProjectionGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );

	GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
		   CT_TransverseMercator );

        GTIFKeySet(gtif, ProjNatOriginLatGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lat_0", 0.0 ) );

        GTIFKeySet(gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lon_0", 0.0 ) );

        GTIFKeySet(gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "k", 1.0 ) );

        GTIFKeySet(gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "x_0", 0.0 ) );

        GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"utm") )
    {
        int nZone = (int) OSR_GDV(papszNV,"zone",0);
        const char *south = OSR_GSV(papszNV,"south");

	GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1,
                   ModelTypeProjected);
	GTIFKeySet(gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );
	GTIFKeySet(gtif, ProjectionGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );

	GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
		   CT_TransverseMercator );

        GTIFKeySet(gtif, ProjNatOriginLatGeoKey, TYPE_DOUBLE, 1,
                   0.0 );

        GTIFKeySet(gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1,
                   nZone * 6 - 183.0 );

        GTIFKeySet(gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1,
                   0.9996 );

        GTIFKeySet(gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1,
                   500000.0 );

        if( south != NULL )
            GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1,
                       10000000.0 );
        else
            GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1,
                       0.0 );
    }

    else if( EQUAL(value,"lcc")
             && OSR_GDV(papszNV, "lat_0", 0.0 )
             == OSR_GDV(papszNV, "lat_1", 0.0 ) )
    {
	GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1,
                   ModelTypeProjected);
	GTIFKeySet(gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );
	GTIFKeySet(gtif, ProjectionGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );

	GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
		   CT_LambertConfConic_1SP );

        GTIFKeySet(gtif, ProjNatOriginLatGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lat_0", 0.0 ) );

        GTIFKeySet(gtif, ProjNatOriginLongGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lon_0", 0.0 ) );

        GTIFKeySet(gtif, ProjScaleAtNatOriginGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "k", 1.0 ) );

        GTIFKeySet(gtif, ProjFalseEastingGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "x_0", 0.0 ) );

        GTIFKeySet(gtif, ProjFalseNorthingGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"lcc") )
    {
	GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1,
                   ModelTypeProjected);
	GTIFKeySet(gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );
	GTIFKeySet(gtif, ProjectionGeoKey, TYPE_SHORT, 1,
                   KvUserDefined );

	GTIFKeySet(gtif, ProjCoordTransGeoKey, TYPE_SHORT, 1,
		   CT_LambertConfConic_2SP );

        GTIFKeySet(gtif, ProjFalseOriginLatGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lat_0", 0.0 ) );

        GTIFKeySet(gtif, ProjFalseOriginLongGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lon_0", 0.0 ) );

        GTIFKeySet(gtif, ProjStdParallel1GeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lat_1", 0.0 ) );

        GTIFKeySet(gtif, ProjStdParallel2GeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "lat_2", 0.0 ) );

        GTIFKeySet(gtif, ProjFalseOriginEastingGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "x_0", 0.0 ) );

        GTIFKeySet(gtif, ProjFalseOriginNorthingGeoKey, TYPE_DOUBLE, 1,
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

#ifdef notdef
    else if( EQUAL(value,"bonne") )
    {
        SetBonne( OSR_GDV( papszNV, "lat_1", 0.0 ),
                  OSR_GDV( papszNV, "lon_0", 0.0 ),
                  OSR_GDV( papszNV, "x_0", 0.0 ),
                  OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"cass") )
    {
        SetCS( OSR_GDV( papszNV, "lat_0", 0.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"nzmg") )
    {
        SetNZMG( OSR_GDV( papszNV, "lat_0", -41.0 ),
                 OSR_GDV( papszNV, "lon_0", 173.0 ),
                 OSR_GDV( papszNV, "x_0", 2510000.0 ),
                 OSR_GDV( papszNV, "y_0", 6023150.0 ) );
    }

    else if( EQUAL(value,"cea") )
    {
        SetCEA( OSR_GDV( papszNV, "lat_ts", 0.0 ),
                OSR_GDV( papszNV, "lon_0", 0.0 ),
                OSR_GDV( papszNV, "x_0", 0.0 ),
                OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"merc") /* 2SP form */
             && OSR_GDV(papszNV, "lat_ts", 1000.0) < 999.0 )
    {
        SetMercator2SP( OSR_GDV( papszNV, "lat_ts", 0.0 ),
                        0.0,
                        OSR_GDV( papszNV, "lon_0", 0.0 ),
                        OSR_GDV( papszNV, "x_0", 0.0 ),
                        OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"merc") ) /* 1SP form */
    {
        SetMercator( 0.0,
                     OSR_GDV( papszNV, "lon_0", 0.0 ),
                     OSR_GDV( papszNV, "k", 1.0 ),
                     OSR_GDV( papszNV, "x_0", 0.0 ),
                     OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"stere")
             && ABS(OSR_GDV( papszNV, "lat_0", 0.0 ) - 90) < 0.001 )
    {
        SetPS( OSR_GDV( papszNV, "lat_ts", 90.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "k", 1.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"stere")
             && ABS(OSR_GDV( papszNV, "lat_0", 0.0 ) + 90) < 0.001 )
    {
        SetPS( OSR_GDV( papszNV, "lat_ts", -90.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "k", 1.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUALN(value,"stere",5) /* mostly sterea */
             && CSLFetchNameValue(papszNV,"k") != NULL )
    {
        SetOS( OSR_GDV( papszNV, "lat_0", 0.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "k", 1.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"stere") )
    {
        SetStereographic( OSR_GDV( papszNV, "lat_0", 0.0 ),
                          OSR_GDV( papszNV, "lon_0", 0.0 ),
                          1.0,
                          OSR_GDV( papszNV, "x_0", 0.0 ),
                          OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"eqc") )
    {
        if( OSR_GDV( papszNV, "lat_0", 0.0 ) != OSR_GDV( papszNV, "lat_ts", 0.0 ) )
          SetEquirectangular2( OSR_GDV( papszNV, "lat_0", 0.0 ),
                               OSR_GDV( papszNV, "lon_0", 0.0 )+dfFromGreenwich,
                               OSR_GDV( papszNV, "lat_ts", 0.0 ),
                               OSR_GDV( papszNV, "x_0", 0.0 ),
                               OSR_GDV( papszNV, "y_0", 0.0 ) );
        else
          SetEquirectangular( OSR_GDV( papszNV, "lat_ts", 0.0 ),
                              OSR_GDV( papszNV, "lon_0", 0.0 )+dfFromGreenwich,
                              OSR_GDV( papszNV, "x_0", 0.0 ),
                              OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

   else if( EQUAL(value,"glabsgm") )
   {
       SetGaussLabordeReunion( OSR_GDV( papszNV, "lat_0", -21.116666667 ),
                               OSR_GDV( papszNV, "lon_0", 55.53333333309)+dfFromGreenwich,
                               OSR_GDV( papszNV, "k_0", 1.0 ),
                               OSR_GDV( papszNV, "x_0", 160000.000 ),
                               OSR_GDV( papszNV, "y_0", 50000.000 ) );
   }

    else if( EQUAL(value,"gnom") )
    {
        SetGnomonic( OSR_GDV( papszNV, "lat_0", 0.0 ),
                     OSR_GDV( papszNV, "lon_0", 0.0 ),
                     OSR_GDV( papszNV, "x_0", 0.0 ),
                     OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"ortho") )
    {
        SetOrthographic( OSR_GDV( papszNV, "lat_0", 0.0 ),
                         OSR_GDV( papszNV, "lon_0", 0.0 ),
                         OSR_GDV( papszNV, "x_0", 0.0 ),
                         OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"laea") )
    {
        SetLAEA( OSR_GDV( papszNV, "lat_0", 0.0 ),
                 OSR_GDV( papszNV, "lon_0", 0.0 ),
                 OSR_GDV( papszNV, "x_0", 0.0 ),
                 OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"aeqd") )
    {
        SetAE( OSR_GDV( papszNV, "lat_0", 0.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"eqdc") )
    {
        SetEC( OSR_GDV( papszNV, "lat_1", 0.0 ),
               OSR_GDV( papszNV, "lat_2", 0.0 ),
               OSR_GDV( papszNV, "lat_0", 0.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"mill") )
    {
        SetMC( OSR_GDV( papszNV, "lat_0", 0.0 ),
               OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"moll") )
    {
        SetMollweide( OSR_GDV( papszNV, "lon_0", 0.0 ),
                      OSR_GDV( papszNV, "x_0", 0.0 ),
                      OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"eck4") )
    {
        SetEckertIV( OSR_GDV( papszNV, "lon_0", 0.0 ),
                     OSR_GDV( papszNV, "x_0", 0.0 ),
                     OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"eck6") )
    {
        SetEckertVI( OSR_GDV( papszNV, "lon_0", 0.0 ),
                     OSR_GDV( papszNV, "x_0", 0.0 ),
                     OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"poly") )
    {
        SetPolyconic( OSR_GDV( papszNV, "lat_0", 0.0 ),
                      OSR_GDV( papszNV, "lon_0", 0.0 ),
                      OSR_GDV( papszNV, "x_0", 0.0 ),
                      OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"aea") )
    {
        SetACEA( OSR_GDV( papszNV, "lat_1", 0.0 ),
                 OSR_GDV( papszNV, "lat_2", 0.0 ),
                 OSR_GDV( papszNV, "lat_0", 0.0 ),
                 OSR_GDV( papszNV, "lon_0", 0.0 ),
                 OSR_GDV( papszNV, "x_0", 0.0 ),
                 OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"robin") )
    {
        SetRobinson( OSR_GDV( papszNV, "lon_0", 0.0 ),
                     OSR_GDV( papszNV, "x_0", 0.0 ),
                     OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"vandg") )
    {
        SetVDG( OSR_GDV( papszNV, "lon_0", 0.0 ),
                OSR_GDV( papszNV, "x_0", 0.0 ),
                OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"sinu") )
    {
        SetSinusoidal( OSR_GDV( papszNV, "lon_0", 0.0 ),
                       OSR_GDV( papszNV, "x_0", 0.0 ),
                       OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"gall") )
    {
        SetGS( OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"goode") )
    {
        SetGH( OSR_GDV( papszNV, "lon_0", 0.0 ),
               OSR_GDV( papszNV, "x_0", 0.0 ),
               OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"geos") )
    {
        SetGEOS( OSR_GDV( papszNV, "lon_0", 0.0 ),
                 OSR_GDV( papszNV, "h", 35785831.0 ),
                 OSR_GDV( papszNV, "x_0", 0.0 ),
                 OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"lcc") )
    {
        if( OSR_GDV(papszNV, "lat_0", 0.0 )
            == OSR_GDV(papszNV, "lat_1", 0.0 ) )
        {
            /* 1SP form */
            SetLCC1SP( OSR_GDV( papszNV, "lat_0", 0.0 ),
                       OSR_GDV( papszNV, "lon_0", 0.0 ),
                       OSR_GDV( papszNV, "k_0", 1.0 ),
                       OSR_GDV( papszNV, "x_0", 0.0 ),
                       OSR_GDV( papszNV, "y_0", 0.0 ) );
        }
        else
        {
            /* 2SP form */
            SetLCC( OSR_GDV( papszNV, "lat_1", 0.0 ),
                    OSR_GDV( papszNV, "lat_2", 0.0 ),
                    OSR_GDV( papszNV, "lat_0", 0.0 ),
                    OSR_GDV( papszNV, "lon_0", 0.0 ),
                    OSR_GDV( papszNV, "x_0", 0.0 ),
                    OSR_GDV( papszNV, "y_0", 0.0 ) );
        }
    }

    else if( EQUAL(value,"omerc") )
    {
        SetHOM( OSR_GDV( papszNV, "lat_0", 0.0 ),
                OSR_GDV( papszNV, "lonc", 0.0 ),
                OSR_GDV( papszNV, "alpha", 0.0 ),
                0.0, /* ??? */
                OSR_GDV( papszNV, "k", 1.0 ),
                OSR_GDV( papszNV, "x_0", 0.0 ),
                OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"somerc") )
    {
        SetHOM( OSR_GDV( papszNV, "lat_0", 0.0 ),
                OSR_GDV( papszNV, "lon_0", 0.0 ),
                90.0,  90.0,
                OSR_GDV( papszNV, "k", 1.0 ),
                OSR_GDV( papszNV, "x_0", 0.0 ),
                OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"krovak") )
    {
        SetKrovak( OSR_GDV( papszNV, "lat_0", 0.0 ),
                   OSR_GDV( papszNV, "lon_0", 0.0 ),
                   OSR_GDV( papszNV, "alpha", 0.0 ),
                   0.0, /* pseudo_standard_parallel_1 */
                   OSR_GDV( papszNV, "k", 1.0 ),
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "iwm_p") )
    {
        SetIWMPolyconic( OSR_GDV( papszNV, "lat_1", 0.0 ),
                         OSR_GDV( papszNV, "lat_2", 0.0 ),
                         OSR_GDV( papszNV, "lon_0", 0.0 ),
                         OSR_GDV( papszNV, "x_0", 0.0 ),
                         OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag1") )
    {
        SetWagner( 1, 0.0,
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag2") )
    {
        SetWagner( 2, 0.0,
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag3") )
    {
        SetWagner( 3,
                   OSR_GDV( papszNV, "lat_ts", 0.0 ),
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag1") )
    {
        SetWagner( 4, 0.0,
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag1") )
    {
        SetWagner( 5, 0.0,
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag1") )
    {
        SetWagner( 6, 0.0,
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value, "wag1") )
    {
        SetWagner( 7, 0.0,
                   OSR_GDV( papszNV, "x_0", 0.0 ),
                   OSR_GDV( papszNV, "y_0", 0.0 ) );
    }

    else if( EQUAL(value,"tpeqd") )
    {
        SetTPED( OSR_GDV( papszNV, "lat_1", 0.0 ),
                 OSR_GDV( papszNV, "lon_1", 0.0 ),
                 OSR_GDV( papszNV, "lat_2", 0.0 ),
                 OSR_GDV( papszNV, "lon_2", 0.0 ),
                 OSR_GDV( papszNV, "x_0", 0.0 ),
                 OSR_GDV( papszNV, "y_0", 0.0 ) );
    }
#endif
    else
    {
        /* unsupported coordinate system */
        OSRFreeStringList( papszNV );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Write the GCS if we have it, otherwise write the datum.         */
/* -------------------------------------------------------------------- */
    if( nGCS != KvUserDefined )
    {
        GTIFKeySet( gtif, GeographicTypeGeoKey, TYPE_SHORT,
                    1, nGCS );
    }
    else
    {
        GTIFKeySet( gtif, GeographicTypeGeoKey, TYPE_SHORT, 1,
                    KvUserDefined );
        GTIFKeySet( gtif, GeogGeodeticDatumGeoKey, TYPE_SHORT,
                    1, nDatum );
    }

/* -------------------------------------------------------------------- */
/*      Write the ellipsoid if we don't know the GCS.                   */
/* -------------------------------------------------------------------- */
    if( nGCS == KvUserDefined )
    {
        if( nSpheroid != KvUserDefined )
            GTIFKeySet( gtif, GeogEllipsoidGeoKey, TYPE_SHORT, 1,
                        nSpheroid );
        else
        {
            GTIFKeySet( gtif, GeogEllipsoidGeoKey, TYPE_SHORT, 1,
                        KvUserDefined );
            GTIFKeySet( gtif, GeogSemiMajorAxisGeoKey, TYPE_DOUBLE, 1,
                        dfSemiMajor );
            if( dfInvFlattening == 0.0 )
                GTIFKeySet( gtif, GeogSemiMinorAxisGeoKey, TYPE_DOUBLE, 1,
                            dfSemiMajor );
            else
                GTIFKeySet( gtif, GeogInvFlatteningGeoKey, TYPE_DOUBLE, 1,
                            dfInvFlattening );
        }
    }

/* -------------------------------------------------------------------- */
/*      Linear units translation                                        */
/* -------------------------------------------------------------------- */
    value = OSR_GSV( papszNV, "units" );

    if( value == NULL )
    {
        value = OSR_GSV( papszNV, "to_meter" );
        if( value )
        {
            GTIFKeySet( gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1,
                        KvUserDefined );
            GTIFKeySet( gtif, ProjLinearUnitSizeGeoKey, TYPE_DOUBLE, 1,
                        GTIFAtof(value) );
        }
    }
    else if( EQUAL(value,"meter") || EQUAL(value,"m") )
    {
        GTIFKeySet( gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1,
                    Linear_Meter );
    }
    else if( EQUAL(value,"us-ft") )
    {
        GTIFKeySet( gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1,
                    Linear_Foot_US_Survey );
    }
    else if( EQUAL(value,"ft") )
    {
        GTIFKeySet( gtif, ProjLinearUnitsGeoKey, TYPE_SHORT, 1,
                    Linear_Foot );
    }


    OSRFreeStringList( papszNV );

    return TRUE;
}

/************************************************************************/
/*                          GTIFGetProj4Defn()                          */
/************************************************************************/

char * GTIFGetProj4Defn( GTIFDefn * psDefn )

{
    char	szUnits[64];

    if( psDefn == NULL || !psDefn->DefnSet )
        return CPLStrdup("");

/* ==================================================================== */
/*      Translate the units of measure.                                 */
/*                                                                      */
/*      Note that even with a +units, or +to_meter in effect, it is     */
/*      still assumed that all the projection parameters are in         */
/*      meters.                                                         */
/* ==================================================================== */
    if( psDefn->UOMLength == Linear_Meter )
    {
        strcpy( szUnits, "+units=m " );
    }
    else if( psDefn->UOMLength == Linear_Foot )
    {
        strcpy( szUnits, "+units=ft " );
    }
    else if( psDefn->UOMLength == Linear_Foot_US_Survey )
    {
        strcpy( szUnits, "+units=us-ft " );
    }
    else if( psDefn->UOMLength == Linear_Foot_Indian )
    {
        strcpy( szUnits, "+units=ind-ft " );
    }
    else if( psDefn->UOMLength == Linear_Link )
    {
        strcpy( szUnits, "+units=link " );
    }
    else if( psDefn->UOMLength == Linear_Yard_Indian)
    {
        strcpy( szUnits, "+units=ind-yd " );
    }
    else if( psDefn->UOMLength == Linear_Fathom )
    {
        strcpy( szUnits, "+units=fath " );
    }
    else if( psDefn->UOMLength == Linear_Mile_International_Nautical )
    {
        strcpy( szUnits, "+units=kmi " );
    }
    else
    {
        sprintf( szUnits, "+to_meter=%.10f", psDefn->UOMLengthInMeters );
    }

/* -------------------------------------------------------------------- */
/*      false easting and northing are in meters and that is what       */
/*      PROJ.4 wants regardless of the linear units.                    */
/* -------------------------------------------------------------------- */
    const double dfFalseEasting = psDefn->ProjParm[5];
    const double dfFalseNorthing = psDefn->ProjParm[6];

/* ==================================================================== */
/*      Handle general projection methods.                              */
/* ==================================================================== */

/* -------------------------------------------------------------------- */
/*      Geographic.                                                     */
/* -------------------------------------------------------------------- */
    char szProjection[512];
    szProjection[0] = '\0';

    if(psDefn->Model==ModelTypeGeographic)
    {
        sprintf(szProjection+strlen(szProjection),"+proj=latlong ");
    }

/* -------------------------------------------------------------------- */
/*      UTM - special case override on transverse mercator so things    */
/*      will be more meaningful to the user.                            */
/* -------------------------------------------------------------------- */
    else if( psDefn->MapSys == MapSys_UTM_North )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=utm +zone=%d ",
                 psDefn->Zone );
    }

/* -------------------------------------------------------------------- */
/*      Transverse Mercator                                             */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_TransverseMercator )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=tmerc +lat_0=%.9f +lon_0=%.9f +k=%f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[4],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Oblique Mercator                                                */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_ObliqueMercator_Laborde )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=labrd +lat_0=%.9f +lon_0=%.9f +azi=%.9f +k=%f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[2],
                 psDefn->ProjParm[4],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Mercator							*/
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Mercator )
    {
        if( psDefn->ProjParm[2] != 0.0 ) /* Mercator 2SP: FIXME we need a better way of detecting it */
            sprintf( szProjection+strlen(szProjection),
                    "+proj=merc +lat_ts=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                    psDefn->ProjParm[2],
                    psDefn->ProjParm[1],
                    dfFalseEasting,
                    dfFalseNorthing );
        else
            sprintf( szProjection+strlen(szProjection),
                    "+proj=merc +lat_ts=%.9f +lon_0=%.9f +k=%f +x_0=%.3f +y_0=%.3f ",
                    psDefn->ProjParm[0],
                    psDefn->ProjParm[1],
                    psDefn->ProjParm[4],
                    dfFalseEasting,
                    dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Cassini/Soldner                                                 */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_CassiniSoldner )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=cass +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Oblique Stereographic - Should this really map onto             */
/*      Stereographic?                                                  */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_ObliqueStereographic )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=stere +lat_0=%.9f +lon_0=%.9f +k=%f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[4],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Stereographic                                                   */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Stereographic )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=stere +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Polar Stereographic                                             */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_PolarStereographic )
    {
        if( psDefn->ProjParm[0] > 0.0 )
            sprintf( szProjection+strlen(szProjection),
                     "+proj=stere +lat_0=90 +lat_ts=%.9f +lon_0=%.9f "
                     "+k=%.9f +x_0=%.3f +y_0=%.3f ",
                     psDefn->ProjParm[0],
                     psDefn->ProjParm[1],
                     psDefn->ProjParm[4],
                     dfFalseEasting,
                     dfFalseNorthing );
        else
            sprintf( szProjection+strlen(szProjection),
                     "+proj=stere +lat_0=-90 +lat_ts=%.9f +lon_0=%.9f "
                     "+k=%.9f +x_0=%.3f +y_0=%.3f ",
                     psDefn->ProjParm[0],
                     psDefn->ProjParm[1],
                     psDefn->ProjParm[4],
                     dfFalseEasting,
                     dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Equirectangular                                                 */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Equirectangular )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=eqc +lat_0=%.9f +lon_0=%.9f +lat_ts=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[2],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Gnomonic                                                        */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Gnomonic )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=gnom +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Orthographic                                                    */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Orthographic )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=ortho +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Lambert Azimuthal Equal Area                                    */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_LambertAzimEqualArea )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=laea +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Azimuthal Equidistant                                           */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_AzimuthalEquidistant )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=aeqd +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Miller Cylindrical                                              */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_MillerCylindrical )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=mill +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f +R_A ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Polyconic                                                       */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Polyconic )
    {
        sprintf( szProjection+strlen(szProjection),
           "+proj=poly +lat_0=%.9f +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      AlbersEqualArea                                                 */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_AlbersEqualArea )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=aea +lat_1=%.9f +lat_2=%.9f +lat_0=%.9f +lon_0=%.9f"
                 " +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[2],
                 psDefn->ProjParm[3],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      EquidistantConic                                                */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_EquidistantConic )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=eqdc +lat_1=%.9f +lat_2=%.9f +lat_0=%.9f +lon_0=%.9f"
                 " +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[2],
                 psDefn->ProjParm[3],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Robinson                                                        */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Robinson )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=robin +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      VanDerGrinten                                                   */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_VanDerGrinten )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=vandg +lon_0=%.9f +x_0=%.3f +y_0=%.3f +R_A ",
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      Sinusoidal                                                      */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_Sinusoidal )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=sinu +lon_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[1],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      LambertConfConic_2SP                                            */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_LambertConfConic_2SP )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=lcc +lat_0=%.9f +lon_0=%.9f +lat_1=%.9f +lat_2=%.9f "
                 " +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[2],
                 psDefn->ProjParm[3],
                 dfFalseEasting,
                 dfFalseNorthing );
    }

/* -------------------------------------------------------------------- */
/*      LambertConfConic_1SP                                            */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_LambertConfConic_1SP )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=lcc +lat_0=%.9f +lat_1=%.9f +lon_0=%.9f"
                 " +k_0=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[4],
                 psDefn->ProjParm[5],
                 psDefn->ProjParm[6] );
    }

/* -------------------------------------------------------------------- */
/*      CT_CylindricalEqualArea                                         */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_CylindricalEqualArea )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=cea +lat_ts=%.9f +lon_0=%.9f "
                 " +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[5],
                 psDefn->ProjParm[6] );
    }

/* -------------------------------------------------------------------- */
/*      NewZealandMapGrid                                               */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_NewZealandMapGrid )
    {
        sprintf( szProjection+strlen(szProjection),
                 "+proj=nzmg +lat_0=%.9f +lon_0=%.9f"
                 " +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[5],
                 psDefn->ProjParm[6] );
    }

/* -------------------------------------------------------------------- */
/*      Transverse Mercator - south oriented.                           */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_TransvMercator_SouthOriented )
    {
        /* this appears to be an unsupported formulation with PROJ.4 */
    }

/* -------------------------------------------------------------------- */
/*      ObliqueMercator (Hotine)                                        */
/* -------------------------------------------------------------------- */
    else if( psDefn->CTProjection == CT_ObliqueMercator )
    {
        /* not clear how ProjParm[3] - angle from rectified to skewed grid -
           should be applied ... see the +not_rot flag for PROJ.4.
           Just ignoring for now. */

        sprintf( szProjection+strlen(szProjection),
                 "+proj=omerc +lat_0=%.9f +lonc=%.9f +alpha=%.9f"
                 " +k=%.9f +x_0=%.3f +y_0=%.3f ",
                 psDefn->ProjParm[0],
                 psDefn->ProjParm[1],
                 psDefn->ProjParm[2],
                 psDefn->ProjParm[4],
                 psDefn->ProjParm[5],
                 psDefn->ProjParm[6] );
    }

    else if( psDefn->CTProjection == CT_HotineObliqueMercatorAzimuthCenter )
    {
        /* special case for swiss oblique mercator : see GDAL bug 423 */
        if( fabs(psDefn->ProjParm[2] - 90.0) < 0.0001
            && fabs(psDefn->ProjParm[3]-90.0) < 0.0001 )
        {
            sprintf( szProjection+strlen(szProjection),
                     "+proj=somerc +lat_0=%.16g +lon_0=%.16g"
                     " +k_0=%.16g +x_0=%.16g +y_0=%.16g ",
                     psDefn->ProjParm[0],
                     psDefn->ProjParm[1],
                     psDefn->ProjParm[4],
                     psDefn->ProjParm[5],
                     psDefn->ProjParm[6] );
        }
        else
        {
            sprintf( szProjection+strlen(szProjection),
                     "+proj=omerc +lat_0=%.16g +lonc=%.16g +alpha=%.16g"
                     " +k=%.16g +x_0=%.16g +y_0=%.16g ",
                     psDefn->ProjParm[0],
                     psDefn->ProjParm[1],
                     psDefn->ProjParm[2],
                     psDefn->ProjParm[4],
                     psDefn->ProjParm[5],
                     psDefn->ProjParm[6] );

            /* RSO variant - http://trac.osgeo.org/proj/ticket/62 */
            /* Note that gamma is only supported by PROJ 4.8.0 and later. */
            /* FIXME: how to detect that gamma isn't set to default value */
            /*if( psDefn->ProjParm[3] != 0.0 )
            {
                sprintf( szProjection+strlen(szProjection), "+gamma=%.16g ",
                         psDefn->ProjParm[3] );
            }*/
        }
    }

    GTIFProj4AppendEllipsoid(psDefn, szProjection);

    strcat( szProjection, szUnits );

    /* If we don't have anything, reset */
    if (strstr(szProjection, "+proj=") == NULL) { return CPLStrdup(""); }

    return CPLStrdup( szProjection );
}

/************************************************************************/
/*                        GTIFProj4FromLatLong()                        */
/*                                                                      */
/*      Convert lat/long values to projected coordinate for a           */
/*      particular definition.                                          */
/************************************************************************/

int GTIFProj4FromLatLong( GTIFDefn * psDefn, int nPoints,
                          double *padfX, double *padfY )

{

/* -------------------------------------------------------------------- */
/*      Get a projection definition.                                    */
/* -------------------------------------------------------------------- */
    char *pszProjection = GTIFGetProj4Defn( psDefn );

    if( pszProjection == NULL )
        return FALSE;

    PJ_CONTEXT* ctx = proj_context_create();

    char szLongLat[256];
    strcpy(szLongLat, "+proj=longlat ");
    GTIFProj4AppendEllipsoid(psDefn, szLongLat);

    PJ *psPJ = proj_create_crs_to_crs( ctx, szLongLat, pszProjection, NULL );
    CPLFree( pszProjection );

    if( psPJ == NULL )
    {
        proj_context_destroy(ctx);
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Process each of the points.                                     */
/* -------------------------------------------------------------------- */
    for( int i = 0; i < nPoints; i++ )
    {
        PJ_COORD coord;
        coord.xyzt.x = padfX[i];
        coord.xyzt.y = padfY[i];
        coord.xyzt.z = 0;
        coord.xyzt.t = 0;

        coord = proj_trans(psPJ, PJ_FWD, coord);

        padfX[i] = coord.xyzt.x;
        padfY[i] = coord.xyzt.y;
    }

    proj_destroy( psPJ );
    proj_context_destroy(ctx);

    return TRUE;
}

/************************************************************************/
/*                         GTIFProj4ToLatLong()                         */
/*                                                                      */
/*      Convert projection coordinates to lat/long for a particular     */
/*      definition.                                                     */
/************************************************************************/

int GTIFProj4ToLatLong( GTIFDefn * psDefn, int nPoints,
                        double *padfX, double *padfY )

{
/* -------------------------------------------------------------------- */
/*      Get a projection definition.                                    */
/* -------------------------------------------------------------------- */
    char *pszProjection = GTIFGetProj4Defn( psDefn );

    if( pszProjection == NULL )
        return FALSE;

    PJ_CONTEXT* ctx = proj_context_create();

    char szLongLat[256];
    strcpy(szLongLat, "+proj=longlat ");
    GTIFProj4AppendEllipsoid(psDefn, szLongLat);

    PJ *psPJ = proj_create_crs_to_crs( ctx, pszProjection, szLongLat, NULL );
    CPLFree( pszProjection );

    if( psPJ == NULL )
    {
        proj_context_destroy(ctx);
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Process each of the points.                                     */
/* -------------------------------------------------------------------- */
    for( int i = 0; i < nPoints; i++ )
    {
        PJ_COORD coord;
        coord.xyzt.x = padfX[i];
        coord.xyzt.y = padfY[i];
        coord.xyzt.z = 0;
        coord.xyzt.t = 0;

        coord = proj_trans(psPJ, PJ_FWD, coord);

        padfX[i] = coord.xyzt.x;
        padfY[i] = coord.xyzt.y;
    }

    proj_destroy( psPJ );
    proj_context_destroy(ctx);

    return TRUE;
}
