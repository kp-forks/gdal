/******************************************************************************
 *
 * Project:  WMS Client Driver
 * Purpose:  GDALWMSRasterBand implementation.
 * Author:   Adam Nowacki, nowak@xpam.de
 *
 ******************************************************************************
 * Copyright (c) 2007, Adam Nowacki
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2017, Dmitry Baryshnikov, <polimax@mail.ru>
 * Copyright (c) 2017, NextGIS, <info@nextgis.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "wmsdriver.h"

#include <algorithm>

GDALWMSRasterBand::GDALWMSRasterBand(GDALWMSDataset *parent_dataset, int band,
                                     double scale)
    : m_parent_dataset(parent_dataset), m_scale(scale), m_overview(-1),
      m_color_interp(GCI_Undefined), m_nAdviseReadBX0(-1), m_nAdviseReadBY0(-1),
      m_nAdviseReadBX1(-1), m_nAdviseReadBY1(-1)
{
#ifdef DEBUG_VERBOSE
    printf("[%p] GDALWMSRasterBand::GDALWMSRasterBand(%p, %d, %f)\n", /*ok*/
           this, parent_dataset, band, scale);
#endif

    if (scale == 1.0)
        poDS = parent_dataset;
    else
        poDS = nullptr;
    if (parent_dataset->m_mini_driver_caps.m_overview_dim_computation_method ==
        OVERVIEW_ROUNDED)
    {
        nRasterXSize = static_cast<int>(
            m_parent_dataset->m_data_window.m_sx * scale + 0.5);
        nRasterYSize = static_cast<int>(
            m_parent_dataset->m_data_window.m_sy * scale + 0.5);
    }
    else
    {
        nRasterXSize =
            static_cast<int>(m_parent_dataset->m_data_window.m_sx * scale);
        nRasterYSize =
            static_cast<int>(m_parent_dataset->m_data_window.m_sy * scale);
    }
    nBand = band;
    eDataType = m_parent_dataset->m_data_type;
    nBlockXSize = m_parent_dataset->m_block_size_x;
    nBlockYSize = m_parent_dataset->m_block_size_y;
}

GDALWMSRasterBand::~GDALWMSRasterBand()
{
    while (!m_overviews.empty())
    {
        delete m_overviews.back();
        m_overviews.pop_back();
    }
}

