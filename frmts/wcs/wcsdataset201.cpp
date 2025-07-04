/******************************************************************************
 *
 * Project:  WCS Client Driver
 * Purpose:  Implementation of Dataset class for WCS 2.0.
 * Author:   Ari Jolma <ari dot jolma at gmail dot com>
 *
 ******************************************************************************
 * Copyright (c) 2006, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2017, Ari Jolma
 * Copyright (c) 2017, Finnish Environment Institute
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "cpl_minixml.h"
#include "cpl_http.h"
#include "cpl_conv.h"
#include "gmlutils.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "ogr_spatialref.h"
#include "gmlcoverage.h"

#include <algorithm>

#include "wcsdataset.h"
#include "wcsutils.h"

using namespace WCSUtils;

/************************************************************************/
/*                         CoverageSubtype()                            */
/*                                                                      */
/************************************************************************/

static std::string CoverageSubtype(CPLXMLNode *coverage)
{
    std::string subtype =
        CPLGetXMLValue(coverage, "ServiceParameters.CoverageSubtype", "");
    size_t pos = subtype.find("Coverage");
    if (pos != std::string::npos)
    {
        subtype.erase(pos);
    }
    return subtype;
}

/************************************************************************/
/*                         GetGridNode()                                */
/*                                                                      */
/************************************************************************/

static CPLXMLNode *GetGridNode(CPLXMLNode *coverage, const std::string &subtype)
{
    CPLXMLNode *grid = nullptr;
    // Construct the name of the node that we look under domainSet.
    // For now we can handle RectifiedGrid and ReferenceableGridByVectors.
    // Note that if this is called at GetCoverage stage, the grid should not be
    // NULL.
    std::string path = "domainSet";
    if (subtype == "RectifiedGrid")
    {
        grid = CPLGetXMLNode(coverage, (path + "." + subtype).c_str());
    }
    else if (subtype == "ReferenceableGrid")
    {
        grid = CPLGetXMLNode(coverage,
                             (path + "." + subtype + "ByVectors").c_str());
    }
    if (!grid)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Can't handle coverages of type '%s'.", subtype.c_str());
    }
    return grid;
}

/************************************************************************/
/*                         ParseParameters()                            */
/*                                                                      */
/************************************************************************/

static void ParseParameters(CPLXMLNode *service,
                            std::vector<std::string> &dimensions,
                            std::string &range,
                            std::vector<std::vector<std::string>> &others)
{
    std::vector<std::string> parameters =
        Split(CPLGetXMLValue(service, "Parameters", ""), "&");
    for (unsigned int i = 0; i < parameters.size(); ++i)
    {
        std::vector<std::string> kv = Split(parameters[i].c_str(), "=");
        if (kv.size() < 2)
        {
            continue;
        }
        kv[0] = CPLString(kv[0]).toupper();
        if (kv[0] == "RANGESUBSET")
        {
            range = kv[1];
        }
        else if (kv[0] == "SUBSET")
        {
            dimensions = Split(kv[1].c_str(), ";");
        }
        else
        {
            others.push_back(std::vector<std::string>{kv[0], kv[1]});
        }
    }
    // fallback to service values, if any
    if (range == "")
    {
        range = CPLGetXMLValue(service, "RangeSubset", "");
    }
    if (dimensions.size() == 0)
    {
        dimensions = Split(CPLGetXMLValue(service, "Subset", ""), ";");
    }
}

/************************************************************************/
/*                         GetNativeExtent()                            */
/*                                                                      */
/************************************************************************/

std::vector<double> WCSDataset201::GetNativeExtent(int nXOff, int nYOff,
                                                   int nXSize, int nYSize,
                                                   CPL_UNUSED int nBufXSize,
                                                   CPL_UNUSED int nBufYSize)
{
    std::vector<double> extent;
    // WCS 2.0 extents are the outer edges of outer pixels.
    extent.push_back(m_gt[0] + (nXOff)*m_gt[1]);
    extent.push_back(m_gt[3] + (nYOff + nYSize) * m_gt[5]);
    extent.push_back(m_gt[0] + (nXOff + nXSize) * m_gt[1]);
    extent.push_back(m_gt[3] + (nYOff)*m_gt[5]);
    return extent;
}

/************************************************************************/
/*                        GetCoverageRequest()                          */
/*                                                                      */
/************************************************************************/

