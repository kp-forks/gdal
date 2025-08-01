/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Declaration for Peristable Auxiliary Metadata classes.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef GDAL_PAM_H_INCLUDED
#define GDAL_PAM_H_INCLUDED

//! @cond Doxygen_Suppress

#include "cpl_minixml.h"
#include "gdal_priv.h"
#include <array>
#include <limits>
#include <map>
#include <vector>

class GDALPamRasterBand;

/* Clone Info Flags */

#define GCIF_GEOTRANSFORM 0x01
#define GCIF_PROJECTION 0x02
#define GCIF_METADATA 0x04
#define GCIF_GCPS 0x08

#define GCIF_NODATA 0x001000
#define GCIF_CATEGORYNAMES 0x002000
#define GCIF_MINMAX 0x004000
#define GCIF_SCALEOFFSET 0x008000
#define GCIF_UNITTYPE 0x010000
#define GCIF_COLORTABLE 0x020000
/* Same value as GCIF_COLORTABLE */
#define GCIF_COLORINTERP 0x020000
#define GCIF_BAND_METADATA 0x040000
#define GCIF_RAT 0x080000
#define GCIF_MASK 0x100000
#define GCIF_BAND_DESCRIPTION 0x200000

#define GCIF_ONLY_IF_MISSING 0x10000000
#define GCIF_PROCESS_BANDS 0x20000000

#define GCIF_PAM_DEFAULT                                                       \
    (GCIF_GEOTRANSFORM | GCIF_PROJECTION | GCIF_METADATA | GCIF_GCPS |         \
     GCIF_NODATA | GCIF_CATEGORYNAMES | GCIF_MINMAX | GCIF_SCALEOFFSET |       \
     GCIF_UNITTYPE | GCIF_COLORTABLE | GCIF_BAND_METADATA | GCIF_RAT |         \
     GCIF_MASK | GCIF_ONLY_IF_MISSING | GCIF_PROCESS_BANDS |                   \
     GCIF_BAND_DESCRIPTION)

/* GDAL PAM Flags */
/* ERO 2011/04/13 : GPF_AUXMODE seems to be unimplemented */
#define GPF_DIRTY 0x01              // .pam file needs to be written on close
#define GPF_TRIED_READ_FAILED 0x02  // no need to keep trying to read .pam.
#define GPF_DISABLED 0x04           // do not try any PAM stuff.
#define GPF_AUXMODE 0x08            // store info in .aux (HFA) file.
#define GPF_NOSAVE 0x10             // do not try to save pam info.

/* ==================================================================== */
/*      GDALDatasetPamInfo                                              */
/*                                                                      */
/*      We make these things a separate structure of information        */
/*      primarily so we can modify it without altering the size of      */
/*      the GDALPamDataset.  It is an effort to reduce ABI churn for    */
/*      driver plugins.                                                 */
/* ==================================================================== */
class GDALDatasetPamInfo
{
  public:
    char *pszPamFilename = nullptr;

    std::vector<CPLXMLTreeCloser> m_apoOtherNodes{};

    OGRSpatialReference *poSRS = nullptr;

    bool bHaveGeoTransform = false;
    GDALGeoTransform gt{};

    std::vector<gdal::GCP> asGCPs{};
    OGRSpatialReference *poGCP_SRS = nullptr;

    CPLString osPhysicalFilename{};
    CPLString osSubdatasetName{};
    CPLString osDerivedDatasetName{};
    CPLString osAuxFilename{};

    int bHasMetadata = false;
};

//! @endcond

/* ******************************************************************** */
/*                           GDALPamDataset                             */
/* ******************************************************************** */

/** PAM dataset */
class CPL_DLL GDALPamDataset : public GDALDataset
{
    friend class GDALPamRasterBand;

  private:
    int IsPamFilenameAPotentialSiblingFile();

  protected:
    GDALPamDataset(void);
    //! @cond Doxygen_Suppress
    int nPamFlags = 0;
    GDALDatasetPamInfo *psPam = nullptr;

    virtual CPLXMLNode *SerializeToXML(const char *);
    virtual CPLErr XMLInit(const CPLXMLNode *, const char *);