// Request for x, y but all blocks between bx0-bx1 and by0-by1 should be read
CPLErr GDALWMSRasterBand::ReadBlocks(int x, int y, void *buffer, int bx0,
                                     int by0, int bx1, int by1, int advise_read)
{
    CPLErr ret = CE_None;

    // Get a vector of requests large enough for this call
    std::vector<WMSHTTPRequest> requests(static_cast<size_t>(bx1 - bx0 + 1) *
                                         (by1 - by0 + 1));

    size_t count = 0;  // How many requests are valid
    GDALWMSCache *cache = m_parent_dataset->m_cache;
    int offline = m_parent_dataset->m_offline_mode;
    const char *const *options = m_parent_dataset->GetHTTPRequestOpts();

    for (int iy = by0; iy <= by1; ++iy)
    {
        for (int ix = bx0; ix <= bx1; ++ix)
        {
            WMSHTTPRequest &request = requests[count];
            request.x = ix;
            request.y = iy;
            bool need_this_block = false;
            if (!advise_read)
            {
                for (int ib = 1; ib <= m_parent_dataset->nBands; ++ib)
                {
                    if ((ix == x) && (iy == y) && (ib == nBand))
                    {
                        need_this_block = true;
                    }
                    else
                    {
                        GDALWMSRasterBand *band =
                            static_cast<GDALWMSRasterBand *>(
                                m_parent_dataset->GetRasterBand(ib));
                        if (m_overview >= 0)
                            band = static_cast<GDALWMSRasterBand *>(
                                band->GetOverview(m_overview));
                        if (!band->IsBlockInCache(ix, iy))
                            need_this_block = true;
                    }
                }
            }
            else
            {
                need_this_block = true;
            }

            void *p = ((ix == x) && (iy == y)) ? buffer : nullptr;
            if (need_this_block)
            {
                ret = AskMiniDriverForBlock(request, ix, iy);
                if (ret != CE_None)
                {
                    CPLError(CE_Failure, CPLE_AppDefined, "%s",
                             request.Error.c_str());
                    ret = CE_Failure;
                }
                // A missing tile is signaled by setting a range of "none"
                if (EQUAL(request.Range, "none"))
                {
                    if (!advise_read)
                    {
                        if (EmptyBlock(ix, iy, nBand, p) != CE_None)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "GDALWMS: EmptyBlock failed.");
                            ret = CE_Failure;
                        }
                    }
                    need_this_block = false;
                }
                if (ret == CE_None && cache != nullptr)
                {
                    if (cache->GetItemStatus(request.URL) == CACHE_ITEM_OK)
                    {
                        if (advise_read)
                        {
                            need_this_block = false;
                        }
                        else
                        {
                            if (ReadBlockFromCache(request.URL, ix, iy, nBand,
                                                   p, 0) == CE_None)
                            {
                                need_this_block = false;
                            }
                        }
                    }
                }
            }

            if (need_this_block)
            {
                if (offline)
                {
                    if (!advise_read)
                    {
                        if (EmptyBlock(ix, iy, nBand, p) != CE_None)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "GDALWMS: EmptyBlock failed.");
                            ret = CE_Failure;
                        }
                    }
                }
                else
                {
                    request.options = options;
                    WMSHTTPInitializeRequest(&request);
                    count++;
                }
            }
        }
    }

    // Fetch all the requests, OK to call with count of 0
    if (WMSHTTPFetchMulti(count ? &requests[0] : nullptr,
                          static_cast<int>(count)) != CE_None)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: CPLHTTPFetchMulti failed.");
        ret = CE_Failure;
    }

    for (size_t i = 0; i < count; ++i)
    {
        WMSHTTPRequest &request = requests[i];
        void *p = ((request.x == x) && (request.y == y)) ? buffer : nullptr;
        if (ret == CE_None)
        {
            int success = (request.nStatus == 200) ||
                          (!request.Range.empty() && request.nStatus == 206);
            if (success && (request.pabyData != nullptr) &&
                (request.nDataLen > 0))
            {
                CPLString file_name(
                    BufferToVSIFile(request.pabyData, request.nDataLen));
                if (!file_name.empty())
                {
                    /* check for error xml */
                    if (request.nDataLen >= 20)
                    {
                        const char *download_data =
                            reinterpret_cast<char *>(request.pabyData);
                        if (STARTS_WITH_CI(download_data, "<?xml ") ||
                            STARTS_WITH_CI(download_data, "<!DOCTYPE ") ||
                            STARTS_WITH_CI(download_data, "<ServiceException"))
                        {
                            if (ReportWMSException(file_name) != CE_None)
                            {
                                CPLError(CE_Failure, CPLE_AppDefined,
                                         "GDALWMS: The server returned unknown "
                                         "exception.");
                            }
                            ret = CE_Failure;
                        }
                    }
                    if (ret == CE_None)
                    {
                        if (advise_read &&
                            !m_parent_dataset->m_verify_advise_read)
                        {
                            if (cache != nullptr)
                                cache->Insert(request.URL, file_name);
                        }
                        else
                        {
                            ret = ReadBlockFromFile(file_name, request.x,
                                                    request.y, nBand, p,
                                                    advise_read);
                            if (ret == CE_None)
                            {
                                if (cache != nullptr)
                                    cache->Insert(request.URL, file_name);
                            }
                            else
                            {
                                CPLError(
                                    ret, CPLE_AppDefined,
                                    "GDALWMS: ReadBlockFromFile (%s) failed.",
                                    request.URL.c_str());
                            }
                        }
                    }
                    else if (m_parent_dataset->m_zeroblock_on_serverexceptions)
                    {
                        ret = EmptyBlock(request.x, request.y, nBand, p);
                        if (ret != CE_None)
                            CPLError(ret, CPLE_AppDefined,
                                     "GDALWMS: EmptyBlock failed.");
                    }
                    VSIUnlink(file_name);
                }
            }
            else
            {  // HTTP error
                // One more try to get cached block. For example if no web
                // access available
                CPLDebug("WMS", "ReadBlockFromCache");

                if (m_parent_dataset->m_cache != nullptr)
                    ret = ReadBlockFromCache(request.URL, request.x, request.y,
                                             nBand, p, advise_read);
                else
                    ret = CE_Failure;

                if (ret != CE_None)
                {
                    CPLDebug("WMS", "After ReadBlockFromCache");
                    if (m_parent_dataset->m_http_zeroblock_codes.find(
                            request.nStatus) !=
                        m_parent_dataset->m_http_zeroblock_codes.end())
                    {
                        if (!advise_read)
                        {
                            ret = EmptyBlock(request.x, request.y, nBand, p);
                            if (ret != CE_None)
                                CPLError(ret, CPLE_AppDefined,
                                         "GDALWMS: EmptyBlock failed.");
                        }
                    }
                    else
                    {
                        ret = CE_Failure;
                        CPLError(ret, CPLE_AppDefined,
                                 "GDALWMS: Unable to download block %d, %d.\n"
                                 "URL: %s\n  HTTP status code: %d, error: %s.\n"
                                 "Add the HTTP status code to "
                                 "<ZeroBlockHttpCodes> to ignore this error "
                                 "(see https://gdal.org/frmt_wms.html).",
                                 request.x, request.y,
                                 !request.URL.empty() ? request.Error.c_str()
                                                      : "(null)",
                                 request.nStatus,
                                 !request.Error.empty() ? request.Error.c_str()
                                                        : "(null)");
                    }
                }
            }
        }
    }

    return ret;
}