std::string
WCSDataset201::GetCoverageRequest(bool scaled, int nBufXSize, int nBufYSize,
                                  const std::vector<double> &extent,
                                  const std::string & /*osBandList*/)
{
    std::string request = CPLGetXMLValue(psService, "ServiceURL", "");
    request = CPLURLAddKVP(request.c_str(), "SERVICE", "WCS");
    request += "&REQUEST=GetCoverage";
    request +=
        "&VERSION=" + std::string(CPLGetXMLValue(psService, "Version", ""));
    request += "&COVERAGEID=" +
               URLEncode(CPLGetXMLValue(psService, "CoverageName", ""));

    // note: native_crs is not really supported
    if (!native_crs)
    {
        std::string crs = URLEncode(CPLGetXMLValue(psService, "SRS", ""));
        request += "&OUTPUTCRS=" + crs;
        request += "&SUBSETTINGCRS=" + crs;
    }

    std::vector<std::string> domain =
        Split(CPLGetXMLValue(psService, "Domain", ""), ",");
    if (domain.size() < 2)
    {
        // eek!
        domain.push_back("E");
        domain.push_back("N");
    }
    const char *x = domain[0].c_str();
    const char *y = domain[1].c_str();
    if (CPLGetXMLBoolean(psService, "SubsetAxisSwap"))
    {
        const char *tmp = x;
        x = y;
        y = tmp;
    }

    std::vector<std::string> low =
        Split(CPLGetXMLValue(psService, "Low", ""), ",");
    std::vector<std::string> high =
        Split(CPLGetXMLValue(psService, "High", ""), ",");
    std::string a = CPLString().Printf("%.17g", extent[0]);
    if (low.size() > 1 && CPLAtof(low[0].c_str()) > extent[0])
    {
        a = low[0];
    }
    std::string b = CPLString().Printf("%.17g", extent[2]);
    if (high.size() > 1 && CPLAtof(high[0].c_str()) < extent[2])
    {
        b = high[0];
    }
    /*
    std::string a = CPLString().Printf(
        "%.17g", MAX(m_gt[0], extent[0]));
    std::string b = CPLString().Printf(
        "%.17g", MIN(m_gt[0] + nRasterXSize * m_gt[1],
    extent[2]));
    */

    // 09-147 KVP Protocol: subset keys must be unique
    // GeoServer: seems to require plain SUBSET for x and y

    request +=
        CPLString().Printf("&SUBSET=%s%%28%s,%s%%29", x, a.c_str(), b.c_str());

    a = CPLString().Printf("%.17g", extent[1]);
    if (low.size() > 1 && CPLAtof(low[1].c_str()) > extent[1])
    {
        a = low[1];
    }
    b = CPLString().Printf("%.17g", extent[3]);
    if (high.size() > 1 && CPLAtof(high[1].c_str()) < extent[3])
    {
        b = high[1];
    }
    /*
    a = CPLString().Printf(
        "%.17g", MAX(m_gt[3] + nRasterYSize * m_gt[5],
    extent[1])); b = CPLString().Printf(
        "%.17g", MIN(m_gt[3], extent[3]));
    */

    request +=
        CPLString().Printf("&SUBSET=%s%%28%s,%s%%29", y, a.c_str(), b.c_str());

    // Dimension and range parameters:
    std::vector<std::string> dimensions;
    std::string range;
    std::vector<std::vector<std::string>> others;
    ParseParameters(psService, dimensions, range, others);

    // set subsets for axis other than x/y
    for (unsigned int i = 0; i < dimensions.size(); ++i)
    {
        size_t pos = dimensions[i].find("(");
        std::string dim = dimensions[i].substr(0, pos);
        if (IndexOf(dim, domain) != -1)
        {
            continue;
        }
        std::vector<std::string> params =
            Split(FromParenthesis(dimensions[i]).c_str(), ",");
        request +=
            "&SUBSET" + CPLString().Printf("%i", i) + "=" + dim + "%28";  // (
        for (unsigned int j = 0; j < params.size(); ++j)
        {
            // todo: %22 (") should be used only for non-numbers
            request += "%22" + params[j] + "%22";
        }
        request += "%29";  // )
    }

    if (scaled)
    {
        CPLString tmp;
        // scaling is expressed in grid axes
        if (CPLGetXMLBoolean(psService, "UseScaleFactor"))
        {
            double fx = fabs((extent[2] - extent[0]) / m_gt[1] /
                             ((double)nBufXSize + 0.5));
            double fy = fabs((extent[3] - extent[1]) / m_gt[5] /
                             ((double)nBufYSize + 0.5));
            tmp.Printf("&SCALEFACTOR=%.15g", MIN(fx, fy));
        }
        else
        {
            std::vector<std::string> grid_axes =
                Split(CPLGetXMLValue(psService, "GridAxes", ""), ",");
            if (grid_axes.size() < 2)
            {
                // eek!
                grid_axes.push_back("E");
                grid_axes.push_back("N");
            }
            tmp.Printf("&SCALESIZE=%s%%28%i%%29,%s%%28%i%%29",
                       grid_axes[0].c_str(), nBufXSize, grid_axes[1].c_str(),
                       nBufYSize);
        }
        request += tmp;
    }

    if (range != "" && range != "*")
    {
        request += "&RANGESUBSET=" + range;
    }

    // other parameters may come from
    // 1) URL (others)
    // 2) Service file
    const char *keys[] = {WCS_URL_PARAMETERS};
    for (unsigned int i = 0; i < CPL_ARRAYSIZE(keys); i++)
    {
        std::string value;
        int ix = IndexOf(CPLString(keys[i]).toupper(), others);
        if (ix >= 0)
        {
            value = others[ix][1];
        }
        else
        {
            value = CPLGetXMLValue(psService, keys[i], "");
        }
        if (value != "")
        {
            request = CPLURLAddKVP(request.c_str(), keys[i], value.c_str());
        }
    }
    // add extra parameters
    std::string extra = CPLGetXMLValue(psService, "Parameters", "");
    if (extra != "")
    {
        std::vector<std::string> pairs = Split(extra.c_str(), "&");
        for (unsigned int i = 0; i < pairs.size(); ++i)
        {
            std::vector<std::string> pair = Split(pairs[i].c_str(), "=");
            request =
                CPLURLAddKVP(request.c_str(), pair[0].c_str(), pair[1].c_str());
        }
    }
    std::vector<std::string> pairs =
        Split(CPLGetXMLValue(psService, "GetCoverageExtra", ""), "&");
    for (unsigned int i = 0; i < pairs.size(); ++i)
    {
        std::vector<std::string> pair = Split(pairs[i].c_str(), "=");
        if (pair.size() > 1)
        {
            request =
                CPLURLAddKVP(request.c_str(), pair[0].c_str(), pair[1].c_str());
        }
    }

    CPLDebug("WCS", "Requesting %s", request.c_str());
    return request;
}