    virtual CPLErr TryLoadXML(CSLConstList papszSiblingFiles = nullptr);
    virtual CPLErr TrySaveXML();

    CPLErr TryLoadAux(CSLConstList papszSiblingFiles = nullptr);
    CPLErr TrySaveAux();

    virtual const char *BuildPamFilename();

    void PamInitialize();
    void PamClear();

    void SetPhysicalFilename(const char *);
    const char *GetPhysicalFilename();
    void SetSubdatasetName(const char *);
    const char *GetSubdatasetName();
    void SetDerivedDatasetName(const char *);
    //! @endcond

  public:
    ~GDALPamDataset() override;

    CPLErr FlushCache(bool bAtClosing) override;

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    CPLErr GetGeoTransform(GDALGeoTransform &) const override;
    CPLErr SetGeoTransform(const GDALGeoTransform &) override;
    void DeleteGeoTransform();

    int GetGCPCount() override;
    const OGRSpatialReference *GetGCPSpatialRef() const override;
    const GDAL_GCP *GetGCPs() override;
    using GDALDataset::SetGCPs;
    CPLErr SetGCPs(int nGCPCount, const GDAL_GCP *pasGCPList,
                   const OGRSpatialReference *poSRS) override;

    CPLErr SetMetadata(char **papszMetadata,
                       const char *pszDomain = "") override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain = "") override;
    char **GetMetadata(const char *pszDomain = "") override;
    const char *GetMetadataItem(const char *pszName,
                                const char *pszDomain = "") override;

    char **GetFileList(void) override;

    void ClearStatistics() override;

    //! @cond Doxygen_Suppress
    virtual CPLErr CloneInfo(GDALDataset *poSrcDS, int nCloneInfoFlags);

    CPLErr IBuildOverviews(const char *pszResampling, int nOverviews,
                           const int *panOverviewList, int nListBands,
                           const int *panBandList, GDALProgressFunc pfnProgress,
                           void *pProgressData,
                           CSLConstList papszOptions) override;

    // "semi private" methods.
    void MarkPamDirty();

    GDALDatasetPamInfo *GetPamInfo()
    {
        return psPam;
    }

    int GetPamFlags()
    {
        return nPamFlags;
    }

    void SetPamFlags(int nValue)
    {
        nPamFlags = nValue;
    }

    //! @endcond

  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALPamDataset)

    // cached return of GetMetadataItem("OVERVIEW_FILE", "OVERVIEWS")
    std::string m_osOverviewFile{};
};

//! @cond Doxygen_Suppress

constexpr double GDAL_PAM_DEFAULT_NODATA_VALUE = 0;
// Parenthesis for external code around std::numeric_limits<>::min/max,
// for external Windows code that might have includes <windows.h> before
// without defining NOMINMAX
constexpr int64_t GDAL_PAM_DEFAULT_NODATA_VALUE_INT64 =
    (std::numeric_limits<int64_t>::min)();
constexpr uint64_t GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64 =
    (std::numeric_limits<uint64_t>::max)();

/* ==================================================================== */
/*      GDALRasterBandPamInfo                                           */
/*                                                                      */
/*      We make these things a separate structure of information        */
/*      primarily so we can modify it without altering the size of      */
/*      the GDALPamDataset.  It is an effort to reduce ABI churn for    */
/*      driver plugins.                                                 */
/* ==================================================================== */
struct GDALRasterBandPamInfo
{
    GDALPamDataset *poParentDS = nullptr;

    bool bNoDataValueSet = false;
    bool bNoDataValueSetAsInt64 = false;
    bool bNoDataValueSetAsUInt64 = false;

    double dfNoDataValue = GDAL_PAM_DEFAULT_NODATA_VALUE;
    int64_t nNoDataValueInt64 = GDAL_PAM_DEFAULT_NODATA_VALUE_INT64;
    uint64_t nNoDataValueUInt64 = GDAL_PAM_DEFAULT_NODATA_VALUE_UINT64;

    GDALColorTable *poColorTable = nullptr;