CPLErr GDALWMSRasterBand::IReadBlock(int x, int y, void *buffer)
{
    int bx0 = x;
    int by0 = y;
    int bx1 = x;
    int by1 = y;

    bool bCancelHint = false;
    if ((m_parent_dataset->m_hint.m_valid) &&
        (m_parent_dataset->m_hint.m_overview == m_overview))
    {
        int tbx0 = m_parent_dataset->m_hint.m_x0 / nBlockXSize;
        int tby0 = m_parent_dataset->m_hint.m_y0 / nBlockYSize;
        int tbx1 = (m_parent_dataset->m_hint.m_x0 +
                    m_parent_dataset->m_hint.m_sx - 1) /
                   nBlockXSize;
        int tby1 = (m_parent_dataset->m_hint.m_y0 +
                    m_parent_dataset->m_hint.m_sy - 1) /
                   nBlockYSize;
        if ((tbx0 <= x) && (tby0 <= y) && (tbx1 >= x) && (tby1 >= y))
        {
            // Avoid downloading a insane number of tiles at once.
            // Limit to 30x30 tiles centered around block of interest.
            bx0 = std::max(x - 15, tbx0);
            by0 = std::max(y - 15, tby0);
            bx1 = std::min(x + 15, tbx1);
            by1 = std::min(y + 15, tby1);
            bCancelHint =
                (bx0 == tbx0 && by0 == tby0 && bx1 == tbx1 && by1 == tby1);
        }
    }

    CPLErr eErr = ReadBlocks(x, y, buffer, bx0, by0, bx1, by1, 0);

    if (bCancelHint)
    {
        m_parent_dataset->m_hint.m_valid = false;
    }

    return eErr;
}

CPLErr GDALWMSRasterBand::IRasterIO(GDALRWFlag rw, int x0, int y0, int sx,
                                    int sy, void *buffer, int bsx, int bsy,
                                    GDALDataType bdt, GSpacing nPixelSpace,
                                    GSpacing nLineSpace,
                                    GDALRasterIOExtraArg *psExtraArg)
{
    CPLErr ret;

    if (rw != GF_Read)
        return CE_Failure;
    if (buffer == nullptr)
        return CE_Failure;
    if ((sx == 0) || (sy == 0) || (bsx == 0) || (bsy == 0))
        return CE_None;

    m_parent_dataset->m_hint.m_x0 = x0;
    m_parent_dataset->m_hint.m_y0 = y0;
    m_parent_dataset->m_hint.m_sx = sx;
    m_parent_dataset->m_hint.m_sy = sy;
    m_parent_dataset->m_hint.m_overview = m_overview;
    m_parent_dataset->m_hint.m_valid = true;
    ret = GDALRasterBand::IRasterIO(rw, x0, y0, sx, sy, buffer, bsx, bsy, bdt,
                                    nPixelSpace, nLineSpace, psExtraArg);
    m_parent_dataset->m_hint.m_valid = false;

    return ret;
}

int GDALWMSRasterBand::HasArbitraryOverviews()
{
    //    return m_parent_dataset->m_mini_driver_caps.m_has_arb_overviews;
    return 0;  // not implemented yet
}

int GDALWMSRasterBand::GetOverviewCount()
{
    return static_cast<int>(m_overviews.size());
}

GDALRasterBand *GDALWMSRasterBand::GetOverview(int n)
{
    if ((!m_overviews.empty()) && (static_cast<size_t>(n) < m_overviews.size()))
        return m_overviews[n];
    else
        return nullptr;
}

bool GDALWMSRasterBand::AddOverview(double scale)
{
    GDALWMSRasterBand *overview =
        new GDALWMSRasterBand(m_parent_dataset, nBand, scale);
    if (overview->GetXSize() == 0 || overview->GetYSize() == 0)
    {
        delete overview;
        return false;
    }
    std::vector<GDALWMSRasterBand *>::iterator it = m_overviews.begin();
    for (; it != m_overviews.end(); ++it)
    {
        GDALWMSRasterBand *p = *it;
        if (p->m_scale < scale)
            break;
    }
    m_overviews.insert(it, overview);
    it = m_overviews.begin();
    for (int i = 0; it != m_overviews.end(); ++it, ++i)
    {
        GDALWMSRasterBand *p = *it;
        p->m_overview = i;
    }
    return true;
}

bool GDALWMSRasterBand::IsBlockInCache(int x, int y)
{
    bool ret = false;
    GDALRasterBlock *b = TryGetLockedBlockRef(x, y);
    if (b != nullptr)
    {
        ret = true;
        b->DropLock();
    }
    return ret;
}

// This is the function that calculates the block coordinates for the fetch
CPLErr GDALWMSRasterBand::AskMiniDriverForBlock(WMSHTTPRequest &r, int x, int y)
{
    GDALWMSImageRequestInfo iri;
    GDALWMSTiledImageRequestInfo tiri;

    ComputeRequestInfo(iri, tiri, x, y);
    return m_parent_dataset->m_mini_driver->TiledImageRequest(r, iri, tiri);
}