/************************************************************************/
/*                        DescribeCoverageRequest()                     */
/*                                                                      */
/************************************************************************/

std::string WCSDataset201::DescribeCoverageRequest()
{
    std::string request = CPLGetXMLValue(psService, "ServiceURL", "");
    request = CPLURLAddKVP(request.c_str(), "SERVICE", "WCS");
    request = CPLURLAddKVP(request.c_str(), "REQUEST", "DescribeCoverage");
    request = CPLURLAddKVP(request.c_str(), "VERSION",
                           CPLGetXMLValue(psService, "Version", "2.0.1"));
    request = CPLURLAddKVP(request.c_str(), "COVERAGEID",
                           CPLGetXMLValue(psService, "CoverageName", ""));
    std::string extra = CPLGetXMLValue(psService, "Parameters", "");
    if (extra != "")
    {
        std::vector<std::string> pairs = Split(extra.c_str(), "&");
        for (unsigned int i = 0; i < pairs.size(); ++i)
        {
            std::vector<std::string> pair = Split(pairs[i].c_str(), "=");
            request =
                CPLURLAddKVP(request.c_str(), pair[0].c_str(), pair[1].c_str());
        }
    }
    extra = CPLGetXMLValue(psService, "DescribeCoverageExtra", "");
    if (extra != "")
    {
        std::vector<std::string> pairs = Split(extra.c_str(), "&");
        for (unsigned int i = 0; i < pairs.size(); ++i)
        {
            std::vector<std::string> pair = Split(pairs[i].c_str(), "=");
            request =
                CPLURLAddKVP(request.c_str(), pair[0].c_str(), pair[1].c_str());
        }
    }
    CPLDebug("WCS", "Requesting %s", request.c_str());
    return request;
}

/************************************************************************/
/*                             GridOffsets()                            */
/*                                                                      */
/************************************************************************/

