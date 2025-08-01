/******************************************************************************
 *
 * Project:  WMS Client Driver
 * Purpose:  Implementation of Dataset and RasterBand classes for WMS
 *           and other similar services.
 * Author:   Adam Nowacki, nowak@xpam.de
 *
 ******************************************************************************
 * Copyright (c) 2007, Adam Nowacki
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2017, Dmitry Baryshnikov, <polimax@mail.ru>
 * Copyright (c) 2017, NextGIS, <info@nextgis.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************
 *
 * dataset.cpp:
 * Initialization of the GDALWMSdriver, parsing the XML configuration file,
 * instantiation of the minidrivers and accessors used by minidrivers.
 *
 ***************************************************************************/

#include "wmsdriver.h"
#include "minidriver_wms.h"
#include "minidriver_tileservice.h"
#include "minidriver_worldwind.h"
#include "minidriver_tms.h"
#include "minidriver_tiled_wms.h"
#include "minidriver_virtualearth.h"

#include <algorithm>

/************************************************************************/
/*                           GDALWMSDataset()                           */
/************************************************************************/
GDALWMSDataset::GDALWMSDataset()
    : m_mini_driver(nullptr), m_cache(nullptr), m_poColorTable(nullptr),
      m_data_type(GDT_Byte), m_block_size_x(0), m_block_size_y(0),
      m_use_advise_read(0), m_verify_advise_read(0), m_offline_mode(0),
      m_http_max_conn(0), m_http_timeout(0), m_http_options(nullptr),
      m_tileOO(nullptr), m_clamp_requests(true), m_unsafeSsl(false),
      m_zeroblock_on_serverexceptions(0), m_default_block_size_x(1024),
      m_default_block_size_y(1024), m_default_tile_count_x(1),
      m_default_tile_count_y(1), m_default_overview_count(-1),
      m_bNeedsDataWindow(true)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_hint.m_valid = false;
    m_data_window.m_sx = -1;
    nBands = 0;
}