void GDALWMSRasterBand::ComputeRequestInfo(GDALWMSImageRequestInfo &iri,
                                           GDALWMSTiledImageRequestInfo &tiri,
                                           int x, int y)
{
    int x0 = std::max(0, x * nBlockXSize);
    int y0 = std::max(0, y * nBlockYSize);
    int x1 = std::max(0, (x + 1) * nBlockXSize);
    int y1 = std::max(0, (y + 1) * nBlockYSize);
    if (m_parent_dataset->m_clamp_requests)
    {
        x0 = std::min(x0, nRasterXSize);
        y0 = std::min(y0, nRasterYSize);
        x1 = std::min(x1, nRasterXSize);
        y1 = std::min(y1, nRasterYSize);
    }

    const double rx = (m_parent_dataset->m_data_window.m_x1 -
                       m_parent_dataset->m_data_window.m_x0) /
                      static_cast<double>(nRasterXSize);
    const double ry = (m_parent_dataset->m_data_window.m_y1 -
                       m_parent_dataset->m_data_window.m_y0) /
                      static_cast<double>(nRasterYSize);
    /* Use different method for x0,y0 and x1,y1 to make sure calculated values
     * are exact for corner requests */
    iri.m_x0 = x0 * rx + m_parent_dataset->m_data_window.m_x0;
    iri.m_y0 = y0 * ry + m_parent_dataset->m_data_window.m_y0;
    iri.m_x1 = m_parent_dataset->m_data_window.m_x1 - (nRasterXSize - x1) * rx;
    iri.m_y1 = m_parent_dataset->m_data_window.m_y1 - (nRasterYSize - y1) * ry;
    iri.m_sx = x1 - x0;
    iri.m_sy = y1 - y0;

    int level = m_overview + 1;
    tiri.m_x = (m_parent_dataset->m_data_window.m_tx >> level) + x;
    tiri.m_y = (m_parent_dataset->m_data_window.m_ty >> level) + y;
    tiri.m_level = m_parent_dataset->m_data_window.m_tlevel - level;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **GDALWMSRasterBand::GetMetadataDomainList()
{
    char **m_list = GDALPamRasterBand::GetMetadataDomainList();
    char **mini_list = m_parent_dataset->m_mini_driver->GetMetadataDomainList();
    if (mini_list != nullptr)
    {
        m_list = CSLMerge(m_list, mini_list);
        CSLDestroy(mini_list);
    }
    return m_list;
}

const char *GDALWMSRasterBand::GetMetadataItem(const char *pszName,
                                               const char *pszDomain)
{
    if (!m_parent_dataset->m_mini_driver_caps.m_has_getinfo ||
        !(pszDomain != nullptr && EQUAL(pszDomain, "LocationInfo") &&
          (STARTS_WITH_CI(pszName, "Pixel_") ||
           STARTS_WITH_CI(pszName, "GeoPixel_"))))
        return GDALPamRasterBand::GetMetadataItem(pszName, pszDomain);

    /* ==================================================================== */
    /*      LocationInfo handling.                                          */
    /* ==================================================================== */

    /* -------------------------------------------------------------------- */
    /*      What pixel are we aiming at?                                    */
    /* -------------------------------------------------------------------- */
    int iPixel, iLine;
    if (STARTS_WITH_CI(pszName, "Pixel_"))
    {
        if (sscanf(pszName + 6, "%d_%d", &iPixel, &iLine) != 2)
            return nullptr;
    }
    else if (STARTS_WITH_CI(pszName, "GeoPixel_"))
    {
        GDALGeoTransform gt, invGT;
        double dfGeoX, dfGeoY;

        {
            dfGeoX = CPLAtof(pszName + 9);
            const char *pszUnderscore = strchr(pszName + 9, '_');
            if (!pszUnderscore)
                return nullptr;
            dfGeoY = CPLAtof(pszUnderscore + 1);
        }

        if (m_parent_dataset->GetGeoTransform(gt) != CE_None)
            return nullptr;

        if (!GDALInvGeoTransform(gt.data(), invGT.data()))
            return nullptr;

        iPixel = static_cast<int>(
            floor(invGT[0] + invGT[1] * dfGeoX + invGT[2] * dfGeoY));
        iLine = static_cast<int>(
            floor(invGT[3] + invGT[4] * dfGeoX + invGT[5] * dfGeoY));

        /* The GetDataset() for the WMS driver is always the main overview
         * level, so rescale */
        /* the values if we are an overview */
        if (m_overview >= 0)
        {
            iPixel = static_cast<int>(
                1.0 * iPixel * GetXSize() /
                m_parent_dataset->GetRasterBand(1)->GetXSize());
            iLine = static_cast<int>(
                1.0 * iLine * GetYSize() /
                m_parent_dataset->GetRasterBand(1)->GetYSize());
        }
    }
    else
        return nullptr;

    if (iPixel < 0 || iLine < 0 || iPixel >= GetXSize() || iLine >= GetYSize())
        return nullptr;

    if (nBand != 1)
    {
        GDALRasterBand *poFirstBand = m_parent_dataset->GetRasterBand(1);
        if (m_overview >= 0)
            poFirstBand = poFirstBand->GetOverview(m_overview);
        if (poFirstBand)
            return poFirstBand->GetMetadataItem(pszName, pszDomain);
    }

    GDALWMSImageRequestInfo iri;
    GDALWMSTiledImageRequestInfo tiri;
    int nBlockXOff = iPixel / nBlockXSize;
    int nBlockYOff = iLine / nBlockYSize;

    ComputeRequestInfo(iri, tiri, nBlockXOff, nBlockYOff);

    CPLString url;
    m_parent_dataset->m_mini_driver->GetTiledImageInfo(
        url, iri, tiri, iPixel % nBlockXSize, iLine % nBlockXSize);

    if (url.empty())
        return nullptr;

    CPLDebug("WMS", "URL = %s", url.c_str());

    if (url == osMetadataItemURL)
    {
        // osMetadataItem.c_str() MUST be used, and not osMetadataItem,
        // otherwise a temporary copy is returned
        return !osMetadataItem.empty() ? osMetadataItem.c_str() : nullptr;
    }

    osMetadataItemURL = url;

    // This is OK, CPLHTTPFetch does not touch the options
    char **papszOptions =
        const_cast<char **>(m_parent_dataset->GetHTTPRequestOpts());
    CPLHTTPResult *psResult = CPLHTTPFetch(url, papszOptions);

    CPLString pszRes;

    if (psResult && psResult->pabyData)
        pszRes = reinterpret_cast<const char *>(psResult->pabyData);
    CPLHTTPDestroyResult(psResult);

    if (pszRes.empty())
    {
        osMetadataItem = "";
        return nullptr;
    }

    osMetadataItem = "<LocationInfo>";
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLXMLNode *psXML = CPLParseXMLString(pszRes);
    CPLPopErrorHandler();
    if (psXML != nullptr && psXML->eType == CXT_Element)
    {
        if (strcmp(psXML->pszValue, "?xml") == 0)
        {
            if (psXML->psNext)
            {
                char *pszXML = CPLSerializeXMLTree(psXML->psNext);
                osMetadataItem += pszXML;
                CPLFree(pszXML);
            }
        }
        else
        {
            osMetadataItem += pszRes;
        }
    }
    else
    {
        char *pszEscapedXML = CPLEscapeString(pszRes, -1, CPLES_XML_BUT_QUOTES);
        osMetadataItem += pszEscapedXML;
        CPLFree(pszEscapedXML);
    }
    if (psXML != nullptr)
        CPLDestroyXMLNode(psXML);

    osMetadataItem += "</LocationInfo>";

    // osMetadataItem.c_str() MUST be used, and not osMetadataItem,
    // otherwise a temporary copy is returned
    return osMetadataItem.c_str();
}

static const int *GetBandMapForExpand(int nSourceBands, int nWmsBands)
{
    static const int bandmap1to1[] = {1};
    static const int bandmap2to1[] = {1};
    static const int bandmap3to1[] = {1};
    static const int bandmap4to1[] = {1};

    static const int bandmap1to2[] = {1, 0};  // 0 == full opaque alpha band
    static const int bandmap2to2[] = {1, 2};
    static const int bandmap3to2[] = {1, 0};
    static const int bandmap4to2[] = {1, 4};

    static const int bandmap1to3[] = {1, 1, 1};
    static const int bandmap2to3[] = {1, 1, 1};
    static const int bandmap3to3[] = {1, 2, 3};
    static const int bandmap4to3[] = {1, 2, 3};

    static const int bandmap1to4[] = {1, 1, 1, 0};
    static const int bandmap2to4[] = {1, 1, 1, 2};
    static const int bandmap3to4[] = {1, 2, 3, 0};
    static const int bandmap4to4[] = {1, 2, 3, 4};

    static const int *const bandmap_selector[4][4] = {
        {bandmap1to1, bandmap2to1, bandmap3to1, bandmap4to1},
        {bandmap1to2, bandmap2to2, bandmap3to2, bandmap4to2},
        {bandmap1to3, bandmap2to3, bandmap3to3, bandmap4to3},
        {bandmap1to4, bandmap2to4, bandmap3to4, bandmap4to4},
    };

    if (nSourceBands > 4 || nSourceBands < 1)
    {
        return nullptr;
    }
    if (nWmsBands > 4 || nWmsBands < 1)
    {
        return nullptr;
    }
    return bandmap_selector[nWmsBands - 1][nSourceBands - 1];
}

CPLErr GDALWMSRasterBand::ReadBlockFromDataset(GDALDataset *ds, int x, int y,
                                               int to_buffer_band, void *buffer,
                                               int advise_read)
{
    CPLErr ret = CE_None;
    GByte *color_table = nullptr;
    int i;

    // CPLDebug("WMS", "ReadBlockFromDataset: to_buffer_band=%d, (x,y)=(%d,
    // %d)", to_buffer_band, x, y);

    /* expected size */
    const int esx = MIN(MAX(0, (x + 1) * nBlockXSize), nRasterXSize) -
                    MIN(MAX(0, x * nBlockXSize), nRasterXSize);
    const int esy = MIN(MAX(0, (y + 1) * nBlockYSize), nRasterYSize) -
                    MIN(MAX(0, y * nBlockYSize), nRasterYSize);

    int sx = ds->GetRasterXSize();
    int sy = ds->GetRasterYSize();
    /* Allow bigger than expected so pre-tiled constant size images work on
     * corners */
    if ((sx > nBlockXSize) || (sy > nBlockYSize) || (sx < esx) || (sy < esy))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: Incorrect size %d x %d of downloaded block, "
                 "expected %d x %d, max %d x %d.",
                 sx, sy, esx, esy, nBlockXSize, nBlockYSize);
        ret = CE_Failure;
    }

    int nDSRasterCount = ds->GetRasterCount();
    if (ret == CE_None)
    {
        if (nDSRasterCount != m_parent_dataset->nBands)
        {
            /* Maybe its an image with color table */
            if ((eDataType == GDT_Byte) && (ds->GetRasterCount() == 1))
            {
                GDALRasterBand *rb = ds->GetRasterBand(1);
                if (rb->GetRasterDataType() == GDT_Byte)
                {
                    GDALColorTable *ct = rb->GetColorTable();
                    if (ct != nullptr)
                    {
                        if (!advise_read)
                        {
                            color_table = new GByte[256 * 4];
                            const int count =
                                MIN(256, ct->GetColorEntryCount());
                            for (i = 0; i < count; ++i)
                            {
                                GDALColorEntry ce;
                                ct->GetColorEntryAsRGB(i, &ce);
                                color_table[i] = static_cast<GByte>(ce.c1);
                                color_table[i + 256] =
                                    static_cast<GByte>(ce.c2);
                                color_table[i + 512] =
                                    static_cast<GByte>(ce.c3);
                                color_table[i + 768] =
                                    static_cast<GByte>(ce.c4);
                            }

                            for (i = count; i < 256; ++i)
                            {
                                color_table[i] = 0;
                                color_table[i + 256] = 0;
                                color_table[i + 512] = 0;
                                color_table[i + 768] = 0;
                            }
                        }
                    }
                    else if (m_parent_dataset->nBands <= 4)
                    {  // Promote single band to fake color table
                        color_table = new GByte[256 * 4];
                        for (i = 0; i < 256; i++)
                        {
                            color_table[i] = static_cast<GByte>(i);
                            color_table[i + 256] = static_cast<GByte>(i);
                            color_table[i + 512] = static_cast<GByte>(i);
                            color_table[i + 768] = 255;  // Transparency
                        }
                        if (m_parent_dataset->nBands == 2)
                        {  // Luma-Alpha fixup
                            for (i = 0; i < 256; i++)
                            {
                                color_table[i + 256] = 255;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!advise_read)
    {
        const int *const bandmap =
            GetBandMapForExpand(nDSRasterCount, m_parent_dataset->nBands);
        for (int ib = 1; ib <= m_parent_dataset->nBands; ++ib)
        {
            if (ret == CE_None)
            {
                void *p = nullptr;
                GDALRasterBlock *b = nullptr;
                if ((buffer != nullptr) && (ib == to_buffer_band))
                {
                    p = buffer;
                }
                else
                {
                    GDALWMSRasterBand *band = static_cast<GDALWMSRasterBand *>(
                        m_parent_dataset->GetRasterBand(ib));
                    if (m_overview >= 0)
                    {
                        band = static_cast<GDALWMSRasterBand *>(
                            band->GetOverview(m_overview));
                    }
                    if (!band->IsBlockInCache(x, y))
                    {
                        b = band->GetLockedBlockRef(x, y, true);
                        if (b != nullptr)
                        {
                            p = b->GetDataRef();
                            if (p == nullptr)
                            {
                                CPLError(CE_Failure, CPLE_AppDefined,
                                         "GDALWMS: GetDataRef returned NULL.");
                                ret = CE_Failure;
                            }
                        }
                    }
                    else
                    {
                        // CPLDebug("WMS", "Band %d, block (x,y)=(%d, %d)
                        // already in cache", band->GetBand(), x, y);
                    }
                }

                if (p != nullptr)
                {
                    int pixel_space = GDALGetDataTypeSizeBytes(eDataType);
                    int line_space = pixel_space * nBlockXSize;
                    if (color_table == nullptr)
                    {
                        if (bandmap == nullptr || bandmap[ib - 1] != 0)
                        {
                            GDALDataType dt = eDataType;
                            int nSourceBand = ib;
                            if (bandmap != nullptr)
                            {
                                nSourceBand = bandmap[ib - 1];
                            }
                            // Get the data from the PNG as stored instead of
                            // converting, if the server asks for that
                            // TODO: This hack is from #3493 - not sure it
                            // really belongs here.
                            if ((GDT_Int16 == dt) &&
                                (GDT_UInt16 ==
                                 ds->GetRasterBand(ib)->GetRasterDataType()))
                            {
                                dt = GDT_UInt16;
                            }

                            if (ds->RasterIO(GF_Read, 0, 0, sx, sy, p, sx, sy,
                                             dt, 1, &nSourceBand, pixel_space,
                                             line_space, 0, nullptr) != CE_None)
                            {
                                CPLError(CE_Failure, CPLE_AppDefined,
                                         "GDALWMS: RasterIO failed on "
                                         "downloaded block.");
                                ret = CE_Failure;
                            }
                        }
                        else  // if( bandmap != nullptr && bandmap[ib - 1] == 0
                              // )
                        {  // parent expects 4 bands but file has fewer count so
                            // generate a all "opaque" 4th band
                            GByte *byte_buffer = reinterpret_cast<GByte *>(p);
                            for (int l_y = 0; l_y < sy; ++l_y)
                            {
                                for (int l_x = 0; l_x < sx; ++l_x)
                                {
                                    const int offset = l_x + l_y * line_space;
                                    byte_buffer[offset] =
                                        255;  // fill with opaque
                                }
                            }
                        }
                    }
                    else if (ib <= 4)
                    {
                        if (ds->RasterIO(GF_Read, 0, 0, sx, sy, p, sx, sy,
                                         eDataType, 1, nullptr, pixel_space,
                                         line_space, 0, nullptr) != CE_None)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "GDALWMS: RasterIO failed on downloaded "
                                     "block.");
                            ret = CE_Failure;
                        }

                        if (ret == CE_None)
                        {
                            GByte *band_color_table =
                                color_table + 256 * (ib - 1);
                            GByte *byte_buffer = reinterpret_cast<GByte *>(p);
                            for (int l_y = 0; l_y < sy; ++l_y)
                            {
                                for (int l_x = 0; l_x < sx; ++l_x)
                                {
                                    const int offset = l_x + l_y * line_space;
                                    byte_buffer[offset] =
                                        band_color_table[byte_buffer[offset]];
                                }
                            }
                        }
                    }
                    else
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "GDALWMS: Color table supports at most 4 "
                                 "components.");
                        ret = CE_Failure;
                    }
                }
                if (b != nullptr)
                {
                    b->DropLock();
                }
            }
        }
    }
    GDALClose(ds);

    if (color_table != nullptr)
    {
        delete[] color_table;
    }

    return ret;
}

CPLErr GDALWMSRasterBand::ReadBlockFromFile(const CPLString &soFileName, int x,
                                            int y, int to_buffer_band,
                                            void *buffer, int advise_read)
{
    GDALDataset *ds = GDALDataset::FromHandle(GDALOpenEx(
        soFileName, GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
        nullptr, m_parent_dataset->m_tileOO, nullptr));
    if (ds == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: Unable to open downloaded block.");
        return CE_Failure;
    }

    return ReadBlockFromDataset(ds, x, y, to_buffer_band, buffer, advise_read);
}

CPLErr GDALWMSRasterBand::ReadBlockFromCache(const char *pszKey, int x, int y,
                                             int to_buffer_band, void *buffer,
                                             int advise_read)
{
    GDALWMSCache *cache = m_parent_dataset->m_cache;
    if (nullptr == cache)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: Unable to open downloaded block.");
        return CE_Failure;
    }
    GDALDataset *ds = cache->GetDataset(pszKey, m_parent_dataset->m_tileOO);
    if (ds == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALWMS: Unable to open downloaded block.");
        return CE_Failure;
    }

    return ReadBlockFromDataset(ds, x, y, to_buffer_band, buffer, advise_read);
}