bool WCSDataset201::GridOffsets(CPLXMLNode *grid, const std::string &subtype,
                                bool swap_grid_axis,
                                std::vector<double> &origin,
                                std::vector<std::vector<double>> &offset,
                                std::vector<std::string> axes, char ***metadata)
{
    // todo: use domain_index

    // origin position, center of cell
    CPLXMLNode *point = CPLGetXMLNode(grid, "origin.Point.pos");
    origin = Flist(
        Split(CPLGetXMLValue(point, nullptr, ""), " ", axis_order_swap), 0, 2);

    // offsets = coefficients of affine transformation from cell coords to
    // CRS coords, (1,2) and (4,5)

    if (subtype == "RectifiedGrid")
    {

        // for rectified grid the geo transform is from origin and offsetVectors
        int i = 0;
        for (CPLXMLNode *node = grid->psChild; node != nullptr;
             node = node->psNext)
        {
            if (node->eType != CXT_Element ||
                !EQUAL(node->pszValue, "offsetVector"))
            {
                continue;
            }
            offset.push_back(Flist(
                Split(CPLGetXMLValue(node, nullptr, ""), " ", axis_order_swap),
                0, 2));
            if (i == 1)
            {
                break;
            }
            i++;
        }
        if (offset.size() < 2)
        {
            // error or not?
            offset.push_back(std::vector<double>{1, 0});  // x
            offset.push_back(std::vector<double>{0, 1});  // y
        }
        // if axis_order_swap
        // the offset order should be swapped
        // Rasdaman does it
        // MapServer and GeoServer not
        if (swap_grid_axis)
        {
            std::swap(offset[0], offset[1]);
        }
    }
    else
    {  // if (coverage_type == "ReferenceableGrid"(ByVector)) {

        // for vector referenceable grid the geo transform is from
        // offsetVector, coefficients, gridAxesSpanned, sequenceRule
        // in generalGridAxis.GeneralGridAxis
        for (CPLXMLNode *node = grid->psChild; node != nullptr;
             node = node->psNext)
        {
            CPLXMLNode *axis = CPLGetXMLNode(node, "GeneralGridAxis");
            if (!axis)
            {
                continue;
            }
            std::string spanned = CPLGetXMLValue(axis, "gridAxesSpanned", "");
            int index = IndexOf(spanned, axes);
            if (index == -1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "This is not a rectilinear grid(?).");
                return false;
            }
            std::string coeffs = CPLGetXMLValue(axis, "coefficients", "");
            if (coeffs != "")
            {
                *metadata = CSLSetNameValue(
                    *metadata,
                    CPLString().Printf("DIMENSION_%i_COEFFS", index).c_str(),
                    coeffs.c_str());
            }
            std::string order =
                CPLGetXMLValue(axis, "sequenceRule.axisOrder", "");
            std::string rule = CPLGetXMLValue(axis, "sequenceRule", "");
            if (!(order == "+1" && rule == "Linear"))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Grids with sequence rule '%s' and axis order '%s' "
                         "are not supported.",
                         rule.c_str(), order.c_str());
                return false;
            }
            CPLXMLNode *offset_node = CPLGetXMLNode(axis, "offsetVector");
            if (offset_node)
            {
                offset.push_back(
                    Flist(Split(CPLGetXMLValue(offset_node, nullptr, ""), " ",
                                axis_order_swap),
                          0, 2));
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Missing offset vector in grid axis.");
                return false;
            }
        }
        // todo: make sure offset order is the same as the axes order but see
        // above
    }
    if (origin.size() < 2 || offset.size() < 2)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Could not parse origin or offset vectors from grid.");
        return false;
    }
    return true;
}

/************************************************************************/
/*                             GetSubdataset()                          */
/*                                                                      */
/************************************************************************/

std::string WCSDataset201::GetSubdataset(const std::string &coverage)
{
    char **metadata = GDALPamDataset::GetMetadata("SUBDATASETS");
    std::string subdataset;
    if (metadata != nullptr)
    {
        for (int i = 0; metadata[i] != nullptr; ++i)
        {
            char *key;
            std::string url = CPLParseNameValue(metadata[i], &key);
            if (key != nullptr && strstr(key, "SUBDATASET_") &&
                strstr(key, "_NAME"))
            {
                if (coverage == CPLURLGetValue(url.c_str(), "coverageId"))
                {
                    subdataset = key;
                    subdataset.erase(subdataset.find("_NAME"), 5);
                    CPLFree(key);
                    break;
                }
            }
            CPLFree(key);
        }
    }
    return subdataset;
}