/************************************************************************/
/*                          ~GDALWMSDataset()                           */
/************************************************************************/
GDALWMSDataset::~GDALWMSDataset()
{
    if (m_mini_driver)
        delete m_mini_driver;
    if (m_cache)
        delete m_cache;
    if (m_poColorTable)
        delete m_poColorTable;
    CSLDestroy(m_http_options);
    CSLDestroy(m_tileOO);
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/
CPLErr GDALWMSDataset::Initialize(CPLXMLNode *config, char **l_papszOpenOptions)
{
    CPLErr ret = CE_None;

    char *pszXML = CPLSerializeXMLTree(config);
    if (pszXML)
    {
        m_osXML = pszXML;
        CPLFree(pszXML);
    }

    // Generic options that apply to all minidrivers

    // UserPwd
    const char *pszUserPwd = CPLGetXMLValue(config, "UserPwd", "");
    if (pszUserPwd[0] != '\0')
        m_osUserPwd = pszUserPwd;

    const char *pszUserAgent = CPLGetXMLValue(config, "UserAgent", "");
    if (pszUserAgent[0] != '\0')
        m_osUserAgent = pszUserAgent;
    else
        m_osUserAgent = CPLGetConfigOption("GDAL_HTTP_USERAGENT", "");

    const char *pszReferer = CPLGetXMLValue(config, "Referer", "");
    if (pszReferer[0] != '\0')
        m_osReferer = pszReferer;

    {
        const char *pszHttpZeroBlockCodes =
            CPLGetXMLValue(config, "ZeroBlockHttpCodes", "");
        if (pszHttpZeroBlockCodes[0] == '\0')
        {
            m_http_zeroblock_codes.insert(204);
        }
        else
        {
            char **kv = CSLTokenizeString2(pszHttpZeroBlockCodes, ",",
                                           CSLT_HONOURSTRINGS);
            for (int i = 0; i < CSLCount(kv); i++)
            {
                int code = atoi(kv[i]);
                if (code <= 0)
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "GDALWMS: Invalid value of ZeroBlockHttpCodes "
                        "\"%s\", comma separated HTTP response codes expected.",
                        kv[i]);
                    ret = CE_Failure;
                    break;
                }
                m_http_zeroblock_codes.insert(code);
            }
            CSLDestroy(kv);
        }
    }

    if (ret == CE_None)
    {
        const char *pszZeroExceptions =
            CPLGetXMLValue(config, "ZeroBlockOnServerException", "");
        if (pszZeroExceptions[0] != '\0')
        {
            m_zeroblock_on_serverexceptions = StrToBool(pszZeroExceptions);
            if (m_zeroblock_on_serverexceptions == -1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Invalid value of ZeroBlockOnServerException "
                         "\"%s\", true/false expected.",
                         pszZeroExceptions);
                ret = CE_Failure;
            }
        }
    }

    if (ret == CE_None)
    {
        const char *max_conn = CPLGetXMLValue(config, "MaxConnections", "");
        if (max_conn[0] == '\0')
        {
            max_conn = CPLGetConfigOption("GDAL_MAX_CONNECTIONS", "");
        }
        if (max_conn[0] != '\0')
        {
            m_http_max_conn = atoi(max_conn);
        }
        else
        {
            m_http_max_conn = 2;
        }
    }

    if (ret == CE_None)
    {
        const char *timeout = CPLGetXMLValue(config, "Timeout", "");
        if (timeout[0] != '\0')
        {
            m_http_timeout = atoi(timeout);
        }
        else
        {
            m_http_timeout =
                atoi(CPLGetConfigOption("GDAL_HTTP_TIMEOUT", "300"));
        }
    }

    if (ret == CE_None)
    {
        m_osAccept = CPLGetXMLValue(config, "Accept", "");
    }

    if (ret == CE_None)
    {
        const char *offline_mode = CPLGetXMLValue(config, "OfflineMode", "");
        if (offline_mode[0] != '\0')
        {
            const int offline_mode_bool = StrToBool(offline_mode);
            if (offline_mode_bool == -1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Invalid value of OfflineMode, true / false "
                         "expected.");
                ret = CE_Failure;
            }
            else
            {
                m_offline_mode = offline_mode_bool;
            }
        }
        else
        {
            m_offline_mode = 0;
        }
    }

    if (ret == CE_None)
    {
        const char *advise_read = CPLGetXMLValue(config, "AdviseRead", "");
        if (advise_read[0] != '\0')
        {
            const int advise_read_bool = StrToBool(advise_read);
            if (advise_read_bool == -1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Invalid value of AdviseRead, true / false "
                         "expected.");
                ret = CE_Failure;
            }
            else
            {
                m_use_advise_read = advise_read_bool;
            }
        }
        else
        {
            m_use_advise_read = 0;
        }
    }

    if (ret == CE_None)
    {
        const char *verify_advise_read =
            CPLGetXMLValue(config, "VerifyAdviseRead", "");
        if (m_use_advise_read)
        {
            if (verify_advise_read[0] != '\0')
            {
                const int verify_advise_read_bool =
                    StrToBool(verify_advise_read);
                if (verify_advise_read_bool == -1)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "GDALWMS: Invalid value of VerifyAdviseRead, true "
                             "/ false expected.");
                    ret = CE_Failure;
                }
                else
                {
                    m_verify_advise_read = verify_advise_read_bool;
                }
            }
            else
            {
                m_verify_advise_read = 1;
            }
        }
    }

    CPLXMLNode *service_node = CPLGetXMLNode(config, "Service");
    if (service_node == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "GDALWMS: No Service specified.");
        return CE_Failure;
    }

    if (ret == CE_None)
    {
        const char *pszEnableCache =
            CPLGetConfigOption("GDAL_ENABLE_WMS_CACHE", "YES");
        CPLXMLNode *cache_node = CPLGetXMLNode(config, "Cache");
        if (cache_node != nullptr && CPLTestBool(pszEnableCache))
        {
            m_cache = new GDALWMSCache();
            if (m_cache->Initialize(
                    CPLGetXMLValue(service_node, "ServerUrl", nullptr),
                    cache_node) != CE_None)
            {
                delete m_cache;
                m_cache = nullptr;
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Failed to initialize cache.");
                ret = CE_Failure;
            }
            else
            {
                // NOTE: Save cache path to metadata. For example, this is
                // useful for deleting a cache folder when removing dataset or
                // to fill the cache for specified area and zoom levels
                SetMetadataItem("CACHE_PATH", m_cache->CachePath(), nullptr);
            }
        }
    }

    if (ret == CE_None)
    {
        const int v = StrToBool(CPLGetXMLValue(config, "UnsafeSSL", "false"));
        if (v == -1)
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "GDALWMS: Invalid value of UnsafeSSL: true or false expected.");
            ret = CE_Failure;
        }
        else
        {
            m_unsafeSsl = v;
        }
    }

    // Initialize the minidriver, which can set parameters for the dataset using
    // member functions

    const CPLString service_name = CPLGetXMLValue(service_node, "name", "");
    if (service_name.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: No Service name specified.");
        return CE_Failure;
    }

    m_mini_driver = NewWMSMiniDriver(service_name);
    if (m_mini_driver == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: No mini-driver registered for '%s'.",
                 service_name.c_str());
        return CE_Failure;
    }

    // Set up minidriver
    m_mini_driver->m_parent_dataset = this;
    if (m_mini_driver->Initialize(service_node, l_papszOpenOptions) != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: Failed to initialize minidriver.");
        delete m_mini_driver;
        m_mini_driver = nullptr;
        ret = CE_Failure;
    }
    else
    {
        m_mini_driver->GetCapabilities(&m_mini_driver_caps);
    }

    /*
      Parameters that could be set by minidriver already
      If the size is set, minidriver has done this already
      A "server" side minidriver needs to set at least:
      - Blocksize (x and y)
      - Clamp flag (defaults to true)
      - DataWindow
      - Band Count
      - Data Type
      It should also initialize and register the bands and overviews.
    */

    if (m_data_window.m_sx < 1)
    {
        int nOverviews = 0;

        if (ret == CE_None)
        {
            m_block_size_x = atoi(CPLGetXMLValue(
                config, "BlockSizeX",
                CPLString().Printf("%d", m_default_block_size_x)));
            m_block_size_y = atoi(CPLGetXMLValue(
                config, "BlockSizeY",
                CPLString().Printf("%d", m_default_block_size_y)));
            if (m_block_size_x <= 0 || m_block_size_y <= 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Invalid value in BlockSizeX or BlockSizeY");
                ret = CE_Failure;
            }
        }

        if (ret == CE_None)
        {
            m_clamp_requests =
                StrToBool(CPLGetXMLValue(config, "ClampRequests", "true"));
            if (m_clamp_requests < 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Invalid value of ClampRequests, true/false "
                         "expected.");
                ret = CE_Failure;
            }
        }

        if (ret == CE_None)
        {
            CPLXMLNode *data_window_node = CPLGetXMLNode(config, "DataWindow");
            if (data_window_node == nullptr && m_bNeedsDataWindow)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: DataWindow missing.");
                ret = CE_Failure;
            }
            else
            {
                CPLString osDefaultX0, osDefaultX1, osDefaultY0, osDefaultY1;
                CPLString osDefaultTileCountX, osDefaultTileCountY,
                    osDefaultTileLevel;
                CPLString osDefaultOverviewCount;
                osDefaultX0.Printf("%.8f", m_default_data_window.m_x0);
                osDefaultX1.Printf("%.8f", m_default_data_window.m_x1);
                osDefaultY0.Printf("%.8f", m_default_data_window.m_y0);
                osDefaultY1.Printf("%.8f", m_default_data_window.m_y1);
                osDefaultTileCountX.Printf("%d", m_default_tile_count_x);
                osDefaultTileCountY.Printf("%d", m_default_tile_count_y);
                if (m_default_data_window.m_tlevel >= 0)
                    osDefaultTileLevel.Printf("%d",
                                              m_default_data_window.m_tlevel);
                if (m_default_overview_count >= 0)
                    osDefaultOverviewCount.Printf("%d",
                                                  m_default_overview_count);
                const char *overview_count = CPLGetXMLValue(
                    config, "OverviewCount", osDefaultOverviewCount);
                const char *ulx =
                    CPLGetXMLValue(data_window_node, "UpperLeftX", osDefaultX0);
                const char *uly =
                    CPLGetXMLValue(data_window_node, "UpperLeftY", osDefaultY0);
                const char *lrx = CPLGetXMLValue(data_window_node,
                                                 "LowerRightX", osDefaultX1);
                const char *lry = CPLGetXMLValue(data_window_node,
                                                 "LowerRightY", osDefaultY1);
                const char *sx = CPLGetXMLValue(data_window_node, "SizeX", "");
                const char *sy = CPLGetXMLValue(data_window_node, "SizeY", "");
                const char *tx = CPLGetXMLValue(data_window_node, "TileX", "0");
                const char *ty = CPLGetXMLValue(data_window_node, "TileY", "0");
                const char *tlevel = CPLGetXMLValue(
                    data_window_node, "TileLevel", osDefaultTileLevel);
                const char *str_tile_count_x = CPLGetXMLValue(
                    data_window_node, "TileCountX", osDefaultTileCountX);
                const char *str_tile_count_y = CPLGetXMLValue(
                    data_window_node, "TileCountY", osDefaultTileCountY);
                const char *y_origin =
                    CPLGetXMLValue(data_window_node, "YOrigin", "default");

                if ((ulx[0] != '\0') && (uly[0] != '\0') && (lrx[0] != '\0') &&
                    (lry[0] != '\0'))
                {
                    m_data_window.m_x0 = CPLAtof(ulx);
                    m_data_window.m_y0 = CPLAtof(uly);
                    m_data_window.m_x1 = CPLAtof(lrx);
                    m_data_window.m_y1 = CPLAtof(lry);
                }
                else
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "GDALWMS: Mandatory elements of DataWindow missing: "
                        "UpperLeftX, UpperLeftY, LowerRightX, LowerRightY.");
                    ret = CE_Failure;
                }

                m_data_window.m_tlevel = atoi(tlevel);
                // Limit to 30 to avoid 1 << m_tlevel overflow
                if (m_data_window.m_tlevel < 0 || m_data_window.m_tlevel > 30)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Invalid value for TileLevel");
                    return CE_Failure;
                }

                if (ret == CE_None)
                {
                    if ((sx[0] != '\0') && (sy[0] != '\0'))
                    {
                        m_data_window.m_sx = atoi(sx);
                        m_data_window.m_sy = atoi(sy);
                    }
                    else if ((tlevel[0] != '\0') &&
                             (str_tile_count_x[0] != '\0') &&
                             (str_tile_count_y[0] != '\0'))
                    {
                        const int tile_count_x = atoi(str_tile_count_x);
                        if (tile_count_x <= 0)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Invalid value for TileCountX");
                            return CE_Failure;
                        }
                        if (tile_count_x > INT_MAX / m_block_size_x ||
                            tile_count_x * m_block_size_x >
                                INT_MAX / (1 << m_data_window.m_tlevel))
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Integer overflow in tile_count_x * "
                                     "m_block_size_x * (1 << "
                                     "m_data_window.m_tlevel)");
                            return CE_Failure;
                        }
                        m_data_window.m_sx = tile_count_x * m_block_size_x *
                                             (1 << m_data_window.m_tlevel);

                        const int tile_count_y = atoi(str_tile_count_y);
                        if (tile_count_y <= 0)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Invalid value for TileCountY");
                            return CE_Failure;
                        }
                        if (tile_count_y > INT_MAX / m_block_size_y ||
                            tile_count_y * m_block_size_y >
                                INT_MAX / (1 << m_data_window.m_tlevel))
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Integer overflow in tile_count_y * "
                                     "m_block_size_y * (1 << "
                                     "m_data_window.m_tlevel)");
                            return CE_Failure;
                        }
                        m_data_window.m_sy = tile_count_y * m_block_size_y *
                                             (1 << m_data_window.m_tlevel);
                    }
                    else
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "GDALWMS: Mandatory elements of DataWindow "
                                 "missing: SizeX, SizeY.");
                        ret = CE_Failure;
                    }
                }
                if (ret == CE_None)
                {
                    if ((tx[0] != '\0') && (ty[0] != '\0'))
                    {
                        m_data_window.m_tx = atoi(tx);
                        m_data_window.m_ty = atoi(ty);
                    }
                    else
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "GDALWMS: Mandatory elements of DataWindow "
                                 "missing: TileX, TileY.");
                        ret = CE_Failure;
                    }
                }

                if (ret == CE_None)
                {
                    if (overview_count[0] != '\0')
                    {
                        nOverviews = atoi(overview_count);
                    }
                    else if (tlevel[0] != '\0')
                    {
                        nOverviews = m_data_window.m_tlevel;
                    }
                    else
                    {
                        const int min_overview_size = std::max(
                            32, std::min(m_block_size_x, m_block_size_y));
                        double a =
                            log(static_cast<double>(std::min(
                                m_data_window.m_sx, m_data_window.m_sy))) /
                                log(2.0) -
                            log(static_cast<double>(min_overview_size)) /
                                log(2.0);
                        nOverviews = std::max(
                            0, std::min(static_cast<int>(ceil(a)), 32));
                    }
                }
                if (ret == CE_None)
                {
                    CPLString y_origin_str = y_origin;
                    if (y_origin_str == "top")
                    {
                        m_data_window.m_y_origin = GDALWMSDataWindow::TOP;
                    }
                    else if (y_origin_str == "bottom")
                    {
                        m_data_window.m_y_origin = GDALWMSDataWindow::BOTTOM;
                    }
                    else if (y_origin_str == "default")
                    {
                        m_data_window.m_y_origin = GDALWMSDataWindow::DEFAULT;
                    }
                    else
                    {
                        CPLError(
                            CE_Failure, CPLE_AppDefined,
                            "GDALWMS: DataWindow YOrigin must be set to "
                            "one of 'default', 'top', or 'bottom', not '%s'.",
                            y_origin_str.c_str());
                        ret = CE_Failure;
                    }
                }
            }
        }

        if (ret == CE_None)
        {
            if (nBands < 1)
                nBands = atoi(CPLGetXMLValue(config, "BandsCount", "3"));
            if (nBands < 1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Bad number of bands.");
                ret = CE_Failure;
            }
        }

        if (ret == CE_None)
        {
            const char *data_type = CPLGetXMLValue(config, "DataType", "Byte");
            if (!STARTS_WITH(data_type, "Byte"))
                SetTileOO("@DATATYPE", data_type);
            m_data_type = GDALGetDataTypeByName(data_type);
            if (m_data_type == GDT_Unknown || m_data_type >= GDT_TypeCount)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Invalid value in DataType. Data type \"%s\" "
                         "is not supported.",
                         data_type);
                ret = CE_Failure;
            }
        }

        // Initialize the bands and the overviews.  Assumes overviews are powers
        // of two
        if (ret == CE_None)
        {
            nRasterXSize = m_data_window.m_sx;
            nRasterYSize = m_data_window.m_sy;

            if (!GDALCheckDatasetDimensions(nRasterXSize, nRasterYSize) ||
                !GDALCheckBandCount(nBands, TRUE))
            {
                return CE_Failure;
            }

            GDALColorInterp default_color_interp[4][4] = {
                {GCI_GrayIndex, GCI_Undefined, GCI_Undefined, GCI_Undefined},
                {GCI_GrayIndex, GCI_AlphaBand, GCI_Undefined, GCI_Undefined},
                {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_Undefined},
                {GCI_RedBand, GCI_GreenBand, GCI_BlueBand, GCI_AlphaBand}};
            for (int i = 0; i < nBands; ++i)
            {
                GDALColorInterp color_interp =
                    (nBands <= 4 && i <= 3 ? default_color_interp[nBands - 1][i]
                                           : GCI_Undefined);
                GDALWMSRasterBand *band = new GDALWMSRasterBand(this, i, 1.0);
                band->m_color_interp = color_interp;
                SetBand(i + 1, band);
                double scale = 0.5;
                for (int j = 0; j < nOverviews; ++j)
                {
                    if (!band->AddOverview(scale))
                        break;
                    band->m_color_interp = color_interp;
                    scale *= 0.5;
                }
            }
        }
    }

    // Let the local configuration override the minidriver supplied projection
    if (ret == CE_None)
    {
        const char *proj = CPLGetXMLValue(config, "Projection", "");
        if (proj[0] != '\0')
        {
            m_oSRS = ProjToSRS(proj);
            if (m_oSRS.IsEmpty())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: Bad projection specified.");
                ret = CE_Failure;
            }
        }
    }

    // Same for Min, Max and NoData, defined per band or per dataset
    // If they are set as null strings, they clear the server declared values
    if (ret == CE_None)
    {
        // Data values are attributes, they include NoData Min and Max
        if (nullptr != CPLGetXMLNode(config, "DataValues"))
        {
            const char *nodata =
                CPLGetXMLValue(config, "DataValues.NoData", "");
            if (strlen(nodata) > 0)
            {
                SetTileOO("@NDV", nodata);
                WMSSetNoDataValue(nodata);
            }
            const char *min = CPLGetXMLValue(config, "DataValues.min", nullptr);
            if (min != nullptr)
                WMSSetMinValue(min);
            const char *max = CPLGetXMLValue(config, "DataValues.max", nullptr);
            if (max != nullptr)
                WMSSetMaxValue(max);
        }
    }

    if (ret == CE_None)
    {
        if (m_oSRS.IsEmpty())
        {
            const auto &oSRS = m_mini_driver->GetSpatialRef();
            if (!oSRS.IsEmpty())
            {
                m_oSRS = oSRS;
            }
        }
    }

    // Finish the minidriver initialization
    if (ret == CE_None)
        m_mini_driver->EndInit();

    return ret;
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/
CPLErr GDALWMSDataset::IRasterIO(GDALRWFlag rw, int x0, int y0, int sx, int sy,
                                 void *buffer, int bsx, int bsy,
                                 GDALDataType bdt, int band_count,
                                 BANDMAP_TYPE band_map, GSpacing nPixelSpace,
                                 GSpacing nLineSpace, GSpacing nBandSpace,
                                 GDALRasterIOExtraArg *psExtraArg)
{
    CPLErr ret;

    if (rw != GF_Read)
        return CE_Failure;
    if (buffer == nullptr)
        return CE_Failure;
    if ((sx == 0) || (sy == 0) || (bsx == 0) || (bsy == 0) || (band_count == 0))
        return CE_None;

    m_hint.m_x0 = x0;
    m_hint.m_y0 = y0;
    m_hint.m_sx = sx;
    m_hint.m_sy = sy;
    m_hint.m_overview = -1;
    m_hint.m_valid = true;
    // printf("[%p] GDALWMSDataset::IRasterIO(x0: %d, y0: %d, sx: %d, sy: %d,
    // bsx: %d, bsy: %d, band_count: %d, band_map: %p)\n", this, x0, y0, sx, sy,
    // bsx, bsy, band_count, band_map);
    ret = GDALDataset::IRasterIO(rw, x0, y0, sx, sy, buffer, bsx, bsy, bdt,
                                 band_count, band_map, nPixelSpace, nLineSpace,
                                 nBandSpace, psExtraArg);
    m_hint.m_valid = false;

    return ret;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/
const OGRSpatialReference *GDALWMSDataset::GetSpatialRef() const
{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/
CPLErr GDALWMSDataset::SetSpatialRef(const OGRSpatialReference *)
{
    return CE_Failure;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/
CPLErr GDALWMSDataset::GetGeoTransform(GDALGeoTransform &gt) const
{
    if (!(m_mini_driver_caps.m_has_geotransform))
    {
        gt = GDALGeoTransform();
        return CE_Failure;
    }
    gt[0] = m_data_window.m_x0;
    gt[1] = (m_data_window.m_x1 - m_data_window.m_x0) /
            static_cast<double>(m_data_window.m_sx);
    gt[2] = 0.0;
    gt[3] = m_data_window.m_y0;
    gt[4] = 0.0;
    gt[5] = (m_data_window.m_y1 - m_data_window.m_y0) /
            static_cast<double>(m_data_window.m_sy);
    return CE_None;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/
CPLErr GDALWMSDataset::SetGeoTransform(const GDALGeoTransform &)
{
    return CE_Failure;
}

/************************************************************************/
/*                             AdviseRead()                             */
/************************************************************************/
CPLErr GDALWMSDataset::AdviseRead(int x0, int y0, int sx, int sy, int bsx,
                                  int bsy, GDALDataType bdt,
                                  CPL_UNUSED int band_count,
                                  CPL_UNUSED int *band_map, char **options)
{
    //    printf("AdviseRead(%d, %d, %d, %d)\n", x0, y0, sx, sy);
    if (m_offline_mode || !m_use_advise_read)
        return CE_None;
    if (m_cache == nullptr)
        return CE_Failure;

    GDALRasterBand *band = GetRasterBand(1);
    if (band == nullptr)
        return CE_Failure;
    return band->AdviseRead(x0, y0, sx, sy, bsx, bsy, bdt, options);
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **GDALWMSDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALPamDataset::GetMetadataDomainList(),
                                   TRUE, "WMS", nullptr);
}

/************************************************************************/
/*                          GetMetadataItem()                           */
/************************************************************************/
const char *GDALWMSDataset::GetMetadataItem(const char *pszName,
                                            const char *pszDomain)
{
    if (pszName != nullptr && EQUAL(pszName, "XML") && pszDomain != nullptr &&
        EQUAL(pszDomain, "WMS"))
    {
        return (m_osXML.size()) ? m_osXML.c_str() : nullptr;
    }

    return GDALPamDataset::GetMetadataItem(pszName, pszDomain);
}

// Builds a CSL of options or returns the previous one
const char *const *GDALWMSDataset::GetHTTPRequestOpts()
{
    if (m_http_options != nullptr)
        return m_http_options;

    char **opts = nullptr;
    if (m_http_timeout != -1)
        opts = CSLAddString(opts, CPLOPrintf("TIMEOUT=%d", m_http_timeout));

    if (!m_osUserAgent.empty())
        opts = CSLAddNameValue(opts, "USERAGENT", m_osUserAgent);
    else
        opts = CSLAddString(
            opts, "USERAGENT=GDAL WMS driver (https://gdal.org/frmt_wms.html)");

    if (!m_osReferer.empty())
        opts = CSLAddNameValue(opts, "REFERER", m_osReferer);

    if (m_unsafeSsl >= 1)
        opts = CSLAddString(opts, "UNSAFESSL=1");

    if (!m_osUserPwd.empty())
        opts = CSLAddNameValue(opts, "USERPWD", m_osUserPwd);

    if (m_http_max_conn > 0)
        opts = CSLAddString(opts, CPLOPrintf("MAXCONN=%d", m_http_max_conn));

    if (!m_osAccept.empty())
        opts = CSLAddNameValue(opts, "ACCEPT", m_osAccept.c_str());

    m_http_options = opts;
    return m_http_options;
}

void GDALWMSDataset::SetTileOO(const char *pszName, const char *pszValue)
{
    if (pszName == nullptr || strlen(pszName) == 0)
        return;
    int oldidx = CSLFindName(m_tileOO, pszName);
    if (oldidx >= 0)
        m_tileOO = CSLRemoveStrings(m_tileOO, oldidx, 1, nullptr);
    if (pszValue != nullptr && strlen(pszValue))
        m_tileOO = CSLAddNameValue(m_tileOO, pszName, pszValue);
}