CPLErr GDALWMSRasterBand::EmptyBlock(int x, int y, int to_buffer_band,
                                     void *buffer)
{
    CPLErr ret = CE_None;

    for (int ib = 1; ib <= m_parent_dataset->nBands; ++ib)
    {
        if (ret == CE_None)
        {
            void *p = nullptr;
            GDALRasterBlock *b = nullptr;
            GDALWMSRasterBand *band = static_cast<GDALWMSRasterBand *>(
                m_parent_dataset->GetRasterBand(ib));
            if (m_overview >= 0)
                band = static_cast<GDALWMSRasterBand *>(
                    band->GetOverview(m_overview));
            if ((buffer != nullptr) && (ib == to_buffer_band))
            {
                p = buffer;
            }
            else
            {
                if (!band->IsBlockInCache(x, y))
                {
                    b = band->GetLockedBlockRef(x, y, true);
                    if (b != nullptr)
                    {
                        p = b->GetDataRef();
                        if (p == nullptr)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "GDALWMS: GetDataRef returned NULL.");
                            ret = CE_Failure;
                        }
                    }
                }
            }
            if (p != nullptr)
            {
                int hasNDV;
                double valNDV = band->GetNoDataValue(&hasNDV);
                if (!hasNDV)
                    valNDV = 0;
                GDALCopyWords(&valNDV, GDT_Float64, 0, p, eDataType,
                              GDALGetDataTypeSizeBytes(eDataType),
                              nBlockXSize * nBlockYSize);
            }
            if (b != nullptr)
            {
                b->DropLock();
            }
        }
    }

    return ret;
}