/************************************************************************/
/*                             SetFormat()                              */
/*                                                                      */
/************************************************************************/

bool WCSDataset201::SetFormat(CPLXMLNode *coverage)
{
    // set the Format value in service,
    // unless it is set by the user
    std::string format = CPLGetXMLValue(psService, "Format", "");

    // todo: check the value against list of supported formats?
    if (format != "")
    {
        return true;
    }

    /*      We will prefer anything that sounds like TIFF, otherwise        */
    /*      falling back to the first supported format.  Should we          */
    /*      consider preferring the nativeFormat if available?              */

    char **metadata = GDALPamDataset::GetMetadata(nullptr);
    const char *value =
        CSLFetchNameValue(metadata, "WCS_GLOBAL#formatSupported");
    if (value == nullptr)
    {
        format = CPLGetXMLValue(coverage, "ServiceParameters.nativeFormat", "");
    }
    else
    {
        std::vector<std::string> format_list = Split(value, ",");
        for (unsigned j = 0; j < format_list.size(); ++j)
        {
            if (CPLString(format_list[j]).ifind("tiff") != std::string::npos)
            {
                format = format_list[j];
                break;
            }
        }
        if (format == "" && format_list.size() > 0)
        {
            format = format_list[0];
        }
    }
    if (format != "")
    {
        CPLSetXMLValue(psService, "Format", format.c_str());
        bServiceDirty = true;
        return true;
    }
    else
    {
        return false;
    }
}

/************************************************************************/
/*                         ParseGridFunction()                          */
/*                                                                      */
/************************************************************************/

bool WCSDataset201::ParseGridFunction(CPLXMLNode *coverage,
                                      std::vector<int> &axisOrder)
{
    CPLXMLNode *function =
        CPLGetXMLNode(coverage, "coverageFunction.GridFunction");
    if (function)
    {
        std::string path = "sequenceRule";
        std::string sequenceRule = CPLGetXMLValue(function, path.c_str(), "");
        path += ".axisOrder";
        axisOrder =
            Ilist(Split(CPLGetXMLValue(function, path.c_str(), ""), " "));
        // for now require simple
        if (sequenceRule != "Linear")
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Can't handle '%s' coverages.", sequenceRule.c_str());
            return false;
        }
    }
    return true;
}

/************************************************************************/
/*                             ParseRange()                             */
/*                                                                      */
/************************************************************************/

int WCSDataset201::ParseRange(CPLXMLNode *coverage,
                              const std::string &range_subset, char ***metadata)
{
    int fields = 0;
    // Default is to include all (types permitting?)
    // Can also be controlled with Range parameter

    // The contents of a rangeType is a swe:DataRecord
    const char *path = "rangeType.DataRecord";
    CPLXMLNode *record = CPLGetXMLNode(coverage, path);
    if (!record)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attributes are not defined in a DataRecord, giving up.");
        return 0;
    }

    // mapserver does not like field names, it wants indexes
    // so we should be able to give those

    // if Range is set remove those not in it
    std::vector<std::string> range = Split(range_subset.c_str(), ",");
    // todo: add check for range subsetting profile existence in server metadata
    // here
    unsigned int range_index = 0;  // index for reading from range
    bool in_band_range = false;

    unsigned int field_index = 1;
    std::vector<std::string> nodata_array;

    for (CPLXMLNode *field = record->psChild; field != nullptr;
         field = field->psNext)
    {
        if (field->eType != CXT_Element || !EQUAL(field->pszValue, "field"))
        {
            continue;
        }
        std::string fname = CPLGetXMLValue(field, "name", "");
        bool include = true;

        if (range.size() > 0)
        {
            include = false;
            if (range_index < range.size())
            {
                std::string current_range = range[range_index];
                std::string fname_test;

                if (atoi(current_range.c_str()) != 0)
                {
                    fname_test = CPLString().Printf("%i", field_index);
                }
                else
                {
                    fname_test = fname;
                }

                if (current_range == "*")
                {
                    include = true;
                }
                else if (current_range == fname_test)
                {
                    include = true;
                    range_index += 1;
                }
                else if (current_range.find(fname_test + ":") !=
                         std::string::npos)
                {
                    include = true;
                    in_band_range = true;
                }
                else if (current_range.find(":" + fname_test) !=
                         std::string::npos)
                {
                    include = true;
                    in_band_range = false;
                    range_index += 1;
                }
                else if (in_band_range)
                {
                    include = true;
                }
            }
        }

        if (include)
        {
            const std::string key =
                CPLString().Printf("FIELD_%i_", field_index);
            *metadata = CSLSetNameValue(*metadata, (key + "NAME").c_str(),
                                        fname.c_str());

            std::string nodata =
                CPLGetXMLValue(field, "Quantity.nilValues.NilValue", "");
            if (nodata != "")
            {
                *metadata = CSLSetNameValue(*metadata, (key + "NODATA").c_str(),
                                            nodata.c_str());
            }

            std::string descr =
                CPLGetXMLValue(field, "Quantity.description", "");
            if (descr != "")
            {
                *metadata = CSLSetNameValue(*metadata, (key + "DESCR").c_str(),
                                            descr.c_str());
            }

            path = "Quantity.constraint.AllowedValues.interval";
            std::string interval = CPLGetXMLValue(field, path, "");
            if (interval != "")
            {
                *metadata = CSLSetNameValue(
                    *metadata, (key + "INTERVAL").c_str(), interval.c_str());
            }

            nodata_array.push_back(std::move(nodata));
            fields += 1;
        }

        field_index += 1;
    }

    if (fields == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "No data fields found (bad Range?).");
    }
    else
    {
        // todo: default to the first one?
        bServiceDirty = CPLUpdateXML(psService, "NoDataValue",
                                     Join(nodata_array, ",").c_str()) ||
                        bServiceDirty;
    }

    return fields;
}