    GDALColorInterp eColorInterp = GCI_Undefined;

    char *pszUnitType = nullptr;
    char **papszCategoryNames = nullptr;

    double dfOffset = 0.0;
    double dfScale = 1.0;

    int bHaveMinMax = FALSE;
    double dfMin = 0;
    double dfMax = 0;

    int bHaveStats = FALSE;
    double dfMean = 0;
    double dfStdDev = 0;

    CPLXMLNode *psSavedHistograms = nullptr;

    GDALRasterAttributeTable *poDefaultRAT = nullptr;

    bool bOffsetSet = false;
    bool bScaleSet = false;

    void CopyFrom(const GDALRasterBandPamInfo &sOther);
};

//! @endcond
/* ******************************************************************** */
/*                          GDALPamRasterBand                           */
/* ******************************************************************** */

/** PAM raster band */
class CPL_DLL GDALPamRasterBand : public GDALRasterBand
{
    friend class GDALPamDataset;

  protected:
    //! @cond Doxygen_Suppress
    virtual CPLXMLNode *SerializeToXML(const char *pszVRTPath);
    virtual CPLErr XMLInit(const CPLXMLNode *, const char *);

    void PamInitialize();
    void PamClear();
    void PamInitializeNoParent();
    void MarkPamDirty();

    GDALRasterBandPamInfo *psPam = nullptr;
    //! @endcond

  public:
    GDALPamRasterBand();
    //! @cond Doxygen_Suppress
    explicit GDALPamRasterBand(int bForceCachedIO);
    //! @endcond
    ~GDALPamRasterBand() override;

    void SetDescription(const char *) override;

    CPLErr SetNoDataValue(double) override;
    CPLErr SetNoDataValueAsInt64(int64_t nNoData) override;
    CPLErr SetNoDataValueAsUInt64(uint64_t nNoData) override;
    double GetNoDataValue(int *pbSuccess = nullptr) override;
    int64_t GetNoDataValueAsInt64(int *pbSuccess = nullptr) override;
    uint64_t GetNoDataValueAsUInt64(int *pbSuccess = nullptr) override;
    CPLErr DeleteNoDataValue() override;

    CPLErr SetColorTable(GDALColorTable *) override;
    GDALColorTable *GetColorTable() override;

    CPLErr SetColorInterpretation(GDALColorInterp) override;
    GDALColorInterp GetColorInterpretation() override;

    const char *GetUnitType() override;
    CPLErr SetUnitType(const char *) override;

    char **GetCategoryNames() override;
    CPLErr SetCategoryNames(char **) override;

    double GetOffset(int *pbSuccess = nullptr) override;
    CPLErr SetOffset(double) override;
    double GetScale(int *pbSuccess = nullptr) override;
    CPLErr SetScale(double) override;

    CPLErr GetHistogram(double dfMin, double dfMax, int nBuckets,
                        GUIntBig *panHistogram, int bIncludeOutOfRange,
                        int bApproxOK, GDALProgressFunc,
                        void *pProgressData) override;

    CPLErr GetDefaultHistogram(double *pdfMin, double *pdfMax, int *pnBuckets,
                               GUIntBig **ppanHistogram, int bForce,
                               GDALProgressFunc, void *pProgressData) override;

    CPLErr SetDefaultHistogram(double dfMin, double dfMax, int nBuckets,
                               GUIntBig *panHistogram) override;

    CPLErr SetMetadata(char **papszMetadata,
                       const char *pszDomain = "") override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain = "") override;

    GDALRasterAttributeTable *GetDefaultRAT() override;
    CPLErr SetDefaultRAT(const GDALRasterAttributeTable *) override;

    //! @cond Doxygen_Suppress
    // new in GDALPamRasterBand.
    virtual CPLErr CloneInfo(GDALRasterBand *poSrcBand, int nCloneInfoFlags);

    // "semi private" methods.
    GDALRasterBandPamInfo *GetPamInfo()
    {
        return psPam;
    }

    //! @endcond
  private:
    CPL_DISALLOW_COPY_ASSIGN(GDALPamRasterBand)

    void ResetNoDataValues();
};