CPLErr GDALWMSRasterBand::ReportWMSException(const char *file_name)
{
    CPLErr ret = CE_None;
    int reported_errors_count = 0;

    CPLXMLNode *orig_root = CPLParseXMLFile(file_name);
    CPLXMLNode *root = orig_root;
    if (root != nullptr)
    {
        root = CPLGetXMLNode(root, "=ServiceExceptionReport");
    }
    if (root != nullptr)
    {
        CPLXMLNode *n = CPLGetXMLNode(root, "ServiceException");
        while (n != nullptr)
        {
            const char *exception = CPLGetXMLValue(n, "=ServiceException", "");
            const char *exception_code =
                CPLGetXMLValue(n, "=ServiceException.code", "");
            if (exception[0] != '\0')
            {
                if (exception_code[0] != '\0')
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "GDALWMS: The server returned exception code '%s': %s",
                        exception_code, exception);
                    ++reported_errors_count;
                }
                else
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "GDALWMS: The server returned exception: %s",
                             exception);
                    ++reported_errors_count;
                }
            }
            else if (exception_code[0] != '\0')
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "GDALWMS: The server returned exception code '%s'.",
                         exception_code);
                ++reported_errors_count;
            }

            n = n->psNext;
            if (n != nullptr)
            {
                n = CPLGetXMLNode(n, "=ServiceException");
            }
        }
    }
    else
    {
        ret = CE_Failure;
    }
    if (orig_root != nullptr)
    {
        CPLDestroyXMLNode(orig_root);
    }

    if (reported_errors_count == 0)
    {
        ret = CE_Failure;
    }

    return ret;
}