/************************************************************************/
/*                          ExtractGridInfo()                           */
/*                                                                      */
/*      Collect info about grid from describe coverage for WCS 2.0.     */
/*                                                                      */
/************************************************************************/

bool WCSDataset201::ExtractGridInfo()
{
    // this is for checking what's in service and for filling in empty slots in
    // it if the service file can be considered ready for use, this could be
    // skipped

    CPLXMLNode *coverage = CPLGetXMLNode(psService, "CoverageDescription");

    if (coverage == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CoverageDescription missing from service.");
        return false;
    }

    std::string subtype = CoverageSubtype(coverage);

    // get CRS from boundedBy.Envelope and set the native flag to true
    // below we may set the CRS again but that won't be native (however, non
    // native CRS is not yet supported) also axis order swap is set
    std::string path = "boundedBy.Envelope";
    CPLXMLNode *envelope = CPLGetXMLNode(coverage, path.c_str());
    if (envelope == nullptr)
    {
        path = "boundedBy.EnvelopeWithTimePeriod";
        envelope = CPLGetXMLNode(coverage, path.c_str());
        if (envelope == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Missing boundedBy.Envelope");
            return false;
        }
    }
    std::vector<std::string> bbox = ParseBoundingBox(envelope);
    if (!SetCRS(ParseCRS(envelope), true) || bbox.size() < 2)
    {
        return false;
    }

    // has the user set the domain?
    std::vector<std::string> domain =
        Split(CPLGetXMLValue(psService, "Domain", ""), ",");

    // names of axes
    std::vector<std::string> axes =
        Split(CPLGetXMLValue(coverage, (path + ".axisLabels").c_str(), ""), " ",
              axis_order_swap);
    std::vector<std::string> uoms =
        Split(CPLGetXMLValue(coverage, (path + ".uomLabels").c_str(), ""), " ",
              axis_order_swap);

    if (axes.size() < 2)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The coverage has less than 2 dimensions or no axisLabels.");
        return false;
    }

    std::vector<int> domain_indexes = IndexOf(domain, axes);
    if (Contains(domain_indexes, -1))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Axis in given domain does not exist in coverage.");
        return false;
    }
    if (domain_indexes.size() == 0)
    {  // default is the first two
        domain_indexes.push_back(0);
        domain_indexes.push_back(1);
    }
    if (domain.size() == 0)
    {
        domain.push_back(axes[0]);
        domain.push_back(axes[1]);
        CPLSetXMLValue(psService, "Domain", Join(domain, ",").c_str());
        bServiceDirty = true;
    }

    // GridFunction (is optional)
    // We support only linear grid functions.
    // axisOrder determines how data is arranged in the grid <order><axis
    // number> specifically: +2 +1 => swap grid envelope and the order of the
    // offsets
    std::vector<int> axisOrder;
    if (!ParseGridFunction(coverage, axisOrder))
    {
        return false;
    }

    const char *md_domain = "";
    char **metadata = CSLDuplicate(
        GetMetadata(md_domain));  // coverage metadata to be added/updated

    metadata = CSLSetNameValue(metadata, "DOMAIN", Join(domain, ",").c_str());

    // add coverage metadata: GeoServer TimeDomain

    CPLXMLNode *timedomain =
        CPLGetXMLNode(coverage, "metadata.Extension.TimeDomain");
    if (timedomain)
    {
        std::vector<std::string> timePositions;
        // "//timePosition"
        for (CPLXMLNode *node = timedomain->psChild; node != nullptr;
             node = node->psNext)
        {
            if (node->eType != CXT_Element ||
                strcmp(node->pszValue, "TimeInstant") != 0)
            {
                continue;
            }
            for (CPLXMLNode *node2 = node->psChild; node2 != nullptr;
                 node2 = node2->psNext)
            {
                if (node2->eType != CXT_Element ||
                    strcmp(node2->pszValue, "timePosition") != 0)
                {
                    continue;
                }
                timePositions.push_back(CPLGetXMLValue(node2, "", ""));
            }
        }
        metadata = CSLSetNameValue(metadata, "TimeDomain",
                                   Join(timePositions, ",").c_str());
    }

    // dimension metadata

    std::vector<std::string> slow =
        Split(bbox[0].c_str(), " ", axis_order_swap);
    std::vector<std::string> shigh =
        Split(bbox[1].c_str(), " ", axis_order_swap);
    bServiceDirty = CPLUpdateXML(psService, "Low", Join(slow, ",").c_str()) ||
                    bServiceDirty;
    bServiceDirty = CPLUpdateXML(psService, "High", Join(shigh, ",").c_str()) ||
                    bServiceDirty;
    if (slow.size() < 2 || shigh.size() < 2)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The coverage has less than 2 dimensions.");
        CSLDestroy(metadata);
        return false;
    }
    // todo: if our x,y domain is not the first two? use domain_indexes?
    std::vector<double> low = Flist(slow, 0, 2);
    std::vector<double> high = Flist(shigh, 0, 2);
    std::vector<double> env;
    env.insert(env.end(), low.begin(), low.begin() + 2);
    env.insert(env.end(), high.begin(), high.begin() + 2);

    for (unsigned int i = 0; i < axes.size(); ++i)
    {
        const std::string key = CPLString().Printf("DIMENSION_%i_", i);
        metadata =
            CSLSetNameValue(metadata, (key + "AXIS").c_str(), axes[i].c_str());
        if (i < uoms.size())
        {
            metadata = CSLSetNameValue(metadata, (key + "UOM").c_str(),
                                       uoms[i].c_str());
        }
        if (i < 2)
        {
            metadata = CSLSetNameValue(
                metadata, (key + "INTERVAL").c_str(),
                CPLString().Printf("%.15g,%.15g", low[i], high[i]));
        }
        else if (i < slow.size() && i < shigh.size())
        {
            metadata = CSLSetNameValue(
                metadata, (key + "INTERVAL").c_str(),
                CPLString().Printf("%s,%s", slow[i].c_str(), shigh[i].c_str()));
        }
        else if (i < bbox.size())
        {
            metadata = CSLSetNameValue(metadata, (key + "INTERVAL").c_str(),
                                       bbox[i].c_str());
        }
    }

    // domainSet
    // requirement 23: the srsName here _shall_ be the same as in boundedBy
    // => we ignore it
    // the CRS of this dataset is from boundedBy (unless it is overridden)
    // this is the size of this dataset
    // this gives the geotransform of this dataset (unless there is CRS
    // override)

    CPLXMLNode *grid = GetGridNode(coverage, subtype);
    if (!grid)
    {
        CSLDestroy(metadata);
        return false;
    }

    //
    bool swap_grid_axis = false;
    if (axisOrder.size() >= 2 && axisOrder[domain_indexes[0]] == 2 &&
        axisOrder[domain_indexes[1]] == 1)
    {
        swap_grid_axis = !CPLGetXMLBoolean(psService, "NoGridAxisSwap");
    }
    path = "limits.GridEnvelope";
    std::vector<std::vector<int>> size =
        ParseGridEnvelope(CPLGetXMLNode(grid, path.c_str()), swap_grid_axis);
    std::vector<int> grid_size;
    if (size.size() < 2)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Can't parse the grid envelope.");
        CSLDestroy(metadata);
        return false;
    }

    grid_size.push_back(size[1][domain_indexes[0]] -
                        size[0][domain_indexes[0]] + 1);
    grid_size.push_back(size[1][domain_indexes[1]] -
                        size[0][domain_indexes[1]] + 1);

    path = "axisLabels";
    bool swap_grid_axis_labels =
        swap_grid_axis || CPLGetXMLBoolean(psService, "GridAxisLabelSwap");
    std::vector<std::string> grid_axes = Split(
        CPLGetXMLValue(grid, path.c_str(), ""), " ", swap_grid_axis_labels);
    // autocorrect MapServer thing
    if (grid_axes.size() >= 2 && grid_axes[0] == "lat" &&
        grid_axes[1] == "long")
    {
        grid_axes[0] = "long";
        grid_axes[1] = "lat";
    }
    bServiceDirty =
        CPLUpdateXML(psService, "GridAxes", Join(grid_axes, ",").c_str()) ||
        bServiceDirty;

    std::vector<double> origin;
    std::vector<std::vector<double>> offsets;
    if (!GridOffsets(grid, subtype, swap_grid_axis, origin, offsets, axes,
                     &metadata))
    {
        CSLDestroy(metadata);
        return false;
    }

    SetGeometry(grid_size, origin, offsets);

    // subsetting and dimension to bands
    std::vector<std::string> dimensions;
    std::string range;
    std::vector<std::vector<std::string>> others;
    ParseParameters(psService, dimensions, range, others);

    // it is ok to have trimming or even slicing for x/y, it just affects our
    // bounding box but that is a todo item todo: BoundGeometry(domain_trim) if
    // domain_trim.size() > 0
    std::vector<std::vector<double>> domain_trim;

    // are all dimensions that are not x/y domain sliced?
    // if not, bands can't be defined, see below
    bool dimensions_are_ok = true;
    for (unsigned int i = 0; i < axes.size(); ++i)
    {
        std::vector<std::string> params;
        for (unsigned int j = 0; j < dimensions.size(); ++j)
        {
            if (dimensions[j].find(axes[i] + "(") != std::string::npos)
            {
                params = Split(FromParenthesis(dimensions[j]).c_str(), ",");
                break;
            }
        }
        int domain_index = IndexOf(axes[i], domain);
        if (domain_index != -1)
        {
            domain_trim.push_back(Flist(params, 0, 2));
            continue;
        }
        // size == 1 => sliced
        if (params.size() != 1)
        {
            dimensions_are_ok = false;
        }
    }
    // todo: add metadata: note: no bands, you need to subset to get data

    // check for CRS override
    std::string crs = CPLGetXMLValue(psService, "SRS", "");
    if (crs != "" && crs != osCRS)
    {
        if (!SetCRS(crs, false))
        {
            CSLDestroy(metadata);
            return false;
        }
        // todo: support CRS override, it requires warping the grid to the new
        // CRS SetGeometry(grid_size, origin, offsets);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CRS override not yet supported.");
        CSLDestroy(metadata);
        return false;
    }

    // todo: ElevationDomain, DimensionDomain

    // rangeType

    // get the field metadata
    // get the count of fields
    // if Range is set in service that may limit the fields
    int fields = ParseRange(coverage, range, &metadata);
    // if fields is 0 an error message has been emitted
    // but we let this go on since the user may be experimenting
    // and she wants to see the resulting metadata and not just an error message
    // situation is ~the same when bands == 0 when we exit here

    // todo: do this only if metadata is dirty
    this->SetMetadata(metadata, md_domain);
    CSLDestroy(metadata);
    TrySaveXML();

    // determine the band count
    int bands = 0;
    if (dimensions_are_ok)
    {
        bands = fields;
    }
    bServiceDirty =
        CPLUpdateXML(psService, "BandCount", CPLString().Printf("%d", bands)) ||
        bServiceDirty;

    // set the Format value in service, unless it is set
    // by the user, either through direct edit or options
    if (!SetFormat(coverage))
    {
        // all attempts to find a format have failed...
        CPLError(CE_Failure, CPLE_AppDefined,
                 "All attempts to find a format have failed, giving up.");
        return false;
    }

    return true;
}