//! @cond Doxygen_Suppress

/* ******************************************************************** */
/*                          GDALPamMultiDim                             */
/* ******************************************************************** */

/** Class that serializes/deserializes metadata on multidimensional objects.
 * Currently SRS on GDALMDArray.
 */
class CPL_DLL GDALPamMultiDim
{
    struct Private;
    std::unique_ptr<Private> d;

    void Load();
    void Save();

  public:
    explicit GDALPamMultiDim(const std::string &osFilename);
    virtual ~GDALPamMultiDim();

    std::shared_ptr<OGRSpatialReference>
    GetSpatialRef(const std::string &osArrayFullName,
                  const std::string &osContext);

    void SetSpatialRef(const std::string &osArrayFullName,
                       const std::string &osContext,
                       const OGRSpatialReference *poSRS);

    CPLErr GetStatistics(const std::string &osArrayFullName,
                         const std::string &osContext, bool bApproxOK,
                         double *pdfMin, double *pdfMax, double *pdfMean,
                         double *pdfStdDev, GUInt64 *pnValidCount);

    void SetStatistics(const std::string &osArrayFullName,
                       const std::string &osContext, bool bApproxStats,
                       double dfMin, double dfMax, double dfMean,
                       double dfStdDev, GUInt64 nValidCount);

    void ClearStatistics();

    void ClearStatistics(const std::string &osArrayFullName,
                         const std::string &osContext);

    static std::shared_ptr<GDALPamMultiDim>
    GetPAM(const std::shared_ptr<GDALMDArray> &poParent);
};

/* ******************************************************************** */
/*                          GDALPamMDArray                              */
/* ******************************************************************** */

/** Class that relies on GDALPamMultiDim to serializes/deserializes metadata. */
class CPL_DLL GDALPamMDArray : public GDALMDArray
{
    std::shared_ptr<GDALPamMultiDim> m_poPam;

  protected:
    GDALPamMDArray(const std::string &osParentName, const std::string &osName,
                   const std::shared_ptr<GDALPamMultiDim> &poPam,
                   const std::string &osContext = std::string());

    bool SetStatistics(bool bApproxStats, double dfMin, double dfMax,
                       double dfMean, double dfStdDev, GUInt64 nValidCount,
                       CSLConstList papszOptions) override;

  public:
    const std::shared_ptr<GDALPamMultiDim> &GetPAM() const
    {
        return m_poPam;
    }

    CPLErr GetStatistics(bool bApproxOK, bool bForce, double *pdfMin,
                         double *pdfMax, double *pdfMean, double *padfStdDev,
                         GUInt64 *pnValidCount, GDALProgressFunc pfnProgress,
                         void *pProgressData) override;

    void ClearStatistics() override;

    bool SetSpatialRef(const OGRSpatialReference *poSRS) override;

    std::shared_ptr<OGRSpatialReference> GetSpatialRef() const override;
};

// These are mainly helper functions for internal use.
int CPL_DLL PamParseHistogram(CPLXMLNode *psHistItem, double *pdfMin,
                              double *pdfMax, int *pnBuckets,
                              GUIntBig **ppanHistogram,
                              int *pbIncludeOutOfRange, int *pbApproxOK);
CPLXMLNode CPL_DLL *PamFindMatchingHistogram(CPLXMLNode *psSavedHistograms,
                                             double dfMin, double dfMax,
                                             int nBuckets,
                                             int bIncludeOutOfRange,
                                             int bApproxOK);
CPLXMLNode CPL_DLL *PamHistogramToXMLTree(double dfMin, double dfMax,
                                          int nBuckets, GUIntBig *panHistogram,
                                          int bIncludeOutOfRange, int bApprox);

// For managing the proxy file database.
const char CPL_DLL *PamGetProxy(const char *);
const char CPL_DLL *PamAllocateProxy(const char *);
const char CPL_DLL *PamDeallocateProxy(const char *);
void CPL_DLL PamCleanProxyDB(void);

//! @endcond

#endif /* ndef GDAL_PAM_H_INCLUDED */