CPLErr GDALWMSRasterBand::AdviseRead(int nXOff, int nYOff, int nXSize,
                                     int nYSize, int nBufXSize, int nBufYSize,
                                     GDALDataType eDT, char **papszOptions)
{
    //    printf("AdviseRead(%d, %d, %d, %d)\n", nXOff, nYOff, nXSize, nYSize);
    if (m_parent_dataset->m_offline_mode ||
        !m_parent_dataset->m_use_advise_read)
        return CE_None;
    if (m_parent_dataset->m_cache == nullptr)
        return CE_Failure;

    /* ==================================================================== */
    /*      Do we have overviews that would be appropriate to satisfy       */
    /*      this request?                                                   */
    /* ==================================================================== */
    if ((nBufXSize < nXSize || nBufYSize < nYSize) && GetOverviewCount() > 0)
    {
        const int nOverview = GDALBandGetBestOverviewLevel2(
            this, nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize, nullptr);
        if (nOverview >= 0)
        {
            GDALRasterBand *poOverviewBand = GetOverview(nOverview);
            if (poOverviewBand == nullptr)
                return CE_Failure;

            return poOverviewBand->AdviseRead(nXOff, nYOff, nXSize, nYSize,
                                              nBufXSize, nBufYSize, eDT,
                                              papszOptions);
        }
    }

    int bx0 = nXOff / nBlockXSize;
    int by0 = nYOff / nBlockYSize;
    int bx1 = (nXOff + nXSize - 1) / nBlockXSize;
    int by1 = (nYOff + nYSize - 1) / nBlockYSize;

    // Avoid downloading a insane number of tiles
    const int MAX_TILES = 1000;  // arbitrary number
    if ((bx1 - bx0 + 1) > MAX_TILES / (by1 - by0 + 1))
    {
        CPLDebug("WMS", "Too many tiles for AdviseRead()");
        return CE_Failure;
    }

    if (m_nAdviseReadBX0 == bx0 && m_nAdviseReadBY0 == by0 &&
        m_nAdviseReadBX1 == bx1 && m_nAdviseReadBY1 == by1)
    {
        return CE_None;
    }
    m_nAdviseReadBX0 = bx0;
    m_nAdviseReadBY0 = by0;
    m_nAdviseReadBX1 = bx1;
    m_nAdviseReadBY1 = by1;

    return ReadBlocks(0, 0, nullptr, bx0, by0, bx1, by1, 1);
}

GDALColorInterp GDALWMSRasterBand::GetColorInterpretation()
{
    return m_color_interp;
}

CPLErr GDALWMSRasterBand::SetColorInterpretation(GDALColorInterp eNewInterp)
{
    m_color_interp = eNewInterp;
    return CE_None;
}

// Utility function, returns a value from a vector corresponding to the band
// index or the first entry
static double getBandValue(const std::vector<double> &v, size_t idx)
{
    idx--;
    if (v.size() > idx)
        return v[idx];
    return v[0];
}

double GDALWMSRasterBand::GetNoDataValue(int *pbSuccess)
{
    std::vector<double> &v = m_parent_dataset->vNoData;
    if (v.empty())
        return GDALPamRasterBand::GetNoDataValue(pbSuccess);
    if (pbSuccess)
        *pbSuccess = TRUE;
    return getBandValue(v, nBand);
}

double GDALWMSRasterBand::GetMinimum(int *pbSuccess)
{
    std::vector<double> &v = m_parent_dataset->vMin;
    if (v.empty())
        return GDALPamRasterBand::GetMinimum(pbSuccess);
    if (pbSuccess)
        *pbSuccess = TRUE;
    return getBandValue(v, nBand);
}

double GDALWMSRasterBand::GetMaximum(int *pbSuccess)
{
    std::vector<double> &v = m_parent_dataset->vMax;
    if (v.empty())
        return GDALPamRasterBand::GetMaximum(pbSuccess);
    if (pbSuccess)
        *pbSuccess = TRUE;
    return getBandValue(v, nBand);
}

GDALColorTable *GDALWMSRasterBand::GetColorTable()
{
    return m_parent_dataset->m_poColorTable;
}
