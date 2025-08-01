/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Base class for raster file formats.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1998, 2003, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal.h"
#include "gdal_priv.h"

#include <array>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <utility>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_hash_set.h"
#include "cpl_multiproc.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "cpl_vsi_error.h"
#include "gdal_alg.h"
#include "ogr_api.h"
#include "ogr_attrind.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_featurestyle.h"
#include "ogr_gensql.h"
#include "ogr_geometry.h"
#include "ogr_p.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"
#include "ograpispy.h"
#include "ogrsf_frmts.h"
#include "ogrunionlayer.h"
#include "ogr_swq.h"

#include "../frmts/derived/derivedlist.h"

#ifdef SQLITE_ENABLED
#include "../sqlite/ogrsqliteexecutesql.h"
#endif

extern const swq_field_type SpecialFieldTypes[SPECIAL_FIELD_COUNT];

CPL_C_START
GDALAsyncReader *GDALGetDefaultAsyncReader(GDALDataset *poDS, int nXOff,
                                           int nYOff, int nXSize, int nYSize,
                                           void *pBuf, int nBufXSize,
                                           int nBufYSize, GDALDataType eBufType,
                                           int nBandCount, int *panBandMap,
                                           int nPixelSpace, int nLineSpace,
                                           int nBandSpace, char **papszOptions);
CPL_C_END

enum class GDALAllowReadWriteMutexState
{
    RW_MUTEX_STATE_UNKNOWN,
    RW_MUTEX_STATE_ALLOWED,
    RW_MUTEX_STATE_DISABLED
};

const GIntBig TOTAL_FEATURES_NOT_INIT = -2;
const GIntBig TOTAL_FEATURES_UNKNOWN = -1;

class GDALDataset::Private
{
    CPL_DISALLOW_COPY_ASSIGN(Private)

  public:
    CPLMutex *hMutex = nullptr;
    std::map<GIntBig, int> oMapThreadToMutexTakenCount{};
#ifdef DEBUG_EXTRA
    std::map<GIntBig, int> oMapThreadToMutexTakenCountSaved{};
#endif
    GDALAllowReadWriteMutexState eStateReadWriteMutex =
        GDALAllowReadWriteMutexState::RW_MUTEX_STATE_UNKNOWN;
    int nCurrentLayerIdx = 0;
    int nLayerCount = -1;
    GIntBig nFeatureReadInLayer = 0;
    GIntBig nFeatureReadInDataset = 0;
    GIntBig nTotalFeaturesInLayer = TOTAL_FEATURES_NOT_INIT;
    GIntBig nTotalFeatures = TOTAL_FEATURES_NOT_INIT;
    OGRLayer *poCurrentLayer = nullptr;

    std::mutex m_oMutexWKT{};

    char *m_pszWKTCached = nullptr;
    OGRSpatialReference *m_poSRSCached = nullptr;
    char *m_pszWKTGCPCached = nullptr;
    OGRSpatialReference *m_poSRSGCPCached = nullptr;

    GDALDataset *poParentDataset = nullptr;

    bool m_bOverviewsEnabled = true;

    std::vector<int>
        m_anBandMap{};  // used by RasterIO(). Values are 1, 2, etc.

    Private() = default;
};

struct SharedDatasetCtxt
{
    // PID of the thread that mark the dataset as shared
    // This may not be the actual PID, but the responsiblePID.
    GIntBig nPID;
    char *pszDescription;
    char *pszConcatenatedOpenOptions;
    int nOpenFlags;

    GDALDataset *poDS;
};

// Set of datasets opened as shared datasets (with GDALOpenShared)
// The values in the set are of type SharedDatasetCtxt.
static CPLHashSet *phSharedDatasetSet = nullptr;

// Set of all datasets created in the constructor of GDALDataset.
// In the case of a shared dataset, memorize the PID of the thread
// that marked the dataset as shared, so that we can remove it from
// the phSharedDatasetSet in the destructor of the dataset, even
// if GDALClose is called from a different thread.
static std::map<GDALDataset *, GIntBig> *poAllDatasetMap = nullptr;

static CPLMutex *hDLMutex = nullptr;

// Static array of all datasets. Used by GDALGetOpenDatasets.
// Not thread-safe. See GDALGetOpenDatasets.
static GDALDataset **ppDatasets = nullptr;

static unsigned long GDALSharedDatasetHashFunc(const void *elt)
{
    const SharedDatasetCtxt *psStruct =
        static_cast<const SharedDatasetCtxt *>(elt);
    return static_cast<unsigned long>(
        CPLHashSetHashStr(psStruct->pszDescription) ^
        CPLHashSetHashStr(psStruct->pszConcatenatedOpenOptions) ^
        psStruct->nOpenFlags ^ psStruct->nPID);
}

static int GDALSharedDatasetEqualFunc(const void *elt1, const void *elt2)
{
    const SharedDatasetCtxt *psStruct1 =
        static_cast<const SharedDatasetCtxt *>(elt1);
    const SharedDatasetCtxt *psStruct2 =
        static_cast<const SharedDatasetCtxt *>(elt2);
    return strcmp(psStruct1->pszDescription, psStruct2->pszDescription) == 0 &&
           strcmp(psStruct1->pszConcatenatedOpenOptions,
                  psStruct2->pszConcatenatedOpenOptions) == 0 &&
           psStruct1->nPID == psStruct2->nPID &&
           psStruct1->nOpenFlags == psStruct2->nOpenFlags;
}

static void GDALSharedDatasetFreeFunc(void *elt)
{
    SharedDatasetCtxt *psStruct = static_cast<SharedDatasetCtxt *>(elt);
    CPLFree(psStruct->pszDescription);
    CPLFree(psStruct->pszConcatenatedOpenOptions);
    CPLFree(psStruct);
}

static std::string
GDALSharedDatasetConcatenateOpenOptions(CSLConstList papszOpenOptions)
{
    std::string osStr;
    for (const char *pszOption : cpl::Iterate(papszOpenOptions))
        osStr += pszOption;
    return osStr;
}

/************************************************************************/
/* Functions shared between gdalproxypool.cpp and gdaldataset.cpp */
/************************************************************************/

// The open-shared mutex must be used by the ProxyPool too.
CPLMutex **GDALGetphDLMutex()
{
    return &hDLMutex;
}

// The current thread will act in the behalf of the thread of PID
// responsiblePID.
void GDALSetResponsiblePIDForCurrentThread(GIntBig responsiblePID)
{
    GIntBig *pResponsiblePID =
        static_cast<GIntBig *>(CPLGetTLS(CTLS_RESPONSIBLEPID));
    if (pResponsiblePID == nullptr)
    {
        pResponsiblePID = static_cast<GIntBig *>(CPLMalloc(sizeof(GIntBig)));
        CPLSetTLS(CTLS_RESPONSIBLEPID, pResponsiblePID, TRUE);
    }
    *pResponsiblePID = responsiblePID;
}

// Get the PID of the thread that the current thread will act in the behalf of
// By default : the current thread acts in the behalf of itself.
GIntBig GDALGetResponsiblePIDForCurrentThread()
{
    GIntBig *pResponsiblePID =
        static_cast<GIntBig *>(CPLGetTLS(CTLS_RESPONSIBLEPID));
    if (pResponsiblePID == nullptr)
        return CPLGetPID();
    return *pResponsiblePID;
}

/************************************************************************/
/* ==================================================================== */
/*                             GDALDataset                              */
/* ==================================================================== */
/************************************************************************/

/**
 * \class GDALDataset "gdal_priv.h"
 *
 * A dataset encapsulating one or more raster bands.  Details are further
 * discussed in the <a href="https://gdal.org/user/raster_data_model.html">GDAL
 * Raster Data Model</a>.
 *
 * Use GDALOpen() or GDALOpenShared() to create a GDALDataset for a named file,
 * or GDALDriver::Create() or GDALDriver::CreateCopy() to create a new
 * dataset.
 */

/************************************************************************/
/*                            GDALDataset()                             */
/************************************************************************/

//! @cond Doxygen_Suppress
GDALDataset::GDALDataset()
    : GDALDataset(CPLTestBool(CPLGetConfigOption("GDAL_FORCE_CACHING", "NO")))
{
}

GDALDataset::GDALDataset(int bForceCachedIOIn)
    : bForceCachedIO(CPL_TO_BOOL(bForceCachedIOIn)),
      m_poPrivate(new(std::nothrow) GDALDataset::Private)
{
}

//! @endcond

/************************************************************************/
/*                            ~GDALDataset()                            */
/************************************************************************/

/**
 * \brief Destroy an open GDALDataset.
 *
 * This is the accepted method of closing a GDAL dataset and deallocating
 * all resources associated with it.
 *
 * Equivalent of the C callable GDALClose().  Except that GDALClose() first
 * decrements the reference count, and then closes only if it has dropped to
 * zero.
 *
 * For Windows users, it is not recommended to use the delete operator on the
 * dataset object because of known issues when allocating and freeing memory
 * across module boundaries. Calling GDALClose() is then a better option.
 */

GDALDataset::~GDALDataset()

{
    // we don't want to report destruction of datasets that
    // were never really open or meant as internal
    if (!bIsInternal && (nBands != 0 || !EQUAL(GetDescription(), "")))
    {
        if (CPLGetPID() != GDALGetResponsiblePIDForCurrentThread())
            CPLDebug("GDAL",
                     "GDALClose(%s, this=%p) (pid=%d, responsiblePID=%d)",
                     GetDescription(), this, static_cast<int>(CPLGetPID()),
                     static_cast<int>(GDALGetResponsiblePIDForCurrentThread()));
        else
            CPLDebug("GDAL", "GDALClose(%s, this=%p)", GetDescription(), this);
    }

    if (IsMarkedSuppressOnClose())
    {
        if (poDriver == nullptr ||
            // Someone issuing Create("foo.tif") on a
            // memory driver doesn't expect files with those names to be deleted
            // on a file system...
            // This is somewhat messy. Ideally there should be a way for the
            // driver to overload the default behavior
            (!EQUAL(poDriver->GetDescription(), "MEM") &&
             !EQUAL(poDriver->GetDescription(), "Memory")))
        {
            VSIUnlink(GetDescription());
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Remove dataset from the "open" dataset list.                    */
    /* -------------------------------------------------------------------- */
    if (!bIsInternal)
    {
        CPLMutexHolderD(&hDLMutex);
        if (poAllDatasetMap)
        {
            std::map<GDALDataset *, GIntBig>::iterator oIter =
                poAllDatasetMap->find(this);
            CPLAssert(oIter != poAllDatasetMap->end());

            UnregisterFromSharedDataset();

            poAllDatasetMap->erase(oIter);

            if (poAllDatasetMap->empty())
            {
                delete poAllDatasetMap;
                poAllDatasetMap = nullptr;
                if (phSharedDatasetSet)
                {
                    CPLHashSetDestroy(phSharedDatasetSet);
                }
                phSharedDatasetSet = nullptr;
                CPLFree(ppDatasets);
                ppDatasets = nullptr;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Destroy the raster bands if they exist.                         */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < nBands && papoBands != nullptr; ++i)
    {
        if (papoBands[i] != nullptr)
            delete papoBands[i];
        papoBands[i] = nullptr;
    }

    CPLFree(papoBands);

    if (m_poStyleTable)
    {
        delete m_poStyleTable;
        m_poStyleTable = nullptr;
    }

    if (m_poPrivate != nullptr)
    {
        if (m_poPrivate->hMutex != nullptr)
            CPLDestroyMutex(m_poPrivate->hMutex);

#if defined(__COVERITY__) || defined(DEBUG)
        // Not needed since at destruction there is no risk of concurrent use.
        std::lock_guard oLock(m_poPrivate->m_oMutexWKT);
#endif
        CPLFree(m_poPrivate->m_pszWKTCached);
        if (m_poPrivate->m_poSRSCached)
        {
            m_poPrivate->m_poSRSCached->Release();
        }
        CPLFree(m_poPrivate->m_pszWKTGCPCached);
        if (m_poPrivate->m_poSRSGCPCached)
        {
            m_poPrivate->m_poSRSGCPCached->Release();
        }
    }

    delete m_poPrivate;

    CSLDestroy(papszOpenOptions);
}

/************************************************************************/
/*                             Close()                                  */
/************************************************************************/

/** Do final cleanup before a dataset is destroyed.
 *
 * This method is typically called by GDALClose() or the destructor of a
 * GDALDataset subclass. It might also be called by C++ users before
 * destroying a dataset. It should not be called on a shared dataset whose
 * reference count is greater than one.
 *
 * It gives a last chance to the closing process to return an error code if
 * something goes wrong, in particular in creation / update scenarios where
 * file write or network communication might occur when finalizing the dataset.
 *
 * Implementations should be robust to this method to be called several times
 * (on subsequent calls, it should do nothing and return CE_None).
 * Once it has been called, no other method than Close() or the dataset
 * destructor should be called. RasterBand or OGRLayer owned by the dataset
 * should be assumed as no longer being valid.
 *
 * If a driver implements this method, it must also call it from its
 * dataset destructor.
 *
 * A typical implementation might look as the following
 * \code{.cpp}
 *
 *  MyDataset::~MyDataset()
 *  {
 *     try
 *     {
 *         MyDataset::Close();
 *     }
 *     catch (const std::exception &exc)
 *     {
 *         // If Close() can throw exception
 *         CPLError(CE_Failure, CPLE_AppDefined,
 *                  "Exception thrown in MyDataset::Close(): %s",
 *                  exc.what());
 *     }
 *     catch (...)
 *     {
 *         // If Close() can throw exception
 *         CPLError(CE_Failure, CPLE_AppDefined,
 *                  "Exception thrown in MyDataset::Close()");
 *     }
 *  }
 *
 *  CPLErr MyDataset::Close()
 *  {
 *      CPLErr eErr = CE_None;
 *      if( nOpenFlags != OPEN_FLAGS_CLOSED )
 *      {
 *          if( MyDataset::FlushCache(true) != CE_None )
 *              eErr = CE_Failure;
 *
 *          // Do something driver specific
 *          if (m_fpImage)
 *          {
 *              if( VSIFCloseL(m_fpImage) != 0 )
 *              {
 *                  CPLError(CE_Failure, CPLE_FileIO, "VSIFCloseL() failed");
 *                  eErr = CE_Failure;
 *              }
 *          }
 *
 *          // Call parent Close() implementation.
 *          if( MyParentDatasetClass::Close() != CE_None )
 *              eErr = CE_Failure;
 *      }
 *      return eErr;
 *  }
 * \endcode
 *
 * @since GDAL 3.7
 */
CPLErr GDALDataset::Close()
{
    // Call UnregisterFromSharedDataset() before altering nOpenFlags
    UnregisterFromSharedDataset();

    nOpenFlags = OPEN_FLAGS_CLOSED;
    return CE_None;
}

/************************************************************************/
/*                UnregisterFromSharedDataset()                         */
/************************************************************************/

void GDALDataset::UnregisterFromSharedDataset()
{
    if (!(!bIsInternal && bShared && poAllDatasetMap && phSharedDatasetSet))
        return;

    CPLMutexHolderD(&hDLMutex);

    std::map<GDALDataset *, GIntBig>::iterator oIter =
        poAllDatasetMap->find(this);
    CPLAssert(oIter != poAllDatasetMap->end());
    const GIntBig nPIDCreatorForShared = oIter->second;
    bShared = false;
    SharedDatasetCtxt sStruct;
    sStruct.nPID = nPIDCreatorForShared;
    sStruct.nOpenFlags = nOpenFlags & ~GDAL_OF_SHARED;
    sStruct.pszDescription = const_cast<char *>(GetDescription());
    std::string osConcatenatedOpenOptions =
        GDALSharedDatasetConcatenateOpenOptions(papszOpenOptions);
    sStruct.pszConcatenatedOpenOptions = &osConcatenatedOpenOptions[0];
    sStruct.poDS = nullptr;
    SharedDatasetCtxt *psStruct = static_cast<SharedDatasetCtxt *>(
        CPLHashSetLookup(phSharedDatasetSet, &sStruct));
    if (psStruct && psStruct->poDS == this)
    {
        CPLHashSetRemove(phSharedDatasetSet, psStruct);
    }
    else
    {
        CPLDebug("GDAL",
                 "Should not happen. Cannot find %s, "
                 "this=%p in phSharedDatasetSet",
                 GetDescription(), this);
    }
}

/************************************************************************/
/*                      AddToDatasetOpenList()                          */
/************************************************************************/

void GDALDataset::AddToDatasetOpenList()
{
    /* -------------------------------------------------------------------- */
    /*      Add this dataset to the open dataset list.                      */
    /* -------------------------------------------------------------------- */
    bIsInternal = false;

    CPLMutexHolderD(&hDLMutex);

    if (poAllDatasetMap == nullptr)
        poAllDatasetMap = new std::map<GDALDataset *, GIntBig>;
    (*poAllDatasetMap)[this] = -1;
}

/************************************************************************/
/*                             FlushCache()                             */
/************************************************************************/

/**
 * \brief Flush all write cached data to disk.
 *
 * Any raster (or other GDAL) data written via GDAL calls, but buffered
 * internally will be written to disk.
 *
 * The default implementation of this method just calls the FlushCache() method
 * on each of the raster bands and the SyncToDisk() method
 * on each of the layers.  Conceptually, calling FlushCache() on a dataset
 * should include any work that might be accomplished by calling SyncToDisk()
 * on layers in that dataset.
 *
 * Using this method does not prevent use from calling GDALClose()
 * to properly close a dataset and ensure that important data not addressed
 * by FlushCache() is written in the file.
 *
 * This method is the same as the C function GDALFlushCache().
 *
 * @param bAtClosing Whether this is called from a GDALDataset destructor
 * @return CE_None in case of success (note: return value added in GDAL 3.7)
 */

CPLErr GDALDataset::FlushCache(bool bAtClosing)

{
    CPLErr eErr = CE_None;
    // This sometimes happens if a dataset is destroyed before completely
    // built.

    if (papoBands)
    {
        for (int i = 0; i < nBands; ++i)
        {
            if (papoBands[i])
            {
                if (papoBands[i]->FlushCache(bAtClosing) != CE_None)
                    eErr = CE_Failure;
            }
        }
    }

    const int nLayers = GetLayerCount();
    // cppcheck-suppress knownConditionTrueFalse
    if (nLayers > 0)
    {
        CPLMutexHolderD(m_poPrivate ? &(m_poPrivate->hMutex) : nullptr);
        for (int i = 0; i < nLayers; ++i)
        {
            OGRLayer *poLayer = GetLayer(i);

            if (poLayer)
            {
                if (poLayer->SyncToDisk() != OGRERR_NONE)
                    eErr = CE_Failure;
            }
        }
    }

    return eErr;
}

/************************************************************************/
/*                           GDALFlushCache()                           */
/************************************************************************/

/**
 * \brief Flush all write cached data to disk.
 *
 * @see GDALDataset::FlushCache().
 * @return CE_None in case of success (note: return value added in GDAL 3.7)
 */

CPLErr CPL_STDCALL GDALFlushCache(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALFlushCache", CE_Failure);

    return GDALDataset::FromHandle(hDS)->FlushCache(false);
}

/************************************************************************/
/*                             DropCache()                              */
/************************************************************************/

/**
* \brief Drop all write cached data
*
* This method is the same as the C function GDALDropCache().
*
* @return CE_None in case of success
* @since 3.9
*/

CPLErr GDALDataset::DropCache()

{
    CPLErr eErr = CE_None;

    if (papoBands)
    {
        for (int i = 0; i < nBands; ++i)
        {
            if (papoBands[i])
            {
                if (papoBands[i]->DropCache() != CE_None)
                    eErr = CE_Failure;
            }
        }
    }

    return eErr;
}

/************************************************************************/
/*                           GDALDropCache()                           */
/************************************************************************/

/**
* \brief Drop all write cached data
*
* @see GDALDataset::DropCache().
* @return CE_None in case of success
* @since 3.9
*/

CPLErr CPL_STDCALL GDALDropCache(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALDropCache", CE_Failure);

    return GDALDataset::FromHandle(hDS)->DropCache();
}

/************************************************************************/
/*                      GetEstimatedRAMUsage()                          */
/************************************************************************/

/**
 * \brief Return the intrinsic RAM usage of this dataset.
 *
 * The returned value should take into account caches in the underlying driver
 * and decoding library, but not the usage related to the GDAL block cache.
 *
 * At time of writing, this method is only implemented in the JP2OpenJPEG
 * driver. For single-tiled JPEG2000 images, the decoding of the image,
 * even partially, involves allocating at least
 * width * height * number_of_bands * sizeof(uint32_t) bytes inside the libopenjp2
 * library.
 *
 * This method is used by the GDALDatasetPool class, itself used by the GDAL VRT
 * driver, to determine how long a dataset in the pool must be kept open, given
 * the RAM usage of the dataset with respect to the usable total RAM.
 *
 * @since GDAL 3.7
 * @return RAM usage in bytes, or -1 if unknown (the default implementation
 * returns -1)
 */

GIntBig GDALDataset::GetEstimatedRAMUsage()
{
    return -1;
}

/************************************************************************/
/*                        BlockBasedFlushCache()                        */
/*                                                                      */
/*      This helper method can be called by the                         */
/*      GDALDataset::FlushCache() for particular drivers to ensure      */
/*      that buffers will be flushed in a manner suitable for pixel     */
/*      interleaved (by block) IO.  That is, if all the bands have      */
/*      the same size blocks then a given block will be flushed for     */
/*      all bands before proceeding to the next block.                  */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALDataset::BlockBasedFlushCache(bool bAtClosing)

{
    GDALRasterBand *poBand1 = GetRasterBand(1);
    if (poBand1 == nullptr || (IsMarkedSuppressOnClose() && bAtClosing))
    {
        return GDALDataset::FlushCache(bAtClosing);
    }

    int nBlockXSize = 0;
    int nBlockYSize = 0;
    poBand1->GetBlockSize(&nBlockXSize, &nBlockYSize);

    /* -------------------------------------------------------------------- */
    /*      Verify that all bands match.                                    */
    /* -------------------------------------------------------------------- */
    for (int iBand = 1; iBand < nBands; ++iBand)
    {
        GDALRasterBand *poBand = GetRasterBand(iBand + 1);

        int nThisBlockXSize, nThisBlockYSize;
        poBand->GetBlockSize(&nThisBlockXSize, &nThisBlockYSize);
        if (nThisBlockXSize != nBlockXSize && nThisBlockYSize != nBlockYSize)
        {
            return GDALDataset::FlushCache(bAtClosing);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Now flush writable data.                                        */
    /* -------------------------------------------------------------------- */
    for (int iY = 0; iY < poBand1->nBlocksPerColumn; ++iY)
    {
        for (int iX = 0; iX < poBand1->nBlocksPerRow; ++iX)
        {
            for (int iBand = 0; iBand < nBands; ++iBand)
            {
                const CPLErr eErr = papoBands[iBand]->FlushBlock(iX, iY);

                if (eErr != CE_None)
                    return CE_Failure;
            }
        }
    }
    return CE_None;
}

/************************************************************************/
/*                          RasterInitialize()                          */
/*                                                                      */
/*      Initialize raster size                                          */
/************************************************************************/

void GDALDataset::RasterInitialize(int nXSize, int nYSize)

{
    CPLAssert(nXSize > 0 && nYSize > 0);

    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
}

//! @endcond

/************************************************************************/
/*                              AddBand()                               */
/************************************************************************/

/**
 * \fn GDALDataset::AddBand(GDALDataType, char**)
 * \brief Add a band to a dataset.
 *
 * This method will add a new band to the dataset if the underlying format
 * supports this action.  Most formats do not.
 *
 * Note that the new GDALRasterBand is not returned.  It may be fetched
 * after successful completion of the method by calling
 * GDALDataset::GetRasterBand(GDALDataset::GetRasterCount()) as the newest
 * band will always be the last band.
 *
 * @param eType the data type of the pixels in the new band.
 *
 * @param papszOptions a list of NAME=VALUE option strings.  The supported
 * options are format specific.  NULL may be passed by default.
 *
 * @return CE_None on success or CE_Failure on failure.
 */

CPLErr GDALDataset::AddBand(CPL_UNUSED GDALDataType eType,
                            CPL_UNUSED char **papszOptions)

{
    ReportError(CE_Failure, CPLE_NotSupported,
                "Dataset does not support the AddBand() method.");

    return CE_Failure;
}

/************************************************************************/
/*                            GDALAddBand()                             */
/************************************************************************/

/**
 * \brief Add a band to a dataset.
 *
 * @see GDALDataset::AddBand().
 */

CPLErr CPL_STDCALL GDALAddBand(GDALDatasetH hDataset, GDALDataType eType,
                               CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDataset, "GDALAddBand", CE_Failure);

    return GDALDataset::FromHandle(hDataset)->AddBand(
        eType, const_cast<char **>(papszOptions));
}

/************************************************************************/
/*                              SetBand()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
/**  Set a band in the band array, updating the band count, and array size
 * appropriately.
 *
 * @param nNewBand new band number (indexing starts at 1)
 * @param poBand band object.
 */

void GDALDataset::SetBand(int nNewBand, GDALRasterBand *poBand)

{
    /* -------------------------------------------------------------------- */
    /*      Do we need to grow the bands list?                              */
    /* -------------------------------------------------------------------- */
    if (nBands < nNewBand || papoBands == nullptr)
    {
        GDALRasterBand **papoNewBands = nullptr;

        if (papoBands == nullptr)
            papoNewBands = static_cast<GDALRasterBand **>(VSICalloc(
                sizeof(GDALRasterBand *), std::max(nNewBand, nBands)));
        else
            papoNewBands = static_cast<GDALRasterBand **>(
                VSIRealloc(papoBands, sizeof(GDALRasterBand *) *
                                          std::max(nNewBand, nBands)));
        if (papoNewBands == nullptr)
        {
            ReportError(CE_Failure, CPLE_OutOfMemory,
                        "Cannot allocate band array");
            return;
        }

        papoBands = papoNewBands;

        for (int i = nBands; i < nNewBand; ++i)
            papoBands[i] = nullptr;

        nBands = std::max(nBands, nNewBand);

        if (m_poPrivate)
        {
            for (int i = static_cast<int>(m_poPrivate->m_anBandMap.size());
                 i < nBands; ++i)
            {
                m_poPrivate->m_anBandMap.push_back(i + 1);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Set the band.  Resetting the band is currently not permitted.   */
    /* -------------------------------------------------------------------- */
    if (papoBands[nNewBand - 1] != nullptr)
    {
        ReportError(CE_Failure, CPLE_NotSupported,
                    "Cannot set band %d as it is already set", nNewBand);
        return;
    }

    papoBands[nNewBand - 1] = poBand;

    /* -------------------------------------------------------------------- */
    /*      Set back reference information on the raster band.  Note        */
    /*      that the GDALDataset is a friend of the GDALRasterBand          */
    /*      specifically to allow this.                                     */
    /* -------------------------------------------------------------------- */
    poBand->nBand = nNewBand;
    poBand->poDS = this;
    poBand->nRasterXSize = nRasterXSize;
    poBand->nRasterYSize = nRasterYSize;
    poBand->eAccess = eAccess;  // Default access to be same as dataset.
}

//! @endcond

/************************************************************************/
/*                              SetBand()                               */
/************************************************************************/

//! @cond Doxygen_Suppress
/**  Set a band in the band array, updating the band count, and array size
 * appropriately.
 *
 * @param nNewBand new band number (indexing starts at 1)
 * @param poBand band object.
 */

void GDALDataset::SetBand(int nNewBand, std::unique_ptr<GDALRasterBand> poBand)
{
    SetBand(nNewBand, poBand.release());
}

//! @endcond

/************************************************************************/
/*                           GetRasterXSize()                           */
/************************************************************************/

/**

 \brief Fetch raster width in pixels.

 Equivalent of the C function GDALGetRasterXSize().

 @return the width in pixels of raster bands in this GDALDataset.

*/

int GDALDataset::GetRasterXSize() const
{
    return nRasterXSize;
}

/************************************************************************/
/*                         GDALGetRasterXSize()                         */
/************************************************************************/

/**
 * \brief Fetch raster width in pixels.
 *
 * @see GDALDataset::GetRasterXSize().
 */

int CPL_STDCALL GDALGetRasterXSize(GDALDatasetH hDataset)

{
    VALIDATE_POINTER1(hDataset, "GDALGetRasterXSize", 0);

    return GDALDataset::FromHandle(hDataset)->GetRasterXSize();
}

/************************************************************************/
/*                           GetRasterYSize()                           */
/************************************************************************/

/**

 \brief Fetch raster height in pixels.

 Equivalent of the C function GDALGetRasterYSize().

 @return the height in pixels of raster bands in this GDALDataset.

*/

int GDALDataset::GetRasterYSize() const
{
    return nRasterYSize;
}

/************************************************************************/
/*                         GDALGetRasterYSize()                         */
/************************************************************************/

/**
 * \brief Fetch raster height in pixels.
 *
 * @see GDALDataset::GetRasterYSize().
 */

int CPL_STDCALL GDALGetRasterYSize(GDALDatasetH hDataset)

{
    VALIDATE_POINTER1(hDataset, "GDALGetRasterYSize", 0);

    return GDALDataset::FromHandle(hDataset)->GetRasterYSize();
}

/************************************************************************/
/*                           GetRasterBand()                            */
/************************************************************************/

/**

 \brief Fetch a band object for a dataset.

 See GetBands() for a C++ iterator version of this method.

 Equivalent of the C function GDALGetRasterBand().

 @param nBandId the index number of the band to fetch, from 1 to
                GetRasterCount().

 @return the nBandId th band object

*/

GDALRasterBand *GDALDataset::GetRasterBand(int nBandId)

{
    if (papoBands)
    {
        if (nBandId < 1 || nBandId > nBands)
        {
            ReportError(CE_Failure, CPLE_IllegalArg,
                        "GDALDataset::GetRasterBand(%d) - Illegal band #\n",
                        nBandId);
            return nullptr;
        }

        return papoBands[nBandId - 1];
    }
    return nullptr;
}

/************************************************************************/
/*                           GetRasterBand()                            */
/************************************************************************/

/**

 \brief Fetch a band object for a dataset.

 See GetBands() for a C++ iterator version of this method.

 Equivalent of the C function GDALGetRasterBand().

 @param nBandId the index number of the band to fetch, from 1 to
                GetRasterCount().

 @return the nBandId th band object

*/

const GDALRasterBand *GDALDataset::GetRasterBand(int nBandId) const

{
    if (papoBands)
    {
        if (nBandId < 1 || nBandId > nBands)
        {
            ReportError(CE_Failure, CPLE_IllegalArg,
                        "GDALDataset::GetRasterBand(%d) - Illegal band #\n",
                        nBandId);
            return nullptr;
        }

        return papoBands[nBandId - 1];
    }
    return nullptr;
}

/************************************************************************/
/*                         GDALGetRasterBand()                          */
/************************************************************************/

/**
 * \brief Fetch a band object for a dataset.
 * @see GDALDataset::GetRasterBand().
 */

GDALRasterBandH CPL_STDCALL GDALGetRasterBand(GDALDatasetH hDS, int nBandId)

{
    VALIDATE_POINTER1(hDS, "GDALGetRasterBand", nullptr);

    return GDALRasterBand::ToHandle(
        GDALDataset::FromHandle(hDS)->GetRasterBand(nBandId));
}

/************************************************************************/
/*                           GetRasterCount()                           */
/************************************************************************/

/**
 * \brief Fetch the number of raster bands on this dataset.
 *
 * Same as the C function GDALGetRasterCount().
 *
 * @return the number of raster bands.
 */

int GDALDataset::GetRasterCount() const
{
    return papoBands ? nBands : 0;
}

/************************************************************************/
/*                         GDALGetRasterCount()                         */
/************************************************************************/

/**
 * \brief Fetch the number of raster bands on this dataset.
 *
 * @see GDALDataset::GetRasterCount().
 */

int CPL_STDCALL GDALGetRasterCount(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetRasterCount", 0);

    return GDALDataset::FromHandle(hDS)->GetRasterCount();
}

/************************************************************************/
/*                          GetProjectionRef()                          */
/************************************************************************/

/**
 * \brief Fetch the projection definition string for this dataset.
 *
 * Same as the C function GDALGetProjectionRef().
 *
 * The returned string defines the projection coordinate system of the
 * image in OpenGIS WKT format.  It should be suitable for use with the
 * OGRSpatialReference class.
 *
 * When a projection definition is not available an empty (but not NULL)
 * string is returned.
 *
 * \note Starting with GDAL 3.0, this is a compatibility layer around
 * GetSpatialRef()
 *
 * @return a pointer to an internal projection reference string.  It should
 * not be altered, freed or expected to last for long.
 *
 * @see https://gdal.org/tutorials/osr_api_tut.html
 */

const char *GDALDataset::GetProjectionRef() const
{
    const auto poSRS = GetSpatialRef();
    if (!poSRS || !m_poPrivate)
    {
        return "";
    }
    char *pszWKT = nullptr;
    poSRS->exportToWkt(&pszWKT);
    if (!pszWKT)
    {
        return "";
    }

    // If called on a thread-safe dataset, we might be called by several
    // threads, so make sure our accesses to m_pszWKTCached are protected
    // by a mutex.
    std::lock_guard oLock(m_poPrivate->m_oMutexWKT);
    if (m_poPrivate->m_pszWKTCached &&
        strcmp(pszWKT, m_poPrivate->m_pszWKTCached) == 0)
    {
        CPLFree(pszWKT);
        return m_poPrivate->m_pszWKTCached;
    }
    CPLFree(m_poPrivate->m_pszWKTCached);
    m_poPrivate->m_pszWKTCached = pszWKT;
    return m_poPrivate->m_pszWKTCached;
}

/************************************************************************/
/*                           GetSpatialRef()                            */
/************************************************************************/

/**
 * \brief Fetch the spatial reference for this dataset.
 *
 * Same as the C function GDALGetSpatialRef().
 *
 * When a projection definition is not available, null is returned. If used on
 * a dataset where there are GCPs and not a geotransform, this method returns
 * null. Use GetGCPSpatialRef() instead.
 *
 * @since GDAL 3.0
 *
 * @return a pointer to an internal object. It should not be altered or freed.
 * Its lifetime will be the one of the dataset object, or until the next
 * call to this method.
 *
 * @see https://gdal.org/tutorials/osr_api_tut.html
 */

const OGRSpatialReference *GDALDataset::GetSpatialRef() const
{
    return nullptr;
}

/************************************************************************/
/*                        GDALGetSpatialRef()                           */
/************************************************************************/

/**
 * \brief Fetch the spatial reference for this dataset.
 *
 * @since GDAL 3.0
 *
 * @see GDALDataset::GetSpatialRef()
 */

OGRSpatialReferenceH GDALGetSpatialRef(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetSpatialRef", nullptr);

    return OGRSpatialReference::ToHandle(const_cast<OGRSpatialReference *>(
        GDALDataset::FromHandle(hDS)->GetSpatialRef()));
}

/************************************************************************/
/*                        GDALGetProjectionRef()                        */
/************************************************************************/

/**
 * \brief Fetch the projection definition string for this dataset.
 *
 * @see GDALDataset::GetProjectionRef()
 */

const char *CPL_STDCALL GDALGetProjectionRef(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetProjectionRef", nullptr);

    return GDALDataset::FromHandle(hDS)->GetProjectionRef();
}

/************************************************************************/
/*                           SetProjection()                            */
/************************************************************************/

/**
 * \brief Set the projection reference string for this dataset.
 *
 * The string should be in OGC WKT or PROJ.4 format.  An error may occur
 * because of incorrectly specified projection strings, because the dataset
 * is not writable, or because the dataset does not support the indicated
 * projection.  Many formats do not support writing projections.
 *
 * This method is the same as the C GDALSetProjection() function.
 *
 * \note Startig with GDAL 3.0, this is a compatibility layer around
 * SetSpatialRef()

 * @param pszProjection projection reference string.
 *
 * @return CE_Failure if an error occurs, otherwise CE_None.
 */

CPLErr GDALDataset::SetProjection(const char *pszProjection)
{
    if (pszProjection && pszProjection[0] != '\0')
    {
        OGRSpatialReference oSRS;
        oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (oSRS.SetFromUserInput(pszProjection) != OGRERR_NONE)
        {
            return CE_Failure;
        }
        return SetSpatialRef(&oSRS);
    }
    else
    {
        return SetSpatialRef(nullptr);
    }
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

/**
 * \brief Set the spatial reference system for this dataset.
 *
 * An error may occur because the dataset
 * is not writable, or because the dataset does not support the indicated
 * projection. Many formats do not support writing projections.
 *
 * This method is the same as the C GDALSetSpatialRef() function.
 *
 * @since GDAL 3.0

 * @param poSRS spatial reference system object. nullptr can potentially be
 * passed for drivers that support unsetting the SRS.
 *
 * @return CE_Failure if an error occurs, otherwise CE_None.
 */

CPLErr GDALDataset::SetSpatialRef(CPL_UNUSED const OGRSpatialReference *poSRS)
{
    if (!(GetMOFlags() & GMO_IGNORE_UNIMPLEMENTED))
        ReportError(CE_Failure, CPLE_NotSupported,
                    "Dataset does not support the SetSpatialRef() method.");
    return CE_Failure;
}

/************************************************************************/
/*                         GDALSetSpatialRef()                          */
/************************************************************************/

/**
 * \brief Set the spatial reference system for this dataset.
 *
 * @since GDAL 3.0
 *
 * @see GDALDataset::SetSpatialRef()
 */

CPLErr GDALSetSpatialRef(GDALDatasetH hDS, OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER1(hDS, "GDALSetSpatialRef", CE_Failure);

    return GDALDataset::FromHandle(hDS)->SetSpatialRef(
        OGRSpatialReference::FromHandle(hSRS));
}

/************************************************************************/
/*                         GDALSetProjection()                          */
/************************************************************************/

/**
 * \brief Set the projection reference string for this dataset.
 *
 * @see GDALDataset::SetProjection()
 */

CPLErr CPL_STDCALL GDALSetProjection(GDALDatasetH hDS,
                                     const char *pszProjection)

{
    VALIDATE_POINTER1(hDS, "GDALSetProjection", CE_Failure);

    return GDALDataset::FromHandle(hDS)->SetProjection(pszProjection);
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

/**
 * \brief Fetch the affine transformation coefficients.
 *
 * Fetches the coefficients for transforming between pixel/line (P,L) raster
 * space, and projection coordinates (Xp,Yp) space.
 *
 * \code
 *   Xp = gt[0] + P*gt[1] + L*gt[2];
 *   Yp = gt[3] + P*padfTransform[4] + L*gt[5];
 * \endcode
 *
 * In a north up image, gt[1] is the pixel width, and
 * gt[5] is the pixel height.  The upper left corner of the
 * upper left pixel is at position (gt[0],gt[3]).
 *
 * The default transform is (0,1,0,0,0,1) and should be returned even when
 * a CE_Failure error is returned, such as for formats that don't support
 * transformation to projection coordinates.
 *
 * This method does the same thing as the C GDALGetGeoTransform() function.
 *
 * @param gt an existing six double buffer into which the
 * transformation will be placed.
 *
 * @return CE_None on success, or CE_Failure if no transform can be fetched.
 *
 * @since 3.12
 */

CPLErr GDALDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = GDALGeoTransform();

    return CE_Failure;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

/**
 * \brief Fetch the affine transformation coefficients.
 *
 * Fetches the coefficients for transforming between pixel/line (P,L) raster
 * space, and projection coordinates (Xp,Yp) space.
 *
 * \code
 *   Xp = padfTransform[0] + P*padfTransform[1] + L*padfTransform[2];
 *   Yp = padfTransform[3] + P*padfTransform[4] + L*padfTransform[5];
 * \endcode
 *
 * In a north up image, padfTransform[1] is the pixel width, and
 * padfTransform[5] is the pixel height.  The upper left corner of the
 * upper left pixel is at position (padfTransform[0],padfTransform[3]).
 *
 * The default transform is (0,1,0,0,0,1) and should be returned even when
 * a CE_Failure error is returned, such as for formats that don't support
 * transformation to projection coordinates.
 *
 * This method does the same thing as the C GDALGetGeoTransform() function.
 *
 * @param padfTransform an existing six double buffer into which the
 * transformation will be placed.
 *
 * @return CE_None on success, or CE_Failure if no transform can be fetched.
 *
 * @deprecated since 3.12. Use GetGeoTransform(GDALGeoTransform&) instead
 */

CPLErr GDALDataset::GetGeoTransform(double *padfTransform) const

{
    return GetGeoTransform(
        *reinterpret_cast<GDALGeoTransform *>(padfTransform));
}

/************************************************************************/
/*                        GDALGetGeoTransform()                         */
/************************************************************************/

/**
 * \brief Fetch the affine transformation coefficients.
 *
 * @see GDALDataset::GetGeoTransform()
 */

CPLErr CPL_STDCALL GDALGetGeoTransform(GDALDatasetH hDS, double *padfTransform)

{
    VALIDATE_POINTER1(hDS, "GDALGetGeoTransform", CE_Failure);

    return GDALDataset::FromHandle(hDS)->GetGeoTransform(
        *reinterpret_cast<GDALGeoTransform *>(padfTransform));
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

/**
 * \fn GDALDataset::SetGeoTransform(const GDALGeoTransform&)
 * \brief Set the affine transformation coefficients.
 *
 * See GetGeoTransform() for details on the meaning of the padfTransform
 * coefficients.
 *
 * This method does the same thing as the C GDALSetGeoTransform() function.
 *
 * @param gt the transformation coefficients to be written with the dataset.
 *
 * @return CE_None on success, or CE_Failure if this transform cannot be
 * written.
 *
 * @since 3.12
 */

CPLErr GDALDataset::SetGeoTransform(CPL_UNUSED const GDALGeoTransform &gt)

{
    if (!(GetMOFlags() & GMO_IGNORE_UNIMPLEMENTED))
        ReportError(CE_Failure, CPLE_NotSupported,
                    "SetGeoTransform() not supported for this dataset.");

    return CE_Failure;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

/**
 * \brief Set the affine transformation coefficients.
 *
 * See GetGeoTransform() for details on the meaning of the padfTransform
 * coefficients.
 *
 * This method does the same thing as the C GDALSetGeoTransform() function.
 *
 * @param padfTransform a six double buffer containing the transformation
 * coefficients to be written with the dataset.
 *
 * @return CE_None on success, or CE_Failure if this transform cannot be
 * written.
 *
 * @deprecated since 3.12. Use SetGeoTransform(const GDALGeoTransform&) instead
 */
CPLErr GDALDataset::SetGeoTransform(const double *padfTransform)

{
    return SetGeoTransform(
        *reinterpret_cast<const GDALGeoTransform *>(padfTransform));
}

/************************************************************************/
/*                        GDALSetGeoTransform()                         */
/************************************************************************/

/**
 * \brief Set the affine transformation coefficients.
 *
 * @see GDALDataset::SetGeoTransform()
 */

CPLErr CPL_STDCALL GDALSetGeoTransform(GDALDatasetH hDS,
                                       const double *padfTransform)

{
    VALIDATE_POINTER1(hDS, "GDALSetGeoTransform", CE_Failure);
    VALIDATE_POINTER1(padfTransform, "GDALSetGeoTransform", CE_Failure);

    return GDALDataset::FromHandle(hDS)->SetGeoTransform(
        *reinterpret_cast<const GDALGeoTransform *>(padfTransform));
}

/************************************************************************/
/*                         GetInternalHandle()                          */
/************************************************************************/

/**
 * \fn GDALDataset::GetInternalHandle(const char*)
 * \brief Fetch a format specific internally meaningful handle.
 *
 * This method is the same as the C GDALGetInternalHandle() method.
 *
 * @param pszHandleName the handle name desired.  The meaningful names
 * will be specific to the file format.
 *
 * @return the desired handle value, or NULL if not recognized/supported.
 */

void *GDALDataset::GetInternalHandle(CPL_UNUSED const char *pszHandleName)

{
    return nullptr;
}

/************************************************************************/
/*                       GDALGetInternalHandle()                        */
/************************************************************************/

/**
 * \brief Fetch a format specific internally meaningful handle.
 *
 * @see GDALDataset::GetInternalHandle()
 */

void *CPL_STDCALL GDALGetInternalHandle(GDALDatasetH hDS,
                                        const char *pszRequest)

{
    VALIDATE_POINTER1(hDS, "GDALGetInternalHandle", nullptr);

    return GDALDataset::FromHandle(hDS)->GetInternalHandle(pszRequest);
}

/************************************************************************/
/*                             GetDriver()                              */
/************************************************************************/

/**
 * \brief Fetch the driver to which this dataset relates.
 *
 * This method is the same as the C GDALGetDatasetDriver() function.
 *
 * @return the driver on which the dataset was created with GDALOpen() or
 * GDALCreate().
 */

GDALDriver *GDALDataset::GetDriver()
{
    return poDriver;
}

/************************************************************************/
/*                        GDALGetDatasetDriver()                        */
/************************************************************************/

/**
 * \brief Fetch the driver to which this dataset relates.
 *
 * @see GDALDataset::GetDriver()
 */

GDALDriverH CPL_STDCALL GDALGetDatasetDriver(GDALDatasetH hDataset)

{
    VALIDATE_POINTER1(hDataset, "GDALGetDatasetDriver", nullptr);

    return static_cast<GDALDriverH>(
        GDALDataset::FromHandle(hDataset)->GetDriver());
}

/************************************************************************/
/*                             Reference()                              */
/************************************************************************/

/**
 * \brief Add one to dataset reference count.
 *
 * The reference is one after instantiation.
 *
 * This method is the same as the C GDALReferenceDataset() function.
 *
 * @return the post-increment reference count.
 */

int GDALDataset::Reference()
{
    return ++nRefCount;
}

/************************************************************************/
/*                        GDALReferenceDataset()                        */
/************************************************************************/

/**
 * \brief Add one to dataset reference count.
 *
 * @see GDALDataset::Reference()
 */

int CPL_STDCALL GDALReferenceDataset(GDALDatasetH hDataset)

{
    VALIDATE_POINTER1(hDataset, "GDALReferenceDataset", 0);

    return GDALDataset::FromHandle(hDataset)->Reference();
}

/************************************************************************/
/*                            Dereference()                             */
/************************************************************************/

/**
 * \brief Subtract one from dataset reference count.
 *
 * The reference is one after instantiation.  Generally when the reference
 * count has dropped to zero the dataset may be safely deleted (closed).
 *
 * This method is the same as the C GDALDereferenceDataset() function.
 *
 * @return the post-decrement reference count.
 */

int GDALDataset::Dereference()
{
    return --nRefCount;
}

/************************************************************************/
/*                       GDALDereferenceDataset()                       */
/************************************************************************/

/**
 * \brief Subtract one from dataset reference count.
 *
 * @see GDALDataset::Dereference()
 */

int CPL_STDCALL GDALDereferenceDataset(GDALDatasetH hDataset)

{
    VALIDATE_POINTER1(hDataset, "GDALDereferenceDataset", 0);

    return GDALDataset::FromHandle(hDataset)->Dereference();
}

/************************************************************************/
/*                            ReleaseRef()                              */
/************************************************************************/

/**
 * \brief Drop a reference to this object, and destroy if no longer referenced.
 * @return TRUE if the object has been destroyed.
 * @since GDAL 2.2
 */

int GDALDataset::ReleaseRef()

{
    if (Dereference() <= 0)
    {
        nRefCount = 1;
        delete this;
        return TRUE;
    }
    return FALSE;
}

/************************************************************************/
/*                        GDALReleaseDataset()                          */
/************************************************************************/

/**
 * \brief Drop a reference to this object, and destroy if no longer referenced.
 *
 * @see GDALDataset::ReleaseRef()
 * @since GDAL 2.2
 */

int CPL_STDCALL GDALReleaseDataset(GDALDatasetH hDataset)

{
    VALIDATE_POINTER1(hDataset, "GDALReleaseDataset", 0);

    return GDALDataset::FromHandle(hDataset)->ReleaseRef();
}

/************************************************************************/
/*                             GetShared()                              */
/************************************************************************/

/**
 * \brief Returns shared flag.
 *
 * @return TRUE if the GDALDataset is available for sharing, or FALSE if not.
 */

int GDALDataset::GetShared() const
{
    return bShared;
}

/************************************************************************/
/*                            MarkAsShared()                            */
/************************************************************************/

/**
 * \brief Mark this dataset as available for sharing.
 */

void GDALDataset::MarkAsShared()

{
    CPLAssert(!bShared);

    bShared = true;
    if (bIsInternal)
        return;

    GIntBig nPID = GDALGetResponsiblePIDForCurrentThread();

    // Insert the dataset in the set of shared opened datasets.
    CPLMutexHolderD(&hDLMutex);
    if (phSharedDatasetSet == nullptr)
        phSharedDatasetSet =
            CPLHashSetNew(GDALSharedDatasetHashFunc, GDALSharedDatasetEqualFunc,
                          GDALSharedDatasetFreeFunc);

    SharedDatasetCtxt *psStruct =
        static_cast<SharedDatasetCtxt *>(CPLMalloc(sizeof(SharedDatasetCtxt)));
    psStruct->poDS = this;
    psStruct->nPID = nPID;
    psStruct->nOpenFlags = nOpenFlags & ~GDAL_OF_SHARED;
    psStruct->pszDescription = CPLStrdup(GetDescription());
    std::string osConcatenatedOpenOptions =
        GDALSharedDatasetConcatenateOpenOptions(papszOpenOptions);
    psStruct->pszConcatenatedOpenOptions =
        CPLStrdup(osConcatenatedOpenOptions.c_str());
    if (CPLHashSetLookup(phSharedDatasetSet, psStruct) != nullptr)
    {
        GDALSharedDatasetFreeFunc(psStruct);
        ReportError(CE_Failure, CPLE_AppDefined,
                    "An existing shared dataset already has this description. "
                    "This should not happen.");
    }
    else
    {
        CPLHashSetInsert(phSharedDatasetSet, psStruct);

        (*poAllDatasetMap)[this] = nPID;
    }
}

/************************************************************************/
/*                        MarkSuppressOnClose()                         */
/************************************************************************/

/** Set that the dataset must be deleted on close.
 *
 * This is the same as C function GDALDatasetMarkSuppressOnClose()
 */
void GDALDataset::MarkSuppressOnClose()
{
    bSuppressOnClose = true;
}

/************************************************************************/
/*                   GDALDatasetMarkSuppressOnClose()                   */
/************************************************************************/

/** Set that the dataset must be deleted on close.
 *
 * This is the same as C++ method GDALDataset::MarkSuppressOnClose()
 *
 * @since GDAL 3.12
 */

void GDALDatasetMarkSuppressOnClose(GDALDatasetH hDS)
{
    VALIDATE_POINTER0(hDS, "GDALDatasetMarkSuppressOnClose");

    return GDALDataset::FromHandle(hDS)->MarkSuppressOnClose();
}

/************************************************************************/
/*                       UnMarkSuppressOnClose()                        */
/************************************************************************/

/** Remove the flag requesting the dataset to be deleted on close. */
void GDALDataset::UnMarkSuppressOnClose()
{
    bSuppressOnClose = false;
}

/************************************************************************/
/*                        CleanupPostFileClosing()                      */
/************************************************************************/

/** This method should be called by driver implementations in their destructor,
 * after having closed all files, but before having freed resources that
 * are needed for their GetFileList() implementation.
 * This is used to implement MarkSuppressOnClose behavior.
 */
void GDALDataset::CleanupPostFileClosing()
{
    if (IsMarkedSuppressOnClose())
    {
        char **papszFileList = GetFileList();
        for (int i = 0; papszFileList && papszFileList[i]; ++i)
            VSIUnlink(papszFileList[i]);
        CSLDestroy(papszFileList);
    }
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

/**
 * \brief Get number of GCPs.
 *
 * This method is the same as the C function GDALGetGCPCount().
 *
 * @return number of GCPs for this dataset.  Zero if there are none.
 */

int GDALDataset::GetGCPCount()
{
    return 0;
}

/************************************************************************/
/*                          GDALGetGCPCount()                           */
/************************************************************************/

/**
 * \brief Get number of GCPs.
 *
 * @see GDALDataset::GetGCPCount()
 */

int CPL_STDCALL GDALGetGCPCount(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetGCPCount", 0);

    return GDALDataset::FromHandle(hDS)->GetGCPCount();
}

/************************************************************************/
/*                          GetGCPProjection()                          */
/************************************************************************/

/**
 * \brief Get output projection for GCPs.
 *
 * This method is the same as the C function GDALGetGCPProjection().
 *
 * The projection string follows the normal rules from GetProjectionRef().
 *
 * \note Starting with GDAL 3.0, this is a compatibility layer around
 * GetGCPSpatialRef()
 *
 * @return internal projection string or "" if there are no GCPs.
 *  It should not be altered, freed or expected to last for long.
 */

const char *GDALDataset::GetGCPProjection()
{
    const auto poSRS = GetGCPSpatialRef();
    if (!poSRS || !m_poPrivate)
    {
        return "";
    }
    char *pszWKT = nullptr;
    poSRS->exportToWkt(&pszWKT);
    if (!pszWKT)
    {
        return "";
    }

    // If called on a thread-safe dataset, we might be called by several
    // threads, so make sure our accesses to m_pszWKTCached are protected
    // by a mutex.
    std::lock_guard oLock(m_poPrivate->m_oMutexWKT);
    if (m_poPrivate->m_pszWKTGCPCached &&
        strcmp(pszWKT, m_poPrivate->m_pszWKTGCPCached) == 0)
    {
        CPLFree(pszWKT);
        return m_poPrivate->m_pszWKTGCPCached;
    }
    CPLFree(m_poPrivate->m_pszWKTGCPCached);
    m_poPrivate->m_pszWKTGCPCached = pszWKT;
    return m_poPrivate->m_pszWKTGCPCached;
}

/************************************************************************/
/*                          GetGCPSpatialRef()                          */
/************************************************************************/

/**
 * \brief Get output spatial reference system for GCPs.
 *
 * Same as the C function GDALGetGCPSpatialRef().
 *
 * When a SRS is not available, null is returned. If used on
 * a dataset where there is a geotransform, and not GCPs, this method returns
 * null. Use GetSpatialRef() instead.
 *
 * @since GDAL 3.0
 *
 * @return a pointer to an internal object. It should not be altered or freed.
 * Its lifetime will be the one of the dataset object, or until the next
 * call to this method.
 */

const OGRSpatialReference *GDALDataset::GetGCPSpatialRef() const
{
    return nullptr;
}

/************************************************************************/
/*                       GDALGetGCPSpatialRef()                         */
/************************************************************************/

/**
 * \brief Get output spatial reference system for GCPs.
 *
 * @since GDAL 3.0
 *
 * @see GDALDataset::GetGCPSpatialRef()
 */

OGRSpatialReferenceH GDALGetGCPSpatialRef(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetGCPSpatialRef", nullptr);

    return OGRSpatialReference::ToHandle(const_cast<OGRSpatialReference *>(
        GDALDataset::FromHandle(hDS)->GetGCPSpatialRef()));
}

/************************************************************************/
/*                        GDALGetGCPProjection()                        */
/************************************************************************/

/**
 * \brief Get output projection for GCPs.
 *
 * @see GDALDataset::GetGCPProjection()
 */

const char *CPL_STDCALL GDALGetGCPProjection(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetGCPProjection", nullptr);

    return GDALDataset::FromHandle(hDS)->GetGCPProjection();
}

/************************************************************************/
/*                               GetGCPs()                              */
/************************************************************************/

/**
 * \brief Fetch GCPs.
 *
 * This method is the same as the C function GDALGetGCPs().
 *
 * @return pointer to internal GCP structure list.  It should not be modified,
 * and may change on the next GDAL call.
 */

const GDAL_GCP *GDALDataset::GetGCPs()
{
    return nullptr;
}

/************************************************************************/
/*                            GDALGetGCPs()                             */
/************************************************************************/

/**
 * \brief Fetch GCPs.
 *
 * @see GDALDataset::GetGCPs()
 */

const GDAL_GCP *CPL_STDCALL GDALGetGCPs(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetGCPs", nullptr);

    return GDALDataset::FromHandle(hDS)->GetGCPs();
}

/************************************************************************/
/*                              SetGCPs()                               */
/************************************************************************/

/**
 * \brief Assign GCPs.
 *
 * This method is the same as the C function GDALSetGCPs().
 *
 * This method assigns the passed set of GCPs to this dataset, as well as
 * setting their coordinate system.  Internally copies are made of the
 * coordinate system and list of points, so the caller remains responsible for
 * deallocating these arguments if appropriate.
 *
 * Most formats do not support setting of GCPs, even formats that can
 * handle GCPs.  These formats will return CE_Failure.
 *
 * \note Startig with GDAL 3.0, this is a compatibility layer around
 * SetGCPs(int, const GDAL_GCP*, const char*)
 *
 * @param nGCPCount number of GCPs being assigned.
 *
 * @param pasGCPList array of GCP structures being assign (nGCPCount in array).
 *
 * @param pszGCPProjection the new OGC WKT coordinate system to assign for the
 * GCP output coordinates.  This parameter should be "" if no output coordinate
 * system is known.
 *
 * @return CE_None on success, CE_Failure on failure (including if action is
 * not supported for this format).
 */

CPLErr GDALDataset::SetGCPs(int nGCPCount, const GDAL_GCP *pasGCPList,
                            const char *pszGCPProjection)

{
    if (pszGCPProjection && pszGCPProjection[0] != '\0')
    {
        OGRSpatialReference oSRS;
        oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (oSRS.importFromWkt(pszGCPProjection) != OGRERR_NONE)
        {
            return CE_Failure;
        }
        return SetGCPs(nGCPCount, pasGCPList, &oSRS);
    }
    else
    {
        return SetGCPs(nGCPCount, pasGCPList,
                       static_cast<const OGRSpatialReference *>(nullptr));
    }
}

/************************************************************************/
/*                              SetGCPs()                               */
/************************************************************************/

/**
 * \brief Assign GCPs.
 *
 * This method is the same as the C function GDALSetGCPs().
 *
 * This method assigns the passed set of GCPs to this dataset, as well as
 * setting their coordinate system.  Internally copies are made of the
 * coordinate system and list of points, so the caller remains responsible for
 * deallocating these arguments if appropriate.
 *
 * Most formats do not support setting of GCPs, even formats that can
 * handle GCPs.  These formats will return CE_Failure.
 *
 * @since GDAL 3.0
 *
 * @param nGCPCount number of GCPs being assigned.
 *
 * @param pasGCPList array of GCP structures being assign (nGCPCount in array).
 *
 * @param poGCP_SRS the new coordinate reference system to assign for the
 * GCP output coordinates.  This parameter should be null if no output
 * coordinate system is known.
 *
 * @return CE_None on success, CE_Failure on failure (including if action is
 * not supported for this format).
 */

CPLErr GDALDataset::SetGCPs(CPL_UNUSED int nGCPCount,
                            CPL_UNUSED const GDAL_GCP *pasGCPList,
                            CPL_UNUSED const OGRSpatialReference *poGCP_SRS)

{
    if (!(GetMOFlags() & GMO_IGNORE_UNIMPLEMENTED))
        ReportError(CE_Failure, CPLE_NotSupported,
                    "Dataset does not support the SetGCPs() method.");

    return CE_Failure;
}

/************************************************************************/
/*                            GDALSetGCPs()                             */
/************************************************************************/

/**
 * \brief Assign GCPs.
 *
 * @see GDALDataset::SetGCPs(int, const GDAL_GCP*, const char*)
 */

CPLErr CPL_STDCALL GDALSetGCPs(GDALDatasetH hDS, int nGCPCount,
                               const GDAL_GCP *pasGCPList,
                               const char *pszGCPProjection)

{
    VALIDATE_POINTER1(hDS, "GDALSetGCPs", CE_Failure);

    return GDALDataset::FromHandle(hDS)->SetGCPs(nGCPCount, pasGCPList,
                                                 pszGCPProjection);
}

/************************************************************************/
/*                           GDALSetGCPs2()                             */
/************************************************************************/

/**
 * \brief Assign GCPs.
 *
 * @since GDAL 3.0
 * @see GDALDataset::SetGCPs(int, const GDAL_GCP*, const OGRSpatialReference*)
 */

CPLErr GDALSetGCPs2(GDALDatasetH hDS, int nGCPCount, const GDAL_GCP *pasGCPList,
                    OGRSpatialReferenceH hSRS)

{
    VALIDATE_POINTER1(hDS, "GDALSetGCPs2", CE_Failure);

    return GDALDataset::FromHandle(hDS)->SetGCPs(
        nGCPCount, pasGCPList, OGRSpatialReference::FromHandle(hSRS));
}

/************************************************************************/
/*                           BuildOverviews()                           */
/************************************************************************/

/**
 * \brief Build raster overview(s)
 *
 * If the operation is not supported for the indicated dataset, then
 * CE_Failure is returned, and CPLGetLastErrorNo() will return
 * CPLE_NotSupported.
 *
 * Depending on the actual file format, all overviews level can be also
 * deleted by specifying nOverviews == 0. This works at least for external
 * overviews (.ovr), TIFF internal overviews, etc.
 *
 * Starting with GDAL 3.2, the GDAL_NUM_THREADS configuration option can be set
 * to "ALL_CPUS" or a integer value to specify the number of threads to use for
 * overview computation.
 *
 * This method is the same as the C function GDALBuildOverviewsEx().
 *
 * @param pszResampling one of "AVERAGE", "AVERAGE_MAGPHASE", "RMS",
 * "BILINEAR", "CUBIC", "CUBICSPLINE", "GAUSS", "LANCZOS", "MODE", "NEAREST",
 * or "NONE" controlling the downsampling method applied.
 * @param nOverviews number of overviews to build, or 0 to clean overviews.
 * @param panOverviewList the list of overview decimation factors (positive
 *                        integers, normally larger or equal to 2) to build, or
 *                        NULL if nOverviews == 0.
 * @param nListBands number of bands to build overviews for in panBandList.
 * Build for all bands if this is 0.
 * @param panBandList list of band numbers.
 * @param pfnProgress a function to call to report progress, or NULL.
 * @param pProgressData application data to pass to the progress function.
 * @param papszOptions (GDAL >= 3.6) NULL terminated list of options as
 *                     key=value pairs, or NULL
 *
 * @return CE_None on success or CE_Failure if the operation doesn't work.
 *
 * For example, to build overview level 2, 4 and 8 on all bands the following
 * call could be made:
 * \code{.cpp}
 *   int       anOverviewList[3] = { 2, 4, 8 };
 *
 *   poDataset->BuildOverviews( "NEAREST", 3, anOverviewList, 0, nullptr,
 *                              GDALDummyProgress, nullptr );
 * \endcode
 *
 * @see GDALRegenerateOverviewsEx()
 */

CPLErr GDALDataset::BuildOverviews(const char *pszResampling, int nOverviews,
                                   const int *panOverviewList, int nListBands,
                                   const int *panBandList,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData,
                                   CSLConstList papszOptions)
{
    int *panAllBandList = nullptr;

    if (nListBands == 0)
    {
        nListBands = GetRasterCount();
        panAllBandList =
            static_cast<int *>(CPLMalloc(sizeof(int) * nListBands));
        for (int i = 0; i < nListBands; ++i)
            panAllBandList[i] = i + 1;

        panBandList = panAllBandList;
    }

    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    for (int i = 0; i < nOverviews; ++i)
    {
        if (panOverviewList[i] <= 0)
        {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "panOverviewList[%d] = %d is invalid. It must be a "
                     "positive value",
                     i, panOverviewList[i]);
            CPLFree(panAllBandList);
            return CE_Failure;
        }
    }

    // At time of writing, all overview generation options are actually
    // expected to be passed as configuration options.
    std::vector<std::unique_ptr<CPLConfigOptionSetter>> apoConfigOptionSetter;
    for (const auto &[pszKey, pszValue] : cpl::IterateNameValue(papszOptions))
    {
        apoConfigOptionSetter.emplace_back(
            std::make_unique<CPLConfigOptionSetter>(pszKey, pszValue, false));
    }

    const CPLErr eErr =
        IBuildOverviews(pszResampling, nOverviews, panOverviewList, nListBands,
                        panBandList, pfnProgress, pProgressData, papszOptions);

    if (panAllBandList != nullptr)
        CPLFree(panAllBandList);

    return eErr;
}

/************************************************************************/
/*                         GDALBuildOverviews()                         */
/************************************************************************/

/**
 * \brief Build raster overview(s)
 *
 * @see GDALDataset::BuildOverviews() and GDALBuildOverviews()
 */

CPLErr CPL_STDCALL GDALBuildOverviews(GDALDatasetH hDataset,
                                      const char *pszResampling, int nOverviews,
                                      const int *panOverviewList,
                                      int nListBands, const int *panBandList,
                                      GDALProgressFunc pfnProgress,
                                      void *pProgressData)

{
    VALIDATE_POINTER1(hDataset, "GDALBuildOverviews", CE_Failure);

    return GDALDataset::FromHandle(hDataset)->BuildOverviews(
        pszResampling, nOverviews, panOverviewList, nListBands, panBandList,
        pfnProgress, pProgressData, nullptr);
}

/************************************************************************/
/*                         GDALBuildOverviews()                         */
/************************************************************************/

/**
 * \brief Build raster overview(s)
 *
 * @see GDALDataset::BuildOverviews()
 * @since GDAL 3.6
 */

CPLErr CPL_STDCALL
GDALBuildOverviewsEx(GDALDatasetH hDataset, const char *pszResampling,
                     int nOverviews, const int *panOverviewList, int nListBands,
                     const int *panBandList, GDALProgressFunc pfnProgress,
                     void *pProgressData, CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDataset, "GDALBuildOverviews", CE_Failure);

    return GDALDataset::FromHandle(hDataset)->BuildOverviews(
        pszResampling, nOverviews, panOverviewList, nListBands, panBandList,
        pfnProgress, pProgressData, papszOptions);
}

/************************************************************************/
/*                          IBuildOverviews()                           */
/*                                                                      */
/*      Default implementation.                                         */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALDataset::IBuildOverviews(const char *pszResampling, int nOverviews,
                                    const int *panOverviewList, int nListBands,
                                    const int *panBandList,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData,
                                    CSLConstList papszOptions)

{
    if (oOvManager.IsInitialized())
        return oOvManager.BuildOverviews(
            nullptr, pszResampling, nOverviews, panOverviewList, nListBands,
            panBandList, pfnProgress, pProgressData, papszOptions);
    else
    {
        ReportError(CE_Failure, CPLE_NotSupported,
                    "BuildOverviews() not supported for this dataset.");

        return CE_Failure;
    }
}

//! @endcond

/************************************************************************/
/*                            AddOverviews()                            */
/*                                                                      */
/*      Default implementation.                                         */
/************************************************************************/

/**
 * \brief Add overview from existing dataset(s)
 *
 * This function creates new overview levels or refresh existing one from
 * the list of provided overview datasets.
 * Source overviews may come from any GDAL supported format, provided they
 * have the same number of bands and geospatial extent than the target
 * dataset.
 *
 * If the operation is not supported for the indicated dataset, then
 * CE_Failure is returned, and CPLGetLastErrorNo() will return
 * CPLE_NotSupported.
 *
 * At time of writing, this method is only implemented for internal overviews
 * of GeoTIFF datasets and external overviews in GeoTIFF format.
 *
 * @param apoSrcOvrDS Vector of source overviews.
 * @param pfnProgress a function to call to report progress, or NULL.
 * @param pProgressData application data to pass to the progress function.
 * @param papszOptions NULL terminated list of options as
 *                     key=value pairs, or NULL. None currently
 *
 * @return CE_None on success or CE_Failure if the operation doesn't work.
 * @since 3.12
 */
CPLErr GDALDataset::AddOverviews(const std::vector<GDALDataset *> &apoSrcOvrDS,
                                 GDALProgressFunc pfnProgress,
                                 void *pProgressData, CSLConstList papszOptions)
{
    if (oOvManager.IsInitialized())
    {
        return oOvManager.AddOverviews(nullptr, apoSrcOvrDS, pfnProgress,
                                       pProgressData, papszOptions);
    }
    else
    {
        ReportError(CE_Failure, CPLE_NotSupported,
                    "AddOverviews() not supported for this dataset.");
        return CE_Failure;
    }
}

/************************************************************************/
/*                             IRasterIO()                              */
/*                                                                      */
/*      The default implementation of IRasterIO() is, in the general    */
/*      case to pass the request off to each band objects rasterio      */
/*      methods with appropriate arguments. In some cases, it might     */
/*      choose instead the BlockBasedRasterIO() implementation.         */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                              int nXSize, int nYSize, void *pData,
                              int nBufXSize, int nBufYSize,
                              GDALDataType eBufType, int nBandCount,
                              BANDMAP_TYPE panBandMap, GSpacing nPixelSpace,
                              GSpacing nLineSpace, GSpacing nBandSpace,
                              GDALRasterIOExtraArg *psExtraArg)

{
    const char *pszInterleave = nullptr;

    CPLAssert(nullptr != pData);

    const bool bHasSubpixelShift =
        psExtraArg->bFloatingPointWindowValidity &&
        psExtraArg->eResampleAlg != GRIORA_NearestNeighbour &&
        (nXOff != psExtraArg->dfXOff || nYOff != psExtraArg->dfYOff);

    if (!bHasSubpixelShift && nXSize == nBufXSize && nYSize == nBufYSize &&
        nBandCount > 1 &&
        (pszInterleave = GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE")) !=
            nullptr &&
        EQUAL(pszInterleave, "PIXEL"))
    {
        return BlockBasedRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                  nBufXSize, nBufYSize, eBufType, nBandCount,
                                  panBandMap, nPixelSpace, nLineSpace,
                                  nBandSpace, psExtraArg);
    }

    if (eRWFlag == GF_Read &&
        (psExtraArg->eResampleAlg == GRIORA_Cubic ||
         psExtraArg->eResampleAlg == GRIORA_CubicSpline ||
         psExtraArg->eResampleAlg == GRIORA_Bilinear ||
         psExtraArg->eResampleAlg == GRIORA_Lanczos) &&
        !(nXSize == nBufXSize && nYSize == nBufYSize) && nBandCount > 1)
    {
        if (nBufXSize < nXSize && nBufYSize < nYSize && AreOverviewsEnabled())
        {
            int bTried = FALSE;
            const CPLErr eErr = TryOverviewRasterIO(
                eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize,
                nBufYSize, eBufType, nBandCount, panBandMap, nPixelSpace,
                nLineSpace, nBandSpace, psExtraArg, &bTried);
            if (bTried)
                return eErr;
        }

        GDALDataType eFirstBandDT = GDT_Unknown;
        int nFirstMaskFlags = 0;
        GDALRasterBand *poFirstMaskBand = nullptr;
        int nOKBands = 0;

        // Check if bands share the same mask band
        for (int i = 0; i < nBandCount; ++i)
        {
            GDALRasterBand *poBand = GetRasterBand(panBandMap[i]);
            if ((nBufXSize < nXSize || nBufYSize < nYSize) &&
                poBand->GetOverviewCount())
            {
                // Could be improved to select the appropriate overview.
                break;
            }
            if (poBand->GetColorTable() != nullptr)
            {
                break;
            }
            const GDALDataType eDT = poBand->GetRasterDataType();
            if (GDALDataTypeIsComplex(eDT))
            {
                break;
            }
            if (i == 0)
            {
                eFirstBandDT = eDT;
                nFirstMaskFlags = poBand->GetMaskFlags();
                if (nFirstMaskFlags == GMF_NODATA)
                {
                    // The dataset-level resampling code is not ready for nodata
                    // Fallback to band-level resampling
                    break;
                }
                poFirstMaskBand = poBand->GetMaskBand();
            }
            else
            {
                if (eDT != eFirstBandDT)
                {
                    break;
                }
                int nMaskFlags = poBand->GetMaskFlags();
                if (nMaskFlags == GMF_NODATA)
                {
                    // The dataset-level resampling code is not ready for nodata
                    // Fallback to band-level resampling
                    break;
                }
                GDALRasterBand *poMaskBand = poBand->GetMaskBand();
                if (nFirstMaskFlags == GMF_ALL_VALID &&
                    nMaskFlags == GMF_ALL_VALID)
                {
                    // Ok.
                }
                else if (poFirstMaskBand == poMaskBand)
                {
                    // Ok.
                }
                else
                {
                    break;
                }
            }

            ++nOKBands;
        }

        GDALProgressFunc pfnProgressGlobal = psExtraArg->pfnProgress;
        void *pProgressDataGlobal = psExtraArg->pProgressData;

        CPLErr eErr = CE_None;
        if (nOKBands > 0)
        {
            if (nOKBands < nBandCount)
            {
                psExtraArg->pfnProgress = GDALScaledProgress;
                psExtraArg->pProgressData = GDALCreateScaledProgress(
                    0.0, static_cast<double>(nOKBands) / nBandCount,
                    pfnProgressGlobal, pProgressDataGlobal);
                if (psExtraArg->pProgressData == nullptr)
                    psExtraArg->pfnProgress = nullptr;
            }

            eErr = RasterIOResampled(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                     pData, nBufXSize, nBufYSize, eBufType,
                                     nOKBands, panBandMap, nPixelSpace,
                                     nLineSpace, nBandSpace, psExtraArg);

            if (nOKBands < nBandCount)
            {
                GDALDestroyScaledProgress(psExtraArg->pProgressData);
            }
        }
        if (eErr == CE_None && nOKBands < nBandCount)
        {
            if (nOKBands > 0)
            {
                psExtraArg->pfnProgress = GDALScaledProgress;
                psExtraArg->pProgressData = GDALCreateScaledProgress(
                    static_cast<double>(nOKBands) / nBandCount, 1.0,
                    pfnProgressGlobal, pProgressDataGlobal);
                if (psExtraArg->pProgressData == nullptr)
                    psExtraArg->pfnProgress = nullptr;
            }
            eErr = BandBasedRasterIO(
                eRWFlag, nXOff, nYOff, nXSize, nYSize,
                static_cast<GByte *>(pData) + nBandSpace * nOKBands, nBufXSize,
                nBufYSize, eBufType, nBandCount - nOKBands,
                panBandMap + nOKBands, nPixelSpace, nLineSpace, nBandSpace,
                psExtraArg);
            if (nOKBands > 0)
            {
                GDALDestroyScaledProgress(psExtraArg->pProgressData);
            }
        }

        psExtraArg->pfnProgress = pfnProgressGlobal;
        psExtraArg->pProgressData = pProgressDataGlobal;

        return eErr;
    }

    return BandBasedRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                             nBufXSize, nBufYSize, eBufType, nBandCount,
                             panBandMap, nPixelSpace, nLineSpace, nBandSpace,
                             psExtraArg);
}

//! @endcond

/************************************************************************/
/*                         BandBasedRasterIO()                          */
/*                                                                      */
/*      Pass the request off to each band objects rasterio methods with */
/*      appropriate arguments.                                          */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALDataset::BandBasedRasterIO(
    GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
    void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    int nBandCount, const int *panBandMap, GSpacing nPixelSpace,
    GSpacing nLineSpace, GSpacing nBandSpace, GDALRasterIOExtraArg *psExtraArg)

{
    int iBandIndex;
    CPLErr eErr = CE_None;

    GDALProgressFunc pfnProgressGlobal = psExtraArg->pfnProgress;
    void *pProgressDataGlobal = psExtraArg->pProgressData;

    for (iBandIndex = 0; iBandIndex < nBandCount && eErr == CE_None;
         ++iBandIndex)
    {
        GDALRasterBand *poBand = GetRasterBand(panBandMap[iBandIndex]);

        if (poBand == nullptr)
        {
            eErr = CE_Failure;
            break;
        }

        GByte *pabyBandData =
            static_cast<GByte *>(pData) + iBandIndex * nBandSpace;

        if (nBandCount > 1)
        {
            psExtraArg->pfnProgress = GDALScaledProgress;
            psExtraArg->pProgressData = GDALCreateScaledProgress(
                1.0 * iBandIndex / nBandCount,
                1.0 * (iBandIndex + 1) / nBandCount, pfnProgressGlobal,
                pProgressDataGlobal);
            if (psExtraArg->pProgressData == nullptr)
                psExtraArg->pfnProgress = nullptr;
        }

        eErr = poBand->IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                 pabyBandData, nBufXSize, nBufYSize, eBufType,
                                 nPixelSpace, nLineSpace, psExtraArg);

        if (nBandCount > 1)
            GDALDestroyScaledProgress(psExtraArg->pProgressData);
    }

    psExtraArg->pfnProgress = pfnProgressGlobal;
    psExtraArg->pProgressData = pProgressDataGlobal;

    return eErr;
}

//! @endcond

/************************************************************************/
/*               ValidateRasterIOOrAdviseReadParameters()               */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLErr GDALDataset::ValidateRasterIOOrAdviseReadParameters(
    const char *pszCallingFunc, int *pbStopProcessingOnCENone, int nXOff,
    int nYOff, int nXSize, int nYSize, int nBufXSize, int nBufYSize,
    int nBandCount, const int *panBandMap)
{

    /* -------------------------------------------------------------------- */
    /*      Some size values are "noop".  Lets just return to avoid         */
    /*      stressing lower level functions.                                */
    /* -------------------------------------------------------------------- */
    if (nXSize < 1 || nYSize < 1 || nBufXSize < 1 || nBufYSize < 1)
    {
        CPLDebug("GDAL",
                 "%s skipped for odd window or buffer size.\n"
                 "  Window = (%d,%d)x%dx%d\n"
                 "  Buffer = %dx%d",
                 pszCallingFunc, nXOff, nYOff, nXSize, nYSize, nBufXSize,
                 nBufYSize);

        *pbStopProcessingOnCENone = TRUE;
        return CE_None;
    }

    CPLErr eErr = CE_None;
    *pbStopProcessingOnCENone = FALSE;

    if (nXOff < 0 || nXOff > INT_MAX - nXSize ||
        nXOff + nXSize > nRasterXSize || nYOff < 0 ||
        nYOff > INT_MAX - nYSize || nYOff + nYSize > nRasterYSize)
    {
        ReportError(CE_Failure, CPLE_IllegalArg,
                    "Access window out of range in %s.  Requested "
                    "(%d,%d) of size %dx%d on raster of %dx%d.",
                    pszCallingFunc, nXOff, nYOff, nXSize, nYSize, nRasterXSize,
                    nRasterYSize);
        eErr = CE_Failure;
    }

    if (panBandMap == nullptr && nBandCount > GetRasterCount())
    {
        ReportError(CE_Failure, CPLE_IllegalArg,
                    "%s: nBandCount cannot be greater than %d", pszCallingFunc,
                    GetRasterCount());
        eErr = CE_Failure;
    }

    for (int i = 0; i < nBandCount && eErr == CE_None; ++i)
    {
        int iBand = (panBandMap != nullptr) ? panBandMap[i] : i + 1;
        if (iBand < 1 || iBand > GetRasterCount())
        {
            ReportError(
                CE_Failure, CPLE_IllegalArg,
                "%s: panBandMap[%d] = %d, this band does not exist on dataset.",
                pszCallingFunc, i, iBand);
            eErr = CE_Failure;
        }

        if (eErr == CE_None && GetRasterBand(iBand) == nullptr)
        {
            ReportError(
                CE_Failure, CPLE_IllegalArg,
                "%s: panBandMap[%d]=%d, this band should exist but is NULL!",
                pszCallingFunc, i, iBand);
            eErr = CE_Failure;
        }
    }

    return eErr;
}

//! @endcond

/************************************************************************/
/*                              RasterIO()                              */
/************************************************************************/

/**
 * \brief Read/write a region of image data from multiple bands.
 *
 * This method allows reading a region of one or more GDALRasterBands from
 * this dataset into a buffer,  or writing data from a buffer into a region
 * of the GDALRasterBands.  It automatically takes care of data type
 * translation if the data type (eBufType) of the buffer is different than
 * that of the GDALRasterBand.
 * The method also takes care of image decimation / replication if the
 * buffer size (nBufXSize x nBufYSize) is different than the size of the
 * region being accessed (nXSize x nYSize).
 *
 * The window of interest expressed by (nXOff, nYOff, nXSize, nYSize) should be
 * fully within the raster space, that is nXOff >= 0, nYOff >= 0,
 * nXOff + nXSize <= GetRasterXSize() and nYOff + nYSize <= GetRasterYSize().
 * If reads larger than the raster space are wished, GDALTranslate() might be used.
 * Or use nLineSpace and a possibly shifted pData value.
 *
 * The nPixelSpace, nLineSpace and nBandSpace parameters allow reading into or
 * writing from various organization of buffers.
 *
 * Some formats may efficiently implement decimation into a buffer by
 * reading from lower resolution overview images. The logic of the default
 * implementation in the base class GDALRasterBand is the following one. It
 * computes a target_downscaling_factor from the window of interest and buffer
 * size which is min(nXSize/nBufXSize, nYSize/nBufYSize).
 * It then walks through overviews and will select the first one whose
 * downscaling factor is greater than target_downscaling_factor / 1.2.
 *
 * Let's assume we have overviews at downscaling factors 2, 4 and 8.
 * The relationship between target_downscaling_factor and the select overview
 * level is the following one:
 *
 * target_downscaling_factor  | selected_overview
 * -------------------------  | -----------------
 * ]0,       2 / 1.2]         | full resolution band
 * ]2 / 1.2, 4 / 1.2]         | 2x downsampled band
 * ]4 / 1.2, 8 / 1.2]         | 4x downsampled band
 * ]8 / 1.2, infinity[        | 8x downsampled band
 *
 * Note that starting with GDAL 3.9, this 1.2 oversampling factor can be
 * modified by setting the GDAL_OVERVIEW_OVERSAMPLING_THRESHOLD configuration
 * option. Also note that starting with GDAL 3.9, when the resampling algorithm
 * specified in psExtraArg->eResampleAlg is different from GRIORA_NearestNeighbour,
 * this oversampling threshold defaults to 1. Consequently if there are overviews
 * of downscaling factor 2, 4 and 8, and the desired downscaling factor is
 * 7.99, the overview of factor 4 will be selected for a non nearest resampling.
 *
 * For highest performance full resolution data access, read and write
 * on "block boundaries" as returned by GetBlockSize(), or use the
 * ReadBlock() and WriteBlock() methods.
 *
 * This method is the same as the C GDALDatasetRasterIO() or
 * GDALDatasetRasterIOEx() functions.
 *
 * @param eRWFlag Either GF_Read to read a region of data, or GF_Write to
 * write a region of data.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param pData The buffer into which the data should be read, or from which
 * it should be written.  This buffer must contain at least
 * nBufXSize * nBufYSize * nBandCount words of type eBufType.  It is organized
 * in left to right,top to bottom pixel order.  Spacing is controlled by the
 * nPixelSpace, and nLineSpace parameters.
 * Note that even with eRWFlag==GF_Write, the content of the buffer might be
 * temporarily modified during the execution of this method (and eventually
 * restored back to its original content), so it is not safe to use a buffer
 * stored in a read-only section of the calling program.
 *
 * @param nBufXSize the width of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param nBufYSize the height of the buffer image into which the desired
 * region is to be read, or from which it is to be written.
 *
 * @param eBufType the type of the pixel values in the pData data buffer. The
 * pixel values will automatically be translated to/from the GDALRasterBand
 * data type as needed. Most driver implementations will use GDALCopyWords64()
 * to perform data type translation.
 *
 * @param nBandCount the number of bands being read or written.
 *
 * @param panBandMap the list of nBandCount band numbers being read/written.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands. (Note: before GDAL 3.10, argument type was "int*", and
 * not "const int*")
 *
 * @param nPixelSpace The byte offset from the start of one pixel value in
 * pData to the start of the next pixel value within a scanline. If defaulted
 * (0) the size of the datatype eBufType is used.
 *
 * @param nLineSpace The byte offset from the start of one scanline in
 * pData to the start of the next. If defaulted (0) the size of the datatype
 * eBufType * nBufXSize is used.
 *
 * @param nBandSpace the byte offset from the start of one bands data to the
 * start of the next. If defaulted (0) the value will be
 * nLineSpace * nBufYSize implying band sequential organization
 * of the data buffer.
 *
 * @param psExtraArg (new in GDAL 2.0) pointer to a GDALRasterIOExtraArg
 * structure with additional arguments to specify resampling and progress
 * callback, or NULL for default behavior. The GDAL_RASTERIO_RESAMPLING
 * configuration option can also be defined to override the default resampling
 * to one of BILINEAR, CUBIC, CUBICSPLINE, LANCZOS, AVERAGE or MODE.
 *
 * @return CE_Failure if the access fails, otherwise CE_None.
 */

CPLErr GDALDataset::RasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, const int *panBandMap,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg)

{
    GDALRasterIOExtraArg sExtraArg;
    if (psExtraArg == nullptr)
    {
        INIT_RASTERIO_EXTRA_ARG(sExtraArg);

        // 4 below inits are not strictly needed but make Coverity Scan
        // happy
        sExtraArg.dfXOff = nXOff;
        sExtraArg.dfYOff = nYOff;
        sExtraArg.dfXSize = nXSize;
        sExtraArg.dfYSize = nYSize;

        psExtraArg = &sExtraArg;
    }
    else if (CPL_UNLIKELY(psExtraArg->nVersion >
                          RASTERIO_EXTRA_ARG_CURRENT_VERSION))
    {
        ReportError(CE_Failure, CPLE_AppDefined,
                    "Unhandled version of GDALRasterIOExtraArg");
        return CE_Failure;
    }

    GDALRasterIOExtraArgSetResampleAlg(psExtraArg, nXSize, nYSize, nBufXSize,
                                       nBufYSize);

    if (CPL_UNLIKELY(nullptr == pData))
    {
        ReportError(CE_Failure, CPLE_AppDefined,
                    "The buffer into which the data should be read is null");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Do some validation of parameters.                               */
    /* -------------------------------------------------------------------- */

    if (CPL_UNLIKELY(eRWFlag != GF_Read && eRWFlag != GF_Write))
    {
        ReportError(
            CE_Failure, CPLE_IllegalArg,
            "eRWFlag = %d, only GF_Read (0) and GF_Write (1) are legal.",
            eRWFlag);
        return CE_Failure;
    }

    if (eRWFlag == GF_Write)
    {
        if (CPL_UNLIKELY(eAccess != GA_Update))
        {
            ReportError(CE_Failure, CPLE_AppDefined,
                        "Write operation not permitted on dataset opened "
                        "in read-only mode");
            return CE_Failure;
        }
    }

    int bStopProcessing = FALSE;
    CPLErr eErr = ValidateRasterIOOrAdviseReadParameters(
        "RasterIO()", &bStopProcessing, nXOff, nYOff, nXSize, nYSize, nBufXSize,
        nBufYSize, nBandCount, panBandMap);
    if (eErr != CE_None || bStopProcessing)
        return eErr;
    if (CPL_UNLIKELY(eBufType == GDT_Unknown || eBufType == GDT_TypeCount))
    {
        ReportError(CE_Failure, CPLE_AppDefined,
                    "Illegal GDT_Unknown/GDT_TypeCount argument");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      If pixel and line spacing are defaulted assign reasonable      */
    /*      value assuming a packed buffer.                                 */
    /* -------------------------------------------------------------------- */
    if (nPixelSpace == 0)
        nPixelSpace = GDALGetDataTypeSizeBytes(eBufType);

    if (nLineSpace == 0)
    {
        nLineSpace = nPixelSpace * nBufXSize;
    }

    if (nBandSpace == 0 && nBandCount > 1)
    {
        nBandSpace = nLineSpace * nBufYSize;
    }

    if (panBandMap == nullptr)
    {
        if (!m_poPrivate)
            return CE_Failure;
        CPLAssert(static_cast<int>(m_poPrivate->m_anBandMap.size()) == nBands);
        panBandMap = m_poPrivate->m_anBandMap.data();
    }

    int bCallLeaveReadWrite = EnterReadWrite(eRWFlag);

    /* -------------------------------------------------------------------- */
    /*      We are being forced to use cached IO instead of a driver        */
    /*      specific implementation.                                        */
    /* -------------------------------------------------------------------- */
    if (bForceCachedIO)
    {
        eErr = BlockBasedRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                  nBufXSize, nBufYSize, eBufType, nBandCount,
                                  panBandMap, nPixelSpace, nLineSpace,
                                  nBandSpace, psExtraArg);
    }

    /* -------------------------------------------------------------------- */
    /*      Call the format specific function.                              */
    /* -------------------------------------------------------------------- */
    else
    {
        eErr = IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                         nBufXSize, nBufYSize, eBufType, nBandCount,
                         // TODO: remove this const_cast once IRasterIO()
                         // takes a const int*
                         const_cast<int *>(panBandMap), nPixelSpace, nLineSpace,
                         nBandSpace, psExtraArg);
    }

    if (bCallLeaveReadWrite)
        LeaveReadWrite();

    return eErr;
}

/************************************************************************/
/*                        GDALDatasetRasterIO()                         */
/************************************************************************/

/**
 * \brief Read/write a region of image data from multiple bands.
 *
 * Use GDALDatasetRasterIOEx() if 64 bit spacings or extra arguments (resampling
 * resolution, progress callback, etc. are needed)
 *
 * Note: before GDAL 3.10, panBandMap type was "int*", and not "const int*"
 *
 * @see GDALDataset::RasterIO()
 */

CPLErr CPL_STDCALL GDALDatasetRasterIO(GDALDatasetH hDS, GDALRWFlag eRWFlag,
                                       int nXOff, int nYOff, int nXSize,
                                       int nYSize, void *pData, int nBufXSize,
                                       int nBufYSize, GDALDataType eBufType,
                                       int nBandCount, const int *panBandMap,
                                       int nPixelSpace, int nLineSpace,
                                       int nBandSpace)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetRasterIO", CE_Failure);

    GDALDataset *poDS = GDALDataset::FromHandle(hDS);

    return poDS->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                          nBufXSize, nBufYSize, eBufType, nBandCount,
                          panBandMap, nPixelSpace, nLineSpace, nBandSpace,
                          nullptr);
}

/************************************************************************/
/*                       GDALDatasetRasterIOEx()                        */
/************************************************************************/

/**
 * \brief Read/write a region of image data from multiple bands.
 *
 * Note: before GDAL 3.10, panBandMap type was "int*", and not "const int*"
 *
 * @see GDALDataset::RasterIO()
 * @since GDAL 2.0
 */

CPLErr CPL_STDCALL GDALDatasetRasterIOEx(
    GDALDatasetH hDS, GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
    int nYSize, void *pData, int nBufXSize, int nBufYSize,
    GDALDataType eBufType, int nBandCount, const int *panBandMap,
    GSpacing nPixelSpace, GSpacing nLineSpace, GSpacing nBandSpace,
    GDALRasterIOExtraArg *psExtraArg)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetRasterIOEx", CE_Failure);

    GDALDataset *poDS = GDALDataset::FromHandle(hDS);

    return poDS->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                          nBufXSize, nBufYSize, eBufType, nBandCount,
                          panBandMap, nPixelSpace, nLineSpace, nBandSpace,
                          psExtraArg);
}

/************************************************************************/
/*                          GetOpenDatasets()                           */
/************************************************************************/

/**
 * \brief Fetch all open GDAL dataset handles.
 *
 * This method is the same as the C function GDALGetOpenDatasets().
 *
 * NOTE: This method is not thread safe.  The returned list may change
 * at any time and it should not be freed.
 *
 * @param pnCount integer into which to place the count of dataset pointers
 * being returned.
 *
 * @return a pointer to an array of dataset handles.
 */

GDALDataset **GDALDataset::GetOpenDatasets(int *pnCount)

{
    CPLMutexHolderD(&hDLMutex);

    if (poAllDatasetMap == nullptr)
    {
        *pnCount = 0;
        return nullptr;
    }

    *pnCount = static_cast<int>(poAllDatasetMap->size());
    ppDatasets = static_cast<GDALDataset **>(
        CPLRealloc(ppDatasets, (*pnCount) * sizeof(GDALDataset *)));
    std::map<GDALDataset *, GIntBig>::iterator oIter = poAllDatasetMap->begin();
    for (int i = 0; oIter != poAllDatasetMap->end(); ++oIter, ++i)
        ppDatasets[i] = oIter->first;
    return ppDatasets;
}

/************************************************************************/
/*                        GDALGetOpenDatasets()                         */
/************************************************************************/

/**
 * \brief Fetch all open GDAL dataset handles.
 *
 * @see GDALDataset::GetOpenDatasets()
 */

void CPL_STDCALL GDALGetOpenDatasets(GDALDatasetH **ppahDSList, int *pnCount)

{
    VALIDATE_POINTER0(ppahDSList, "GDALGetOpenDatasets");
    VALIDATE_POINTER0(pnCount, "GDALGetOpenDatasets");

    *ppahDSList =
        reinterpret_cast<GDALDatasetH *>(GDALDataset::GetOpenDatasets(pnCount));
}

/************************************************************************/
/*                        GDALCleanOpenDatasetsList()                   */
/************************************************************************/

// Useful when called from the child of a fork(), to avoid closing
// the datasets of the parent at the child termination.
void GDALNullifyOpenDatasetsList()
{
    poAllDatasetMap = nullptr;
    phSharedDatasetSet = nullptr;
    ppDatasets = nullptr;
    hDLMutex = nullptr;
}

/************************************************************************/
/*                             GDALGetAccess()                          */
/************************************************************************/

/**
 * \brief Return access flag
 *
 * @see GDALDataset::GetAccess()
 */

int CPL_STDCALL GDALGetAccess(GDALDatasetH hDS)
{
    VALIDATE_POINTER1(hDS, "GDALGetAccess", 0);

    return GDALDataset::FromHandle(hDS)->GetAccess();
}

/************************************************************************/
/*                             AdviseRead()                             */
/************************************************************************/

/**
 * \brief Advise driver of upcoming read requests.
 *
 * Some GDAL drivers operate more efficiently if they know in advance what
 * set of upcoming read requests will be made.  The AdviseRead() method allows
 * an application to notify the driver of the region and bands of interest,
 * and at what resolution the region will be read.
 *
 * Many drivers just ignore the AdviseRead() call, but it can dramatically
 * accelerate access via some drivers.
 *
 * Depending on call paths, drivers might receive several calls to
 * AdviseRead() with the same parameters.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param nBufXSize the width of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param nBufYSize the height of the buffer image into which the desired
 * region is to be read, or from which it is to be written.
 *
 * @param eBufType the type of the pixel values in the pData data buffer.  The
 * pixel values will automatically be translated to/from the GDALRasterBand
 * data type as needed.
 *
 * @param nBandCount the number of bands being read or written.
 *
 * @param panBandMap the list of nBandCount band numbers being read/written.
 * Note band numbers are 1 based.   This may be NULL to select the first
 * nBandCount bands.
 *
 * @param papszOptions a list of name=value strings with special control
 * options.  Normally this is NULL.
 *
 * @return CE_Failure if the request is invalid and CE_None if it works or
 * is ignored.
 */

CPLErr GDALDataset::AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                               int nBufXSize, int nBufYSize,
                               GDALDataType eBufType, int nBandCount,
                               int *panBandMap, char **papszOptions)

{
    /* -------------------------------------------------------------------- */
    /*      Do some validation of parameters.                               */
    /* -------------------------------------------------------------------- */
    int bStopProcessing = FALSE;
    CPLErr eErr = ValidateRasterIOOrAdviseReadParameters(
        "AdviseRead()", &bStopProcessing, nXOff, nYOff, nXSize, nYSize,
        nBufXSize, nBufYSize, nBandCount, panBandMap);
    if (eErr != CE_None || bStopProcessing)
        return eErr;

    for (int iBand = 0; iBand < nBandCount; ++iBand)
    {
        GDALRasterBand *poBand = nullptr;

        if (panBandMap == nullptr)
            poBand = GetRasterBand(iBand + 1);
        else
            poBand = GetRasterBand(panBandMap[iBand]);

        if (poBand == nullptr)
            return CE_Failure;

        eErr = poBand->AdviseRead(nXOff, nYOff, nXSize, nYSize, nBufXSize,
                                  nBufYSize, eBufType, papszOptions);

        if (eErr != CE_None)
            return eErr;
    }

    return CE_None;
}

/************************************************************************/
/*                       GDALDatasetAdviseRead()                        */
/************************************************************************/

/**
 * \brief Advise driver of upcoming read requests.
 *
 * @see GDALDataset::AdviseRead()
 */
CPLErr CPL_STDCALL GDALDatasetAdviseRead(GDALDatasetH hDS, int nXOff, int nYOff,
                                         int nXSize, int nYSize, int nBufXSize,
                                         int nBufYSize, GDALDataType eDT,
                                         int nBandCount, int *panBandMap,
                                         CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetAdviseRead", CE_Failure);

    return GDALDataset::FromHandle(hDS)->AdviseRead(
        nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize, eDT, nBandCount,
        panBandMap, const_cast<char **>(papszOptions));
}

/************************************************************************/
/*                         GDALAntiRecursionStruct                      */
/************************************************************************/

// Prevent infinite recursion.
struct GDALAntiRecursionStruct
{
    struct DatasetContext
    {
        std::string osFilename;
        int nOpenFlags;
        std::string osAllowedDrivers;

        DatasetContext(const std::string &osFilenameIn, int nOpenFlagsIn,
                       const std::string &osAllowedDriversIn)
            : osFilename(osFilenameIn), nOpenFlags(nOpenFlagsIn),
              osAllowedDrivers(osAllowedDriversIn)
        {
        }
    };

    struct DatasetContextCompare
    {
        bool operator()(const DatasetContext &lhs,
                        const DatasetContext &rhs) const
        {
            return lhs.osFilename < rhs.osFilename ||
                   (lhs.osFilename == rhs.osFilename &&
                    (lhs.nOpenFlags < rhs.nOpenFlags ||
                     (lhs.nOpenFlags == rhs.nOpenFlags &&
                      lhs.osAllowedDrivers < rhs.osAllowedDrivers)));
        }
    };

    std::set<DatasetContext, DatasetContextCompare> aosDatasetNamesWithFlags{};
    int nRecLevel = 0;
    std::map<std::string, int> m_oMapDepth{};
};

#ifdef _WIN32
// Currently thread_local and C++ objects don't work well with DLL on Windows
static void FreeAntiRecursionOpen(void *pData)
{
    delete static_cast<GDALAntiRecursionStruct *>(pData);
}

static GDALAntiRecursionStruct &GetAntiRecursionOpen()
{
    static GDALAntiRecursionStruct dummy;
    int bMemoryErrorOccurred = false;
    void *pData =
        CPLGetTLSEx(CTLS_GDALOPEN_ANTIRECURSION, &bMemoryErrorOccurred);
    if (bMemoryErrorOccurred)
    {
        return dummy;
    }
    if (pData == nullptr)
    {
        auto pAntiRecursion = new GDALAntiRecursionStruct();
        CPLSetTLSWithFreeFuncEx(CTLS_GDALOPEN_ANTIRECURSION, pAntiRecursion,
                                FreeAntiRecursionOpen, &bMemoryErrorOccurred);
        if (bMemoryErrorOccurred)
        {
            delete pAntiRecursion;
            return dummy;
        }
        return *pAntiRecursion;
    }
    return *static_cast<GDALAntiRecursionStruct *>(pData);
}
#else
static thread_local GDALAntiRecursionStruct g_tls_antiRecursion;

static GDALAntiRecursionStruct &GetAntiRecursionOpen()
{
    return g_tls_antiRecursion;
}
#endif

//! @cond Doxygen_Suppress
GDALAntiRecursionGuard::GDALAntiRecursionGuard(const std::string &osIdentifier)
    : m_psAntiRecursionStruct(&GetAntiRecursionOpen()),
      m_osIdentifier(osIdentifier),
      m_nDepth(++m_psAntiRecursionStruct->m_oMapDepth[m_osIdentifier])
{
    CPLAssert(!osIdentifier.empty());
}

GDALAntiRecursionGuard::GDALAntiRecursionGuard(
    const GDALAntiRecursionGuard &other, const std::string &osIdentifier)
    : m_psAntiRecursionStruct(other.m_psAntiRecursionStruct),
      m_osIdentifier(osIdentifier.empty()
                         ? osIdentifier
                         : other.m_osIdentifier + osIdentifier),
      m_nDepth(m_osIdentifier.empty()
                   ? 0
                   : ++m_psAntiRecursionStruct->m_oMapDepth[m_osIdentifier])
{
}

GDALAntiRecursionGuard::~GDALAntiRecursionGuard()
{
    if (!m_osIdentifier.empty())
    {
        --m_psAntiRecursionStruct->m_oMapDepth[m_osIdentifier];
    }
}

//! @endcond

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

/**
 * \brief Fetch files forming dataset.
 *
 * Returns a list of files believed to be part of this dataset.  If it returns
 * an empty list of files it means there is believed to be no local file
 * system files associated with the dataset (for instance a virtual dataset).
 * The returned file list is owned by the caller and should be deallocated
 * with CSLDestroy().
 *
 * The returned filenames will normally be relative or absolute paths
 * depending on the path used to originally open the dataset.  The strings
 * will be UTF-8 encoded.
 *
 * This method is the same as the C GDALGetFileList() function.
 *
 * @return NULL or a NULL terminated array of file names.
 */

char **GDALDataset::GetFileList()

{
    CPLString osMainFilename = GetDescription();
    VSIStatBufL sStat;

    GDALAntiRecursionStruct &sAntiRecursion = GetAntiRecursionOpen();
    GDALAntiRecursionStruct::DatasetContext datasetCtxt(osMainFilename, 0,
                                                        std::string());
    auto &aosDatasetList = sAntiRecursion.aosDatasetNamesWithFlags;
    if (cpl::contains(aosDatasetList, datasetCtxt))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Is the main filename even a real filesystem object?             */
    /* -------------------------------------------------------------------- */
    int bMainFileReal =
        VSIStatExL(osMainFilename, &sStat, VSI_STAT_EXISTS_FLAG) == 0;

    /* -------------------------------------------------------------------- */
    /*      Form new list.                                                  */
    /* -------------------------------------------------------------------- */
    char **papszList = nullptr;

    if (bMainFileReal)
        papszList = CSLAddString(papszList, osMainFilename);

    if (sAntiRecursion.nRecLevel == 100)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GetFileList() called with too many recursion levels");
        return papszList;
    }
    ++sAntiRecursion.nRecLevel;

    /* -------------------------------------------------------------------- */
    /*      Do we have a known overview file?                               */
    /* -------------------------------------------------------------------- */
    if (oOvManager.IsInitialized() && oOvManager.poODS != nullptr)
    {
        auto iter = aosDatasetList.insert(datasetCtxt).first;
        char **papszOvrList = oOvManager.poODS->GetFileList();
        papszList = CSLInsertStrings(papszList, -1, papszOvrList);
        CSLDestroy(papszOvrList);
        aosDatasetList.erase(iter);
    }

    /* -------------------------------------------------------------------- */
    /*      Do we have a known mask file?                                   */
    /* -------------------------------------------------------------------- */
    if (oOvManager.HaveMaskFile())
    {
        auto iter = aosDatasetList.insert(std::move(datasetCtxt)).first;
        for (const char *pszFile :
             CPLStringList(oOvManager.poMaskDS->GetFileList()))
        {
            if (CSLFindString(papszList, pszFile) < 0)
                papszList = CSLAddString(papszList, pszFile);
        }
        aosDatasetList.erase(iter);
    }

    --sAntiRecursion.nRecLevel;

    return papszList;
}

/************************************************************************/
/*                          GDALGetFileList()                           */
/************************************************************************/

/**
 * \brief Fetch files forming dataset.
 *
 * @see GDALDataset::GetFileList()
 */

char **CPL_STDCALL GDALGetFileList(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALGetFileList", nullptr);

    return GDALDataset::FromHandle(hDS)->GetFileList();
}

/************************************************************************/
/*                           CreateMaskBand()                           */
/************************************************************************/

/**
 * \brief Adds a mask band to the dataset
 *
 * The default implementation of the CreateMaskBand() method is implemented
 * based on similar rules to the .ovr handling implemented using the
 * GDALDefaultOverviews object. A TIFF file with the extension .msk will
 * be created with the same basename as the original file, and it will have
 * one band.
 * The mask images will be deflate compressed tiled images with the same
 * block size as the original image if possible.
 * It will have INTERNAL_MASK_FLAGS_xx metadata items set at the dataset
 * level, where xx matches the band number of a band of the main dataset. The
 * value of those items will be the one of the nFlagsIn parameter.
 *
 * Note that if you got a mask band with a previous call to GetMaskBand(), it
 * might be invalidated by CreateMaskBand(). So you have to call GetMaskBand()
 * again.
 *
 * @since GDAL 1.5.0
 *
 * @param nFlagsIn 0 or combination of GMF_PER_DATASET / GMF_ALPHA.
 *                 GMF_PER_DATASET will be always set, even if not explicitly
 *                 specified.
 * @return CE_None on success or CE_Failure on an error.
 *
 * @see https://gdal.org/development/rfc/rfc15_nodatabitmask.html
 * @see GDALRasterBand::CreateMaskBand()
 *
 */
CPLErr GDALDataset::CreateMaskBand(int nFlagsIn)

{
    if (oOvManager.IsInitialized())
    {
        CPLErr eErr = oOvManager.CreateMaskBand(nFlagsIn, -1);
        if (eErr != CE_None)
            return eErr;

        // Invalidate existing raster band masks.
        for (int i = 0; i < nBands; ++i)
        {
            GDALRasterBand *poBand = papoBands[i];
            poBand->poMask.reset();
        }

        return CE_None;
    }

    ReportError(CE_Failure, CPLE_NotSupported,
                "CreateMaskBand() not supported for this dataset.");

    return CE_Failure;
}

/************************************************************************/
/*                     GDALCreateDatasetMaskBand()                      */
/************************************************************************/

/**
 * \brief Adds a mask band to the dataset
 * @see GDALDataset::CreateMaskBand()
 */
CPLErr CPL_STDCALL GDALCreateDatasetMaskBand(GDALDatasetH hDS, int nFlags)

{
    VALIDATE_POINTER1(hDS, "GDALCreateDatasetMaskBand", CE_Failure);

    return GDALDataset::FromHandle(hDS)->CreateMaskBand(nFlags);
}

/************************************************************************/
/*                              GDALOpen()                              */
/************************************************************************/

/**
 * \brief Open a raster file as a GDALDataset.
 *
 * This function will try to open the passed file, or virtual dataset
 * name by invoking the Open method of each registered GDALDriver in turn.
 * The first successful open will result in a returned dataset.  If all
 * drivers fail then NULL is returned and an error is issued.
 *
 * Several recommendations :
 * <ul>
 * <li>If you open a dataset object with GA_Update access, it is not recommended
 * to open a new dataset on the same underlying file.</li>
 * <li>The returned dataset should only be accessed by one thread at a time. If
 * you want to use it from different threads, you must add all necessary code
 * (mutexes, etc.)  to avoid concurrent use of the object. (Some drivers, such
 * as GeoTIFF, maintain internal state variables that are updated each time a
 * new block is read, thus preventing concurrent use.) </li>
 * </ul>
 *
 * For drivers supporting the VSI virtual file API, it is possible to open a
 * file in a .zip archive (see VSIInstallZipFileHandler()), in a
 * .tar/.tar.gz/.tgz archive (see VSIInstallTarFileHandler()) or on a HTTP / FTP
 * server (see VSIInstallCurlFileHandler())
 *
 * \sa GDALOpenShared()
 * \sa GDALOpenEx()
 *
 * @param pszFilename the name of the file to access.  In the case of
 * exotic drivers this may not refer to a physical file, but instead contain
 * information for the driver on how to access a dataset.  It should be in UTF-8
 * encoding.
 *
 * @param eAccess the desired access, either GA_Update or GA_ReadOnly.  Many
 * drivers support only read only access.
 *
 * @return A GDALDatasetH handle or NULL on failure.  For C++ applications
 * this handle can be cast to a GDALDataset *.
 */

GDALDatasetH CPL_STDCALL GDALOpen(const char *pszFilename, GDALAccess eAccess)

{
    const int nUpdateFlag = eAccess == GA_Update ? GDAL_OF_UPDATE : 0;
    const int nOpenFlags = GDAL_OF_RASTER | nUpdateFlag | GDAL_OF_VERBOSE_ERROR;
    GDALDatasetH hDataset =
        GDALOpenEx(pszFilename, nOpenFlags, nullptr, nullptr, nullptr);
    return hDataset;
}

/************************************************************************/
/*                             GetSharedDS()                            */
/************************************************************************/

static GDALDataset *GetSharedDS(const char *pszFilename,
                                unsigned int nOpenFlags,
                                const char *const *papszOpenOptions)
{
    CPLMutexHolderD(&hDLMutex);

    if (phSharedDatasetSet != nullptr)
    {
        const GIntBig nThisPID = GDALGetResponsiblePIDForCurrentThread();
        SharedDatasetCtxt sStruct;

        sStruct.nPID = nThisPID;
        sStruct.pszDescription = const_cast<char *>(pszFilename);
        sStruct.nOpenFlags = nOpenFlags & ~GDAL_OF_SHARED;
        std::string osConcatenatedOpenOptions =
            GDALSharedDatasetConcatenateOpenOptions(papszOpenOptions);
        sStruct.pszConcatenatedOpenOptions = &osConcatenatedOpenOptions[0];
        sStruct.poDS = nullptr;
        SharedDatasetCtxt *psStruct = static_cast<SharedDatasetCtxt *>(
            CPLHashSetLookup(phSharedDatasetSet, &sStruct));
        if (psStruct == nullptr && (nOpenFlags & GDAL_OF_UPDATE) == 0)
        {
            sStruct.nOpenFlags |= GDAL_OF_UPDATE;
            psStruct = static_cast<SharedDatasetCtxt *>(
                CPLHashSetLookup(phSharedDatasetSet, &sStruct));
        }
        if (psStruct)
        {
            return psStruct->poDS;
        }
    }
    return nullptr;
}

/************************************************************************/
/*                             GDALOpenEx()                             */
/************************************************************************/

/**
 * \brief Open a raster or vector file as a GDALDataset.
 *
 * This function will try to open the passed file, or virtual dataset
 * name by invoking the Open method of each registered GDALDriver in turn.
 * The first successful open will result in a returned dataset.  If all
 * drivers fail then NULL is returned and an error is issued.
 *
 * Several recommendations :
 * <ul>
 * <li>If you open a dataset object with GDAL_OF_UPDATE access, it is not
 * recommended to open a new dataset on the same underlying file.</li>
 * <li>The returned dataset should only be accessed by one thread at a time. If
 * you want to use it from different threads, you must add all necessary code
 * (mutexes, etc.)  to avoid concurrent use of the object. (Some drivers, such
 * as GeoTIFF, maintain internal state variables that are updated each time a
 * new block is read, thus preventing concurrent use.) </li>
 * </ul>
 *
 * For drivers supporting the VSI virtual file API, it is possible to open a
 * file in a .zip archive (see VSIInstallZipFileHandler()), in a
 * .tar/.tar.gz/.tgz archive (see VSIInstallTarFileHandler()) or on a HTTP / FTP
 * server (see VSIInstallCurlFileHandler())
 *
 * In order to reduce the need for searches through the operating system
 * file system machinery, it is possible to give an optional list of files with
 * the papszSiblingFiles parameter.
 * This is the list of all files at the same level in the file system as the
 * target file, including the target file. The filenames must not include any
 * path components, are essentially just the output of VSIReadDir() on the
 * parent directory. If the target object does not have filesystem semantics
 * then the file list should be NULL.
 *
 * @param pszFilename the name of the file to access.  In the case of
 * exotic drivers this may not refer to a physical file, but instead contain
 * information for the driver on how to access a dataset.  It should be in UTF-8
 * encoding.
 *
 * @param nOpenFlags a combination of GDAL_OF_ flags that may be combined
 * through logical or operator.
 * <ul>
 * <li>Driver kind:
 *   <ul>
 *     <li>GDAL_OF_RASTER for raster drivers,</li>
 *     <li>GDAL_OF_MULTIDIM_RASTER for multidimensional raster drivers,</li>
 *     <li>GDAL_OF_VECTOR for vector drivers,</li>
 *     <li>GDAL_OF_GNM for Geographic Network Model drivers.</li>
 *    </ul>
 * GDAL_OF_RASTER and GDAL_OF_MULTIDIM_RASTER are generally mutually
 * exclusive. If none of the value is specified, GDAL_OF_RASTER | GDAL_OF_VECTOR
 * | GDAL_OF_GNM is implied.
 * </li>
 * <li>Access mode: GDAL_OF_READONLY (exclusive)or GDAL_OF_UPDATE.
 * </li>
 * <li>Shared mode: GDAL_OF_SHARED. If set,
 * it allows the sharing of GDALDataset handles for a dataset with other callers
 * that have set GDAL_OF_SHARED. In particular, GDALOpenEx() will first consult
 * its list of currently open and shared GDALDataset's, and if the
 * GetDescription() name for one exactly matches the pszFilename passed to
 * GDALOpenEx() it will be referenced and returned, if GDALOpenEx() is called
 * from the same thread.
 * </li>
 * <li>Thread safe mode: GDAL_OF_THREAD_SAFE (added in 3.10).
 * This must be use in combination with GDAL_OF_RASTER, and is mutually
 * exclusive with GDAL_OF_UPDATE, GDAL_OF_VECTOR, GDAL_OF_MULTIDIM_RASTER or
 * GDAL_OF_GNM.
 * </li>
 * <li>Verbose error: GDAL_OF_VERBOSE_ERROR. If set,
 * a failed attempt to open the file will lead to an error message to be
 * reported.
 * </li>
 * </ul>
 *
 * @param papszAllowedDrivers NULL to consider all candidate drivers, or a NULL
 * terminated list of strings with the driver short names that must be
 * considered.
 *
 * @param papszOpenOptions NULL, or a NULL terminated list of strings with open
 * options passed to candidate drivers. An option exists for all drivers,
 * OVERVIEW_LEVEL=level, to select a particular overview level of a dataset.
 * The level index starts at 0. The level number can be suffixed by "only" to
 * specify that only this overview level must be visible, and not sub-levels.
 * Open options are validated by default, and a warning is emitted in case the
 * option is not recognized. In some scenarios, it might be not desirable (e.g.
 * when not knowing which driver will open the file), so the special open option
 * VALIDATE_OPEN_OPTIONS can be set to NO to avoid such warnings. Alternatively,
 * since GDAL 2.1, an option name can be preceded by the @ character to indicate
 * that it may not cause a warning if the driver doesn't declare this option.
 * Starting with GDAL 3.3, OVERVIEW_LEVEL=NONE is supported to indicate that
 * no overviews should be exposed.
 *
 * @param papszSiblingFiles NULL, or a NULL terminated list of strings that are
 * filenames that are auxiliary to the main filename. If NULL is passed, a
 * probing of the file system will be done.
 *
 * @return A GDALDatasetH handle or NULL on failure.  For C++ applications
 * this handle can be cast to a GDALDataset *.
 *
 * @since GDAL 2.0
 */

GDALDatasetH CPL_STDCALL GDALOpenEx(const char *pszFilename,
                                    unsigned int nOpenFlags,
                                    const char *const *papszAllowedDrivers,
                                    const char *const *papszOpenOptions,
                                    const char *const *papszSiblingFiles)
{
    VALIDATE_POINTER1(pszFilename, "GDALOpen", nullptr);

    // Hack for the ZARR driver. We translate the CACHE_KERCHUNK_JSON
    // into VSIKERCHUNK_USE_CACHE config option
    std::unique_ptr<CPLConfigOptionSetter> poVSIKERCHUNK_USE_CACHESetter;
    if (CPLFetchBool(papszOpenOptions, "CACHE_KERCHUNK_JSON", false))
    {
        poVSIKERCHUNK_USE_CACHESetter = std::make_unique<CPLConfigOptionSetter>(
            "VSIKERCHUNK_USE_CACHE", "YES", false);
    }

    // Do some sanity checks on incompatible flags with thread-safe mode.
    if ((nOpenFlags & GDAL_OF_THREAD_SAFE) != 0)
    {
        const struct
        {
            int nFlag;
            const char *pszFlagName;
        } asFlags[] = {
            {GDAL_OF_UPDATE, "GDAL_OF_UPDATE"},
            {GDAL_OF_VECTOR, "GDAL_OF_VECTOR"},
            {GDAL_OF_MULTIDIM_RASTER, "GDAL_OF_MULTIDIM_RASTER"},
            {GDAL_OF_GNM, "GDAL_OF_GNM"},
        };

        for (const auto &asFlag : asFlags)
        {
            if ((nOpenFlags & asFlag.nFlag) != 0)
            {
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "GDAL_OF_THREAD_SAFE and %s are mutually "
                         "exclusive",
                         asFlag.pszFlagName);
                return nullptr;
            }
        }
    }

    // If no driver kind is specified, assume all are to be probed.
    if ((nOpenFlags & GDAL_OF_KIND_MASK) == 0)
        nOpenFlags |= GDAL_OF_KIND_MASK & ~GDAL_OF_MULTIDIM_RASTER;

    /* -------------------------------------------------------------------- */
    /*      In case of shared dataset, first scan the existing list to see  */
    /*      if it could already contain the requested dataset.              */
    /* -------------------------------------------------------------------- */
    if (nOpenFlags & GDAL_OF_SHARED)
    {
        if (nOpenFlags & GDAL_OF_INTERNAL)
        {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "GDAL_OF_SHARED and GDAL_OF_INTERNAL are exclusive");
            return nullptr;
        }

        auto poSharedDS =
            GetSharedDS(pszFilename, nOpenFlags, papszOpenOptions);
        if (poSharedDS)
        {
            poSharedDS->Reference();
            return poSharedDS;
        }
    }

    GDALDriverManager *poDM = GetGDALDriverManager();
    // CPLLocaleC  oLocaleForcer;

    CPLErrorReset();
    VSIErrorReset();
    CPLAssert(nullptr != poDM);

    // Build GDALOpenInfo just now to avoid useless file stat'ing if a
    // shared dataset was asked before.
    GDALOpenInfo oOpenInfo(pszFilename, nOpenFlags,
                           const_cast<char **>(papszSiblingFiles));
    oOpenInfo.papszAllowedDrivers = papszAllowedDrivers;

    GDALAntiRecursionStruct &sAntiRecursion = GetAntiRecursionOpen();
    if (sAntiRecursion.nRecLevel == 100)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALOpen() called with too many recursion levels");
        return nullptr;
    }

    std::string osAllowedDrivers;
    for (const char *pszDriverName : cpl::Iterate(papszAllowedDrivers))
        osAllowedDrivers += pszDriverName;
    auto dsCtxt = GDALAntiRecursionStruct::DatasetContext(
        std::string(pszFilename), nOpenFlags, osAllowedDrivers);
    if (cpl::contains(sAntiRecursion.aosDatasetNamesWithFlags, dsCtxt))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "GDALOpen() called on %s recursively", pszFilename);
        return nullptr;
    }

    // Remove leading @ if present.
    char **papszOpenOptionsCleaned =
        CSLDuplicate(const_cast<char **>(papszOpenOptions));
    for (char **papszIter = papszOpenOptionsCleaned; papszIter && *papszIter;
         ++papszIter)
    {
        char *pszOption = *papszIter;
        if (pszOption[0] == '@')
            memmove(pszOption, pszOption + 1, strlen(pszOption + 1) + 1);
    }

    oOpenInfo.papszOpenOptions = papszOpenOptionsCleaned;
    oOpenInfo.nOpenFlags |= GDAL_OF_FROM_GDALOPEN;

#ifdef OGRAPISPY_ENABLED
    const bool bUpdate = (nOpenFlags & GDAL_OF_UPDATE) != 0;
    const int iSnapshot =
        (nOpenFlags & GDAL_OF_VECTOR) != 0 && (nOpenFlags & GDAL_OF_RASTER) == 0
            ? OGRAPISpyOpenTakeSnapshot(pszFilename, bUpdate)
            : INT_MIN;
#endif

    const int nDriverCount = poDM->GetDriverCount(/*bIncludeHidden=*/true);
    GDALDriver *poMissingPluginDriver = nullptr;
    std::vector<GDALDriver *> apoSecondPassDrivers;

    // Lookup of matching driver for dataset can involve up to 2 passes:
    // - in the first pass, all drivers that are compabile of the request mode
    //   (raster/vector/etc.) are probed using their Identify() method if it
    //   exists. If the Identify() method returns FALSE, the driver is skipped.
    //   If the Identify() methods returns GDAL_IDENTIFY_UNKNOWN and that the
    //   driver is a deferred-loading plugin, it is added to the
    //   apoSecondPassDrivers list for potential later probing, and execution
    //   continues to the next driver in the list.
    //   Otherwise if Identify() returns non-FALSE, the Open() method is used.
    //   If Open() returns a non-NULL dataset, the loop stops and it is
    //   returned. Otherwise looping over remaining drivers continues.
    // - the second pass is optional, only if at least one driver was added
    //   into apoSecondPassDrivers during the first pass. It is similar
    //   to the first pass except it runs only on apoSecondPassDrivers drivers.
    //   And the Open() method of such drivers is used, causing them to be
    //   loaded for real.
    int iPass = 1;
retry:
    for (int iDriver = 0;
         iDriver < (iPass == 1 ? nDriverCount
                               : static_cast<int>(apoSecondPassDrivers.size()));
         ++iDriver)
    {
        GDALDriver *poDriver =
            iPass == 1 ? poDM->GetDriver(iDriver, /*bIncludeHidden=*/true)
                       : apoSecondPassDrivers[iDriver];
        if (papszAllowedDrivers != nullptr &&
            CSLFindString(papszAllowedDrivers,
                          GDALGetDriverShortName(poDriver)) == -1)
        {
            continue;
        }

        if (poDriver->GetMetadataItem(GDAL_DCAP_OPEN) == nullptr)
            continue;

        if ((nOpenFlags & GDAL_OF_RASTER) != 0 &&
            (nOpenFlags & GDAL_OF_VECTOR) == 0 &&
            poDriver->GetMetadataItem(GDAL_DCAP_RASTER) == nullptr)
            continue;
        if ((nOpenFlags & GDAL_OF_VECTOR) != 0 &&
            (nOpenFlags & GDAL_OF_RASTER) == 0 &&
            poDriver->GetMetadataItem(GDAL_DCAP_VECTOR) == nullptr)
            continue;
        if ((nOpenFlags & GDAL_OF_MULTIDIM_RASTER) != 0 &&
            (nOpenFlags & GDAL_OF_RASTER) == 0 &&
            poDriver->GetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER) == nullptr)
            continue;

        // Remove general OVERVIEW_LEVEL open options from list before passing
        // it to the driver, if it isn't a driver specific option already.
        char **papszTmpOpenOptions = nullptr;
        char **papszTmpOpenOptionsToValidate = nullptr;
        char **papszOptionsToValidate = const_cast<char **>(papszOpenOptions);
        if (CSLFetchNameValue(papszOpenOptionsCleaned, "OVERVIEW_LEVEL") !=
                nullptr &&
            !poDriver->HasOpenOption("OVERVIEW_LEVEL"))
        {
            papszTmpOpenOptions = CSLDuplicate(papszOpenOptionsCleaned);
            papszTmpOpenOptions =
                CSLSetNameValue(papszTmpOpenOptions, "OVERVIEW_LEVEL", nullptr);
            oOpenInfo.papszOpenOptions = papszTmpOpenOptions;

            papszOptionsToValidate = CSLDuplicate(papszOptionsToValidate);
            papszOptionsToValidate = CSLSetNameValue(papszOptionsToValidate,
                                                     "OVERVIEW_LEVEL", nullptr);
            papszTmpOpenOptionsToValidate = papszOptionsToValidate;
        }

        const int nIdentifyRes =
            poDriver->pfnIdentifyEx
                ? poDriver->pfnIdentifyEx(poDriver, &oOpenInfo)
            : poDriver->pfnIdentify ? poDriver->pfnIdentify(&oOpenInfo)
                                    : GDAL_IDENTIFY_UNKNOWN;
        if (nIdentifyRes == FALSE)
        {
            CSLDestroy(papszTmpOpenOptions);
            CSLDestroy(papszTmpOpenOptionsToValidate);
            oOpenInfo.papszOpenOptions = papszOpenOptionsCleaned;
            continue;
        }
        else if (iPass == 1 && nIdentifyRes < 0 &&
                 poDriver->pfnOpen == nullptr &&
                 poDriver->GetMetadataItem("IS_NON_LOADED_PLUGIN"))
        {
            // Not loaded plugin
            apoSecondPassDrivers.push_back(poDriver);
            CSLDestroy(papszTmpOpenOptions);
            CSLDestroy(papszTmpOpenOptionsToValidate);
            oOpenInfo.papszOpenOptions = papszOpenOptionsCleaned;
            continue;
        }

        const bool bIdentifyRes = nIdentifyRes == GDAL_IDENTIFY_TRUE;
        if (bIdentifyRes)
        {
            GDALValidateOpenOptions(poDriver, papszOptionsToValidate);
        }

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        const bool bFpAvailableBefore = oOpenInfo.fpL != nullptr;
        CPLErrorReset();
#endif

        sAntiRecursion.nRecLevel++;
        sAntiRecursion.aosDatasetNamesWithFlags.insert(dsCtxt);

        GDALDataset *poDS = poDriver->Open(&oOpenInfo, false);

        sAntiRecursion.nRecLevel--;
        sAntiRecursion.aosDatasetNamesWithFlags.erase(dsCtxt);

        if (poDriver->pfnOpen != nullptr)
        {
            // If we couldn't determine for sure with Identify() (it returned
            // -1), but Open() managed to open the file, post validate options.
            if (poDS != nullptr &&
                (poDriver->pfnIdentify || poDriver->pfnIdentifyEx) &&
                !bIdentifyRes)
            {
                GDALValidateOpenOptions(poDriver, papszOptionsToValidate);
            }
        }
        else if (poDriver->pfnOpenWithDriverArg != nullptr)
        {
            // do nothing
        }
        else if (bIdentifyRes &&
                 poDriver->GetMetadataItem("MISSING_PLUGIN_FILENAME"))
        {
            if (!poMissingPluginDriver)
            {
                poMissingPluginDriver = poDriver;
            }
        }
        else
        {
            // should not happen given the GDAL_DCAP_OPEN check
            CSLDestroy(papszTmpOpenOptions);
            CSLDestroy(papszTmpOpenOptionsToValidate);
            oOpenInfo.papszOpenOptions = papszOpenOptionsCleaned;
            continue;
        }

        CSLDestroy(papszTmpOpenOptions);
        CSLDestroy(papszTmpOpenOptionsToValidate);
        oOpenInfo.papszOpenOptions = papszOpenOptionsCleaned;

        if (poDS != nullptr)
        {
            if (poDS->papszOpenOptions == nullptr)
            {
                poDS->papszOpenOptions = papszOpenOptionsCleaned;
                papszOpenOptionsCleaned = nullptr;
            }

            // Deal with generic OVERVIEW_LEVEL open option, unless it is
            // driver specific.
            if (CSLFetchNameValue(papszOpenOptions, "OVERVIEW_LEVEL") !=
                    nullptr &&
                !poDriver->HasOpenOption("OVERVIEW_LEVEL"))
            {
                CPLString osVal(
                    CSLFetchNameValue(papszOpenOptions, "OVERVIEW_LEVEL"));
                const int nOvrLevel = EQUAL(osVal, "NONE") ? -1 : atoi(osVal);
                const bool bThisLevelOnly =
                    nOvrLevel == -1 || osVal.ifind("only") != std::string::npos;
                GDALDataset *poOvrDS =
                    GDALCreateOverviewDataset(poDS, nOvrLevel, bThisLevelOnly);
                if (poOvrDS && (nOpenFlags & GDAL_OF_SHARED) != 0)
                {
                    if (strcmp(pszFilename, poDS->GetDescription()) != 0)
                    {
                        CPLError(
                            CE_Warning, CPLE_NotSupported,
                            "A dataset opened by GDALOpenShared should have "
                            "the same filename (%s) "
                            "and description (%s)",
                            pszFilename, poDS->GetDescription());
                    }
                    else
                    {
                        CSLDestroy(poDS->papszOpenOptions);
                        poDS->papszOpenOptions = CSLDuplicate(papszOpenOptions);
                        poDS->papszOpenOptions = CSLSetNameValue(
                            poDS->papszOpenOptions, "OVERVIEW_LEVEL", nullptr);
                    }
                }
                poDS->ReleaseRef();
                poDS = poOvrDS;
                if (poDS == nullptr)
                {
                    if (nOpenFlags & GDAL_OF_VERBOSE_ERROR)
                    {
                        CPLError(CE_Failure, CPLE_OpenFailed,
                                 "Cannot open overview level %d of %s",
                                 nOvrLevel, pszFilename);
                    }
                }
                else
                {
                    // For thread-safe opening, currently poDS is what will be
                    // the "master" dataset owned by the thread-safe dataset
                    // returned to the user, hence we do not register it as a
                    // visible one in the open dataset list, or mark it as shared.
                    if (!(nOpenFlags & GDAL_OF_INTERNAL) &&
                        !(nOpenFlags & GDAL_OF_THREAD_SAFE))
                    {
                        poDS->AddToDatasetOpenList();
                    }
                    if (nOpenFlags & GDAL_OF_SHARED)
                    {
                        CSLDestroy(poDS->papszOpenOptions);
                        poDS->papszOpenOptions = CSLDuplicate(papszOpenOptions);
                        poDS->nOpenFlags = nOpenFlags;
                        if (!(nOpenFlags & GDAL_OF_THREAD_SAFE))
                            poDS->MarkAsShared();
                    }
                }
            }
            else if (nOpenFlags & GDAL_OF_SHARED)
            {
                if (strcmp(pszFilename, poDS->GetDescription()) != 0)
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "A dataset opened by GDALOpenShared should have "
                             "the same filename (%s) "
                             "and description (%s)",
                             pszFilename, poDS->GetDescription());
                }
                else if (!(nOpenFlags & GDAL_OF_THREAD_SAFE))
                {
                    // For thread-safe opening, currently poDS is what will be
                    // the "master" dataset owned by the thread-safe dataset
                    // returned to the user, hence we do not or mark it as shared.
                    poDS->MarkAsShared();
                }
            }

            VSIErrorReset();

            CSLDestroy(papszOpenOptionsCleaned);

#ifdef OGRAPISPY_ENABLED
            if (iSnapshot != INT_MIN)
            {
                GDALDatasetH hDS = GDALDataset::ToHandle(poDS);
                OGRAPISpyOpen(pszFilename, bUpdate, iSnapshot, &hDS);
                poDS = GDALDataset::FromHandle(hDS);
            }
#endif

            if (poDS)
            {
                poDS->m_bCanBeReopened = true;

                if ((nOpenFlags & GDAL_OF_THREAD_SAFE) != 0)
                {
                    poDS =
                        GDALGetThreadSafeDataset(
                            std::unique_ptr<GDALDataset>(poDS), GDAL_OF_RASTER)
                            .release();
                    if (poDS)
                    {
                        poDS->m_bCanBeReopened = true;
                        poDS->poDriver = poDriver;
                        poDS->nOpenFlags = nOpenFlags;
                        if (!(nOpenFlags & GDAL_OF_INTERNAL))
                            poDS->AddToDatasetOpenList();
                        if (nOpenFlags & GDAL_OF_SHARED)
                            poDS->MarkAsShared();
                    }
                }
            }

            return poDS;
        }

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (bFpAvailableBefore && oOpenInfo.fpL == nullptr)
        {
            // In case the file descriptor was "consumed" by a driver
            // that ultimately failed, re-open it for next drivers.
            oOpenInfo.fpL = VSIFOpenL(
                pszFilename, (oOpenInfo.eAccess == GA_Update) ? "r+b" : "rb");
        }
#else
        if (CPLGetLastErrorNo() != 0 && CPLGetLastErrorType() > CE_Warning)
        {
            CSLDestroy(papszOpenOptionsCleaned);

#ifdef OGRAPISPY_ENABLED
            if (iSnapshot != INT_MIN)
            {
                GDALDatasetH hDS = nullptr;
                OGRAPISpyOpen(pszFilename, bUpdate, iSnapshot, &hDS);
            }
#endif
            return nullptr;
        }
#endif
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (iPass == 1 && !apoSecondPassDrivers.empty())
    {
        CPLDebugOnly("GDAL", "GDALOpen(): Second pass");
        iPass = 2;
        goto retry;
    }

    CSLDestroy(papszOpenOptionsCleaned);

#ifdef OGRAPISPY_ENABLED
    if (iSnapshot != INT_MIN)
    {
        GDALDatasetH hDS = nullptr;
        OGRAPISpyOpen(pszFilename, bUpdate, iSnapshot, &hDS);
    }
#endif

    if (nOpenFlags & GDAL_OF_VERBOSE_ERROR)
    {
        if (nDriverCount == 0)
        {
            CPLError(CE_Failure, CPLE_OpenFailed, "No driver registered.");
        }
        else if (poMissingPluginDriver)
        {
            std::string osMsg("`");
            osMsg += pszFilename;
            osMsg += "' not recognized as being in a supported file format. "
                     "It could have been recognized by driver ";
            osMsg += poMissingPluginDriver->GetDescription();
            osMsg += ", but plugin ";
            osMsg +=
                GDALGetMessageAboutMissingPluginDriver(poMissingPluginDriver);

            CPLError(CE_Failure, CPLE_OpenFailed, "%s", osMsg.c_str());
        }
        // Check to see if there was a filesystem error, and report it if so.
        // If not, return a more generic error.
        else if (!VSIToCPLError(CE_Failure, CPLE_OpenFailed))
        {
            if (oOpenInfo.bStatOK)
            {
                CPLError(CE_Failure, CPLE_OpenFailed,
                         "`%s' not recognized as being in a supported file "
                         "format.",
                         pszFilename);
            }
            else
            {
                // If Stat failed and no VSI error was set, assume it is because
                // the file did not exist on the filesystem.
                CPLError(CE_Failure, CPLE_OpenFailed,
                         "`%s' does not exist in the file system, "
                         "and is not recognized as a supported dataset name.",
                         pszFilename);
            }
        }
    }

    return nullptr;
}

/************************************************************************/
/*                           GDALOpenShared()                           */
/************************************************************************/

/**
 * \brief Open a raster file as a GDALDataset.
 *
 * This function works the same as GDALOpen(), but allows the sharing of
 * GDALDataset handles for a dataset with other callers to GDALOpenShared().
 *
 * In particular, GDALOpenShared() will first consult its list of currently
 * open and shared GDALDataset's, and if the GetDescription() name for one
 * exactly matches the pszFilename passed to GDALOpenShared() it will be
 * referenced and returned.
 *
 * Starting with GDAL 1.6.0, if GDALOpenShared() is called on the same
 * pszFilename from two different threads, a different GDALDataset object will
 * be returned as it is not safe to use the same dataset from different threads,
 * unless the user does explicitly use mutexes in its code.
 *
 * For drivers supporting the VSI virtual file API, it is possible to open a
 * file in a .zip archive (see VSIInstallZipFileHandler()), in a
 * .tar/.tar.gz/.tgz archive (see VSIInstallTarFileHandler()) or on a HTTP / FTP
 * server (see VSIInstallCurlFileHandler())
 *
 * \sa GDALOpen()
 * \sa GDALOpenEx()
 *
 * @param pszFilename the name of the file to access.  In the case of
 * exotic drivers this may not refer to a physical file, but instead contain
 * information for the driver on how to access a dataset.  It should be in
 * UTF-8 encoding.
 *
 * @param eAccess the desired access, either GA_Update or GA_ReadOnly.  Many
 * drivers support only read only access.
 *
 * @return A GDALDatasetH handle or NULL on failure.  For C++ applications
 * this handle can be cast to a GDALDataset *.
 */

GDALDatasetH CPL_STDCALL GDALOpenShared(const char *pszFilename,
                                        GDALAccess eAccess)
{
    VALIDATE_POINTER1(pszFilename, "GDALOpenShared", nullptr);
    return GDALOpenEx(pszFilename,
                      GDAL_OF_RASTER |
                          (eAccess == GA_Update ? GDAL_OF_UPDATE : 0) |
                          GDAL_OF_SHARED | GDAL_OF_VERBOSE_ERROR,
                      nullptr, nullptr, nullptr);
}

/************************************************************************/
/*                             GDALClose()                              */
/************************************************************************/

/**
 * \brief Close GDAL dataset.
 *
 * For non-shared datasets (opened with GDALOpen()) the dataset is closed
 * using the C++ "delete" operator, recovering all dataset related resources.
 * For shared datasets (opened with GDALOpenShared()) the dataset is
 * dereferenced, and closed only if the referenced count has dropped below 1.
 *
 * @param hDS The dataset to close.  May be cast from a "GDALDataset *".
 * @return CE_None in case of success (return value since GDAL 3.7). On a
 * shared dataset whose reference count is not dropped below 1, CE_None will
 * be returned.
 */

CPLErr CPL_STDCALL GDALClose(GDALDatasetH hDS)

{
    if (!hDS)
        return CE_None;

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpyPreClose(hDS);
#endif

    GDALDataset *poDS = GDALDataset::FromHandle(hDS);

    if (poDS->GetShared())
    {
        /* --------------------------------------------------------------------
         */
        /*      If this file is in the shared dataset list then dereference */
        /*      it, and only delete/remote it if the reference count has */
        /*      dropped to zero. */
        /* --------------------------------------------------------------------
         */
        if (poDS->Dereference() > 0)
            return CE_None;

        CPLErr eErr = poDS->Close();
        delete poDS;

#ifdef OGRAPISPY_ENABLED
        if (bOGRAPISpyEnabled)
            OGRAPISpyPostClose();
#endif

        return eErr;
    }

    /* -------------------------------------------------------------------- */
    /*      This is not shared dataset, so directly delete it.              */
    /* -------------------------------------------------------------------- */
    CPLErr eErr = poDS->Close();
    delete poDS;

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpyPostClose();
#endif
    return eErr;
}

/************************************************************************/
/*                        GDALDumpOpenDataset()                         */
/************************************************************************/

static int GDALDumpOpenSharedDatasetsForeach(void *elt, void *user_data)
{
    SharedDatasetCtxt *psStruct = static_cast<SharedDatasetCtxt *>(elt);
    FILE *fp = static_cast<FILE *>(user_data);
    GDALDataset *poDS = psStruct->poDS;

    const char *pszDriverName = poDS->GetDriver() == nullptr
                                    ? "DriverIsNULL"
                                    : poDS->GetDriver()->GetDescription();

    poDS->Reference();
    CPL_IGNORE_RET_VAL(
        VSIFPrintf(fp, "  %d %c %-6s %7d %dx%dx%d %s\n", poDS->Dereference(),
                   poDS->GetShared() ? 'S' : 'N', pszDriverName,
                   static_cast<int>(psStruct->nPID), poDS->GetRasterXSize(),
                   poDS->GetRasterYSize(), poDS->GetRasterCount(),
                   poDS->GetDescription()));

    return TRUE;
}

static int GDALDumpOpenDatasetsForeach(GDALDataset *poDS, FILE *fp)
{

    // Don't list shared datasets. They have already been listed by
    // GDALDumpOpenSharedDatasetsForeach.
    if (poDS->GetShared())
        return TRUE;

    const char *pszDriverName = poDS->GetDriver() == nullptr
                                    ? "DriverIsNULL"
                                    : poDS->GetDriver()->GetDescription();

    poDS->Reference();
    CPL_IGNORE_RET_VAL(
        VSIFPrintf(fp, "  %d %c %-6s %7d %dx%dx%d %s\n", poDS->Dereference(),
                   poDS->GetShared() ? 'S' : 'N', pszDriverName, -1,
                   poDS->GetRasterXSize(), poDS->GetRasterYSize(),
                   poDS->GetRasterCount(), poDS->GetDescription()));

    return TRUE;
}

/**
 * \brief List open datasets.
 *
 * Dumps a list of all open datasets (shared or not) to the indicated
 * text file (may be stdout or stderr).   This function is primarily intended
 * to assist in debugging "dataset leaks" and reference counting issues.
 * The information reported includes the dataset name, referenced count,
 * shared status, driver name, size, and band count.
 */

int CPL_STDCALL GDALDumpOpenDatasets(FILE *fp)

{
    VALIDATE_POINTER1(fp, "GDALDumpOpenDatasets", 0);

    CPLMutexHolderD(&hDLMutex);

    if (poAllDatasetMap == nullptr)
        return 0;

    CPL_IGNORE_RET_VAL(VSIFPrintf(fp, "Open GDAL Datasets:\n"));

    for (const auto &oIter : *poAllDatasetMap)
    {
        GDALDumpOpenDatasetsForeach(oIter.first, fp);
    }

    if (phSharedDatasetSet != nullptr)
    {
        CPLHashSetForeach(phSharedDatasetSet, GDALDumpOpenSharedDatasetsForeach,
                          fp);
    }
    return static_cast<int>(poAllDatasetMap->size());
}

/************************************************************************/
/*                        BeginAsyncReader()                            */
/************************************************************************/

/**
 * \brief Sets up an asynchronous data request
 *
 * This method establish an asynchronous raster read request for the
 * indicated window on the dataset into the indicated buffer.  The parameters
 * for windowing, buffer size, buffer type and buffer organization are similar
 * to those for GDALDataset::RasterIO(); however, this call only launches
 * the request and filling the buffer is accomplished via calls to
 * GetNextUpdatedRegion() on the return GDALAsyncReader session object.
 *
 * Once all processing for the created session is complete, or if no further
 * refinement of the request is required, the GDALAsyncReader object should
 * be destroyed with the GDALDataset::EndAsyncReader() method.
 *
 * Note that the data buffer (pData) will potentially continue to be
 * updated as long as the session lives, but it is not deallocated when
 * the session (GDALAsyncReader) is destroyed with EndAsyncReader().  It
 * should be deallocated by the application at that point.
 *
 * Additional information on asynchronous IO in GDAL may be found at:
 *   https://gdal.org/development/rfc/rfc24_progressive_data_support.html
 *
 * This method is the same as the C GDALBeginAsyncReader() function.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param pBuf The buffer into which the data should be read. This buffer must
 * contain at least nBufXSize * nBufYSize * nBandCount words of type eBufType.
 * It is organized in left to right,top to bottom pixel order.  Spacing is
 * controlled by the nPixelSpace, and nLineSpace parameters.
 *
 * @param nBufXSize the width of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param nBufYSize the height of the buffer image into which the desired
 * region is to be read, or from which it is to be written.
 *
 * @param eBufType the type of the pixel values in the pData data buffer.  The
 * pixel values will automatically be translated to/from the GDALRasterBand
 * data type as needed.
 *
 * @param nBandCount the number of bands being read or written.
 *
 * @param panBandMap the list of nBandCount band numbers being read/written.
 * Note band numbers are 1 based.   This may be NULL to select the first
 * nBandCount bands.
 *
 * @param nPixelSpace The byte offset from the start of one pixel value in
 * pData to the start of the next pixel value within a scanline.  If defaulted
 * (0) the size of the datatype eBufType is used.
 *
 * @param nLineSpace The byte offset from the start of one scanline in
 * pData to the start of the next.  If defaulted the size of the datatype
 * eBufType * nBufXSize is used.
 *
 * @param nBandSpace the byte offset from the start of one bands data to the
 * start of the next.  If defaulted (zero) the value will be
 * nLineSpace * nBufYSize implying band sequential organization
 * of the data buffer.
 *
 * @param papszOptions Driver specific control options in a string list or NULL.
 * Consult driver documentation for options supported.
 *
 * @return The GDALAsyncReader object representing the request.
 */

GDALAsyncReader *GDALDataset::BeginAsyncReader(
    int nXOff, int nYOff, int nXSize, int nYSize, void *pBuf, int nBufXSize,
    int nBufYSize, GDALDataType eBufType, int nBandCount, int *panBandMap,
    int nPixelSpace, int nLineSpace, int nBandSpace, char **papszOptions)
{
    // See gdaldefaultasync.cpp

    return GDALGetDefaultAsyncReader(this, nXOff, nYOff, nXSize, nYSize, pBuf,
                                     nBufXSize, nBufYSize, eBufType, nBandCount,
                                     panBandMap, nPixelSpace, nLineSpace,
                                     nBandSpace, papszOptions);
}

/************************************************************************/
/*                        GDALBeginAsyncReader()                      */
/************************************************************************/

/**
 * \brief Sets up an asynchronous data request
 *
 * This method establish an asynchronous raster read request for the
 * indicated window on the dataset into the indicated buffer.  The parameters
 * for windowing, buffer size, buffer type and buffer organization are similar
 * to those for GDALDataset::RasterIO(); however, this call only launches
 * the request and filling the buffer is accomplished via calls to
 * GetNextUpdatedRegion() on the return GDALAsyncReader session object.
 *
 * Once all processing for the created session is complete, or if no further
 * refinement of the request is required, the GDALAsyncReader object should
 * be destroyed with the GDALDataset::EndAsyncReader() method.
 *
 * Note that the data buffer (pData) will potentially continue to be
 * updated as long as the session lives, but it is not deallocated when
 * the session (GDALAsyncReader) is destroyed with EndAsyncReader().  It
 * should be deallocated by the application at that point.
 *
 * Additional information on asynchronous IO in GDAL may be found at:
 *   https://gdal.org/development/rfc/rfc24_progressive_data_support.html
 *
 * This method is the same as the C++ GDALDataset::BeginAsyncReader() method.
 *
 * @param hDS handle to the dataset object.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param pBuf The buffer into which the data should be read. This buffer must
 * contain at least nBufXSize * nBufYSize * nBandCount words of type eBufType.
 * It is organized in left to right,top to bottom pixel order.  Spacing is
 * controlled by the nPixelSpace, and nLineSpace parameters.
 *
 * @param nBufXSize the width of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param nBufYSize the height of the buffer image into which the desired
 * region is to be read, or from which it is to be written.
 *
 * @param eBufType the type of the pixel values in the pData data buffer.  The
 * pixel values will automatically be translated to/from the GDALRasterBand
 * data type as needed.
 *
 * @param nBandCount the number of bands being read or written.
 *
 * @param panBandMap the list of nBandCount band numbers being read/written.
 * Note band numbers are 1 based.   This may be NULL to select the first
 * nBandCount bands.
 *
 * @param nPixelSpace The byte offset from the start of one pixel value in
 * pData to the start of the next pixel value within a scanline.  If defaulted
 * (0) the size of the datatype eBufType is used.
 *
 * @param nLineSpace The byte offset from the start of one scanline in
 * pData to the start of the next.  If defaulted the size of the datatype
 * eBufType * nBufXSize is used.
 *
 * @param nBandSpace the byte offset from the start of one bands data to the
 * start of the next.  If defaulted (zero) the value will be
 * nLineSpace * nBufYSize implying band sequential organization
 * of the data buffer.
 *
 * @param papszOptions Driver specific control options in a string list or NULL.
 * Consult driver documentation for options supported.
 *
 * @return handle representing the request.
 */

GDALAsyncReaderH CPL_STDCALL GDALBeginAsyncReader(
    GDALDatasetH hDS, int nXOff, int nYOff, int nXSize, int nYSize, void *pBuf,
    int nBufXSize, int nBufYSize, GDALDataType eBufType, int nBandCount,
    int *panBandMap, int nPixelSpace, int nLineSpace, int nBandSpace,
    CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDS, "GDALDataset", nullptr);
    return static_cast<GDALAsyncReaderH>(
        GDALDataset::FromHandle(hDS)->BeginAsyncReader(
            nXOff, nYOff, nXSize, nYSize, pBuf, nBufXSize, nBufYSize, eBufType,
            nBandCount, panBandMap, nPixelSpace, nLineSpace, nBandSpace,
            const_cast<char **>(papszOptions)));
}

/************************************************************************/
/*                        EndAsyncReader()                            */
/************************************************************************/

/**
 * End asynchronous request.
 *
 * This method destroys an asynchronous io request and recovers all
 * resources associated with it.
 *
 * This method is the same as the C function GDALEndAsyncReader().
 *
 * @param poARIO pointer to a GDALAsyncReader
 */

void GDALDataset::EndAsyncReader(GDALAsyncReader *poARIO)
{
    delete poARIO;
}

/************************************************************************/
/*                        GDALEndAsyncReader()                        */
/************************************************************************/

/**
 * End asynchronous request.
 *
 * This method destroys an asynchronous io request and recovers all
 * resources associated with it.
 *
 * This method is the same as the C++ method GDALDataset::EndAsyncReader().
 *
 * @param hDS handle to the dataset object.
 * @param hAsyncReaderH handle returned by GDALBeginAsyncReader()
 */

void CPL_STDCALL GDALEndAsyncReader(GDALDatasetH hDS,
                                    GDALAsyncReaderH hAsyncReaderH)
{
    VALIDATE_POINTER0(hDS, "GDALDataset");
    VALIDATE_POINTER0(hAsyncReaderH, "GDALAsyncReader");
    GDALDataset::FromHandle(hDS)->EndAsyncReader(
        static_cast<GDALAsyncReader *>(hAsyncReaderH));
}

/************************************************************************/
/*                       CloseDependentDatasets()                       */
/************************************************************************/

/**
 * Drop references to any other datasets referenced by this dataset.
 *
 * This method should release any reference to other datasets (e.g. a VRT
 * dataset to its sources), but not close the current dataset itself.
 *
 * If at least, one reference to a dependent dataset has been dropped,
 * this method should return TRUE. Otherwise it *should* return FALSE.
 * (Failure to return the proper value might result in infinite loop)
 *
 * This method can be called several times on a given dataset. After
 * the first time, it should not do anything and return FALSE.
 *
 * The driver implementation may choose to destroy its raster bands,
 * so be careful not to call any method on the raster bands afterwards.
 *
 * Basically the only safe action you can do after calling
 * CloseDependentDatasets() is to call the destructor.
 *
 * Note: the only legitimate caller of CloseDependentDatasets() is
 * GDALDriverManager::~GDALDriverManager()
 *
 * @return TRUE if at least one reference to another dataset has been dropped.
 */
int GDALDataset::CloseDependentDatasets()
{
    return oOvManager.CloseDependentDatasets();
}

/************************************************************************/
/*                            ReportError()                             */
/************************************************************************/

#ifndef DOXYGEN_XML
/**
 * \brief Emits an error related to a dataset.
 *
 * This function is a wrapper for regular CPLError(). The only difference
 * with CPLError() is that it prepends the error message with the dataset
 * name.
 *
 * @param eErrClass one of CE_Warning, CE_Failure or CE_Fatal.
 * @param err_no the error number (CPLE_*) from cpl_error.h.
 * @param fmt a printf() style format string.  Any additional arguments
 * will be treated as arguments to fill in this format in a manner
 * similar to printf().
 *
 * @since GDAL 1.9.0
 */

void GDALDataset::ReportError(CPLErr eErrClass, CPLErrorNum err_no,
                              const char *fmt, ...) const
{
    va_list args;
    va_start(args, fmt);
    ReportErrorV(GetDescription(), eErrClass, err_no, fmt, args);
    va_end(args);
}

/**
 * \brief Emits an error related to a dataset (static method)
 *
 * This function is a wrapper for regular CPLError(). The only difference
 * with CPLError() is that it prepends the error message with the dataset
 * name.
 *
 * @param pszDSName dataset name.
 * @param eErrClass one of CE_Warning, CE_Failure or CE_Fatal.
 * @param err_no the error number (CPLE_*) from cpl_error.h.
 * @param fmt a printf() style format string.  Any additional arguments
 * will be treated as arguments to fill in this format in a manner
 * similar to printf().
 *
 * @since GDAL 3.2.0
 */

void GDALDataset::ReportError(const char *pszDSName, CPLErr eErrClass,
                              CPLErrorNum err_no, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ReportErrorV(pszDSName, eErrClass, err_no, fmt, args);
    va_end(args);
}

void GDALDataset::ReportErrorV(const char *pszDSName, CPLErr eErrClass,
                               CPLErrorNum err_no, const char *fmt,
                               va_list args)
{
    pszDSName = CPLGetFilename(pszDSName);
    if (pszDSName[0] != '\0')
    {
        CPLError(eErrClass, err_no, "%s",
                 std::string(pszDSName)
                     .append(": ")
                     .append(CPLString().vPrintf(fmt, args))
                     .c_str());
    }
    else
    {
        CPLErrorV(eErrClass, err_no, fmt, args);
    }
}
#endif

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/
char **GDALDataset::GetMetadata(const char *pszDomain)
{
#ifndef WITHOUT_DERIVED
    if (pszDomain != nullptr && EQUAL(pszDomain, "DERIVED_SUBDATASETS"))
    {
        oDerivedMetadataList.Clear();

        // First condition: at least one raster band.
        if (GetRasterCount() > 0)
        {
            // Check if there is at least one complex band.
            bool hasAComplexBand = false;

            for (int rasterId = 1; rasterId <= GetRasterCount(); ++rasterId)
            {
                if (GDALDataTypeIsComplex(
                        GetRasterBand(rasterId)->GetRasterDataType()))
                {
                    hasAComplexBand = true;
                    break;
                }
            }

            unsigned int nbSupportedDerivedDS = 0;
            const DerivedDatasetDescription *poDDSDesc =
                GDALGetDerivedDatasetDescriptions(&nbSupportedDerivedDS);

            int nNumDataset = 1;
            for (unsigned int derivedId = 0; derivedId < nbSupportedDerivedDS;
                 ++derivedId)
            {
                if (hasAComplexBand ||
                    CPLString(poDDSDesc[derivedId].pszInputPixelType) !=
                        "complex")
                {
                    oDerivedMetadataList.SetNameValue(
                        CPLSPrintf("DERIVED_SUBDATASET_%d_NAME", nNumDataset),
                        CPLSPrintf("DERIVED_SUBDATASET:%s:%s",
                                   poDDSDesc[derivedId].pszDatasetName,
                                   GetDescription()));

                    CPLString osDesc(
                        CPLSPrintf("%s from %s",
                                   poDDSDesc[derivedId].pszDatasetDescription,
                                   GetDescription()));
                    oDerivedMetadataList.SetNameValue(
                        CPLSPrintf("DERIVED_SUBDATASET_%d_DESC", nNumDataset),
                        osDesc.c_str());

                    nNumDataset++;
                }
            }
        }
        return oDerivedMetadataList.List();
    }
#endif

    return GDALMajorObject::GetMetadata(pszDomain);
}

// clang-format off

/**
 * \fn GDALDataset::SetMetadata( char ** papszMetadata, const char * pszDomain)
 * \brief Set metadata.
 *
 * CAUTION: depending on the format, older values of the updated information
 * might still be found in the file in a "ghost" state, even if no longer
 * accessible through the GDAL API. This is for example the case of the GTiff
 * format (this is not a exhaustive list)
 *
 * The C function GDALSetMetadata() does the same thing as this method.
 *
 * @param papszMetadata the metadata in name=value string list format to
 * apply.
 * @param pszDomain the domain of interest.  Use "" or NULL for the default
 * domain.
 * @return CE_None on success, CE_Failure on failure and CE_Warning if the
 * metadata has been accepted, but is likely not maintained persistently
 * by the underlying object between sessions.
 */

/**
 * \fn GDALDataset::SetMetadataItem( const char * pszName, const char * pszValue, const char * pszDomain)
 * \brief Set single metadata item.
 *
 * CAUTION: depending on the format, older values of the updated information
 * might still be found in the file in a "ghost" state, even if no longer
 * accessible through the GDAL API. This is for example the case of the GTiff
 * format (this is not a exhaustive list)
 *
 * The C function GDALSetMetadataItem() does the same thing as this method.
 *
 * @param pszName the key for the metadata item to fetch.
 * @param pszValue the value to assign to the key.
 * @param pszDomain the domain to set within, use NULL for the default domain.
 *
 * @return CE_None on success, or an error code on failure.
 */

// clang-format on

/************************************************************************/
/*                            GetMetadataDomainList()                   */
/************************************************************************/

char **GDALDataset::GetMetadataDomainList()
{
    char **currentDomainList = CSLDuplicate(oMDMD.GetDomainList());

    // Ensure that we do not duplicate DERIVED domain.
    if (GetRasterCount() > 0 &&
        CSLFindString(currentDomainList, "DERIVED_SUBDATASETS") == -1)
    {
        currentDomainList =
            CSLAddString(currentDomainList, "DERIVED_SUBDATASETS");
    }
    return currentDomainList;
}

/************************************************************************/
/*                            GetDriverName()                           */
/************************************************************************/

/** Return driver name.
 * @return driver name.
 */
const char *GDALDataset::GetDriverName()
{
    if (poDriver)
        return poDriver->GetDescription();
    return "";
}

/************************************************************************/
/*                     GDALDatasetReleaseResultSet()                    */
/************************************************************************/

/**
 \brief Release results of ExecuteSQL().

 This function should only be used to deallocate OGRLayers resulting from
 an ExecuteSQL() call on the same GDALDataset.  Failure to deallocate a
 results set before destroying the GDALDataset may cause errors.

 This function is the same as the C++ method GDALDataset::ReleaseResultSet()

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param hLayer the result of a previous ExecuteSQL() call.

*/
void GDALDatasetReleaseResultSet(GDALDatasetH hDS, OGRLayerH hLayer)

{
    VALIDATE_POINTER0(hDS, "GDALDatasetReleaseResultSet");

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_ReleaseResultSet(hDS, hLayer);
#endif

    GDALDataset::FromHandle(hDS)->ReleaseResultSet(
        OGRLayer::FromHandle(hLayer));
}

/************************************************************************/
/*                       GDALDatasetGetLayerCount()                     */
/************************************************************************/

/**
 \brief Get the number of layers in this dataset.

 This function is the same as the C++ method GDALDataset::GetLayerCount()

 @since GDAL 2.0

 @param hDS the dataset handle.
 @return layer count.
*/

int GDALDatasetGetLayerCount(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetH", 0);

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_GetLayerCount(reinterpret_cast<GDALDatasetH>(hDS));
#endif

    return GDALDataset::FromHandle(hDS)->GetLayerCount();
}

/************************************************************************/
/*                        GDALDatasetGetLayer()                         */
/************************************************************************/

/**
 \brief Fetch a layer by index.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 This function is the same as the C++ method GDALDataset::GetLayer()

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param iLayer a layer number between 0 and GetLayerCount()-1.

 @return the layer, or NULL if iLayer is out of range or an error occurs.
*/

OGRLayerH GDALDatasetGetLayer(GDALDatasetH hDS, int iLayer)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetGetLayer", nullptr);

    OGRLayerH hLayer =
        OGRLayer::ToHandle(GDALDataset::FromHandle(hDS)->GetLayer(iLayer));

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_GetLayer(hDS, iLayer, hLayer);
#endif

    return hLayer;
}

/************************************************************************/
/*                     GDALDatasetGetLayerByName()                      */
/************************************************************************/

/**
 \brief Fetch a layer by name.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 This function is the same as the C++ method GDALDataset::GetLayerByName()

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param pszName the layer name of the layer to fetch.

 @return the layer, or NULL if Layer is not found or an error occurs.
*/

OGRLayerH GDALDatasetGetLayerByName(GDALDatasetH hDS, const char *pszName)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetGetLayerByName", nullptr);

    OGRLayerH hLayer = OGRLayer::ToHandle(
        GDALDataset::FromHandle(hDS)->GetLayerByName(pszName));

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_GetLayerByName(hDS, pszName, hLayer);
#endif

    return hLayer;
}

/************************************************************************/
/*                        GDALDatasetIsLayerPrivate()                   */
/************************************************************************/

/**
 \brief Returns true if the layer at the specified index is deemed a private or
 system table, or an internal detail only.

 This function is the same as the C++ method GDALDataset::IsLayerPrivate()

 @since GDAL 3.4

 @param hDS the dataset handle.
 @param iLayer a layer number between 0 and GetLayerCount()-1.

 @return true if the layer is a private or system table.
*/

int GDALDatasetIsLayerPrivate(GDALDatasetH hDS, int iLayer)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetIsLayerPrivate", false);

    const bool res = GDALDataset::FromHandle(hDS)->IsLayerPrivate(iLayer);

    return res ? 1 : 0;
}

/************************************************************************/
/*                            GetLayerIndex()                           */
/************************************************************************/

/**
 \brief Returns the index of the layer specified by name.

 @since GDAL 3.12

 @param pszName layer name (not NULL)

 @return an index >= 0, or -1 if not found.
*/

int GDALDataset::GetLayerIndex(const char *pszName)
{
    const int nLayerCount = GetLayerCount();
    int iMatch = -1;
    for (int i = 0; i < nLayerCount; ++i)
    {
        if (const auto poLayer = GetLayer(i))
        {
            const char *pszLayerName = poLayer->GetDescription();
            if (strcmp(pszName, pszLayerName) == 0)
            {
                iMatch = i;
                break;
            }
            else if (EQUAL(pszName, pszLayerName))
            {
                iMatch = i;
            }
        }
    }
    return iMatch;
}

/************************************************************************/
/*                        GDALDatasetDeleteLayer()                      */
/************************************************************************/

/**
 \brief Delete the indicated layer from the datasource.

 If this function is supported
 the ODsCDeleteLayer capability will test TRUE on the GDALDataset.

 This method is the same as the C++ method GDALDataset::DeleteLayer().

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param iLayer the index of the layer to delete.

 @return OGRERR_NONE on success, or OGRERR_UNSUPPORTED_OPERATION if deleting
 layers is not supported for this datasource.

*/
OGRErr GDALDatasetDeleteLayer(GDALDatasetH hDS, int iLayer)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetH", OGRERR_INVALID_HANDLE);

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_DeleteLayer(hDS, iLayer);
#endif

    return GDALDataset::FromHandle(hDS)->DeleteLayer(iLayer);
}

/************************************************************************/
/*                            CreateLayer()                             */
/************************************************************************/

/**
\brief This method attempts to create a new layer on the dataset with the
indicated name, coordinate system, geometry type.

The papszOptions argument
can be used to control driver specific creation options.  These options are
normally documented in the format specific documentation.
That function will try to validate the creation option list passed to the
driver with the GDALValidateCreationOptions() method. This check can be
disabled by defining the configuration option GDAL_VALIDATE_CREATION_OPTIONS set
to NO.

Drivers should extend the ICreateLayer() method and not
CreateLayer(). CreateLayer() adds validation of layer creation options, before
delegating the actual work to ICreateLayer().

This method is the same as the C function GDALDatasetCreateLayer() and the
deprecated OGR_DS_CreateLayer().

Example:

\code{.cpp}
#include "gdal.h"
#include "cpl_string.h"

...

        OGRLayer *poLayer;
        char     **papszOptions;

        if( !poDS->TestCapability( ODsCCreateLayer ) )
        {
        ...
        }

        papszOptions = CSLSetNameValue( papszOptions, "DIM", "2" );
        poLayer = poDS->CreateLayer( "NewLayer", nullptr, wkbUnknown,
                                     papszOptions );
        CSLDestroy( papszOptions );

        if( poLayer == NULL )
        {
            ...
        }
\endcode

@param pszName the name for the new layer.  This should ideally not
match any existing layer on the datasource.
@param poSpatialRef the coordinate system to use for the new layer, or NULL if
no coordinate system is available.
@param eGType the geometry type for the layer.  Use wkbUnknown if there
are no constraints on the types geometry to be written.
@param papszOptions a StringList of name=value options.  Options are driver
specific.

@return NULL is returned on failure, or a new OGRLayer handle on success.

*/

OGRLayer *GDALDataset::CreateLayer(const char *pszName,
                                   const OGRSpatialReference *poSpatialRef,
                                   OGRwkbGeometryType eGType,
                                   CSLConstList papszOptions)

{
    if (eGType == wkbNone)
    {
        return CreateLayer(pszName, nullptr, papszOptions);
    }
    else
    {
        OGRGeomFieldDefn oGeomFieldDefn("", eGType);
        oGeomFieldDefn.SetSpatialRef(poSpatialRef);
        return CreateLayer(pszName, &oGeomFieldDefn, papszOptions);
    }
}

/**
\brief This method attempts to create a new layer on the dataset with the
indicated name and geometry field definition.

When poGeomFieldDefn is not null, most drivers should honor
poGeomFieldDefn->GetType() and poGeomFieldDefn->GetSpatialRef().
Drivers that honor poGeomFieldDefn->GetCoordinatePrecision() will declare the
GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION capability. Drivers may honor
poGeomFieldDefn->GetNameRef() and poGeomFieldDefn->IsNullable(), but there are
very few currently.

Note that even if a geometry coordinate precision is set and a driver honors the
GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION capability, geometries passed to
OGRLayer::CreateFeature() and OGRLayer::SetFeature() are assumed to be compatible
with the coordinate precision. That is they are assumed to be valid once their
coordinates are rounded to it. If it might not be the case, the user may set
the OGR_APPLY_GEOM_SET_PRECISION configuration option before calling CreateFeature()
or SetFeature() to force the OGRGeometry::SetPrecision() method to be called on
the passed geometries.

The papszOptions argument
can be used to control driver specific creation options. These options are
normally documented in the format specific documentation.
This function will try to validate the creation option list passed to the
driver with the GDALValidateCreationOptions() method. This check can be
disabled by defining the configuration option GDAL_VALIDATE_CREATION_OPTIONS set
to NO.

Drivers should extend the ICreateLayer() method and not
CreateLayer(). CreateLayer() adds validation of layer creation options, before
delegating the actual work to ICreateLayer().

This method is the same as the C function GDALDatasetCreateLayerFromGeomFieldDefn().

@param pszName the name for the new layer.  This should ideally not
match any existing layer on the datasource.
@param poGeomFieldDefn the geometry field definition to use for the new layer,
or NULL if there is no geometry field.
@param papszOptions a StringList of name=value options.  Options are driver
specific.

@return NULL is returned on failure, or a new OGRLayer handle on success.
@since 3.9

*/

OGRLayer *GDALDataset::CreateLayer(const char *pszName,
                                   const OGRGeomFieldDefn *poGeomFieldDefn,
                                   CSLConstList papszOptions)

{
    if (CPLTestBool(
            CPLGetConfigOption("GDAL_VALIDATE_CREATION_OPTIONS", "YES")))
    {
        ValidateLayerCreationOptions(papszOptions);
    }

    OGRLayer *poLayer;
    if (poGeomFieldDefn)
    {
        OGRGeomFieldDefn oGeomFieldDefn(poGeomFieldDefn);
        if (OGR_GT_IsNonLinear(poGeomFieldDefn->GetType()) &&
            !TestCapability(ODsCCurveGeometries))
        {
            oGeomFieldDefn.SetType(
                OGR_GT_GetLinear(poGeomFieldDefn->GetType()));
        }

        poLayer = ICreateLayer(pszName, &oGeomFieldDefn, papszOptions);
    }
    else
    {
        poLayer = ICreateLayer(pszName, nullptr, papszOptions);
    }
#ifdef DEBUG
    if (poLayer != nullptr && OGR_GT_IsNonLinear(poLayer->GetGeomType()) &&
        !poLayer->TestCapability(OLCCurveGeometries))
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Inconsistent driver: Layer geometry type is non-linear, but "
                 "TestCapability(OLCCurveGeometries) returns FALSE.");
    }
#endif

    return poLayer;
}

//! @cond Doxygen_Suppress

// Technical override to avoid ambiguous choice between the old and new
// new CreateLayer() signatures.
OGRLayer *GDALDataset::CreateLayer(const char *pszName)
{
    OGRGeomFieldDefn oGeomFieldDefn("", wkbUnknown);
    return CreateLayer(pszName, &oGeomFieldDefn, nullptr);
}

// Technical override to avoid ambiguous choice between the old and new
// new CreateLayer() signatures.
OGRLayer *GDALDataset::CreateLayer(const char *pszName, std::nullptr_t)
{
    OGRGeomFieldDefn oGeomFieldDefn("", wkbUnknown);
    return CreateLayer(pszName, &oGeomFieldDefn, nullptr);
}

//!@endcond

/************************************************************************/
/*                         GDALDatasetCreateLayer()                     */
/************************************************************************/

/**
\brief This function attempts to create a new layer on the dataset with the
indicated name, coordinate system, geometry type.

The papszOptions argument can be used to control driver specific creation
options.  These options are normally documented in the format specific
documentation.

This method is the same as the C++ method GDALDataset::CreateLayer().

Example:

\code{.c}
#include "gdal.h"
#include "cpl_string.h"

...

        OGRLayerH  hLayer;
        char     **papszOptions;

        if( !GDALDatasetTestCapability( hDS, ODsCCreateLayer ) )
        {
        ...
        }

        papszOptions = CSLSetNameValue( papszOptions, "DIM", "2" );
        hLayer = GDALDatasetCreateLayer( hDS, "NewLayer", NULL, wkbUnknown,
                                         papszOptions );
        CSLDestroy( papszOptions );

        if( hLayer == NULL )
        {
            ...
        }
\endcode

@since GDAL 2.0

@param hDS the dataset handle
@param pszName the name for the new layer.  This should ideally not
match any existing layer on the datasource.
@param hSpatialRef the coordinate system to use for the new layer, or NULL if
no coordinate system is available.
@param eGType the geometry type for the layer.  Use wkbUnknown if there
are no constraints on the types geometry to be written.
@param papszOptions a StringList of name=value options.  Options are driver
specific.

@return NULL is returned on failure, or a new OGRLayer handle on success.

*/

OGRLayerH GDALDatasetCreateLayer(GDALDatasetH hDS, const char *pszName,
                                 OGRSpatialReferenceH hSpatialRef,
                                 OGRwkbGeometryType eGType,
                                 CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetCreateLayer", nullptr);

    if (pszName == nullptr)
    {
        CPLError(CE_Failure, CPLE_ObjectNull,
                 "Name was NULL in GDALDatasetCreateLayer");
        return nullptr;
    }

    OGRLayerH hLayer =
        OGRLayer::ToHandle(GDALDataset::FromHandle(hDS)->CreateLayer(
            pszName, OGRSpatialReference::FromHandle(hSpatialRef), eGType,
            const_cast<char **>(papszOptions)));

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_CreateLayer(hDS, pszName, hSpatialRef, eGType,
                                 const_cast<char **>(papszOptions), hLayer);
#endif

    return hLayer;
}

/************************************************************************/
/*                 GDALDatasetCreateLayerFromGeomFieldDefn()            */
/************************************************************************/

/**
\brief This function attempts to create a new layer on the dataset with the
indicated name and geometry field.

When poGeomFieldDefn is not null, most drivers should honor
poGeomFieldDefn->GetType() and poGeomFieldDefn->GetSpatialRef().
Drivers that honor poGeomFieldDefn->GetCoordinatePrecision() will declare the
GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION capability. Drivers may honor
poGeomFieldDefn->GetNameRef() and poGeomFieldDefn->IsNullable(), but there are
very few currently.

Note that even if a geometry coordinate precision is set and a driver honors the
GDAL_DCAP_HONOR_GEOM_COORDINATE_PRECISION capability, geometries passed to
OGRLayer::CreateFeature() and OGRLayer::SetFeature() are assumed to be compatible
with the coordinate precision. That is they are assumed to be valid once their
coordinates are rounded to it. If it might not be the case, the user may set
the OGR_APPLY_GEOM_SET_PRECISION configuration option before calling CreateFeature()
or SetFeature() to force the OGRGeometry::SetPrecision() method to be called on
the passed geometries.

The papszOptions argument can be used to control driver specific creation
options.  These options are normally documented in the format specific
documentation.

This method is the same as the C++ method GDALDataset::CreateLayer().

@param hDS the dataset handle
@param pszName the name for the new layer.  This should ideally not
match any existing layer on the datasource.
@param hGeomFieldDefn the geometry field definition. May be NULL to indicate
a non-spatial file (or if adding geometry fields later with OGR_L_CreateGeomField()
for drivers supporting that interface).
@param papszOptions a StringList of name=value options.  Options are driver
specific.

@return NULL is returned on failure, or a new OGRLayer handle on success.

@since GDAL 3.9

*/

OGRLayerH
GDALDatasetCreateLayerFromGeomFieldDefn(GDALDatasetH hDS, const char *pszName,
                                        OGRGeomFieldDefnH hGeomFieldDefn,
                                        CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetCreateLayerFromGeomFieldDefn", nullptr);

    if (!pszName)
    {
        CPLError(CE_Failure, CPLE_ObjectNull,
                 "Name was NULL in GDALDatasetCreateLayerFromGeomFieldDefn");
        return nullptr;
    }

    OGRLayerH hLayer =
        OGRLayer::ToHandle(GDALDataset::FromHandle(hDS)->CreateLayer(
            pszName, OGRGeomFieldDefn::FromHandle(hGeomFieldDefn),
            papszOptions));
    return hLayer;
}

/************************************************************************/
/*                         GDALDatasetCopyLayer()                       */
/************************************************************************/

/**
 \brief Duplicate an existing layer.

 This function creates a new layer, duplicate the field definitions of the
 source layer and then duplicate each features of the source layer.
 The papszOptions argument
 can be used to control driver specific creation options.  These options are
 normally documented in the format specific documentation.
 The source layer may come from another dataset.

 This method is the same as the C++ method GDALDataset::CopyLayer()

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param hSrcLayer source layer.
 @param pszNewName the name of the layer to create.
 @param papszOptions a StringList of name=value options.  Options are driver
                     specific.

 @return a handle to the layer, or NULL if an error occurs.
*/
OGRLayerH GDALDatasetCopyLayer(GDALDatasetH hDS, OGRLayerH hSrcLayer,
                               const char *pszNewName,
                               CSLConstList papszOptions)

{
    VALIDATE_POINTER1(hDS, "OGR_DS_CopyGDALDatasetCopyLayerLayer", nullptr);
    VALIDATE_POINTER1(hSrcLayer, "GDALDatasetCopyLayer", nullptr);
    VALIDATE_POINTER1(pszNewName, "GDALDatasetCopyLayer", nullptr);

    return OGRLayer::ToHandle(GDALDataset::FromHandle(hDS)->CopyLayer(
        OGRLayer::FromHandle(hSrcLayer), pszNewName,
        const_cast<char **>(papszOptions)));
}

/************************************************************************/
/*                        GDALDatasetExecuteSQL()                       */
/************************************************************************/

/**
 \brief Execute an SQL statement against the data store.

 The result of an SQL query is either NULL for statements that are in error,
 or that have no results set, or an OGRLayer pointer representing a results
 set from the query.  Note that this OGRLayer is in addition to the layers
 in the data store and must be destroyed with
 ReleaseResultSet() before the dataset is closed
 (destroyed).

 This method is the same as the C++ method GDALDataset::ExecuteSQL()

 For more information on the SQL dialect supported internally by OGR
 review the <a href="https://gdal.org/user/ogr_sql_dialect.html">OGR SQL</a>
 document.  Some drivers (i.e. Oracle and PostGIS) pass the SQL directly through
 to the underlying RDBMS.

 Starting with OGR 1.10, the <a
 href="https://gdal.org/user/sql_sqlite_dialect.html">SQLITE dialect</a> can
 also be used.

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param pszStatement the SQL statement to execute.
 @param hSpatialFilter geometry which represents a spatial filter. Can be NULL.

 @param pszDialect allows control of the statement dialect. If set to NULL, the
 OGR SQL engine will be used, except for RDBMS drivers that will use their
 dedicated SQL engine, unless OGRSQL is explicitly passed as the
 dialect. Starting with OGR 1.10, the SQLITE dialect can also be used.

 @return an OGRLayer containing the results of the query.  Deallocate with
 GDALDatasetReleaseResultSet().

*/

OGRLayerH GDALDatasetExecuteSQL(GDALDatasetH hDS, const char *pszStatement,
                                OGRGeometryH hSpatialFilter,
                                const char *pszDialect)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetExecuteSQL", nullptr);

    OGRLayerH hLayer =
        OGRLayer::ToHandle(GDALDataset::FromHandle(hDS)->ExecuteSQL(
            pszStatement, OGRGeometry::FromHandle(hSpatialFilter), pszDialect));

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_DS_ExecuteSQL(hDS, pszStatement, hSpatialFilter, pszDialect,
                                hLayer);
#endif

    return hLayer;
}

/************************************************************************/
/*                        GDALDatasetAbortSQL()                         */
/************************************************************************/

/**
 \brief Abort any SQL statement running in the data store.

 This function can be safely called from any thread (pending that the dataset
 object is still alive). Driver implementations will make sure that it can be
 called in a thread-safe way.

 This might not be implemented by all drivers. At time of writing, only SQLite,
 GPKG and PG drivers implement it

 This method is the same as the C++ method GDALDataset::AbortSQL()

 @since GDAL 3.2.0

 @param hDS the dataset handle.

 @return OGRERR_NONE on success, or OGRERR_UNSUPPORTED_OPERATION if AbortSQL
 is not supported for this datasource. .

*/

OGRErr GDALDatasetAbortSQL(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetAbortSQL", OGRERR_FAILURE);
    return GDALDataset::FromHandle(hDS)->AbortSQL();
}

/************************************************************************/
/*                      GDALDatasetGetStyleTable()                      */
/************************************************************************/

/**
 \brief Returns dataset style table.

 This function is the same as the C++ method GDALDataset::GetStyleTable()

 @since GDAL 2.0

 @param hDS the dataset handle
 @return handle to a style table which should not be modified or freed by the
 caller.
*/

OGRStyleTableH GDALDatasetGetStyleTable(GDALDatasetH hDS)

{
    VALIDATE_POINTER1(hDS, "OGR_DS_GetStyleTable", nullptr);

    return reinterpret_cast<OGRStyleTableH>(
        GDALDataset::FromHandle(hDS)->GetStyleTable());
}

/************************************************************************/
/*                    GDALDatasetSetStyleTableDirectly()                */
/************************************************************************/

/**
 \brief Set dataset style table.

 This function operate exactly as GDALDatasetSetStyleTable() except that it
 assumes ownership of the passed table.

 This function is the same as the C++ method
 GDALDataset::SetStyleTableDirectly()

 @since GDAL 2.0

 @param hDS the dataset handle
 @param hStyleTable style table handle to set

*/

void GDALDatasetSetStyleTableDirectly(GDALDatasetH hDS,
                                      OGRStyleTableH hStyleTable)

{
    VALIDATE_POINTER0(hDS, "OGR_DS_SetStyleTableDirectly");

    GDALDataset::FromHandle(hDS)->SetStyleTableDirectly(
        reinterpret_cast<OGRStyleTable *>(hStyleTable));
}

/************************************************************************/
/*                     GDALDatasetSetStyleTable()                       */
/************************************************************************/

/**
 \brief Set dataset style table.

 This function operate exactly as GDALDatasetSetStyleTableDirectly() except that
 it assumes ownership of the passed table.

 This function is the same as the C++ method GDALDataset::SetStyleTable()

 @since GDAL 2.0

 @param hDS the dataset handle
 @param hStyleTable style table handle to set

*/

void GDALDatasetSetStyleTable(GDALDatasetH hDS, OGRStyleTableH hStyleTable)

{
    VALIDATE_POINTER0(hDS, "OGR_DS_SetStyleTable");
    VALIDATE_POINTER0(hStyleTable, "OGR_DS_SetStyleTable");

    GDALDataset::FromHandle(hDS)->SetStyleTable(
        reinterpret_cast<OGRStyleTable *>(hStyleTable));
}

/************************************************************************/
/*                    ValidateLayerCreationOptions()                    */
/************************************************************************/

//! @cond Doxygen_Suppress
int GDALDataset::ValidateLayerCreationOptions(const char *const *papszLCO)
{
    const char *pszOptionList =
        GetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST);
    if (pszOptionList == nullptr && poDriver != nullptr)
    {
        pszOptionList =
            poDriver->GetMetadataItem(GDAL_DS_LAYER_CREATIONOPTIONLIST);
    }
    CPLString osDataset;
    osDataset.Printf("dataset %s", GetDescription());
    return GDALValidateOptions(pszOptionList, papszLCO, "layer creation option",
                               osDataset);
}

//! @endcond

/************************************************************************/
/*                              Release()                               */
/************************************************************************/

/**
\brief Drop a reference to this dataset, and if the reference count drops to one
close (destroy) the dataset.

This method is the same as the C function OGRReleaseDataSource().

@deprecated. In GDAL 2, use GDALClose() instead

@return OGRERR_NONE on success or an error code.
*/

OGRErr GDALDataset::Release()

{
    ReleaseRef();
    return OGRERR_NONE;
}

/************************************************************************/
/*                            GetRefCount()                             */
/************************************************************************/

/**
\brief Fetch reference count.

This method is the same as the C function OGR_DS_GetRefCount().

In GDAL 1.X, this method used to be in the OGRDataSource class.

@return the current reference count for the datasource object itself.
*/

int GDALDataset::GetRefCount() const
{
    return nRefCount;
}

/************************************************************************/
/*                         GetSummaryRefCount()                         */
/************************************************************************/

/**
\brief Fetch reference count of datasource and all owned layers.

This method is the same as the C function  OGR_DS_GetSummaryRefCount().

In GDAL 1.X, this method used to be in the OGRDataSource class.

@deprecated

@return the current summary reference count for the datasource and its layers.
*/

int GDALDataset::GetSummaryRefCount() const

{
    CPLMutexHolderD(m_poPrivate ? &(m_poPrivate->hMutex) : nullptr);
    int nSummaryCount = nRefCount;
    GDALDataset *poUseThis = const_cast<GDALDataset *>(this);

    for (int iLayer = 0; iLayer < poUseThis->GetLayerCount(); ++iLayer)
        nSummaryCount += poUseThis->GetLayer(iLayer)->GetRefCount();

    return nSummaryCount;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

/**
 \brief This method attempts to create a new layer on the dataset with the
 indicated name, coordinate system, geometry type.

 This method is reserved to implementation by drivers.

 The papszOptions argument can be used to control driver specific creation
 options.  These options are normally documented in the format specific
 documentation.

 @param pszName the name for the new layer.  This should ideally not
 match any existing layer on the datasource.
 @param poGeomFieldDefn the geometry field definition to use for the new layer,
 or NULL if there is no geometry field.
 @param papszOptions a StringList of name=value options.  Options are driver
 specific.

 @return NULL is returned on failure, or a new OGRLayer handle on success.

 @since GDAL 2.0 (prototype modified in 3.9)
*/

OGRLayer *
GDALDataset::ICreateLayer(CPL_UNUSED const char *pszName,
                          CPL_UNUSED const OGRGeomFieldDefn *poGeomFieldDefn,
                          CPL_UNUSED CSLConstList papszOptions)

{
    CPLError(CE_Failure, CPLE_NotSupported,
             "CreateLayer() not supported by this dataset.");

    return nullptr;
}

/************************************************************************/
/*                             CopyLayer()                              */
/************************************************************************/

/**
 \brief Duplicate an existing layer.

 This method creates a new layer, duplicate the field definitions of the
 source layer and then duplicate each features of the source layer.
 The papszOptions argument
 can be used to control driver specific creation options.  These options are
 normally documented in the format specific documentation.
 The source layer may come from another dataset.

 This method is the same as the C function GDALDatasetCopyLayer() and the
 deprecated OGR_DS_CopyLayer().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param poSrcLayer source layer.
 @param pszNewName the name of the layer to create.
 @param papszOptions a StringList of name=value options.  Options are driver
                     specific. There is a common option to set output layer
                     spatial reference: DST_SRSWKT. The option should be in
                     WKT format. Starting with GDAL 3.7, the common option
                     COPY_MD can be set to NO to prevent the default copying
                     of the metadata from the source layer to the target layer.

 @return a handle to the layer, or NULL if an error occurs.
*/

OGRLayer *GDALDataset::CopyLayer(OGRLayer *poSrcLayer, const char *pszNewName,
                                 char **papszOptions)

{
    /* -------------------------------------------------------------------- */
    /*      Create the layer.                                               */
    /* -------------------------------------------------------------------- */
    if (!TestCapability(ODsCCreateLayer))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "This datasource does not support creation of layers.");
        return nullptr;
    }

    const char *pszSRSWKT = CSLFetchNameValue(papszOptions, "DST_SRSWKT");
    OGRSpatialReference oDstSpaRef(pszSRSWKT);
    oDstSpaRef.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRFeatureDefn *poSrcDefn = poSrcLayer->GetLayerDefn();
    OGRLayer *poDstLayer = nullptr;

    CPLStringList aosCleanedUpOptions(CSLDuplicate(papszOptions));
    aosCleanedUpOptions.SetNameValue("DST_SRSWKT", nullptr);
    aosCleanedUpOptions.SetNameValue("COPY_MD", nullptr);

    CPLErrorReset();
    const int nSrcGeomFieldCount = poSrcDefn->GetGeomFieldCount();
    if (nSrcGeomFieldCount == 1)
    {
        OGRGeomFieldDefn oGeomFieldDefn(poSrcDefn->GetGeomFieldDefn(0));
        if (pszSRSWKT)
            oGeomFieldDefn.SetSpatialRef(&oDstSpaRef);
        poDstLayer = ICreateLayer(pszNewName, &oGeomFieldDefn,
                                  aosCleanedUpOptions.List());
    }
    else
    {
        poDstLayer =
            ICreateLayer(pszNewName, nullptr, aosCleanedUpOptions.List());
    }

    if (poDstLayer == nullptr)
        return nullptr;

    if (CPLTestBool(CSLFetchNameValueDef(papszOptions, "COPY_MD", "YES")))
    {
        char **papszMD = poSrcLayer->GetMetadata();
        if (papszMD)
            poDstLayer->SetMetadata(papszMD);
    }

    /* -------------------------------------------------------------------- */
    /*      Add fields.  Default to copy all fields, and make sure to       */
    /*      establish a mapping between indices, rather than names, in      */
    /*      case the target datasource has altered it (e.g. Shapefile       */
    /*      limited to 10 char field names).                                */
    /* -------------------------------------------------------------------- */
    const int nSrcFieldCount = poSrcDefn->GetFieldCount();

    // Initialize the index-to-index map to -1's.
    std::vector<int> anMap(nSrcFieldCount, -1);

    // Caution: At the time of writing, the MapInfo driver
    // returns NULL until a field has been added.
    OGRFeatureDefn *poDstFDefn = poDstLayer->GetLayerDefn();
    int nDstFieldCount = poDstFDefn ? poDstFDefn->GetFieldCount() : 0;
    for (int iField = 0; iField < nSrcFieldCount; ++iField)
    {
        OGRFieldDefn *poSrcFieldDefn = poSrcDefn->GetFieldDefn(iField);
        OGRFieldDefn oFieldDefn(poSrcFieldDefn);

        // The field may have been already created at layer creation.
        int iDstField = -1;
        if (poDstFDefn)
            iDstField = poDstFDefn->GetFieldIndex(oFieldDefn.GetNameRef());
        if (iDstField >= 0)
        {
            anMap[iField] = iDstField;
        }
        else if (poDstLayer->CreateField(&oFieldDefn) == OGRERR_NONE)
        {
            // Now that we've created a field, GetLayerDefn() won't return NULL.
            if (poDstFDefn == nullptr)
                poDstFDefn = poDstLayer->GetLayerDefn();

            // Sanity check: if it fails, the driver is buggy.
            if (poDstFDefn != nullptr &&
                poDstFDefn->GetFieldCount() != nDstFieldCount + 1)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "The output driver has claimed to have added the %s "
                         "field, but it did not!",
                         oFieldDefn.GetNameRef());
            }
            else
            {
                anMap[iField] = nDstFieldCount;
                ++nDstFieldCount;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    std::unique_ptr<OGRCoordinateTransformation> poCT;
    OGRSpatialReference *sourceSRS = poSrcLayer->GetSpatialRef();
    if (sourceSRS != nullptr && pszSRSWKT != nullptr && !oDstSpaRef.IsEmpty() &&
        sourceSRS->IsSame(&oDstSpaRef) == FALSE)
    {
        poCT.reset(OGRCreateCoordinateTransformation(sourceSRS, &oDstSpaRef));
        if (nullptr == poCT)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "This input/output spatial reference is not supported.");
            return nullptr;
        }
    }
    /* -------------------------------------------------------------------- */
    /*      Create geometry fields.                                         */
    /* -------------------------------------------------------------------- */
    if (nSrcGeomFieldCount > 1 &&
        TestCapability(ODsCCreateGeomFieldAfterCreateLayer))
    {

        for (int iField = 0; iField < nSrcGeomFieldCount; ++iField)
        {
            if (nullptr == pszSRSWKT)
            {
                poDstLayer->CreateGeomField(
                    poSrcDefn->GetGeomFieldDefn(iField));
            }
            else
            {
                OGRGeomFieldDefn *pDstGeomFieldDefn =
                    poSrcDefn->GetGeomFieldDefn(iField);
                pDstGeomFieldDefn->SetSpatialRef(&oDstSpaRef);
                poDstLayer->CreateGeomField(pDstGeomFieldDefn);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Check if the destination layer supports transactions and set a  */
    /*      default number of features in a single transaction.             */
    /* -------------------------------------------------------------------- */
    const int nGroupTransactions =
        poDstLayer->TestCapability(OLCTransactions) ? 128 : 0;

    /* -------------------------------------------------------------------- */
    /*      Transfer features.                                              */
    /* -------------------------------------------------------------------- */
    poSrcLayer->ResetReading();

    if (nGroupTransactions <= 0)
    {
        while (true)
        {
            auto poFeature =
                std::unique_ptr<OGRFeature>(poSrcLayer->GetNextFeature());

            if (poFeature == nullptr)
                break;

            CPLErrorReset();
            auto poDstFeature =
                std::make_unique<OGRFeature>(poDstLayer->GetLayerDefn());

            if (poDstFeature->SetFrom(poFeature.get(), anMap.data(), TRUE) !=
                OGRERR_NONE)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to translate feature " CPL_FRMT_GIB
                         " from layer %s.",
                         poFeature->GetFID(), poSrcDefn->GetName());
                return poDstLayer;
            }

            if (nullptr != poCT)
            {
                for (int iField = 0; iField < nSrcGeomFieldCount; ++iField)
                {
                    OGRGeometry *pGeom = poDstFeature->GetGeomFieldRef(iField);
                    if (nullptr == pGeom)
                        continue;

                    const OGRErr eErr = pGeom->transform(poCT.get());
                    if (eErr == OGRERR_NONE)
                        continue;

                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to transform geometry " CPL_FRMT_GIB
                             " from layer %s.",
                             poFeature->GetFID(), poSrcDefn->GetName());
                    return poDstLayer;
                }
            }

            poDstFeature->SetFID(poFeature->GetFID());

            CPLErrorReset();
            if (poDstLayer->CreateFeature(poDstFeature.get()) != OGRERR_NONE)
            {
                return poDstLayer;
            }
        }
    }
    else
    {
        std::vector<std::unique_ptr<OGRFeature>> apoDstFeatures;
        try
        {
            apoDstFeatures.resize(nGroupTransactions);
        }
        catch (const std::exception &e)
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "%s", e.what());
            return poDstLayer;
        }
        bool bStopTransfer = false;
        while (!bStopTransfer)
        {
            /* --------------------------------------------------------------------
             */
            /*      Fill the array with features. */
            /* --------------------------------------------------------------------
             */
            // Number of features in the temporary array.
            int nFeatCount = 0;  // Used after for.
            for (nFeatCount = 0; nFeatCount < nGroupTransactions; ++nFeatCount)
            {
                auto poFeature =
                    std::unique_ptr<OGRFeature>(poSrcLayer->GetNextFeature());

                if (poFeature == nullptr)
                {
                    bStopTransfer = true;
                    break;
                }

                CPLErrorReset();
                apoDstFeatures[nFeatCount] =
                    std::make_unique<OGRFeature>(poDstLayer->GetLayerDefn());

                if (apoDstFeatures[nFeatCount]->SetFrom(
                        poFeature.get(), anMap.data(), TRUE) != OGRERR_NONE)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to translate feature " CPL_FRMT_GIB
                             " from layer %s.",
                             poFeature->GetFID(), poSrcDefn->GetName());
                    bStopTransfer = true;
                    poFeature.reset();
                    break;
                }

                if (nullptr != poCT)
                {
                    for (int iField = 0; iField < nSrcGeomFieldCount; ++iField)
                    {
                        OGRGeometry *pGeom =
                            apoDstFeatures[nFeatCount]->GetGeomFieldRef(iField);
                        if (nullptr == pGeom)
                            continue;

                        const OGRErr eErr = pGeom->transform(poCT.get());
                        if (eErr == OGRERR_NONE)
                            continue;

                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Unable to transform geometry " CPL_FRMT_GIB
                                 " from layer %s.",
                                 poFeature->GetFID(), poSrcDefn->GetName());
                        bStopTransfer = true;
                        poFeature.reset();
                        break;
                    }
                }

                if (poFeature)
                {
                    apoDstFeatures[nFeatCount]->SetFID(poFeature->GetFID());
                }
            }

            CPLErrorReset();
            bool bStopTransaction = false;
            while (!bStopTransaction)
            {
                bStopTransaction = true;
                if (poDstLayer->StartTransaction() != OGRERR_NONE)
                    break;
                for (int i = 0; i < nFeatCount; ++i)
                {
                    if (poDstLayer->CreateFeature(apoDstFeatures[i].get()) !=
                        OGRERR_NONE)
                    {
                        bStopTransfer = true;
                        bStopTransaction = false;
                        break;
                    }
                    apoDstFeatures[i].reset();
                }
                if (bStopTransaction)
                {
                    if (poDstLayer->CommitTransaction() != OGRERR_NONE)
                        break;
                }
                else
                {
                    poDstLayer->RollbackTransaction();
                }
            }
        }
    }

    return poDstLayer;
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

/**
 \fn GDALDataset::DeleteLayer(int)
 \brief Delete the indicated layer from the datasource.

 If this method is supported
 the ODsCDeleteLayer capability will test TRUE on the GDALDataset.

 This method is the same as the C function GDALDatasetDeleteLayer() and the
 deprecated OGR_DS_DeleteLayer().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param iLayer the index of the layer to delete.

 @return OGRERR_NONE on success, or OGRERR_UNSUPPORTED_OPERATION if deleting
 layers is not supported for this datasource.

*/

OGRErr GDALDataset::DeleteLayer(CPL_UNUSED int iLayer)

{
    CPLError(CE_Failure, CPLE_NotSupported,
             "DeleteLayer() not supported by this dataset.");

    return OGRERR_UNSUPPORTED_OPERATION;
}

/************************************************************************/
/*                           GetLayerByName()                           */
/************************************************************************/

/**
 \brief Fetch a layer by name.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 This method is the same as the C function GDALDatasetGetLayerByName() and the
 deprecated OGR_DS_GetLayerByName().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param pszName the layer name of the layer to fetch.

 @return the layer, or NULL if Layer is not found or an error occurs.
*/

OGRLayer *GDALDataset::GetLayerByName(const char *pszName)

{
    CPLMutexHolderD(m_poPrivate ? &(m_poPrivate->hMutex) : nullptr);

    if (!pszName)
        return nullptr;

    // First a case sensitive check.
    for (int i = 0; i < GetLayerCount(); ++i)
    {
        OGRLayer *poLayer = GetLayer(i);

        if (strcmp(pszName, poLayer->GetName()) == 0)
            return poLayer;
    }

    // Then case insensitive.
    for (int i = 0; i < GetLayerCount(); ++i)
    {
        OGRLayer *poLayer = GetLayer(i);

        if (EQUAL(pszName, poLayer->GetName()))
            return poLayer;
    }

    return nullptr;
}

//! @cond Doxygen_Suppress
/************************************************************************/
/*                       ProcessSQLCreateIndex()                        */
/*                                                                      */
/*      The correct syntax for creating an index in our dialect of      */
/*      SQL is:                                                         */
/*                                                                      */
/*        CREATE INDEX ON <layername> USING <columnname>                */
/************************************************************************/

OGRErr GDALDataset::ProcessSQLCreateIndex(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    if (CSLCount(papszTokens) != 6 || !EQUAL(papszTokens[0], "CREATE") ||
        !EQUAL(papszTokens[1], "INDEX") || !EQUAL(papszTokens[2], "ON") ||
        !EQUAL(papszTokens[4], "USING"))
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in CREATE INDEX command.\n"
                 "Was '%s'\n"
                 "Should be of form 'CREATE INDEX ON <table> USING <field>'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = GetLayerByName(papszTokens[3]);
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CREATE INDEX ON failed, no such layer as `%s'.",
                 papszTokens[3]);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Does this layer even support attribute indexes?                 */
    /* -------------------------------------------------------------------- */
    if (poLayer->GetIndex() == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "CREATE INDEX ON not supported by this driver.");
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named field.                                           */
    /* -------------------------------------------------------------------- */
    int i = poLayer->GetLayerDefn()->GetFieldIndex(papszTokens[5]);

    CSLDestroy(papszTokens);

    if (i >= poLayer->GetLayerDefn()->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "`%s' failed, field not found.",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Attempt to create the index.                                    */
    /* -------------------------------------------------------------------- */
    OGRErr eErr = poLayer->GetIndex()->CreateIndex(i);
    if (eErr == OGRERR_NONE)
    {
        eErr = poLayer->GetIndex()->IndexAllFeatures(i);
    }
    else
    {
        if (strlen(CPLGetLastErrorMsg()) == 0)
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot '%s'", pszSQLCommand);
    }

    return eErr;
}

/************************************************************************/
/*                        ProcessSQLDropIndex()                         */
/*                                                                      */
/*      The correct syntax for dropping one or more indexes in          */
/*      the OGR SQL dialect is:                                         */
/*                                                                      */
/*          DROP INDEX ON <layername> [USING <columnname>]              */
/************************************************************************/

OGRErr GDALDataset::ProcessSQLDropIndex(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    if ((CSLCount(papszTokens) != 4 && CSLCount(papszTokens) != 6) ||
        !EQUAL(papszTokens[0], "DROP") || !EQUAL(papszTokens[1], "INDEX") ||
        !EQUAL(papszTokens[2], "ON") ||
        (CSLCount(papszTokens) == 6 && !EQUAL(papszTokens[4], "USING")))
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in DROP INDEX command.\n"
                 "Was '%s'\n"
                 "Should be of form 'DROP INDEX ON <table> [USING <field>]'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = GetLayerByName(papszTokens[3]);
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DROP INDEX ON failed, no such layer as `%s'.",
                 papszTokens[3]);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Does this layer even support attribute indexes?                 */
    /* -------------------------------------------------------------------- */
    if (poLayer->GetIndex() == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Indexes not supported by this driver.");
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      If we were not given a field name, drop all indexes.            */
    /* -------------------------------------------------------------------- */
    if (CSLCount(papszTokens) == 4)
    {
        for (int i = 0; i < poLayer->GetLayerDefn()->GetFieldCount(); ++i)
        {
            OGRAttrIndex *poAttrIndex;

            poAttrIndex = poLayer->GetIndex()->GetFieldIndex(i);
            if (poAttrIndex != nullptr)
            {
                const OGRErr eErr = poLayer->GetIndex()->DropIndex(i);
                if (eErr != OGRERR_NONE)
                {
                    CSLDestroy(papszTokens);
                    return eErr;
                }
            }
        }

        CSLDestroy(papszTokens);
        return OGRERR_NONE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named field.                                           */
    /* -------------------------------------------------------------------- */
    int i = poLayer->GetLayerDefn()->GetFieldIndex(papszTokens[5]);
    CSLDestroy(papszTokens);

    if (i >= poLayer->GetLayerDefn()->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "`%s' failed, field not found.",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Attempt to drop the index.                                      */
    /* -------------------------------------------------------------------- */
    const OGRErr eErr = poLayer->GetIndex()->DropIndex(i);

    return eErr;
}

/************************************************************************/
/*                        ProcessSQLDropTable()                         */
/*                                                                      */
/*      The correct syntax for dropping a table (layer) in the OGR SQL  */
/*      dialect is:                                                     */
/*                                                                      */
/*          DROP TABLE <layername>                                      */
/************************************************************************/

OGRErr GDALDataset::ProcessSQLDropTable(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    if (CSLCount(papszTokens) != 3 || !EQUAL(papszTokens[0], "DROP") ||
        !EQUAL(papszTokens[1], "TABLE"))
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in DROP TABLE command.\n"
                 "Was '%s'\n"
                 "Should be of form 'DROP TABLE <table>'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = nullptr;

    int i = 0;  // Used after for.
    for (; i < GetLayerCount(); ++i)
    {
        poLayer = GetLayer(i);

        if (poLayer != nullptr && EQUAL(poLayer->GetName(), papszTokens[2]))
            break;
        poLayer = nullptr;
    }

    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "DROP TABLE failed, no such layer as `%s'.", papszTokens[2]);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    CSLDestroy(papszTokens);

    /* -------------------------------------------------------------------- */
    /*      Delete it.                                                      */
    /* -------------------------------------------------------------------- */

    return DeleteLayer(i);
}

//! @endcond

/************************************************************************/
/*                    GDALDatasetParseSQLType()                       */
/************************************************************************/

/* All arguments will be altered */
static OGRFieldType GDALDatasetParseSQLType(char *pszType, int &nWidth,
                                            int &nPrecision)
{
    char *pszParenthesis = strchr(pszType, '(');
    if (pszParenthesis)
    {
        nWidth = atoi(pszParenthesis + 1);
        *pszParenthesis = '\0';
        char *pszComma = strchr(pszParenthesis + 1, ',');
        if (pszComma)
            nPrecision = atoi(pszComma + 1);
    }

    OGRFieldType eType = OFTString;
    if (EQUAL(pszType, "INTEGER"))
        eType = OFTInteger;
    else if (EQUAL(pszType, "INTEGER[]"))
        eType = OFTIntegerList;
    else if (EQUAL(pszType, "FLOAT") || EQUAL(pszType, "NUMERIC") ||
             EQUAL(pszType, "DOUBLE") /* unofficial alias */ ||
             EQUAL(pszType, "REAL") /* unofficial alias */)
        eType = OFTReal;
    else if (EQUAL(pszType, "FLOAT[]") || EQUAL(pszType, "NUMERIC[]") ||
             EQUAL(pszType, "DOUBLE[]") /* unofficial alias */ ||
             EQUAL(pszType, "REAL[]") /* unofficial alias */)
        eType = OFTRealList;
    else if (EQUAL(pszType, "CHARACTER") ||
             EQUAL(pszType, "TEXT") /* unofficial alias */ ||
             EQUAL(pszType, "STRING") /* unofficial alias */ ||
             EQUAL(pszType, "VARCHAR") /* unofficial alias */)
        eType = OFTString;
    else if (EQUAL(pszType, "TEXT[]") ||
             EQUAL(pszType, "STRING[]") /* unofficial alias */ ||
             EQUAL(pszType, "VARCHAR[]") /* unofficial alias */)
        eType = OFTStringList;
    else if (EQUAL(pszType, "DATE"))
        eType = OFTDate;
    else if (EQUAL(pszType, "TIME"))
        eType = OFTTime;
    else if (EQUAL(pszType, "TIMESTAMP") ||
             EQUAL(pszType, "DATETIME") /* unofficial alias */)
        eType = OFTDateTime;
    else
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Unsupported column type '%s'. Defaulting to VARCHAR",
                 pszType);

    return eType;
}

/************************************************************************/
/*                    ProcessSQLAlterTableAddColumn()                   */
/*                                                                      */
/*      The correct syntax for adding a column in the OGR SQL           */
/*      dialect is:                                                     */
/*                                                                      */
/*       ALTER TABLE <layername> ADD [COLUMN] <columnname> <columntype> */
/************************************************************************/

//! @cond Doxygen_Suppress
OGRErr GDALDataset::ProcessSQLAlterTableAddColumn(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    const char *pszLayerName = nullptr;
    const char *pszColumnName = nullptr;
    int iTypeIndex = 0;
    const int nTokens = CSLCount(papszTokens);

    if (nTokens >= 7 && EQUAL(papszTokens[0], "ALTER") &&
        EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "ADD") &&
        EQUAL(papszTokens[4], "COLUMN"))
    {
        pszLayerName = papszTokens[2];
        pszColumnName = papszTokens[5];
        iTypeIndex = 6;
    }
    else if (nTokens >= 6 && EQUAL(papszTokens[0], "ALTER") &&
             EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "ADD"))
    {
        pszLayerName = papszTokens[2];
        pszColumnName = papszTokens[4];
        iTypeIndex = 5;
    }
    else
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in ALTER TABLE ADD COLUMN command.\n"
                 "Was '%s'\n"
                 "Should be of form 'ALTER TABLE <layername> ADD [COLUMN] "
                 "<columnname> <columntype>'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Merge type components into a single string if there were split  */
    /*      with spaces                                                     */
    /* -------------------------------------------------------------------- */
    CPLString osType;
    for (int i = iTypeIndex; i < nTokens; ++i)
    {
        osType += papszTokens[i];
        CPLFree(papszTokens[i]);
    }
    char *pszType = papszTokens[iTypeIndex] = CPLStrdup(osType);
    papszTokens[iTypeIndex + 1] = nullptr;

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = GetLayerByName(pszLayerName);
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such layer as `%s'.", pszSQLCommand,
                 pszLayerName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Add column.                                                     */
    /* -------------------------------------------------------------------- */

    int nWidth = 0;
    int nPrecision = 0;
    OGRFieldType eType = GDALDatasetParseSQLType(pszType, nWidth, nPrecision);
    OGRFieldDefn oFieldDefn(pszColumnName, eType);
    oFieldDefn.SetWidth(nWidth);
    oFieldDefn.SetPrecision(nPrecision);

    CSLDestroy(papszTokens);

    return poLayer->CreateField(&oFieldDefn);
}

/************************************************************************/
/*                    ProcessSQLAlterTableDropColumn()                  */
/*                                                                      */
/*      The correct syntax for dropping a column in the OGR SQL         */
/*      dialect is:                                                     */
/*                                                                      */
/*          ALTER TABLE <layername> DROP [COLUMN] <columnname>          */
/************************************************************************/

OGRErr GDALDataset::ProcessSQLAlterTableDropColumn(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    const char *pszLayerName = nullptr;
    const char *pszColumnName = nullptr;
    if (CSLCount(papszTokens) == 6 && EQUAL(papszTokens[0], "ALTER") &&
        EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "DROP") &&
        EQUAL(papszTokens[4], "COLUMN"))
    {
        pszLayerName = papszTokens[2];
        pszColumnName = papszTokens[5];
    }
    else if (CSLCount(papszTokens) == 5 && EQUAL(papszTokens[0], "ALTER") &&
             EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "DROP"))
    {
        pszLayerName = papszTokens[2];
        pszColumnName = papszTokens[4];
    }
    else
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in ALTER TABLE DROP COLUMN command.\n"
                 "Was '%s'\n"
                 "Should be of form 'ALTER TABLE <layername> DROP [COLUMN] "
                 "<columnname>'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = GetLayerByName(pszLayerName);
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such layer as `%s'.", pszSQLCommand,
                 pszLayerName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the field.                                                 */
    /* -------------------------------------------------------------------- */

    int nFieldIndex = poLayer->GetLayerDefn()->GetFieldIndex(pszColumnName);
    if (nFieldIndex < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such field as `%s'.", pszSQLCommand,
                 pszColumnName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Remove it.                                                      */
    /* -------------------------------------------------------------------- */

    CSLDestroy(papszTokens);

    return poLayer->DeleteField(nFieldIndex);
}

/************************************************************************/
/*                 ProcessSQLAlterTableRenameColumn()                   */
/*                                                                      */
/*      The correct syntax for renaming a column in the OGR SQL         */
/*      dialect is:                                                     */
/*                                                                      */
/*       ALTER TABLE <layername> RENAME [COLUMN] <oldname> TO <newname> */
/************************************************************************/

OGRErr GDALDataset::ProcessSQLAlterTableRenameColumn(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    const char *pszLayerName = nullptr;
    const char *pszOldColName = nullptr;
    const char *pszNewColName = nullptr;
    if (CSLCount(papszTokens) == 8 && EQUAL(papszTokens[0], "ALTER") &&
        EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "RENAME") &&
        EQUAL(papszTokens[4], "COLUMN") && EQUAL(papszTokens[6], "TO"))
    {
        pszLayerName = papszTokens[2];
        pszOldColName = papszTokens[5];
        pszNewColName = papszTokens[7];
    }
    else if (CSLCount(papszTokens) == 7 && EQUAL(papszTokens[0], "ALTER") &&
             EQUAL(papszTokens[1], "TABLE") &&
             EQUAL(papszTokens[3], "RENAME") && EQUAL(papszTokens[5], "TO"))
    {
        pszLayerName = papszTokens[2];
        pszOldColName = papszTokens[4];
        pszNewColName = papszTokens[6];
    }
    else
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in ALTER TABLE RENAME COLUMN command.\n"
                 "Was '%s'\n"
                 "Should be of form 'ALTER TABLE <layername> RENAME [COLUMN] "
                 "<columnname> TO <newname>'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = GetLayerByName(pszLayerName);
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such layer as `%s'.", pszSQLCommand,
                 pszLayerName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the field.                                                 */
    /* -------------------------------------------------------------------- */

    const int nFieldIndex =
        poLayer->GetLayerDefn()->GetFieldIndex(pszOldColName);
    if (nFieldIndex < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such field as `%s'.", pszSQLCommand,
                 pszOldColName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Rename column.                                                  */
    /* -------------------------------------------------------------------- */
    OGRFieldDefn *poOldFieldDefn =
        poLayer->GetLayerDefn()->GetFieldDefn(nFieldIndex);
    OGRFieldDefn oNewFieldDefn(poOldFieldDefn);
    oNewFieldDefn.SetName(pszNewColName);

    CSLDestroy(papszTokens);

    return poLayer->AlterFieldDefn(nFieldIndex, &oNewFieldDefn,
                                   ALTER_NAME_FLAG);
}

/************************************************************************/
/*                 ProcessSQLAlterTableAlterColumn()                    */
/*                                                                      */
/*      The correct syntax for altering the type of a column in the     */
/*      OGR SQL dialect is:                                             */
/*                                                                      */
/*   ALTER TABLE <layername> ALTER [COLUMN] <columnname> TYPE <newtype> */
/************************************************************************/

OGRErr GDALDataset::ProcessSQLAlterTableAlterColumn(const char *pszSQLCommand)

{
    char **papszTokens = CSLTokenizeString(pszSQLCommand);

    /* -------------------------------------------------------------------- */
    /*      Do some general syntax checking.                                */
    /* -------------------------------------------------------------------- */
    const char *pszLayerName = nullptr;
    const char *pszColumnName = nullptr;
    int iTypeIndex = 0;
    const int nTokens = CSLCount(papszTokens);

    if (nTokens >= 8 && EQUAL(papszTokens[0], "ALTER") &&
        EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "ALTER") &&
        EQUAL(papszTokens[4], "COLUMN") && EQUAL(papszTokens[6], "TYPE"))
    {
        pszLayerName = papszTokens[2];
        pszColumnName = papszTokens[5];
        iTypeIndex = 7;
    }
    else if (nTokens >= 7 && EQUAL(papszTokens[0], "ALTER") &&
             EQUAL(papszTokens[1], "TABLE") && EQUAL(papszTokens[3], "ALTER") &&
             EQUAL(papszTokens[5], "TYPE"))
    {
        pszLayerName = papszTokens[2];
        pszColumnName = papszTokens[4];
        iTypeIndex = 6;
    }
    else
    {
        CSLDestroy(papszTokens);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Syntax error in ALTER TABLE ALTER COLUMN command.\n"
                 "Was '%s'\n"
                 "Should be of form 'ALTER TABLE <layername> ALTER [COLUMN] "
                 "<columnname> TYPE <columntype>'",
                 pszSQLCommand);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Merge type components into a single string if there were split  */
    /*      with spaces                                                     */
    /* -------------------------------------------------------------------- */
    CPLString osType;
    for (int i = iTypeIndex; i < nTokens; ++i)
    {
        osType += papszTokens[i];
        CPLFree(papszTokens[i]);
    }
    char *pszType = papszTokens[iTypeIndex] = CPLStrdup(osType);
    papszTokens[iTypeIndex + 1] = nullptr;

    /* -------------------------------------------------------------------- */
    /*      Find the named layer.                                           */
    /* -------------------------------------------------------------------- */
    OGRLayer *poLayer = GetLayerByName(pszLayerName);
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such layer as `%s'.", pszSQLCommand,
                 pszLayerName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Find the field.                                                 */
    /* -------------------------------------------------------------------- */

    const int nFieldIndex =
        poLayer->GetLayerDefn()->GetFieldIndex(pszColumnName);
    if (nFieldIndex < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "%s failed, no such field as `%s'.", pszSQLCommand,
                 pszColumnName);
        CSLDestroy(papszTokens);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Alter column.                                                   */
    /* -------------------------------------------------------------------- */

    OGRFieldDefn *poOldFieldDefn =
        poLayer->GetLayerDefn()->GetFieldDefn(nFieldIndex);
    OGRFieldDefn oNewFieldDefn(poOldFieldDefn);

    int nWidth = 0;
    int nPrecision = 0;
    OGRFieldType eType = GDALDatasetParseSQLType(pszType, nWidth, nPrecision);
    oNewFieldDefn.SetType(eType);
    oNewFieldDefn.SetWidth(nWidth);
    oNewFieldDefn.SetPrecision(nPrecision);

    int l_nFlags = 0;
    if (poOldFieldDefn->GetType() != oNewFieldDefn.GetType())
        l_nFlags |= ALTER_TYPE_FLAG;
    if (poOldFieldDefn->GetWidth() != oNewFieldDefn.GetWidth() ||
        poOldFieldDefn->GetPrecision() != oNewFieldDefn.GetPrecision())
        l_nFlags |= ALTER_WIDTH_PRECISION_FLAG;

    CSLDestroy(papszTokens);

    if (l_nFlags == 0)
        return OGRERR_NONE;

    return poLayer->AlterFieldDefn(nFieldIndex, &oNewFieldDefn, l_nFlags);
}

//! @endcond

/************************************************************************/
/*                             ExecuteSQL()                             */
/************************************************************************/

/**
 \brief Execute an SQL statement against the data store.

 The result of an SQL query is either NULL for statements that are in error,
 or that have no results set, or an OGRLayer pointer representing a results
 set from the query.  Note that this OGRLayer is in addition to the layers
 in the data store and must be destroyed with
 ReleaseResultSet() before the dataset is closed
 (destroyed).

 This method is the same as the C function GDALDatasetExecuteSQL() and the
 deprecated OGR_DS_ExecuteSQL().

 For more information on the SQL dialect supported internally by OGR
 review the <a href="https://gdal.org/user/ogr_sql_dialect.html">OGR SQL</a>
 document.  Some drivers (i.e. Oracle and PostGIS) pass the SQL directly through
 to the underlying RDBMS.

 Starting with OGR 1.10, the <a
 href="https://gdal.org/user/sql_sqlite_dialect.html">SQLITE dialect</a> can
 also be used.

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param pszStatement the SQL statement to execute.
 @param poSpatialFilter geometry which represents a spatial filter. Can be NULL.
 @param pszDialect allows control of the statement dialect. If set to NULL, the
 OGR SQL engine will be used, except for RDBMS drivers that will use their
 dedicated SQL engine, unless OGRSQL is explicitly passed as the
 dialect. Starting with OGR 1.10, the SQLITE dialect can also be used.

 @return an OGRLayer containing the results of the query.  Deallocate with
 ReleaseResultSet().

*/

OGRLayer *GDALDataset::ExecuteSQL(const char *pszStatement,
                                  OGRGeometry *poSpatialFilter,
                                  const char *pszDialect)

{
    return ExecuteSQL(pszStatement, poSpatialFilter, pszDialect, nullptr);
}

//! @cond Doxygen_Suppress
OGRLayer *
GDALDataset::ExecuteSQL(const char *pszStatement, OGRGeometry *poSpatialFilter,
                        const char *pszDialect,
                        swq_select_parse_options *poSelectParseOptions)

{
    if (pszDialect != nullptr && EQUAL(pszDialect, "SQLite"))
    {
#ifdef SQLITE_ENABLED
        return OGRSQLiteExecuteSQL(this, pszStatement, poSpatialFilter,
                                   pszDialect);
#else
        CPLError(CE_Failure, CPLE_NotSupported,
                 "The SQLite driver needs to be compiled to support the "
                 "SQLite SQL dialect");
        return nullptr;
#endif
    }

    if (pszDialect != nullptr && !EQUAL(pszDialect, "") &&
        !EQUAL(pszDialect, "OGRSQL"))
    {
        std::string osDialectList = "'OGRSQL'";
#ifdef SQLITE_ENABLED
        osDialectList += ", 'SQLITE'";
#endif
        const char *pszDialects =
            GetMetadataItem(GDAL_DMD_SUPPORTED_SQL_DIALECTS);
        if (pszDialects)
        {
            const CPLStringList aosTokens(
                CSLTokenizeString2(pszDialects, " ", 0));
            for (int i = 0; i < aosTokens.size(); ++i)
            {
                if (!EQUAL(aosTokens[i], "OGRSQL") &&
                    !EQUAL(aosTokens[i], "SQLITE"))
                {
                    osDialectList += ", '";
                    osDialectList += aosTokens[i];
                    osDialectList += "'";
                }
            }
        }
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Dialect '%s' is unsupported. Only supported dialects are %s. "
                 "Defaulting to OGRSQL",
                 pszDialect, osDialectList.c_str());
    }

    /* -------------------------------------------------------------------- */
    /*      Handle CREATE INDEX statements specially.                       */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszStatement, "CREATE INDEX"))
    {
        ProcessSQLCreateIndex(pszStatement);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle DROP INDEX statements specially.                         */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszStatement, "DROP INDEX"))
    {
        ProcessSQLDropIndex(pszStatement);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle DROP TABLE statements specially.                         */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszStatement, "DROP TABLE"))
    {
        ProcessSQLDropTable(pszStatement);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Handle ALTER TABLE statements specially.                        */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszStatement, "ALTER TABLE"))
    {
        char **papszTokens = CSLTokenizeString(pszStatement);
        const int nTokens = CSLCount(papszTokens);
        if (nTokens >= 4 && EQUAL(papszTokens[3], "ADD"))
        {
            ProcessSQLAlterTableAddColumn(pszStatement);
            CSLDestroy(papszTokens);
            return nullptr;
        }
        else if (nTokens >= 4 && EQUAL(papszTokens[3], "DROP"))
        {
            ProcessSQLAlterTableDropColumn(pszStatement);
            CSLDestroy(papszTokens);
            return nullptr;
        }
        else if (nTokens == 6 && EQUAL(papszTokens[3], "RENAME") &&
                 EQUAL(papszTokens[4], "TO"))
        {
            const char *pszSrcTableName = papszTokens[2];
            const char *pszDstTableName = papszTokens[5];
            auto poSrcLayer = GetLayerByName(pszSrcTableName);
            if (poSrcLayer)
            {
                CPL_IGNORE_RET_VAL(poSrcLayer->Rename(pszDstTableName));
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid layer name");
            }
            CSLDestroy(papszTokens);
            return nullptr;
        }
        else if (nTokens >= 4 && EQUAL(papszTokens[3], "RENAME"))
        {
            ProcessSQLAlterTableRenameColumn(pszStatement);
            CSLDestroy(papszTokens);
            return nullptr;
        }
        else if (nTokens >= 4 && EQUAL(papszTokens[3], "ALTER"))
        {
            ProcessSQLAlterTableAlterColumn(pszStatement);
            CSLDestroy(papszTokens);
            return nullptr;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unsupported ALTER TABLE command : %s", pszStatement);
            CSLDestroy(papszTokens);
            return nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Preparse the SQL statement.                                     */
    /* -------------------------------------------------------------------- */
    swq_select *psSelectInfo = new swq_select();
    swq_custom_func_registrar *poCustomFuncRegistrar = nullptr;
    if (poSelectParseOptions != nullptr)
        poCustomFuncRegistrar = poSelectParseOptions->poCustomFuncRegistrar;
    if (psSelectInfo->preparse(pszStatement,
                               poCustomFuncRegistrar != nullptr) != CE_None)
    {
        delete psSelectInfo;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      If there is no UNION ALL, build result layer.                   */
    /* -------------------------------------------------------------------- */
    if (psSelectInfo->poOtherSelect == nullptr)
    {
        return BuildLayerFromSelectInfo(psSelectInfo, poSpatialFilter,
                                        pszDialect, poSelectParseOptions);
    }

    /* -------------------------------------------------------------------- */
    /*      Build result union layer.                                       */
    /* -------------------------------------------------------------------- */
    int nSrcLayers = 0;
    OGRLayer **papoSrcLayers = nullptr;

    do
    {
        swq_select *psNextSelectInfo = psSelectInfo->poOtherSelect;
        psSelectInfo->poOtherSelect = nullptr;

        OGRLayer *poLayer = BuildLayerFromSelectInfo(
            psSelectInfo, poSpatialFilter, pszDialect, poSelectParseOptions);
        if (poLayer == nullptr)
        {
            // Each source layer owns an independent select info.
            for (int i = 0; i < nSrcLayers; ++i)
                delete papoSrcLayers[i];
            CPLFree(papoSrcLayers);

            // So we just have to destroy the remaining select info.
            delete psNextSelectInfo;

            return nullptr;
        }
        else
        {
            papoSrcLayers = static_cast<OGRLayer **>(CPLRealloc(
                papoSrcLayers, sizeof(OGRLayer *) * (nSrcLayers + 1)));
            papoSrcLayers[nSrcLayers] = poLayer;
            ++nSrcLayers;

            psSelectInfo = psNextSelectInfo;
        }
    } while (psSelectInfo != nullptr);

    return new OGRUnionLayer("SELECT", nSrcLayers, papoSrcLayers, TRUE);
}

//! @endcond

/************************************************************************/
/*                             AbortSQL()                             */
/************************************************************************/

/**
 \brief Abort any SQL statement running in the data store.

 This function can be safely called from any thread (pending that the dataset
 object is still alive). Driver implementations will make sure that it can be
 called in a thread-safe way.

 This might not be implemented by all drivers. At time of writing, only SQLite,
 GPKG and PG drivers implement it

 This method is the same as the C method GDALDatasetAbortSQL()

 @since GDAL 3.2.0


*/

OGRErr GDALDataset::AbortSQL()
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "AbortSQL is not supported for this driver.");
    return OGRERR_UNSUPPORTED_OPERATION;
}

/************************************************************************/
/*                        BuildLayerFromSelectInfo()                    */
/************************************************************************/

struct GDALSQLParseInfo
{
    swq_field_list sFieldList;
    int nExtraDSCount;
    GDALDataset **papoExtraDS;
    char *pszWHERE;
};

OGRLayer *GDALDataset::BuildLayerFromSelectInfo(
    swq_select *psSelectInfo, OGRGeometry *poSpatialFilter,
    const char *pszDialect, swq_select_parse_options *poSelectParseOptions)
{
    std::unique_ptr<swq_select> psSelectInfoUnique(psSelectInfo);

    std::unique_ptr<OGRGenSQLResultsLayer> poResults;
    GDALSQLParseInfo *psParseInfo =
        BuildParseInfo(psSelectInfoUnique.get(), poSelectParseOptions);

    if (psParseInfo)
    {
        const auto nErrorCounter = CPLGetErrorCounter();
        poResults = std::make_unique<OGRGenSQLResultsLayer>(
            this, std::move(psSelectInfoUnique), poSpatialFilter,
            psParseInfo->pszWHERE, pszDialect);
        if (CPLGetErrorCounter() > nErrorCounter &&
            CPLGetLastErrorType() != CE_None)
            poResults.reset();
    }

    DestroyParseInfo(psParseInfo);

    return poResults.release();
}

/************************************************************************/
/*                             DestroyParseInfo()                       */
/************************************************************************/

//! @cond Doxygen_Suppress
void GDALDataset::DestroyParseInfo(GDALSQLParseInfo *psParseInfo)
{
    if (psParseInfo == nullptr)
        return;

    CPLFree(psParseInfo->sFieldList.names);
    CPLFree(psParseInfo->sFieldList.types);
    CPLFree(psParseInfo->sFieldList.table_ids);
    CPLFree(psParseInfo->sFieldList.ids);

    // Release the datasets we have opened with OGROpenShared()
    // It is safe to do that as the 'new OGRGenSQLResultsLayer' itself
    // has taken a reference on them, which it will release in its
    // destructor.
    for (int iEDS = 0; iEDS < psParseInfo->nExtraDSCount; ++iEDS)
        GDALClose(psParseInfo->papoExtraDS[iEDS]);

    CPLFree(psParseInfo->papoExtraDS);
    CPLFree(psParseInfo->pszWHERE);
    CPLFree(psParseInfo);
}

/************************************************************************/
/*                            BuildParseInfo()                          */
/************************************************************************/

GDALSQLParseInfo *
GDALDataset::BuildParseInfo(swq_select *psSelectInfo,
                            swq_select_parse_options *poSelectParseOptions)
{
    int nFirstLayerFirstSpecialFieldIndex = 0;

    GDALSQLParseInfo *psParseInfo =
        static_cast<GDALSQLParseInfo *>(CPLCalloc(1, sizeof(GDALSQLParseInfo)));

    /* -------------------------------------------------------------------- */
    /*      Validate that all the source tables are recognized, count       */
    /*      fields.                                                         */
    /* -------------------------------------------------------------------- */
    int nFieldCount = 0;

    for (int iTable = 0; iTable < psSelectInfo->table_count; iTable++)
    {
        swq_table_def *psTableDef = psSelectInfo->table_defs + iTable;
        GDALDataset *poTableDS = this;

        if (psTableDef->data_source != nullptr)
        {
            poTableDS = GDALDataset::FromHandle(
                OGROpenShared(psTableDef->data_source, FALSE, nullptr));
            if (poTableDS == nullptr)
            {
                if (strlen(CPLGetLastErrorMsg()) == 0)
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to open secondary datasource "
                             "`%s' required by JOIN.",
                             psTableDef->data_source);

                DestroyParseInfo(psParseInfo);
                return nullptr;
            }

            // Keep in an array to release at the end of this function.
            psParseInfo->papoExtraDS = static_cast<GDALDataset **>(CPLRealloc(
                psParseInfo->papoExtraDS,
                sizeof(GDALDataset *) * (psParseInfo->nExtraDSCount + 1)));
            psParseInfo->papoExtraDS[psParseInfo->nExtraDSCount++] = poTableDS;
        }

        OGRLayer *poSrcLayer =
            poTableDS->GetLayerByName(psTableDef->table_name);

        if (poSrcLayer == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "SELECT from table %s failed, no such table/featureclass.",
                     psTableDef->table_name);

            DestroyParseInfo(psParseInfo);
            return nullptr;
        }

        nFieldCount += poSrcLayer->GetLayerDefn()->GetFieldCount();
        if (iTable == 0 ||
            (poSelectParseOptions &&
             poSelectParseOptions->bAddSecondaryTablesGeometryFields))
            nFieldCount += poSrcLayer->GetLayerDefn()->GetGeomFieldCount();

        const char *pszFID = poSrcLayer->GetFIDColumn();
        if (pszFID && !EQUAL(pszFID, "") && !EQUAL(pszFID, "FID") &&
            poSrcLayer->GetLayerDefn()->GetFieldIndex(pszFID) < 0)
            nFieldCount++;
    }

    /* -------------------------------------------------------------------- */
    /*      Build the field list for all indicated tables.                  */
    /* -------------------------------------------------------------------- */

    psParseInfo->sFieldList.table_count = psSelectInfo->table_count;
    psParseInfo->sFieldList.table_defs = psSelectInfo->table_defs;

    psParseInfo->sFieldList.count = 0;
    psParseInfo->sFieldList.names = static_cast<char **>(
        CPLMalloc(sizeof(char *) * (nFieldCount + SPECIAL_FIELD_COUNT)));
    psParseInfo->sFieldList.types = static_cast<swq_field_type *>(CPLMalloc(
        sizeof(swq_field_type) * (nFieldCount + SPECIAL_FIELD_COUNT)));
    psParseInfo->sFieldList.table_ids = static_cast<int *>(
        CPLMalloc(sizeof(int) * (nFieldCount + SPECIAL_FIELD_COUNT)));
    psParseInfo->sFieldList.ids = static_cast<int *>(
        CPLMalloc(sizeof(int) * (nFieldCount + SPECIAL_FIELD_COUNT)));

    bool bIsFID64 = false;
    for (int iTable = 0; iTable < psSelectInfo->table_count; iTable++)
    {
        swq_table_def *psTableDef = psSelectInfo->table_defs + iTable;
        GDALDataset *poTableDS = this;

        if (psTableDef->data_source != nullptr)
        {
            poTableDS = GDALDataset::FromHandle(
                OGROpenShared(psTableDef->data_source, FALSE, nullptr));
            CPLAssert(poTableDS != nullptr);
            poTableDS->Dereference();
        }

        OGRLayer *poSrcLayer =
            poTableDS->GetLayerByName(psTableDef->table_name);

        for (int iField = 0;
             iField < poSrcLayer->GetLayerDefn()->GetFieldCount(); iField++)
        {
            OGRFieldDefn *poFDefn =
                poSrcLayer->GetLayerDefn()->GetFieldDefn(iField);
            const int iOutField = psParseInfo->sFieldList.count++;
            psParseInfo->sFieldList.names[iOutField] =
                const_cast<char *>(poFDefn->GetNameRef());
            if (poFDefn->GetType() == OFTInteger)
            {
                if (poFDefn->GetSubType() == OFSTBoolean)
                    psParseInfo->sFieldList.types[iOutField] = SWQ_BOOLEAN;
                else
                    psParseInfo->sFieldList.types[iOutField] = SWQ_INTEGER;
            }
            else if (poFDefn->GetType() == OFTInteger64)
            {
                if (poFDefn->GetSubType() == OFSTBoolean)
                    psParseInfo->sFieldList.types[iOutField] = SWQ_BOOLEAN;
                else
                    psParseInfo->sFieldList.types[iOutField] = SWQ_INTEGER64;
            }
            else if (poFDefn->GetType() == OFTReal)
                psParseInfo->sFieldList.types[iOutField] = SWQ_FLOAT;
            else if (poFDefn->GetType() == OFTString)
                psParseInfo->sFieldList.types[iOutField] = SWQ_STRING;
            else if (poFDefn->GetType() == OFTTime)
                psParseInfo->sFieldList.types[iOutField] = SWQ_TIME;
            else if (poFDefn->GetType() == OFTDate)
                psParseInfo->sFieldList.types[iOutField] = SWQ_DATE;
            else if (poFDefn->GetType() == OFTDateTime)
                psParseInfo->sFieldList.types[iOutField] = SWQ_TIMESTAMP;
            else
                psParseInfo->sFieldList.types[iOutField] = SWQ_OTHER;

            psParseInfo->sFieldList.table_ids[iOutField] = iTable;
            psParseInfo->sFieldList.ids[iOutField] = iField;
        }

        if (iTable == 0)
        {
            nFirstLayerFirstSpecialFieldIndex = psParseInfo->sFieldList.count;
        }

        if (iTable == 0 ||
            (poSelectParseOptions &&
             poSelectParseOptions->bAddSecondaryTablesGeometryFields))
        {

            for (int iField = 0;
                 iField < poSrcLayer->GetLayerDefn()->GetGeomFieldCount();
                 iField++)
            {
                OGRGeomFieldDefn *poFDefn =
                    poSrcLayer->GetLayerDefn()->GetGeomFieldDefn(iField);
                const int iOutField = psParseInfo->sFieldList.count++;
                psParseInfo->sFieldList.names[iOutField] =
                    const_cast<char *>(poFDefn->GetNameRef());
                if (*psParseInfo->sFieldList.names[iOutField] == '\0')
                    psParseInfo->sFieldList.names[iOutField] =
                        const_cast<char *>(OGR_GEOMETRY_DEFAULT_NON_EMPTY_NAME);
                psParseInfo->sFieldList.types[iOutField] = SWQ_GEOMETRY;

                psParseInfo->sFieldList.table_ids[iOutField] = iTable;
                psParseInfo->sFieldList.ids[iOutField] =
                    GEOM_FIELD_INDEX_TO_ALL_FIELD_INDEX(
                        poSrcLayer->GetLayerDefn(), iField);
            }
        }

        if (iTable == 0 && poSrcLayer->GetMetadataItem(OLMD_FID64) != nullptr &&
            EQUAL(poSrcLayer->GetMetadataItem(OLMD_FID64), "YES"))
        {
            bIsFID64 = true;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Expand '*' in 'SELECT *' now before we add the pseudo fields    */
    /* -------------------------------------------------------------------- */
    const bool bAlwaysPrefixWithTableName =
        poSelectParseOptions &&
        poSelectParseOptions->bAlwaysPrefixWithTableName;
    if (psSelectInfo->expand_wildcard(&psParseInfo->sFieldList,
                                      bAlwaysPrefixWithTableName) != CE_None)
    {
        DestroyParseInfo(psParseInfo);
        return nullptr;
    }

    for (int iField = 0; iField < SPECIAL_FIELD_COUNT; iField++)
    {
        psParseInfo->sFieldList.names[psParseInfo->sFieldList.count] =
            const_cast<char *>(SpecialFieldNames[iField]);
        psParseInfo->sFieldList.types[psParseInfo->sFieldList.count] =
            (iField == SPF_FID && bIsFID64) ? SWQ_INTEGER64
                                            : SpecialFieldTypes[iField];
        psParseInfo->sFieldList.table_ids[psParseInfo->sFieldList.count] = 0;
        psParseInfo->sFieldList.ids[psParseInfo->sFieldList.count] =
            nFirstLayerFirstSpecialFieldIndex + iField;
        psParseInfo->sFieldList.count++;
    }

    /* In the case a layer has an explicit FID column name, then add it */
    /* so it can be selected */
    for (int iTable = 0; iTable < psSelectInfo->table_count; iTable++)
    {
        swq_table_def *psTableDef = psSelectInfo->table_defs + iTable;
        GDALDataset *poTableDS = this;

        if (psTableDef->data_source != nullptr)
        {
            poTableDS = GDALDataset::FromHandle(
                OGROpenShared(psTableDef->data_source, FALSE, nullptr));
            CPLAssert(poTableDS != nullptr);
            poTableDS->Dereference();
        }

        OGRLayer *poSrcLayer =
            poTableDS->GetLayerByName(psTableDef->table_name);

        const char *pszFID = poSrcLayer->GetFIDColumn();
        if (pszFID && !EQUAL(pszFID, "") && !EQUAL(pszFID, "FID") &&
            poSrcLayer->GetLayerDefn()->GetFieldIndex(pszFID) < 0)
        {
            const int iOutField = psParseInfo->sFieldList.count++;
            psParseInfo->sFieldList.names[iOutField] =
                const_cast<char *>(pszFID);
            if (poSrcLayer->GetMetadataItem(OLMD_FID64) != nullptr &&
                EQUAL(poSrcLayer->GetMetadataItem(OLMD_FID64), "YES"))
            {
                psParseInfo->sFieldList.types[iOutField] = SWQ_INTEGER64;
            }
            else
            {
                psParseInfo->sFieldList.types[iOutField] = SWQ_INTEGER;
            }
            psParseInfo->sFieldList.table_ids[iOutField] = iTable;
            psParseInfo->sFieldList.ids[iOutField] =
                poSrcLayer->GetLayerDefn()->GetFieldCount() + SPF_FID;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Finish the parse operation.                                     */
    /* -------------------------------------------------------------------- */
    if (psSelectInfo->parse(&psParseInfo->sFieldList, poSelectParseOptions) !=
        CE_None)
    {
        DestroyParseInfo(psParseInfo);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Extract the WHERE expression to use separately.                 */
    /* -------------------------------------------------------------------- */
    if (psSelectInfo->where_expr != nullptr)
    {
        psParseInfo->pszWHERE =
            psSelectInfo->where_expr->Unparse(&psParseInfo->sFieldList, '"');
        // CPLDebug( "OGR", "Unparse() -> %s", pszWHERE );
    }

    return psParseInfo;
}

//! @endcond

/************************************************************************/
/*                          ReleaseResultSet()                          */
/************************************************************************/

/**
 \brief Release results of ExecuteSQL().

 This method should only be used to deallocate OGRLayers resulting from
 an ExecuteSQL() call on the same GDALDataset.  Failure to deallocate a
 results set before destroying the GDALDataset may cause errors.

 This method is the same as the C function GDALDatasetReleaseResultSet() and the
 deprecated OGR_DS_ReleaseResultSet().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param poResultsSet the result of a previous ExecuteSQL() call.
*/

void GDALDataset::ReleaseResultSet(OGRLayer *poResultsSet)

{
    delete poResultsSet;
}

/************************************************************************/
/*                            GetStyleTable()                           */
/************************************************************************/

/**
 \brief Returns dataset style table.

 This method is the same as the C function GDALDatasetGetStyleTable() and the
 deprecated OGR_DS_GetStyleTable().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @return pointer to a style table which should not be modified or freed by the
 caller.
*/

OGRStyleTable *GDALDataset::GetStyleTable()
{
    return m_poStyleTable;
}

/************************************************************************/
/*                         SetStyleTableDirectly()                      */
/************************************************************************/

/**
 \brief Set dataset style table.

 This method operate exactly as SetStyleTable() except that it
 assumes ownership of the passed table.

 This method is the same as the C function GDALDatasetSetStyleTableDirectly()
 and the deprecated OGR_DS_SetStyleTableDirectly().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param poStyleTable pointer to style table to set

*/
void GDALDataset::SetStyleTableDirectly(OGRStyleTable *poStyleTable)
{
    if (m_poStyleTable)
        delete m_poStyleTable;
    m_poStyleTable = poStyleTable;
}

/************************************************************************/
/*                            SetStyleTable()                           */
/************************************************************************/

/**
 \brief Set dataset style table.

 This method operate exactly as SetStyleTableDirectly() except
 that it does not assume ownership of the passed table.

 This method is the same as the C function GDALDatasetSetStyleTable() and the
 deprecated OGR_DS_SetStyleTable().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param poStyleTable pointer to style table to set

*/

void GDALDataset::SetStyleTable(OGRStyleTable *poStyleTable)
{
    if (m_poStyleTable)
        delete m_poStyleTable;
    if (poStyleTable)
        m_poStyleTable = poStyleTable->Clone();
}

/************************************************************************/
/*                         IsGenericSQLDialect()                        */
/************************************************************************/

//! @cond Doxygen_Suppress
int GDALDataset::IsGenericSQLDialect(const char *pszDialect)
{
    return pszDialect != nullptr &&
           (EQUAL(pszDialect, "OGRSQL") || EQUAL(pszDialect, "SQLITE"));
}

//! @endcond

/************************************************************************/
/*                            GetLayerCount()                           */
/************************************************************************/

/**
 \brief Get the number of layers in this dataset.

 This method is the same as the C function GDALDatasetGetLayerCount(),
 and the deprecated OGR_DS_GetLayerCount().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @return layer count.
*/

int GDALDataset::GetLayerCount()
{
    return 0;
}

/************************************************************************/
/*                                GetLayer()                            */
/************************************************************************/

/**
 \fn GDALDataset::GetLayer(int)
 \brief Fetch a layer by index.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 See GetLayers() for a C++ iterator version of this method.

 This method is the same as the C function GDALDatasetGetLayer() and the
 deprecated OGR_DS_GetLayer().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param iLayer a layer number between 0 and GetLayerCount()-1.

 @return the layer, or NULL if iLayer is out of range or an error occurs.

 @see GetLayers()
*/

OGRLayer *GDALDataset::GetLayer(CPL_UNUSED int iLayer)
{
    return nullptr;
}

/************************************************************************/
/*                                IsLayerPrivate()                      */
/************************************************************************/

/**
 \fn GDALDataset::IsLayerPrivate(int)
 \brief Returns true if the layer at the specified index is deemed a private or
 system table, or an internal detail only.

 This method is the same as the C function GDALDatasetIsLayerPrivate().

 @param iLayer a layer number between 0 and GetLayerCount()-1.

 @return true if the layer is a private or system table.

 @since GDAL 3.4
*/

bool GDALDataset::IsLayerPrivate(CPL_UNUSED int iLayer) const
{
    return false;
}

/************************************************************************/
/*                           ResetReading()                             */
/************************************************************************/

/**
 \brief Reset feature reading to start on the first feature.

 This affects GetNextFeature().

 Depending on drivers, this may also have the side effect of calling
 OGRLayer::ResetReading() on the layers of this dataset.

 This method is the same as the C function GDALDatasetResetReading().

 @since GDAL 2.2
*/
void GDALDataset::ResetReading()
{
    if (!m_poPrivate)
        return;
    m_poPrivate->nCurrentLayerIdx = 0;
    m_poPrivate->nLayerCount = -1;
    m_poPrivate->poCurrentLayer = nullptr;
    m_poPrivate->nFeatureReadInLayer = 0;
    m_poPrivate->nFeatureReadInDataset = 0;
    m_poPrivate->nTotalFeaturesInLayer = TOTAL_FEATURES_NOT_INIT;
    m_poPrivate->nTotalFeatures = TOTAL_FEATURES_NOT_INIT;
}

/************************************************************************/
/*                         GDALDatasetResetReading()                    */
/************************************************************************/

/**
 \brief Reset feature reading to start on the first feature.

 This affects GDALDatasetGetNextFeature().

 Depending on drivers, this may also have the side effect of calling
 OGR_L_ResetReading() on the layers of this dataset.

 This method is the same as the C++ method GDALDataset::ResetReading()

 @param hDS dataset handle
 @since GDAL 2.2
*/
void CPL_DLL GDALDatasetResetReading(GDALDatasetH hDS)
{
    VALIDATE_POINTER0(hDS, "GDALDatasetResetReading");

    return GDALDataset::FromHandle(hDS)->ResetReading();
}

/************************************************************************/
/*                          GetNextFeature()                            */
/************************************************************************/

/**
 \brief Fetch the next available feature from this dataset.

 This method is intended for the few drivers where OGRLayer::GetNextFeature()
 is not efficient, but in general OGRLayer::GetNextFeature() is a more
 natural API.

 See GetFeatures() for a C++ iterator version of this method.

 The returned feature becomes the responsibility of the caller to
 delete with OGRFeature::DestroyFeature().

 Depending on the driver, this method may return features from layers in a
 non sequential way. This is what may happen when the
 ODsCRandomLayerRead capability is declared (for example for the
 OSM and GMLAS drivers). When datasets declare this capability, it is strongly
 advised to use GDALDataset::GetNextFeature() instead of
 OGRLayer::GetNextFeature(), as the later might have a slow, incomplete or stub
 implementation.

 The default implementation, used by most drivers, will
 however iterate over each layer, and then over each feature within this
 layer.

 This method takes into account spatial and attribute filters set on layers that
 will be iterated upon.

 The ResetReading() method can be used to start at the beginning again.

 Depending on drivers, this may also have the side effect of calling
 OGRLayer::GetNextFeature() on the layers of this dataset.

 This method is the same as the C function GDALDatasetGetNextFeature().

 @param ppoBelongingLayer a pointer to a OGRLayer* variable to receive the
                          layer to which the object belongs to, or NULL.
                          It is possible that the output of *ppoBelongingLayer
                          to be NULL despite the feature not being NULL.
 @param pdfProgressPct    a pointer to a double variable to receive the
                          percentage progress (in [0,1] range), or NULL.
                          On return, the pointed value might be negative if
                          determining the progress is not possible.
 @param pfnProgress       a progress callback to report progress (for
                          GetNextFeature() calls that might have a long
                          duration) and offer cancellation possibility, or NULL.
 @param pProgressData     user data provided to pfnProgress, or NULL
 @return a feature, or NULL if no more features are available.
 @since GDAL 2.2
 @see GetFeatures()
*/

OGRFeature *GDALDataset::GetNextFeature(OGRLayer **ppoBelongingLayer,
                                        double *pdfProgressPct,
                                        GDALProgressFunc pfnProgress,
                                        void *pProgressData)
{
    if (!m_poPrivate || m_poPrivate->nCurrentLayerIdx < 0)
    {
        if (ppoBelongingLayer != nullptr)
            *ppoBelongingLayer = nullptr;
        if (pdfProgressPct != nullptr)
            *pdfProgressPct = 1.0;
        if (pfnProgress != nullptr)
            pfnProgress(1.0, "", pProgressData);
        return nullptr;
    }

    if (m_poPrivate->poCurrentLayer == nullptr &&
        (pdfProgressPct != nullptr || pfnProgress != nullptr))
    {
        if (m_poPrivate->nLayerCount < 0)
        {
            m_poPrivate->nLayerCount = GetLayerCount();
        }

        if (m_poPrivate->nTotalFeatures == TOTAL_FEATURES_NOT_INIT)
        {
            m_poPrivate->nTotalFeatures = 0;
            for (int i = 0; i < m_poPrivate->nLayerCount; i++)
            {
                OGRLayer *poLayer = GetLayer(i);
                if (poLayer == nullptr ||
                    !poLayer->TestCapability(OLCFastFeatureCount))
                {
                    m_poPrivate->nTotalFeatures = TOTAL_FEATURES_UNKNOWN;
                    break;
                }
                GIntBig nCount = poLayer->GetFeatureCount(FALSE);
                if (nCount < 0)
                {
                    m_poPrivate->nTotalFeatures = TOTAL_FEATURES_UNKNOWN;
                    break;
                }
                m_poPrivate->nTotalFeatures += nCount;
            }
        }
    }

    while (true)
    {
        if (m_poPrivate->poCurrentLayer == nullptr)
        {
            m_poPrivate->poCurrentLayer =
                GetLayer(m_poPrivate->nCurrentLayerIdx);
            if (m_poPrivate->poCurrentLayer == nullptr)
            {
                m_poPrivate->nCurrentLayerIdx = -1;
                if (ppoBelongingLayer != nullptr)
                    *ppoBelongingLayer = nullptr;
                if (pdfProgressPct != nullptr)
                    *pdfProgressPct = 1.0;
                return nullptr;
            }
            m_poPrivate->poCurrentLayer->ResetReading();
            m_poPrivate->nFeatureReadInLayer = 0;
            if (m_poPrivate->nTotalFeatures < 0 && pdfProgressPct != nullptr)
            {
                if (m_poPrivate->poCurrentLayer->TestCapability(
                        OLCFastFeatureCount))
                    m_poPrivate->nTotalFeaturesInLayer =
                        m_poPrivate->poCurrentLayer->GetFeatureCount(FALSE);
                else
                    m_poPrivate->nTotalFeaturesInLayer = 0;
            }
        }
        OGRFeature *poFeature = m_poPrivate->poCurrentLayer->GetNextFeature();
        if (poFeature == nullptr)
        {
            m_poPrivate->nCurrentLayerIdx++;
            m_poPrivate->poCurrentLayer = nullptr;
            continue;
        }

        m_poPrivate->nFeatureReadInLayer++;
        m_poPrivate->nFeatureReadInDataset++;
        if (pdfProgressPct != nullptr || pfnProgress != nullptr)
        {
            double dfPct = 0.0;
            if (m_poPrivate->nTotalFeatures > 0)
            {
                dfPct = 1.0 * m_poPrivate->nFeatureReadInDataset /
                        m_poPrivate->nTotalFeatures;
            }
            else
            {
                dfPct = 1.0 * m_poPrivate->nCurrentLayerIdx /
                        m_poPrivate->nLayerCount;
                if (m_poPrivate->nTotalFeaturesInLayer > 0)
                {
                    dfPct += 1.0 * m_poPrivate->nFeatureReadInLayer /
                             m_poPrivate->nTotalFeaturesInLayer /
                             m_poPrivate->nLayerCount;
                }
            }
            if (pdfProgressPct)
                *pdfProgressPct = dfPct;
            if (pfnProgress)
                pfnProgress(dfPct, "", nullptr);
        }

        if (ppoBelongingLayer != nullptr)
            *ppoBelongingLayer = m_poPrivate->poCurrentLayer;
        return poFeature;
    }
}

/************************************************************************/
/*                     GDALDatasetGetNextFeature()                      */
/************************************************************************/
/**
 \brief Fetch the next available feature from this dataset.

 This method is intended for the few drivers where OGR_L_GetNextFeature()
 is not efficient, but in general OGR_L_GetNextFeature() is a more
 natural API.

 The returned feature becomes the responsibility of the caller to
 delete with OGRFeature::DestroyFeature().

 Depending on the driver, this method may return features from layers in a
 non sequential way. This is what may happen when the
 ODsCRandomLayerRead capability is declared (for example for the
 OSM and GMLAS drivers). When datasets declare this capability, it is strongly
 advised to use GDALDataset::GetNextFeature() instead of
 OGRLayer::GetNextFeature(), as the later might have a slow, incomplete or stub
 implementation.

 The default implementation, used by most drivers, will
 however iterate over each layer, and then over each feature within this
 layer.

 This method takes into account spatial and attribute filters set on layers that
 will be iterated upon.

 The ResetReading() method can be used to start at the beginning again.

 Depending on drivers, this may also have the side effect of calling
 OGRLayer::GetNextFeature() on the layers of this dataset.

 This method is the same as the C++ method GDALDataset::GetNextFeature()

 @param hDS               dataset handle.
 @param phBelongingLayer  a pointer to a OGRLayer* variable to receive the
                          layer to which the object belongs to, or NULL.
                          It is possible that the output of *ppoBelongingLayer
                          to be NULL despite the feature not being NULL.
 @param pdfProgressPct    a pointer to a double variable to receive the
                          percentage progress (in [0,1] range), or NULL.
                          On return, the pointed value might be negative if
                          determining the progress is not possible.
 @param pfnProgress       a progress callback to report progress (for
                          GetNextFeature() calls that might have a long
                          duration) and offer cancellation possibility, or NULL
 @param pProgressData     user data provided to pfnProgress, or NULL
 @return a feature, or NULL if no more features are available.
 @since GDAL 2.2
*/
OGRFeatureH CPL_DLL GDALDatasetGetNextFeature(GDALDatasetH hDS,
                                              OGRLayerH *phBelongingLayer,
                                              double *pdfProgressPct,
                                              GDALProgressFunc pfnProgress,
                                              void *pProgressData)
{
    VALIDATE_POINTER1(hDS, "GDALDatasetGetNextFeature", nullptr);

    return OGRFeature::ToHandle(GDALDataset::FromHandle(hDS)->GetNextFeature(
        reinterpret_cast<OGRLayer **>(phBelongingLayer), pdfProgressPct,
        pfnProgress, pProgressData));
}

/************************************************************************/
/*                            TestCapability()                          */
/************************************************************************/

/**
 \fn GDALDataset::TestCapability( const char * pszCap )
 \brief Test if capability is available.

 One of the following dataset capability names can be passed into this
 method, and a TRUE or FALSE value will be returned indicating whether or not
 the capability is available for this object.

 <ul>
  <li> <b>ODsCCreateLayer</b>: True if this datasource can create new layers.<p>
  <li> <b>ODsCDeleteLayer</b>: True if this datasource can delete existing
          layers.<p>
  <li> <b>ODsCCreateGeomFieldAfterCreateLayer</b>: True if the layers of this
          datasource support CreateGeomField() just after layer creation.<p>
  <li> <b>ODsCCurveGeometries</b>: True if this datasource supports curve
          geometries.<p>
  <li> <b>ODsCTransactions</b>: True if this datasource supports (efficient)
          transactions.<p>
  <li> <b>ODsCEmulatedTransactions</b>: True if this datasource supports
          transactions through emulation.<p>
  <li> <b>ODsCRandomLayerRead</b>: True if this datasource has a dedicated
          GetNextFeature() implementation, potentially returning features from
          layers in a non sequential way.<p>
  <li> <b>ODsCRandomLayerWrite</b>: True if this datasource supports calling
         CreateFeature() on layers in a non sequential way.<p>
  <li> <b>GDsCAddRelationship</b>: True if AddRelationship() is supported</li>
  <li> <b>GDsCDeleteRelationship</b>: True if DeleteRelationship() is supported</li>
  <li> <b>GDsCUpdateRelationship</b>: True if UpdateRelationship() is supported</li>
  <li> <b>GDsCFastGetExtent</b>: True if GetExtent() is fast</li>
  <li> <b>GDsCFastGetExtentWGS84LongLat</b>: True if GetExtentWGS84LongLat() is fast</li>
 </ul>

 The \#define macro forms of the capability names should be used in preference
 to the strings themselves to avoid misspelling.

 This method is the same as the C function GDALDatasetTestCapability() and the
 deprecated OGR_DS_TestCapability().

 In GDAL 1.X, this method used to be in the OGRDataSource class.

 @param pszCap the capability to test.

 @return TRUE if capability available otherwise FALSE.
*/

int GDALDataset::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, GDsCFastGetExtent) ||
        EQUAL(pszCap, GDsCFastGetExtentWGS84LongLat))
    {
        for (auto &&poLayer : GetLayers())
        {
            if (!poLayer->TestCapability(OLCFastGetExtent))
                return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

/************************************************************************/
/*                     GDALDatasetTestCapability()                      */
/************************************************************************/

/**
 \brief Test if capability is available.

 One of the following dataset capability names can be passed into this
 function, and a TRUE or FALSE value will be returned indicating whether or not
 the capability is available for this object.

 <ul>
  <li> <b>ODsCCreateLayer</b>: True if this datasource can create new layers.<p>
  <li> <b>ODsCDeleteLayer</b>: True if this datasource can delete existing
          layers.<p>
  <li> <b>ODsCCreateGeomFieldAfterCreateLayer</b>: True if the layers of this
          datasource support CreateGeomField() just after layer creation.<p>
  <li> <b>ODsCCurveGeometries</b>: True if this datasource supports curve
          geometries.<p>
  <li> <b>ODsCTransactions</b>: True if this datasource supports (efficient)
          transactions.<p>
  <li> <b>ODsCEmulatedTransactions</b>: True if this datasource supports
          transactions through emulation.<p>
  <li> <b>ODsCRandomLayerRead</b>: True if this datasource has a dedicated
          GetNextFeature() implementation, potentially returning features from
          layers in a non sequential way.<p>
  <li> <b>ODsCRandomLayerWrite</b>: True if this datasource supports calling
          CreateFeature() on layers in a non sequential way.<p>
  <li> <b>GDsCAddRelationship</b>: True if AddRelationship() is supported</li>
  <li> <b>GDsCDeleteRelationship</b>: True if DeleteRelationship() is supported</li>
  <li> <b>GDsCUpdateRelationship</b>: True if UpdateRelationship() is supported</li>
  <li> <b>GDsCFastGetExtent</b>: True if GetExtent() is fast</li>
  <li> <b>GDsCFastGetExtentWGS84LongLat</b>: True if GetExtentWGS84LongLat() is fast</li>
 </ul>

 The \#define macro forms of the capability names should be used in preference
 to the strings themselves to avoid misspelling.

 This function is the same as the C++ method GDALDataset::TestCapability()

 @since GDAL 2.0

 @param hDS the dataset handle.
 @param pszCap the capability to test.

 @return TRUE if capability available otherwise FALSE.
*/
int GDALDatasetTestCapability(GDALDatasetH hDS, const char *pszCap)

{
    VALIDATE_POINTER1(hDS, "GDALDatasetTestCapability", 0);
    VALIDATE_POINTER1(pszCap, "GDALDatasetTestCapability", 0);

    return GDALDataset::FromHandle(hDS)->TestCapability(pszCap);
}

/************************************************************************/
/*                           StartTransaction()                         */
/************************************************************************/

/**
 \fn GDALDataset::StartTransaction(int)
 \brief For datasources which support transactions, StartTransaction creates a
`transaction.

 If starting the transaction fails, will return
 OGRERR_FAILURE. Datasources which do not support transactions will
 always return OGRERR_UNSUPPORTED_OPERATION.

 Nested transactions are not supported.

 All changes done after the start of the transaction are definitely applied in
 the datasource if CommitTransaction() is called. They may be canceled by
 calling RollbackTransaction() instead.

 At the time of writing, transactions only apply on vector layers.

 Datasets that support transactions will advertise the ODsCTransactions
 capability.  Use of transactions at dataset level is generally preferred to
 transactions at layer level, whose scope is rarely limited to the layer from
 which it was started.

 In case StartTransaction() fails, neither CommitTransaction() or
 RollbackTransaction() should be called.

 If an error occurs after a successful StartTransaction(), the whole transaction
 may or may not be implicitly canceled, depending on drivers. (e.g.  the PG
 driver will cancel it, SQLite/GPKG not). In any case, in the event of an error,
 an explicit call to RollbackTransaction() should be done to keep things
 balanced.

 By default, when bForce is set to FALSE, only "efficient" transactions will be
 attempted. Some drivers may offer an emulation of transactions, but sometimes
 with significant overhead, in which case the user must explicitly allow for
 such an emulation by setting bForce to TRUE. Drivers that offer emulated
 transactions should advertise the ODsCEmulatedTransactions capability (and not
 ODsCTransactions).

 This function is the same as the C function GDALDatasetStartTransaction().

 @param bForce can be set to TRUE if an emulation, possibly slow, of a
 transaction
               mechanism is acceptable.

 @return OGRERR_NONE on success.
 @since GDAL 2.0
*/

OGRErr GDALDataset::StartTransaction(CPL_UNUSED int bForce)
{
    return OGRERR_UNSUPPORTED_OPERATION;
}

/************************************************************************/
/*                      GDALDatasetStartTransaction()                   */
/************************************************************************/

/**
 \brief For datasources which support transactions, StartTransaction creates a
 transaction.

 If starting the transaction fails, will return
 OGRERR_FAILURE. Datasources which do not support transactions will
 always return OGRERR_UNSUPPORTED_OPERATION.

 Nested transactions are not supported.

 All changes done after the start of the transaction are definitely applied in
 the datasource if CommitTransaction() is called. They may be canceled by
 calling RollbackTransaction() instead.

 At the time of writing, transactions only apply on vector layers.

 Datasets that support transactions will advertise the ODsCTransactions
 capability.
 Use of transactions at dataset level is generally preferred to transactions at
 layer level, whose scope is rarely limited to the layer from which it was
 started.

 In case StartTransaction() fails, neither CommitTransaction() or
 RollbackTransaction() should be called.

 If an error occurs after a successful StartTransaction(), the whole
 transaction may or may not be implicitly canceled, depending on drivers. (e.g.
 the PG driver will cancel it, SQLite/GPKG not). In any case, in the event of an
 error, an explicit call to RollbackTransaction() should be done to keep things
 balanced.

 By default, when bForce is set to FALSE, only "efficient" transactions will be
 attempted. Some drivers may offer an emulation of transactions, but sometimes
 with significant overhead, in which case the user must explicitly allow for
 such an emulation by setting bForce to TRUE. Drivers that offer emulated
 transactions should advertise the ODsCEmulatedTransactions capability (and not
 ODsCTransactions).

 This function is the same as the C++ method GDALDataset::StartTransaction()

 @param hDS the dataset handle.
 @param bForce can be set to TRUE if an emulation, possibly slow, of a
 transaction
               mechanism is acceptable.

 @return OGRERR_NONE on success.
 @since GDAL 2.0
*/
OGRErr GDALDatasetStartTransaction(GDALDatasetH hDS, int bForce)
{
    VALIDATE_POINTER1(hDS, "GDALDatasetStartTransaction",
                      OGRERR_INVALID_HANDLE);

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_Dataset_StartTransaction(hDS, bForce);
#endif

    return GDALDataset::FromHandle(hDS)->StartTransaction(bForce);
}

/************************************************************************/
/*                           CommitTransaction()                        */
/************************************************************************/

/**
 \brief For datasources which support transactions, CommitTransaction commits a
 transaction.

 If no transaction is active, or the commit fails, will return
 OGRERR_FAILURE. Datasources which do not support transactions will
 always return OGRERR_UNSUPPORTED_OPERATION.

 Depending on drivers, this may or may not abort layer sequential readings that
 are active.

 This function is the same as the C function GDALDatasetCommitTransaction().

 @return OGRERR_NONE on success.
 @since GDAL 2.0
*/
OGRErr GDALDataset::CommitTransaction()
{
    return OGRERR_UNSUPPORTED_OPERATION;
}

/************************************************************************/
/*                        GDALDatasetCommitTransaction()                */
/************************************************************************/

/**
 \brief For datasources which support transactions, CommitTransaction commits a
 transaction.

 If no transaction is active, or the commit fails, will return
 OGRERR_FAILURE. Datasources which do not support transactions will
 always return OGRERR_UNSUPPORTED_OPERATION.

 Depending on drivers, this may or may not abort layer sequential readings that
 are active.

 This function is the same as the C++ method GDALDataset::CommitTransaction()

 @return OGRERR_NONE on success.
 @since GDAL 2.0
*/
OGRErr GDALDatasetCommitTransaction(GDALDatasetH hDS)
{
    VALIDATE_POINTER1(hDS, "GDALDatasetCommitTransaction",
                      OGRERR_INVALID_HANDLE);

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_Dataset_CommitTransaction(hDS);
#endif

    return GDALDataset::FromHandle(hDS)->CommitTransaction();
}

/************************************************************************/
/*                           RollbackTransaction()                      */
/************************************************************************/

/**
 \brief For datasources which support transactions, RollbackTransaction will
 roll back a datasource to its state before the start of the current
 transaction.
 If no transaction is active, or the rollback fails, will return
 OGRERR_FAILURE. Datasources which do not support transactions will
 always return OGRERR_UNSUPPORTED_OPERATION.

 This function is the same as the C function GDALDatasetRollbackTransaction().

 @return OGRERR_NONE on success.
 @since GDAL 2.0
*/
OGRErr GDALDataset::RollbackTransaction()
{
    return OGRERR_UNSUPPORTED_OPERATION;
}

/************************************************************************/
/*                     GDALDatasetRollbackTransaction()                 */
/************************************************************************/

/**
 \brief For datasources which support transactions, RollbackTransaction will
 roll back a datasource to its state before the start of the current
 transaction.
 If no transaction is active, or the rollback fails, will return
 OGRERR_FAILURE. Datasources which do not support transactions will
 always return OGRERR_UNSUPPORTED_OPERATION.

 This function is the same as the C++ method GDALDataset::RollbackTransaction().

 @return OGRERR_NONE on success.
 @since GDAL 2.0
*/
OGRErr GDALDatasetRollbackTransaction(GDALDatasetH hDS)
{
    VALIDATE_POINTER1(hDS, "GDALDatasetRollbackTransaction",
                      OGRERR_INVALID_HANDLE);

#ifdef OGRAPISPY_ENABLED
    if (bOGRAPISpyEnabled)
        OGRAPISpy_Dataset_RollbackTransaction(hDS);
#endif

    return GDALDataset::FromHandle(hDS)->RollbackTransaction();
}

//! @cond Doxygen_Suppress

/************************************************************************/
/*                   ShareLockWithParentDataset()                       */
/************************************************************************/

/* To be used typically by the GTiff driver to link overview datasets */
/* with their main dataset, so that they share the same lock */
/* Cf https://github.com/OSGeo/gdal/issues/1488 */
/* The parent dataset should remain alive while the this dataset is alive */

void GDALDataset::ShareLockWithParentDataset(GDALDataset *poParentDataset)
{
    if (m_poPrivate != nullptr)
    {
        m_poPrivate->poParentDataset = poParentDataset;
    }
}

/************************************************************************/
/*                   SetQueryLoggerFunc()                               */
/************************************************************************/

bool GDALDataset::SetQueryLoggerFunc(CPL_UNUSED GDALQueryLoggerFunc callback,
                                     CPL_UNUSED void *context)
{
    return false;
}

/************************************************************************/
/*                          EnterReadWrite()                            */
/************************************************************************/

int GDALDataset::EnterReadWrite(GDALRWFlag eRWFlag)
{
    if (m_poPrivate == nullptr ||
        IsThreadSafe(GDAL_OF_RASTER | (nOpenFlags & GDAL_OF_UPDATE)))
        return FALSE;

    if (m_poPrivate->poParentDataset)
        return m_poPrivate->poParentDataset->EnterReadWrite(eRWFlag);

    if (eAccess == GA_Update)
    {
        if (m_poPrivate->eStateReadWriteMutex ==
            GDALAllowReadWriteMutexState::RW_MUTEX_STATE_UNKNOWN)
        {
            // In case dead-lock would occur, which is not impossible,
            // this can be used to prevent it, but at the risk of other
            // issues.
            if (CPLTestBool(
                    CPLGetConfigOption("GDAL_ENABLE_READ_WRITE_MUTEX", "YES")))
            {
                m_poPrivate->eStateReadWriteMutex =
                    GDALAllowReadWriteMutexState::RW_MUTEX_STATE_ALLOWED;
            }
            else
            {
                m_poPrivate->eStateReadWriteMutex =
                    GDALAllowReadWriteMutexState::RW_MUTEX_STATE_DISABLED;
            }
        }
        if (m_poPrivate->eStateReadWriteMutex ==
            GDALAllowReadWriteMutexState::RW_MUTEX_STATE_ALLOWED)
        {
            // There should be no race related to creating this mutex since
            // it should be first created through IWriteBlock() / IRasterIO()
            // and then GDALRasterBlock might call it from another thread.
#ifdef DEBUG_VERBOSE
            CPLDebug("GDAL",
                     "[Thread " CPL_FRMT_GIB "] Acquiring RW mutex for %s",
                     CPLGetPID(), GetDescription());
#endif
            CPLCreateOrAcquireMutex(&(m_poPrivate->hMutex), 1000.0);

            const int nCountMutex =
                m_poPrivate->oMapThreadToMutexTakenCount[CPLGetPID()]++;
            if (nCountMutex == 0 && eRWFlag == GF_Read)
            {
                CPLReleaseMutex(m_poPrivate->hMutex);
                for (int i = 0; i < nBands; i++)
                {
                    auto blockCache = papoBands[i]->poBandBlockCache;
                    if (blockCache)
                        blockCache->WaitCompletionPendingTasks();
                }
                CPLCreateOrAcquireMutex(&(m_poPrivate->hMutex), 1000.0);
            }

            return TRUE;
        }
    }
    return FALSE;
}

/************************************************************************/
/*                         LeaveReadWrite()                             */
/************************************************************************/

void GDALDataset::LeaveReadWrite()
{
    if (m_poPrivate)
    {
        if (m_poPrivate->poParentDataset)
        {
            m_poPrivate->poParentDataset->LeaveReadWrite();
            return;
        }

        m_poPrivate->oMapThreadToMutexTakenCount[CPLGetPID()]--;
        CPLReleaseMutex(m_poPrivate->hMutex);
#ifdef DEBUG_VERBOSE
        CPLDebug("GDAL", "[Thread " CPL_FRMT_GIB "] Releasing RW mutex for %s",
                 CPLGetPID(), GetDescription());
#endif
    }
}

/************************************************************************/
/*                           InitRWLock()                               */
/************************************************************************/

void GDALDataset::InitRWLock()
{
    if (m_poPrivate)
    {
        if (m_poPrivate->poParentDataset)
        {
            m_poPrivate->poParentDataset->InitRWLock();
            return;
        }

        if (m_poPrivate->eStateReadWriteMutex ==
            GDALAllowReadWriteMutexState::RW_MUTEX_STATE_UNKNOWN)
        {
            if (EnterReadWrite(GF_Write))
                LeaveReadWrite();
        }
    }
}

/************************************************************************/
/*                       DisableReadWriteMutex()                        */
/************************************************************************/

// The mutex logic is broken in multi-threaded situations, for example
// with 2 WarpedVRT datasets being read at the same time. In that
// particular case, the mutex is not needed, so allow the VRTWarpedDataset code
// to disable it.
void GDALDataset::DisableReadWriteMutex()
{
    if (m_poPrivate)
    {
        if (m_poPrivate->poParentDataset)
        {
            m_poPrivate->poParentDataset->DisableReadWriteMutex();
            return;
        }

        m_poPrivate->eStateReadWriteMutex =
            GDALAllowReadWriteMutexState::RW_MUTEX_STATE_DISABLED;
    }
}

/************************************************************************/
/*                      TemporarilyDropReadWriteLock()                  */
/************************************************************************/

void GDALDataset::TemporarilyDropReadWriteLock()
{
    if (m_poPrivate == nullptr)
        return;

    if (m_poPrivate->poParentDataset)
    {
        m_poPrivate->poParentDataset->TemporarilyDropReadWriteLock();
        return;
    }

#ifndef __COVERITY__
    if (m_poPrivate->hMutex)
    {
#ifdef DEBUG_VERBOSE
        CPLDebug("GDAL",
                 "[Thread " CPL_FRMT_GIB "] "
                 "Temporarily drop RW mutex for %s",
                 CPLGetPID(), GetDescription());
#endif
        CPLAcquireMutex(m_poPrivate->hMutex, 1000.0);
        const int nCount =
            m_poPrivate->oMapThreadToMutexTakenCount[CPLGetPID()];
#ifdef DEBUG_EXTRA
        m_poPrivate->oMapThreadToMutexTakenCountSaved[CPLGetPID()] = nCount;
#endif
        for (int i = 0; i < nCount + 1; i++)
        {
            // The mutex is recursive
            CPLReleaseMutex(m_poPrivate->hMutex);
        }
    }
#endif
}

/************************************************************************/
/*                       ReacquireReadWriteLock()                       */
/************************************************************************/

void GDALDataset::ReacquireReadWriteLock()
{
    if (m_poPrivate == nullptr)
        return;

    if (m_poPrivate->poParentDataset)
    {
        m_poPrivate->poParentDataset->ReacquireReadWriteLock();
        return;
    }

#ifndef __COVERITY__
    if (m_poPrivate->hMutex)
    {
#ifdef DEBUG_VERBOSE
        CPLDebug("GDAL",
                 "[Thread " CPL_FRMT_GIB "] "
                 "Reacquire temporarily dropped RW mutex for %s",
                 CPLGetPID(), GetDescription());
#endif
        CPLAcquireMutex(m_poPrivate->hMutex, 1000.0);
        const int nCount =
            m_poPrivate->oMapThreadToMutexTakenCount[CPLGetPID()];
#ifdef DEBUG_EXTRA
        CPLAssert(nCount ==
                  m_poPrivate->oMapThreadToMutexTakenCountSaved[CPLGetPID()]);
#endif
        if (nCount == 0)
            CPLReleaseMutex(m_poPrivate->hMutex);
        for (int i = 0; i < nCount - 1; i++)
        {
            // The mutex is recursive
            CPLAcquireMutex(m_poPrivate->hMutex, 1000.0);
        }
    }
#endif
}

/************************************************************************/
/*                           AcquireMutex()                             */
/************************************************************************/

int GDALDataset::AcquireMutex()
{
    if (m_poPrivate == nullptr)
        return 0;
    if (m_poPrivate->poParentDataset)
    {
        return m_poPrivate->poParentDataset->AcquireMutex();
    }

    return CPLCreateOrAcquireMutex(&(m_poPrivate->hMutex), 1000.0);
}

/************************************************************************/
/*                          ReleaseMutex()                              */
/************************************************************************/

void GDALDataset::ReleaseMutex()
{
    if (m_poPrivate)
    {
        if (m_poPrivate->poParentDataset)
        {
            m_poPrivate->poParentDataset->ReleaseMutex();
            return;
        }

        CPLReleaseMutex(m_poPrivate->hMutex);
    }
}

//! @endcond

/************************************************************************/
/*              GDALDataset::Features::Iterator::Private                */
/************************************************************************/

struct GDALDataset::Features::Iterator::Private
{
    GDALDataset::FeatureLayerPair m_oPair{};
    GDALDataset *m_poDS = nullptr;
    bool m_bEOF = true;
};

GDALDataset::Features::Iterator::Iterator(GDALDataset *poDS, bool bStart)
    : m_poPrivate(new GDALDataset::Features::Iterator::Private())
{
    m_poPrivate->m_poDS = poDS;
    if (bStart)
    {
        poDS->ResetReading();
        m_poPrivate->m_oPair.feature.reset(poDS->GetNextFeature(
            &m_poPrivate->m_oPair.layer, nullptr, nullptr, nullptr));
        m_poPrivate->m_bEOF = m_poPrivate->m_oPair.feature == nullptr;
    }
}

GDALDataset::Features::Iterator::~Iterator() = default;

const GDALDataset::FeatureLayerPair &
GDALDataset::Features::Iterator::operator*() const
{
    return m_poPrivate->m_oPair;
}

GDALDataset::Features::Iterator &GDALDataset::Features::Iterator::operator++()
{
    m_poPrivate->m_oPair.feature.reset(m_poPrivate->m_poDS->GetNextFeature(
        &m_poPrivate->m_oPair.layer, nullptr, nullptr, nullptr));
    m_poPrivate->m_bEOF = m_poPrivate->m_oPair.feature == nullptr;
    return *this;
}

bool GDALDataset::Features::Iterator::operator!=(const Iterator &it) const
{
    return m_poPrivate->m_bEOF != it.m_poPrivate->m_bEOF;
}

/************************************************************************/
/*                            GetFeatures()                             */
/************************************************************************/

/** Function that return an iterable object over features in the dataset
 * layer.
 *
 * This is a C++ iterator friendly version of GetNextFeature().
 *
 * Using this iterator for standard range-based loops is safe, but
 * due to implementation limitations, you shouldn't try to access
 * (dereference) more than one iterator step at a time, since the
 * FeatureLayerPair reference which is returned is reused.
 *
 * Typical use is:
 * \code{.cpp}
 * for( auto&& oFeatureLayerPair: poDS->GetFeatures() )
 * {
 *       std::cout << "Feature of layer " <<
 *               oFeatureLayerPair.layer->GetName() << std::endl;
 *       oFeatureLayerPair.feature->DumpReadable();
 * }
 * \endcode
 *
 * @see GetNextFeature()
 *
 * @since GDAL 2.3
 */
GDALDataset::Features GDALDataset::GetFeatures()
{
    return Features(this);
}

/************************************************************************/
/*                                 begin()                              */
/************************************************************************/

/**
 \brief Return beginning of feature iterator.

 @since GDAL 2.3
*/

const GDALDataset::Features::Iterator GDALDataset::Features::begin() const
{
    return {m_poSelf, true};
}

/************************************************************************/
/*                                  end()                               */
/************************************************************************/

/**
 \brief Return end of feature iterator.

 @since GDAL 2.3
*/

const GDALDataset::Features::Iterator GDALDataset::Features::end() const
{
    return {m_poSelf, false};
}

/************************************************************************/
/*               GDALDataset::Layers::Iterator::Private                 */
/************************************************************************/

struct GDALDataset::Layers::Iterator::Private
{
    OGRLayer *m_poLayer = nullptr;
    int m_iCurLayer = 0;
    int m_nLayerCount = 0;
    GDALDataset *m_poDS = nullptr;
};

GDALDataset::Layers::Iterator::Iterator() : m_poPrivate(new Private())
{
}

// False positive of cppcheck 1.72
// cppcheck-suppress uninitMemberVar
GDALDataset::Layers::Iterator::Iterator(const Iterator &oOther)
    : m_poPrivate(new Private(*(oOther.m_poPrivate)))
{
}

GDALDataset::Layers::Iterator::Iterator(Iterator &&oOther) noexcept
    : m_poPrivate(std::move(oOther.m_poPrivate))
{
}

GDALDataset::Layers::Iterator::Iterator(GDALDataset *poDS, bool bStart)
    : m_poPrivate(new Private())
{
    m_poPrivate->m_poDS = poDS;
    m_poPrivate->m_nLayerCount = poDS->GetLayerCount();
    if (bStart)
    {
        if (m_poPrivate->m_nLayerCount)
            m_poPrivate->m_poLayer = poDS->GetLayer(0);
    }
    else
    {
        m_poPrivate->m_iCurLayer = m_poPrivate->m_nLayerCount;
    }
}

GDALDataset::Layers::Iterator::~Iterator() = default;

// False positive of cppcheck 1.72
// cppcheck-suppress operatorEqVarError
GDALDataset::Layers::Iterator &
GDALDataset::Layers::Iterator::operator=(const Iterator &oOther)
{
    *m_poPrivate = *oOther.m_poPrivate;
    return *this;
}

GDALDataset::Layers::Iterator &GDALDataset::Layers::Iterator::operator=(
    GDALDataset::Layers::Iterator &&oOther) noexcept
{
    m_poPrivate = std::move(oOther.m_poPrivate);
    return *this;
}

OGRLayer *GDALDataset::Layers::Iterator::operator*() const
{
    return m_poPrivate->m_poLayer;
}

GDALDataset::Layers::Iterator &GDALDataset::Layers::Iterator::operator++()
{
    m_poPrivate->m_iCurLayer++;
    if (m_poPrivate->m_iCurLayer < m_poPrivate->m_nLayerCount)
    {
        m_poPrivate->m_poLayer =
            m_poPrivate->m_poDS->GetLayer(m_poPrivate->m_iCurLayer);
    }
    else
    {
        m_poPrivate->m_poLayer = nullptr;
    }
    return *this;
}

GDALDataset::Layers::Iterator GDALDataset::Layers::Iterator::operator++(int)
{
    GDALDataset::Layers::Iterator temp = *this;
    ++(*this);
    return temp;
}

bool GDALDataset::Layers::Iterator::operator!=(const Iterator &it) const
{
    return m_poPrivate->m_iCurLayer != it.m_poPrivate->m_iCurLayer;
}

/************************************************************************/
/*                             GetLayers()                              */
/************************************************************************/

/** Function that returns an iterable object over layers in the dataset.
 *
 * This is a C++ iterator friendly version of GetLayer().
 *
 * Typical use is:
 * \code{.cpp}
 * for( auto&& poLayer: poDS->GetLayers() )
 * {
 *       std::cout << "Layer  << poLayer->GetName() << std::endl;
 * }
 * \endcode
 *
 * @see GetLayer()
 *
 * @since GDAL 2.3
 */
GDALDataset::Layers GDALDataset::GetLayers()
{
    return Layers(this);
}

/************************************************************************/
/*                                 begin()                              */
/************************************************************************/

/**
 \brief Return beginning of layer iterator.

 @since GDAL 2.3
*/

GDALDataset::Layers::Iterator GDALDataset::Layers::begin() const
{
    return {m_poSelf, true};
}

/************************************************************************/
/*                                  end()                               */
/************************************************************************/

/**
 \brief Return end of layer iterator.

 @since GDAL 2.3
*/

GDALDataset::Layers::Iterator GDALDataset::Layers::end() const
{
    return {m_poSelf, false};
}

/************************************************************************/
/*                                  size()                             */
/************************************************************************/

/**
 \brief Get the number of layers in this dataset.

 @return layer count.

 @since GDAL 2.3
*/

size_t GDALDataset::Layers::size() const
{
    return static_cast<size_t>(m_poSelf->GetLayerCount());
}

/************************************************************************/
/*                                operator[]()                          */
/************************************************************************/
/**
 \brief Fetch a layer by index.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 @param iLayer a layer number between 0 and size()-1.

 @return the layer, or nullptr if iLayer is out of range or an error occurs.

 @since GDAL 2.3
*/

OGRLayer *GDALDataset::Layers::operator[](int iLayer)
{
    return m_poSelf->GetLayer(iLayer);
}

/************************************************************************/
/*                                operator[]()                          */
/************************************************************************/
/**
 \brief Fetch a layer by index.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 @param iLayer a layer number between 0 and size()-1.

 @return the layer, or nullptr if iLayer is out of range or an error occurs.

 @since GDAL 2.3
*/

OGRLayer *GDALDataset::Layers::operator[](size_t iLayer)
{
    return m_poSelf->GetLayer(static_cast<int>(iLayer));
}

/************************************************************************/
/*                                operator[]()                          */
/************************************************************************/
/**
 \brief Fetch a layer by name.

 The returned layer remains owned by the
 GDALDataset and should not be deleted by the application.

 @param pszLayerName layer name

 @return the layer, or nullptr if pszLayerName does not match with a layer

 @since GDAL 2.3
*/

OGRLayer *GDALDataset::Layers::operator[](const char *pszLayerName)
{
    return m_poSelf->GetLayerByName(pszLayerName);
}

/************************************************************************/
/*               GDALDataset::Bands::Iterator::Private                 */
/************************************************************************/

struct GDALDataset::Bands::Iterator::Private
{
    GDALRasterBand *m_poBand = nullptr;
    int m_iCurBand = 0;
    int m_nBandCount = 0;
    GDALDataset *m_poDS = nullptr;
};

GDALDataset::Bands::Iterator::Iterator(GDALDataset *poDS, bool bStart)
    : m_poPrivate(new GDALDataset::Bands::Iterator::Private())
{
    m_poPrivate->m_poDS = poDS;
    m_poPrivate->m_nBandCount = poDS->GetRasterCount();
    if (bStart)
    {
        if (m_poPrivate->m_nBandCount)
            m_poPrivate->m_poBand = poDS->GetRasterBand(1);
    }
    else
    {
        m_poPrivate->m_iCurBand = m_poPrivate->m_nBandCount;
    }
}

GDALDataset::Bands::Iterator::~Iterator() = default;

GDALRasterBand *GDALDataset::Bands::Iterator::operator*()
{
    return m_poPrivate->m_poBand;
}

GDALDataset::Bands::Iterator &GDALDataset::Bands::Iterator::operator++()
{
    m_poPrivate->m_iCurBand++;
    if (m_poPrivate->m_iCurBand < m_poPrivate->m_nBandCount)
    {
        m_poPrivate->m_poBand =
            m_poPrivate->m_poDS->GetRasterBand(1 + m_poPrivate->m_iCurBand);
    }
    else
    {
        m_poPrivate->m_poBand = nullptr;
    }
    return *this;
}

bool GDALDataset::Bands::Iterator::operator!=(const Iterator &it) const
{
    return m_poPrivate->m_iCurBand != it.m_poPrivate->m_iCurBand;
}

/************************************************************************/
/*                            GetBands()                           */
/************************************************************************/

/** Function that returns an iterable object over GDALRasterBand in the dataset.
 *
 * This is a C++ iterator friendly version of GetRasterBand().
 *
 * Typical use is:
 * \code{.cpp}
 * for( auto&& poBand: poDS->GetBands() )
 * {
 *       std::cout << "Band  << poBand->GetDescription() << std::endl;
 * }
 * \endcode
 *
 * @see GetRasterBand()
 *
 * @since GDAL 2.3
 */
GDALDataset::Bands GDALDataset::GetBands()
{
    return Bands(this);
}

/************************************************************************/
/*                                 begin()                              */
/************************************************************************/

/**
 \brief Return beginning of band iterator.

 @since GDAL 2.3
*/

const GDALDataset::Bands::Iterator GDALDataset::Bands::begin() const
{
    return {m_poSelf, true};
}

/************************************************************************/
/*                                  end()                               */
/************************************************************************/

/**
 \brief Return end of band iterator.

 @since GDAL 2.3
*/

const GDALDataset::Bands::Iterator GDALDataset::Bands::end() const
{
    return {m_poSelf, false};
}

/************************************************************************/
/*                                  size()                             */
/************************************************************************/

/**
 \brief Get the number of raster bands in this dataset.

 @return raster band count.

 @since GDAL 2.3
*/

size_t GDALDataset::Bands::size() const
{
    return static_cast<size_t>(m_poSelf->GetRasterCount());
}

/************************************************************************/
/*                                operator[]()                          */
/************************************************************************/
/**
 \brief Fetch a raster band by index.

 The returned band remains owned by the
 GDALDataset and should not be deleted by the application.

 @warning Contrary to GDALDataset::GetRasterBand(), the indexing here is
 consistent with the conventions of C/C++, i.e. starting at 0.

 @param iBand a band index between 0 and size()-1.

 @return the band, or nullptr if iBand is out of range or an error occurs.

 @since GDAL 2.3
*/

GDALRasterBand *GDALDataset::Bands::operator[](int iBand)
{
    return m_poSelf->GetRasterBand(1 + iBand);
}

/************************************************************************/
/*                                operator[]()                          */
/************************************************************************/

/**
 \brief Fetch a raster band by index.

 The returned band remains owned by the
 GDALDataset and should not be deleted by the application.

 @warning Contrary to GDALDataset::GetRasterBand(), the indexing here is
 consistent with the conventions of C/C++, i.e. starting at 0.

 @param iBand a band index between 0 and size()-1.

 @return the band, or nullptr if iBand is out of range or an error occurs.

 @since GDAL 2.3
*/

GDALRasterBand *GDALDataset::Bands::operator[](size_t iBand)
{
    return m_poSelf->GetRasterBand(1 + static_cast<int>(iBand));
}

/************************************************************************/
/*                           GetRootGroup()                             */
/************************************************************************/

/**
 \brief Return the root GDALGroup of this dataset.

 Only valid for multidimensional datasets.

 This is the same as the C function GDALDatasetGetRootGroup().

 @since GDAL 3.1
*/

std::shared_ptr<GDALGroup> GDALDataset::GetRootGroup() const
{
    return nullptr;
}

/************************************************************************/
/*                        GetRawBinaryLayout()                          */
/************************************************************************/

//! @cond Doxygen_Suppress
/**
 \brief Return the layout of a dataset that can be considered as a raw binary
 format.

 @param sLayout Structure that will be set if the dataset is a raw binary one.
 @return true if the dataset is a raw binary one.
 @since GDAL 3.1
*/

bool GDALDataset::GetRawBinaryLayout(RawBinaryLayout &sLayout)
{
    CPL_IGNORE_RET_VAL(sLayout);
    return false;
}

//! @endcond

/************************************************************************/
/*                          ClearStatistics()                           */
/************************************************************************/

/**
 \brief Clear statistics

 Only implemented for now in PAM supported datasets

 This is the same as the C function GDALDatasetClearStatistics().

 @since GDAL 3.2
*/

void GDALDataset::ClearStatistics()
{
    auto poRootGroup = GetRootGroup();
    if (poRootGroup)
        poRootGroup->ClearStatistics();
}

/************************************************************************/
/*                        GDALDatasetClearStatistics()                  */
/************************************************************************/

/**
 \brief Clear statistics

 This is the same as the C++ method GDALDataset::ClearStatistics().

 @since GDAL 3.2
*/

void GDALDatasetClearStatistics(GDALDatasetH hDS)
{
    VALIDATE_POINTER0(hDS, __func__);
    GDALDataset::FromHandle(hDS)->ClearStatistics();
}

/************************************************************************/
/*                        GetFieldDomainNames()                         */
/************************************************************************/

/** Returns a list of the names of all field domains stored in the dataset.
 *
 * @note The default implementation assumes that drivers fully populate
 * m_oMapFieldDomains when opening a dataset. If this assumption is incorrect
 * then a specialized implementation of GetFieldDomainNames() must be
 * implemented.
 *
 * @param papszOptions Driver specific options determining how attributes
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return list of field domain names
 * @since GDAL 3.5
 */
std::vector<std::string>
GDALDataset::GetFieldDomainNames(CPL_UNUSED CSLConstList papszOptions) const
{

    std::vector<std::string> names;
    names.reserve(m_oMapFieldDomains.size());
    for (const auto &it : m_oMapFieldDomains)
    {
        names.emplace_back(it.first);
    }
    return names;
}

/************************************************************************/
/*                      GDALDatasetGetFieldDomainNames()                */
/************************************************************************/

/** Returns a list of the names of all field domains stored in the dataset.
 *
 * This is the same as the C++ method GDALDataset::GetFieldDomainNames().
 *
 * @param hDS Dataset handle.
 * @param papszOptions Driver specific options determining how attributes
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return list of field domain names, to be freed with CSLDestroy()
 * @since GDAL 3.5
 */
char **GDALDatasetGetFieldDomainNames(GDALDatasetH hDS,
                                      CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hDS, __func__, nullptr);
    auto names =
        GDALDataset::FromHandle(hDS)->GetFieldDomainNames(papszOptions);
    CPLStringList res;
    for (const auto &name : names)
    {
        res.AddString(name.c_str());
    }
    return res.StealList();
}

/************************************************************************/
/*                        GetFieldDomain()                              */
/************************************************************************/

/** Get a field domain from its name.
 *
 * @return the field domain, or nullptr if not found.
 * @since GDAL 3.3
 */
const OGRFieldDomain *GDALDataset::GetFieldDomain(const std::string &name) const
{
    const auto iter = m_oMapFieldDomains.find(name);
    if (iter == m_oMapFieldDomains.end())
        return nullptr;
    return iter->second.get();
}

/************************************************************************/
/*                      GDALDatasetGetFieldDomain()                     */
/************************************************************************/

/** Get a field domain from its name.
 *
 * This is the same as the C++ method GDALDataset::GetFieldDomain().
 *
 * @param hDS Dataset handle.
 * @param pszName Name of field domain.
 * @return the field domain (ownership remains to the dataset), or nullptr if
 * not found.
 * @since GDAL 3.3
 */
OGRFieldDomainH GDALDatasetGetFieldDomain(GDALDatasetH hDS, const char *pszName)
{
    VALIDATE_POINTER1(hDS, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    return OGRFieldDomain::ToHandle(const_cast<OGRFieldDomain *>(
        GDALDataset::FromHandle(hDS)->GetFieldDomain(pszName)));
}

/************************************************************************/
/*                         AddFieldDomain()                             */
/************************************************************************/

/** Add a field domain to the dataset.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of field domains.
 * At the time of writing (GDAL 3.3), only the Memory and GeoPackage drivers
 * support this operation. A dataset having at least some support for this
 * operation should report the ODsCAddFieldDomain dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @note Drivers should make sure to update m_oMapFieldDomains in order for the
 * default implementation of GetFieldDomainNames() to work correctly, or
 * alternatively a specialized implementation of GetFieldDomainNames() should be
 * implemented.
 *
 * @param domain The domain definition.
 * @param failureReason      Output parameter. Will contain an error message if
 *                           an error occurs.
 * @return true in case of success.
 * @since GDAL 3.3
 */
bool GDALDataset::AddFieldDomain(
    CPL_UNUSED std::unique_ptr<OGRFieldDomain> &&domain,
    std::string &failureReason)
{
    failureReason = "AddFieldDomain not supported by this driver";
    return false;
}

/************************************************************************/
/*                     GDALDatasetAddFieldDomain()                      */
/************************************************************************/

/** Add a field domain to the dataset.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of field domains.
 * At the time of writing (GDAL 3.3), only the Memory and GeoPackage drivers
 * support this operation. A dataset having at least some support for this
 * operation should report the ODsCAddFieldDomain dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the ppszFailureReason output
 * parameter.
 *
 * @param hDS                Dataset handle.
 * @param hFieldDomain       The domain definition. Contrary to the C++ version,
 *                           the passed object is copied.
 * @param ppszFailureReason  Output parameter. Will contain an error message if
 *                           an error occurs (*ppszFailureReason to be freed
 *                           with CPLFree). May be NULL.
 * @return true in case of success.
 * @since GDAL 3.3
 */
bool GDALDatasetAddFieldDomain(GDALDatasetH hDS, OGRFieldDomainH hFieldDomain,
                               char **ppszFailureReason)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    VALIDATE_POINTER1(hFieldDomain, __func__, false);
    auto poDomain = std::unique_ptr<OGRFieldDomain>(
        OGRFieldDomain::FromHandle(hFieldDomain)->Clone());
    if (poDomain == nullptr)
        return false;
    std::string failureReason;
    const bool bRet = GDALDataset::FromHandle(hDS)->AddFieldDomain(
        std::move(poDomain), failureReason);
    if (ppszFailureReason)
    {
        *ppszFailureReason =
            failureReason.empty() ? nullptr : CPLStrdup(failureReason.c_str());
    }
    return bRet;
}

/************************************************************************/
/*                        DeleteFieldDomain()                           */
/************************************************************************/

/** Removes a field domain from the dataset.
 *
 * Only a few drivers will support this operation.
 *
 * At the time of writing (GDAL 3.5), only the Memory and GeoPackage drivers
 * support this operation. A dataset having at least some support for this
 * operation should report the ODsCDeleteFieldDomain dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @note Drivers should make sure to update m_oMapFieldDomains in order for the
 * default implementation of GetFieldDomainNames() to work correctly, or
 * alternatively a specialized implementation of GetFieldDomainNames() should be
 * implemented.
 *
 * @param name The domain name.
 * @param failureReason      Output parameter. Will contain an error message if
 *                           an error occurs.
 * @return true in case of success.
 * @since GDAL 3.5
 */
bool GDALDataset::DeleteFieldDomain(CPL_UNUSED const std::string &name,
                                    std::string &failureReason)
{
    failureReason = "DeleteFieldDomain not supported by this driver";
    return false;
}

/************************************************************************/
/*                  GDALDatasetDeleteFieldDomain()                      */
/************************************************************************/

/** Removes a field domain from the dataset.
 *
 * Only a few drivers will support this operation.
 *
 * At the time of writing (GDAL 3.5), only the Memory and GeoPackage drivers
 * support this operation. A dataset having at least some support for this
 * operation should report the ODsCDeleteFieldDomain dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the ppszFailureReason output
 * parameter.
 *
 * @param hDS                Dataset handle.
 * @param pszName            The domain name.
 * @param ppszFailureReason  Output parameter. Will contain an error message if
 *                           an error occurs (*ppszFailureReason to be freed
 *                           with CPLFree). May be NULL.
 * @return true in case of success.
 * @since GDAL 3.3
 */
bool GDALDatasetDeleteFieldDomain(GDALDatasetH hDS, const char *pszName,
                                  char **ppszFailureReason)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    VALIDATE_POINTER1(pszName, __func__, false);
    std::string failureReason;
    const bool bRet =
        GDALDataset::FromHandle(hDS)->DeleteFieldDomain(pszName, failureReason);
    if (ppszFailureReason)
    {
        *ppszFailureReason =
            failureReason.empty() ? nullptr : CPLStrdup(failureReason.c_str());
    }
    return bRet;
}

/************************************************************************/
/*                       UpdateFieldDomain()                            */
/************************************************************************/

/** Updates an existing field domain by replacing its definition.
 *
 * The existing field domain with matching name will be replaced.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of field domains.
 * At the time of writing (GDAL 3.5), only the Memory driver
 * supports this operation. A dataset having at least some support for this
 * operation should report the ODsCUpdateFieldDomain dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @param domain The domain definition.
 * @param failureReason      Output parameter. Will contain an error message if
 *                           an error occurs.
 * @return true in case of success.
 * @since GDAL 3.5
 */
bool GDALDataset::UpdateFieldDomain(
    CPL_UNUSED std::unique_ptr<OGRFieldDomain> &&domain,
    std::string &failureReason)
{
    failureReason = "UpdateFieldDomain not supported by this driver";
    return false;
}

/************************************************************************/
/*                  GDALDatasetUpdateFieldDomain()                      */
/************************************************************************/

/** Updates an existing field domain by replacing its definition.
 *
 * The existing field domain with matching name will be replaced.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of field domains.
 * At the time of writing (GDAL 3.5), only the Memory driver
 * supports this operation. A dataset having at least some support for this
 * operation should report the ODsCUpdateFieldDomain dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @param hDS                Dataset handle.
 * @param hFieldDomain       The domain definition. Contrary to the C++ version,
 *                           the passed object is copied.
 * @param ppszFailureReason  Output parameter. Will contain an error message if
 *                           an error occurs (*ppszFailureReason to be freed
 *                           with CPLFree). May be NULL.
 * @return true in case of success.
 * @since GDAL 3.5
 */
bool GDALDatasetUpdateFieldDomain(GDALDatasetH hDS,
                                  OGRFieldDomainH hFieldDomain,
                                  char **ppszFailureReason)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    VALIDATE_POINTER1(hFieldDomain, __func__, false);
    auto poDomain = std::unique_ptr<OGRFieldDomain>(
        OGRFieldDomain::FromHandle(hFieldDomain)->Clone());
    if (poDomain == nullptr)
        return false;
    std::string failureReason;
    const bool bRet = GDALDataset::FromHandle(hDS)->UpdateFieldDomain(
        std::move(poDomain), failureReason);
    if (ppszFailureReason)
    {
        *ppszFailureReason =
            failureReason.empty() ? nullptr : CPLStrdup(failureReason.c_str());
    }
    return bRet;
}

/************************************************************************/
/*                        GetRelationshipNames()                        */
/************************************************************************/

/** Returns a list of the names of all relationships stored in the dataset.
 *
 * @param papszOptions Driver specific options determining how relationships
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return list of relationship names
 * @since GDAL 3.6
 */
std::vector<std::string>
GDALDataset::GetRelationshipNames(CPL_UNUSED CSLConstList papszOptions) const
{
    return {};
}

/************************************************************************/
/*                     GDALDatasetGetRelationshipNames()                */
/************************************************************************/

/** Returns a list of the names of all relationships stored in the dataset.
 *
 * This is the same as the C++ method GDALDataset::GetRelationshipNames().
 *
 * @param hDS Dataset handle.
 * @param papszOptions Driver specific options determining how relationships
 * should be retrieved. Pass nullptr for default behavior.
 *
 * @return list of relationship names, to be freed with CSLDestroy()
 * @since GDAL 3.6
 */
char **GDALDatasetGetRelationshipNames(GDALDatasetH hDS,
                                       CSLConstList papszOptions)
{
    VALIDATE_POINTER1(hDS, __func__, nullptr);
    auto names =
        GDALDataset::FromHandle(hDS)->GetRelationshipNames(papszOptions);
    CPLStringList res;
    for (const auto &name : names)
    {
        res.AddString(name.c_str());
    }
    return res.StealList();
}

/************************************************************************/
/*                        GetRelationship()                             */
/************************************************************************/

/** Get a relationship from its name.
 *
 * @return the relationship, or nullptr if not found.
 * @since GDAL 3.6
 */
const GDALRelationship *
GDALDataset::GetRelationship(CPL_UNUSED const std::string &name) const
{
    return nullptr;
}

/************************************************************************/
/*                      GDALDatasetGetRelationship()                    */
/************************************************************************/

/** Get a relationship from its name.
 *
 * This is the same as the C++ method GDALDataset::GetRelationship().
 *
 * @param hDS Dataset handle.
 * @param pszName Name of relationship.
 * @return the relationship (ownership remains to the dataset), or nullptr if
 * not found.
 * @since GDAL 3.6
 */
GDALRelationshipH GDALDatasetGetRelationship(GDALDatasetH hDS,
                                             const char *pszName)
{
    VALIDATE_POINTER1(hDS, __func__, nullptr);
    VALIDATE_POINTER1(pszName, __func__, nullptr);
    return GDALRelationship::ToHandle(const_cast<GDALRelationship *>(
        GDALDataset::FromHandle(hDS)->GetRelationship(pszName)));
}

/************************************************************************/
/*                         AddRelationship()                            */
/************************************************************************/

/** Add a relationship to the dataset.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of relationships.
 *
 * A dataset having at least some support for this
 * operation should report the GDsCAddRelationship dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * When adding a many-to-many relationship
 * (GDALRelationshipCardinality::GRC_MANY_TO_MANY), it is possible to omit the
 * mapping table name (see GDALRelationship::GetMappingTableName) to instruct
 * the driver to create an appropriately named and structured mapping table.
 * Some dataset formats require particular naming conventions and field
 * structures for the mapping table, and delegating the construction of the
 * mapping table to the driver will avoid these pitfalls.
 *
 * @param relationship The relationship definition.
 * @param failureReason      Output parameter. Will contain an error message if
 *                           an error occurs.
 * @return true in case of success.
 * @since GDAL 3.6
 */
bool GDALDataset::AddRelationship(
    CPL_UNUSED std::unique_ptr<GDALRelationship> &&relationship,
    std::string &failureReason)
{
    failureReason = "AddRelationship not supported by this driver";
    return false;
}

/************************************************************************/
/*                     GDALDatasetAddRelationship()                     */
/************************************************************************/

/** Add a relationship to the dataset.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of relationships.
 *
 * A dataset having at least some support for this
 * operation should report the GDsCAddRelationship dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * When adding a many-to-many relationship
 * (GDALRelationshipCardinality::GRC_MANY_TO_MANY), it is possible to omit the
 * mapping table name (see GDALRelationshipGetMappingTableName) to instruct the
 * driver to create an appropriately named and structured mapping table. Some
 * dataset formats require particular naming conventions and field structures
 * for the mapping table, and delegating the construction of the mapping table
 * to the driver will avoid these pitfalls.
 *
 * @param hDS                Dataset handle.
 * @param hRelationship      The relationship definition. Contrary to the C++
 * version, the passed object is copied.
 * @param ppszFailureReason  Output parameter. Will contain an error message if
 *                           an error occurs (*ppszFailureReason to be freed
 *                           with CPLFree). May be NULL.
 * @return true in case of success.
 * @since GDAL 3.6
 */
bool GDALDatasetAddRelationship(GDALDatasetH hDS,
                                GDALRelationshipH hRelationship,
                                char **ppszFailureReason)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    VALIDATE_POINTER1(hRelationship, __func__, false);
    std::unique_ptr<GDALRelationship> poRelationship(
        new GDALRelationship(*GDALRelationship::FromHandle(hRelationship)));
    std::string failureReason;
    const bool bRet = GDALDataset::FromHandle(hDS)->AddRelationship(
        std::move(poRelationship), failureReason);
    if (ppszFailureReason)
    {
        *ppszFailureReason =
            failureReason.empty() ? nullptr : CPLStrdup(failureReason.c_str());
    }
    return bRet;
}

/************************************************************************/
/*                        DeleteRelationship()                          */
/************************************************************************/

/** Removes a relationship from the dataset.
 *
 * Only a few drivers will support this operation.
 *
 * A dataset having at least some support for this
 * operation should report the GDsCDeleteRelationship dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @param name The relationship name.
 * @param failureReason      Output parameter. Will contain an error message if
 *                           an error occurs.
 * @return true in case of success.
 * @since GDAL 3.6
 */
bool GDALDataset::DeleteRelationship(CPL_UNUSED const std::string &name,
                                     std::string &failureReason)
{
    failureReason = "DeleteRelationship not supported by this driver";
    return false;
}

/************************************************************************/
/*                  GDALDatasetDeleteRelationship()                     */
/************************************************************************/

/** Removes a relationship from the dataset.
 *
 * Only a few drivers will support this operation.
 *
 * A dataset having at least some support for this
 * operation should report the GDsCDeleteRelationship dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the ppszFailureReason output
 * parameter.
 *
 * @param hDS                Dataset handle.
 * @param pszName            The relationship name.
 * @param ppszFailureReason  Output parameter. Will contain an error message if
 *                           an error occurs (*ppszFailureReason to be freed
 *                           with CPLFree). May be NULL.
 * @return true in case of success.
 * @since GDAL 3.6
 */
bool GDALDatasetDeleteRelationship(GDALDatasetH hDS, const char *pszName,
                                   char **ppszFailureReason)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    VALIDATE_POINTER1(pszName, __func__, false);
    std::string failureReason;
    const bool bRet = GDALDataset::FromHandle(hDS)->DeleteRelationship(
        pszName, failureReason);
    if (ppszFailureReason)
    {
        *ppszFailureReason =
            failureReason.empty() ? nullptr : CPLStrdup(failureReason.c_str());
    }
    return bRet;
}

/************************************************************************/
/*                       UpdateRelationship()                           */
/************************************************************************/

/** Updates an existing relationship by replacing its definition.
 *
 * The existing relationship with matching name will be replaced.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of relationships.
 * A dataset having at least some support for this
 * operation should report the GDsCUpdateRelationship dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @param relationship   The relationship definition.
 * @param failureReason  Output parameter. Will contain an error message if
 *                       an error occurs.
 * @return true in case of success.
 * @since GDAL 3.6
 */
bool GDALDataset::UpdateRelationship(
    CPL_UNUSED std::unique_ptr<GDALRelationship> &&relationship,
    std::string &failureReason)
{
    failureReason = "UpdateRelationship not supported by this driver";
    return false;
}

/************************************************************************/
/*                  GDALDatasetUpdateRelationship()                     */
/************************************************************************/

/** Updates an existing relationship by replacing its definition.
 *
 * The existing relationship with matching name will be replaced.
 *
 * Only a few drivers will support this operation, and some of them might only
 * support it only for some types of relationships.
 * A dataset having at least some support for this
 * operation should report the GDsCUpdateRelationship dataset capability.
 *
 * Anticipated failures will not be emitted through the CPLError()
 * infrastructure, but will be reported in the failureReason output parameter.
 *
 * @param hDS                Dataset handle.
 * @param hRelationship      The relationship definition. Contrary to the C++
 * version, the passed object is copied.
 * @param ppszFailureReason  Output parameter. Will contain an error message if
 *                           an error occurs (*ppszFailureReason to be freed
 *                           with CPLFree). May be NULL.
 * @return true in case of success.
 * @since GDAL 3.5
 */
bool GDALDatasetUpdateRelationship(GDALDatasetH hDS,
                                   GDALRelationshipH hRelationship,
                                   char **ppszFailureReason)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    VALIDATE_POINTER1(hRelationship, __func__, false);
    std::unique_ptr<GDALRelationship> poRelationship(
        new GDALRelationship(*GDALRelationship::FromHandle(hRelationship)));
    std::string failureReason;
    const bool bRet = GDALDataset::FromHandle(hDS)->UpdateRelationship(
        std::move(poRelationship), failureReason);
    if (ppszFailureReason)
    {
        *ppszFailureReason =
            failureReason.empty() ? nullptr : CPLStrdup(failureReason.c_str());
    }
    return bRet;
}

/************************************************************************/
/*                  GDALDatasetSetQueryLoggerFunc()                     */
/************************************************************************/

/**
 * Sets the SQL query logger callback.
 *
 * When supported by the driver, the callback will be called with
 * the executed SQL text, the error message, the execution time in milliseconds,
 * the number of records fetched/affected and the client status data.
 *
 * A value of -1 in the execution time or in the number of records indicates
 * that the values are unknown.
 *
 * @param hDS                   Dataset handle.
 * @param pfnQueryLoggerFunc    Callback function
 * @param poQueryLoggerArg      Opaque client status data
 * @return                      true in case of success.
 * @since                       GDAL 3.7
 */
bool GDALDatasetSetQueryLoggerFunc(GDALDatasetH hDS,
                                   GDALQueryLoggerFunc pfnQueryLoggerFunc,
                                   void *poQueryLoggerArg)
{
    VALIDATE_POINTER1(hDS, __func__, false);
    return GDALDataset::FromHandle(hDS)->SetQueryLoggerFunc(pfnQueryLoggerFunc,
                                                            poQueryLoggerArg);
}

//! @cond Doxygen_Suppress

/************************************************************************/
/*                       SetEnableOverviews()                           */
/************************************************************************/

void GDALDataset::SetEnableOverviews(bool bEnable)
{
    if (m_poPrivate)
    {
        m_poPrivate->m_bOverviewsEnabled = bEnable;
    }
}

/************************************************************************/
/*                      AreOverviewsEnabled()                           */
/************************************************************************/

bool GDALDataset::AreOverviewsEnabled() const
{
    return m_poPrivate ? m_poPrivate->m_bOverviewsEnabled : true;
}

/************************************************************************/
/*                             IsAllBands()                             */
/************************************************************************/

bool GDALDataset::IsAllBands(int nBandCount, const int *panBandList) const
{
    if (nBands != nBandCount)
        return false;
    if (panBandList)
    {
        for (int i = 0; i < nBandCount; ++i)
        {
            if (panBandList[i] != i + 1)
                return false;
        }
    }
    return true;
}

//! @endcond

/************************************************************************/
/*                       GetCompressionFormats()                        */
/************************************************************************/

/** Return the compression formats that can be natively obtained for the
 * window of interest and requested bands.
 *
 * For example, a tiled dataset may be able to return data in a compressed
 * format if the window of interest matches exactly a tile. For some formats,
 * drivers may also be able to merge several tiles together (not currently
 * implemented though).
 *
 * Each format string is a pseudo MIME type, whose first part can be passed
 * as the pszFormat argument of ReadCompressedData(), with additional
 * parameters specified as key=value with a semi-colon separator.
 *
 * The amount and types of optional parameters passed after the MIME type is
 * format dependent, and driver dependent (some drivers might not be able to
 * return those extra information without doing a rather costly processing).
 *
 * For example, a driver might return "JPEG;frame_type=SOF0_baseline;"
 * "bit_depth=8;num_components=3;subsampling=4:2:0;colorspace=YCbCr", and
 * consequently "JPEG" can be passed as the pszFormat argument of
 * ReadCompressedData(). For JPEG, implementations can use the
 * GDALGetCompressionFormatForJPEG() helper method to generate a string like
 * above from a JPEG codestream.
 *
 * Several values might be returned. For example,
 * the JPEGXL driver will return "JXL", but also potentially "JPEG"
 * if the JPEGXL codestream includes a JPEG reconstruction box.
 *
 * In the general case this method will return an empty list.
 *
 * This is the same as C function GDALDatasetGetCompressionFormats().
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param nBandCount the number of bands being requested.
 *
 * @param panBandList the list of nBandCount band numbers.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands.
 *
 * @return a list of compatible formats (which may be empty)
 *
 * For example, to check if native compression format(s) are available on the
 * whole image:
 * \code{.cpp}
 *   const CPLStringList aosFormats =
 *      poDataset->GetCompressionFormats(0, 0,
 *                                       poDataset->GetRasterXSize(),
 *                                       poDataset->GetRasterYSize(),
 *                                       poDataset->GetRasterCount(),
 *                                       nullptr);
 *   for( const char* pszFormat: aosFormats )
 *   {
 *      // Remove optional parameters and just print out the MIME type.
 *      const CPLStringList aosTokens(CSLTokenizeString2(pszFormat, ";", 0));
 *      printf("Found format %s\n, aosTokens[0]);
 *   }
 * \endcode
 *
 * @since GDAL 3.7
 */
CPLStringList
GDALDataset::GetCompressionFormats(CPL_UNUSED int nXOff, CPL_UNUSED int nYOff,
                                   CPL_UNUSED int nXSize, CPL_UNUSED int nYSize,
                                   CPL_UNUSED int nBandCount,
                                   CPL_UNUSED const int *panBandList)
{
    return CPLStringList();
}

/************************************************************************/
/*                 GDALDatasetGetCompressionFormats()                   */
/************************************************************************/

/** Return the compression formats that can be natively obtained for the
 * window of interest and requested bands.
 *
 * For example, a tiled dataset may be able to return data in a compressed
 * format if the window of interest matches exactly a tile. For some formats,
 * drivers may also be able to merge several tiles together (not currently
 * implemented though).
 *
 * Each format string is a pseudo MIME type, whose first part can be passed
 * as the pszFormat argument of ReadCompressedData(), with additional
 * parameters specified as key=value with a semi-colon separator.
 *
 * The amount and types of optional parameters passed after the MIME type is
 * format dependent, and driver dependent (some drivers might not be able to
 * return those extra information without doing a rather costly processing).
 *
 * For example, a driver might return "JPEG;frame_type=SOF0_baseline;"
 * "bit_depth=8;num_components=3;subsampling=4:2:0;colorspace=YCbCr", and
 * consequently "JPEG" can be passed as the pszFormat argument of
 * ReadCompressedData(). For JPEG, implementations can use the
 * GDALGetCompressionFormatForJPEG() helper method to generate a string like
 * above from a JPEG codestream.
 *
 * Several values might be returned. For example,
 * the JPEGXL driver will return "JXL", but also potentially "JPEG"
 * if the JPEGXL codestream includes a JPEG reconstruction box.
 *
 * In the general case this method will return an empty list.
 *
 * This is the same as C++ method GDALDataset::GetCompressionFormats().
 *
 * @param hDS Dataset handle.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param nBandCount the number of bands being requested.
 *
 * @param panBandList the list of nBandCount band numbers.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands.
 *
 * @return a list of compatible formats (which may be empty) that should be
 * freed with CSLDestroy(), or nullptr.
 *
 * @since GDAL 3.7
 */
char **GDALDatasetGetCompressionFormats(GDALDatasetH hDS, int nXOff, int nYOff,
                                        int nXSize, int nYSize, int nBandCount,
                                        const int *panBandList)
{
    VALIDATE_POINTER1(hDS, __func__, nullptr);
    return GDALDataset::FromHandle(hDS)
        ->GetCompressionFormats(nXOff, nYOff, nXSize, nYSize, nBandCount,
                                panBandList)
        .StealList();
}

/************************************************************************/
/*                         ReadCompressedData()                         */
/************************************************************************/

/** Return the compressed content that can be natively obtained for the
 * window of interest and requested bands.
 *
 * For example, a tiled dataset may be able to return data in compressed format
 * if the window of interest matches exactly a tile. For some formats, drivers
 * may also be example to merge several tiles together (not currently
 * implemented though).
 *
 * The implementation should make sure that the content returned forms a valid
 * standalone file. For example, for the GeoTIFF implementation of this method,
 * when extracting a JPEG tile, the method will automatically add the content
 * of the JPEG Huffman and/or quantization tables that might be stored in the
 * TIFF JpegTables tag, and not in tile data itself.
 *
 * In the general case this method will return CE_Failure.
 *
 * This is the same as C function GDALDatasetReadCompressedData().
 *
 * @param pszFormat Requested compression format (e.g. "JPEG",
 * "WEBP", "JXL"). This is the MIME type of one of the values
 * returned by GetCompressionFormats(). The format string is designed to
 * potentially include at a later point key=value optional parameters separated
 * by a semi-colon character. At time of writing, none are implemented.
 * ReadCompressedData() implementations should verify optional parameters and
 * return CE_Failure if they cannot support one of them.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param nBandCount the number of bands being requested.
 *
 * @param panBandList the list of nBandCount band numbers.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands.
 *
 * @param ppBuffer Pointer to a buffer to store the compressed data or nullptr.
 * If ppBuffer is not nullptr, then pnBufferSize should also not be nullptr.
 * If ppBuffer is not nullptr, and *ppBuffer is not nullptr, then the provided
 * buffer will be filled with the compressed data, provided that pnBufferSize
 * and *pnBufferSize are not nullptr, and *pnBufferSize, indicating the size
 * of *ppBuffer, is sufficiently large to hold the data.
 * If ppBuffer is not nullptr, but *ppBuffer is nullptr, then the method will
 * allocate *ppBuffer using VSIMalloc(), and thus the caller is responsible to
 * free it with VSIFree().
 * If ppBuffer is nullptr, then the compressed data itself will not be returned,
 * but *pnBufferSize will be updated with an upper bound of the size that would
 * be necessary to hold it (if pnBufferSize != nullptr).
 *
 * @param pnBufferSize Output buffer size, or nullptr.
 * If ppBuffer != nullptr && *ppBuffer != nullptr, then pnBufferSize should
 * be != nullptr and *pnBufferSize contain the size of *ppBuffer. If the
 * method is successful, *pnBufferSize will be updated with the actual size
 * used.
 *
 * @param ppszDetailedFormat Pointer to an output string, or nullptr.
 * If ppszDetailedFormat is not nullptr, then, on success, the method will
 * allocate a new string in *ppszDetailedFormat (to be freed with VSIFree())
 * *ppszDetailedFormat might contain strings like
 * "JPEG;frame_type=SOF0_baseline;bit_depth=8;num_components=3;"
 * "subsampling=4:2:0;colorspace=YCbCr" or simply the MIME type.
 * The string will contain at least as much information as what
 * GetCompressionFormats() returns, and potentially more when
 * ppBuffer != nullptr.
 *
 * @return CE_None in case of success, CE_Failure otherwise.
 *
 * For example, to request JPEG content on the whole image and let GDAL deal
 * with the buffer allocation.
 * \code{.cpp}
 *   void* pBuffer = nullptr;
 *   size_t nBufferSize = 0;
 *   CPLErr eErr =
 *      poDataset->ReadCompressedData("JPEG",
 *                                    0, 0,
 *                                    poDataset->GetRasterXSize(),
 *                                    poDataset->GetRasterYSize(),
 *                                    poDataset->GetRasterCount(),
 *                                    nullptr, // panBandList
 *                                    &pBuffer,
 *                                    &nBufferSize,
 *                                    nullptr // ppszDetailedFormat
 *                                   );
 *   if (eErr == CE_None)
 *   {
 *       CPLAssert(pBuffer != nullptr);
 *       CPLAssert(nBufferSize > 0);
 *       VSILFILE* fp = VSIFOpenL("my.jpeg", "wb");
 *       if (fp)
 *       {
 *           VSIFWriteL(pBuffer, nBufferSize, 1, fp);
 *           VSIFCloseL(fp);
 *       }
 *       VSIFree(pBuffer);
 *   }
 * \endcode
 *
 * Or to manage the buffer allocation on your side:
 * \code{.cpp}
 *   size_t nUpperBoundBufferSize = 0;
 *   CPLErr eErr =
 *      poDataset->ReadCompressedData("JPEG",
 *                                    0, 0,
 *                                    poDataset->GetRasterXSize(),
 *                                    poDataset->GetRasterYSize(),
 *                                    poDataset->GetRasterCount(),
 *                                    nullptr, // panBandList
 *                                    nullptr, // ppBuffer,
 *                                    &nUpperBoundBufferSize,
 *                                    nullptr // ppszDetailedFormat
 *                                   );
 *   if (eErr == CE_None)
 *   {
 *       std::vector<uint8_t> myBuffer;
 *       myBuffer.resize(nUpperBoundBufferSize);
 *       void* pBuffer = myBuffer.data();
 *       size_t nActualSize = nUpperBoundBufferSize;
 *       char* pszDetailedFormat = nullptr;
 *       // We also request detailed format, but we could have passed it to
 *       // nullptr as well.
 *       eErr =
 *         poDataset->ReadCompressedData("JPEG",
 *                                       0, 0,
 *                                       poDataset->GetRasterXSize(),
 *                                       poDataset->GetRasterYSize(),
 *                                       poDataset->GetRasterCount(),
 *                                       nullptr, // panBandList
 *                                       &pBuffer,
 *                                       &nActualSize,
 *                                       &pszDetailedFormat);
 *       if (eErr == CE_None)
 *       {
 *          CPLAssert(pBuffer == myBuffer.data()); // pointed value not modified
 *          CPLAssert(nActualSize <= nUpperBoundBufferSize);
 *          myBuffer.resize(nActualSize);
 *          // do something useful
 *          VSIFree(pszDetailedFormat);
 *       }
 *   }
 * \endcode
 *
 * @since GDAL 3.7
 */
CPLErr GDALDataset::ReadCompressedData(
    CPL_UNUSED const char *pszFormat, CPL_UNUSED int nXOff,
    CPL_UNUSED int nYOff, CPL_UNUSED int nXSize, CPL_UNUSED int nYSize,
    CPL_UNUSED int nBandCount, CPL_UNUSED const int *panBandList,
    CPL_UNUSED void **ppBuffer, CPL_UNUSED size_t *pnBufferSize,
    CPL_UNUSED char **ppszDetailedFormat)
{
    return CE_Failure;
}

/************************************************************************/
/*                  GDALDatasetReadCompressedData()                     */
/************************************************************************/

/** Return the compressed content that can be natively obtained for the
 * window of interest and requested bands.
 *
 * For example, a tiled dataset may be able to return data in compressed format
 * if the window of interest matches exactly a tile. For some formats, drivers
 * may also be example to merge several tiles together (not currently
 * implemented though).
 *
 * The implementation should make sure that the content returned forms a valid
 * standalone file. For example, for the GeoTIFF implementation of this method,
 * when extracting a JPEG tile, the method will automatically adds the content
 * of the JPEG Huffman and/or quantization tables that might be stored in the
 * TIFF JpegTables tag, and not in tile data itself.
 *
 * In the general case this method will return CE_Failure.
 *
 * This is the same as C++ method GDALDataset:ReadCompressedData().
 *
 * @param hDS Dataset handle.
 *
 * @param pszFormat Requested compression format (e.g. "JPEG",
 * "WEBP", "JXL"). This is the MIME type of one of the values
 * returned by GetCompressionFormats(). The format string is designed to
 * potentially include at a later point key=value optional parameters separated
 * by a semi-colon character. At time of writing, none are implemented.
 * ReadCompressedData() implementations should verify optional parameters and
 * return CE_Failure if they cannot support one of them.
 *
 * @param nXOff The pixel offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region
 * of the band to be accessed.  This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param nBandCount the number of bands being requested.
 *
 * @param panBandList the list of nBandCount band numbers.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands.
 *
 * @param ppBuffer Pointer to a buffer to store the compressed data or nullptr.
 * If ppBuffer is not nullptr, then pnBufferSize should also not be nullptr.
 * If ppBuffer is not nullptr, and *ppBuffer is not nullptr, then the provided
 * buffer will be filled with the compressed data, provided that pnBufferSize
 * and *pnBufferSize are not nullptr, and *pnBufferSize, indicating the size
 * of *ppBuffer, is sufficiently large to hold the data.
 * If ppBuffer is not nullptr, but *ppBuffer is nullptr, then the method will
 * allocate *ppBuffer using VSIMalloc(), and thus the caller is responsible to
 * free it with VSIFree().
 * If ppBuffer is nullptr, then the compressed data itself will not be returned,
 * but *pnBufferSize will be updated with an upper bound of the size that would
 * be necessary to hold it (if pnBufferSize != nullptr).
 *
 * @param pnBufferSize Output buffer size, or nullptr.
 * If ppBuffer != nullptr && *ppBuffer != nullptr, then pnBufferSize should
 * be != nullptr and *pnBufferSize contain the size of *ppBuffer. If the
 * method is successful, *pnBufferSize will be updated with the actual size
 * used.
 *
 * @param ppszDetailedFormat Pointer to an output string, or nullptr.
 * If ppszDetailedFormat is not nullptr, then, on success, the method will
 * allocate a new string in *ppszDetailedFormat (to be freed with VSIFree())
 * *ppszDetailedFormat might contain strings like
 * "JPEG;frame_type=SOF0_baseline;bit_depth=8;num_components=3;"
 * "subsampling=4:2:0;colorspace=YCbCr" or simply the MIME type.
 * The string will contain at least as much information as what
 * GetCompressionFormats() returns, and potentially more when
 * ppBuffer != nullptr.
 *
 * @return CE_None in case of success, CE_Failure otherwise.
 *
 * @since GDAL 3.7
 */
CPLErr GDALDatasetReadCompressedData(GDALDatasetH hDS, const char *pszFormat,
                                     int nXOff, int nYOff, int nXSize,
                                     int nYSize, int nBandCount,
                                     const int *panBandList, void **ppBuffer,
                                     size_t *pnBufferSize,
                                     char **ppszDetailedFormat)
{
    VALIDATE_POINTER1(hDS, __func__, CE_Failure);
    return GDALDataset::FromHandle(hDS)->ReadCompressedData(
        pszFormat, nXOff, nYOff, nXSize, nYSize, nBandCount, panBandList,
        ppBuffer, pnBufferSize, ppszDetailedFormat);
}

/************************************************************************/
/*                           CanBeCloned()                              */
/************************************************************************/

//! @cond Doxygen_Suppress

/** This method is called by GDALThreadSafeDataset::Create() to determine if
 * it is possible to create a thread-safe wrapper for a dataset, which involves
 * the ability to Clone() it.
 *
 * Implementations of this method must be thread-safe.
 *
 * @param nScopeFlags Combination of GDAL_OF_RASTER, GDAL_OF_VECTOR, etc. flags,
 *                    expressing the intended use for thread-safety.
 *                    Currently, the only valid scope is in the base
 *                    implementation is GDAL_OF_RASTER.
 * @param bCanShareState Determines if cloned datasets are allowed to share
 *                       state with the dataset they have been cloned from.
 *                       If set to true, the dataset from which they have been
 *                       cloned from must remain opened during the lifetime of
 *                       its clones.
 * @return true if the Clone() method is expected to succeed with the same values
 *         of nScopeFlags and bCanShareState.
 */
bool GDALDataset::CanBeCloned(int nScopeFlags,
                              [[maybe_unused]] bool bCanShareState) const
{
    return m_bCanBeReopened && nScopeFlags == GDAL_OF_RASTER;
}

//! @endcond

/************************************************************************/
/*                               Clone()                                */
/************************************************************************/

//! @cond Doxygen_Suppress

/** This method "clones" the current dataset, that is it returns a new instance
 * that is opened on the same underlying "file".
 *
 * The base implementation uses GDALDataset::Open() to re-open the dataset.
 * The MEM driver has a specialized implementation that returns a new instance,
 * but which shares the same memory buffer as this.
 *
 * Implementations of this method must be thread-safe.
 *
 * @param nScopeFlags Combination of GDAL_OF_RASTER, GDAL_OF_VECTOR, etc. flags,
 *                    expressing the intended use for thread-safety.
 *                    Currently, the only valid scope is in the base
 *                    implementation is GDAL_OF_RASTER.
 * @param bCanShareState Determines if cloned datasets are allowed to share
 *                       state with the dataset they have been cloned from.
 *                       If set to true, the dataset from which they have been
 *                       cloned from must remain opened during the lifetime of
 *                       its clones.
 * @return a new instance, or nullptr in case of error.
 */
std::unique_ptr<GDALDataset>
GDALDataset::Clone(int nScopeFlags, [[maybe_unused]] bool bCanShareState) const
{
    CPLStringList aosAllowedDrivers;
    if (poDriver)
        aosAllowedDrivers.AddString(poDriver->GetDescription());
    return std::unique_ptr<GDALDataset>(GDALDataset::Open(
        GetDescription(),
        nScopeFlags | GDAL_OF_INTERNAL | GDAL_OF_VERBOSE_ERROR,
        aosAllowedDrivers.List(), papszOpenOptions));
}

//! @endcond

/************************************************************************/
/*                    GeolocationToPixelLine()                          */
/************************************************************************/

/** Transform georeferenced coordinates to pixel/line coordinates.
 *
 * When poSRS is null, those georeferenced coordinates (dfGeolocX, dfGeolocY)
 * must be in the "natural" SRS of the dataset, that is the one returned by
 * GetSpatialRef() if there is a geotransform, GetGCPSpatialRef() if there are
 * GCPs, WGS 84 if there are RPC coefficients, or the SRS of the geolocation
 * array (generally WGS 84) if there is a geolocation array.
 * If that natural SRS is a geographic one, dfGeolocX must be a longitude, and
 * dfGeolocY a latitude. If that natural SRS is a projected one, dfGeolocX must
 * be a easting, and dfGeolocY a northing.
 *
 * When poSRS is set to a non-null value, (dfGeolocX, dfGeolocY) must be
 * expressed in that CRS, and that tuple must be conformant with the
 * data-axis-to-crs-axis setting of poSRS, that is the one returned by
 * the OGRSpatialReference::GetDataAxisToSRSAxisMapping(). If you want to be sure
 * of the axis order, then make sure to call poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER)
 * before calling this method, and in that case, dfGeolocX must be a longitude
 * or an easting value, and dfGeolocX a latitude or a northing value.
 *
 * This method uses GDALCreateGenImgProjTransformer2() underneath.
 *
 * @param dfGeolocX X coordinate of the position (longitude or easting if poSRS
 * is null, otherwise consistent with poSRS data-axis-to-crs-axis mapping),
 * where interpolation should be done.
 * @param dfGeolocY Y coordinate of the position (latitude or northing if poSRS
 * is null, otherwise consistent with poSRS data-axis-to-crs-axis mapping),
 * where interpolation should be done.
 * @param poSRS If set, override the natural CRS in which dfGeolocX, dfGeolocY are expressed
 * @param[out] pdfPixel Pointer to the variable where to the store the pixel/column coordinate.
 * @param[out] pdfLine Pointer to the variable where to the store the line coordinate.
 * @param papszTransformerOptions Options accepted by GDALCreateGenImgProjTransformer2(), or nullptr.
 *
 * @return CE_None on success, or an error code on failure.
 * @since GDAL 3.11
 */

CPLErr
GDALDataset::GeolocationToPixelLine(double dfGeolocX, double dfGeolocY,
                                    const OGRSpatialReference *poSRS,
                                    double *pdfPixel, double *pdfLine,
                                    CSLConstList papszTransformerOptions) const
{
    CPLStringList aosTO(papszTransformerOptions);

    if (poSRS)
    {
        const char *const apszOptions[] = {"FORMAT=WKT2", nullptr};
        const std::string osWKT = poSRS->exportToWkt(apszOptions);
        aosTO.SetNameValue("DST_SRS", osWKT.c_str());
        const auto eAxisMappingStrategy = poSRS->GetAxisMappingStrategy();
        if (eAxisMappingStrategy == OAMS_TRADITIONAL_GIS_ORDER)
            aosTO.SetNameValue("DST_SRS_AXIS_MAPPING_STRATEGY",
                               "TRADITIONAL_GIS_ORDER");
        else if (eAxisMappingStrategy == OAMS_AUTHORITY_COMPLIANT)
            aosTO.SetNameValue("DST_SRS_AXIS_MAPPING_STRATEGY",
                               "AUTHORITY_COMPLIANT");
        else
        {
            const auto &anValues = poSRS->GetDataAxisToSRSAxisMapping();
            std::string osVal;
            for (int v : anValues)
            {
                if (!osVal.empty())
                    osVal += ',';
                osVal += std::to_string(v);
            }
            aosTO.SetNameValue("DST_SRS_DATA_AXIS_TO_SRS_AXIS_MAPPING",
                               osVal.c_str());
        }
    }

    auto hTransformer = GDALCreateGenImgProjTransformer2(
        GDALDataset::ToHandle(const_cast<GDALDataset *>(this)), nullptr,
        aosTO.List());
    if (hTransformer == nullptr)
    {
        return CE_Failure;
    }

    double z = 0;
    int bSuccess = 0;
    GDALGenImgProjTransform(hTransformer, TRUE, 1, &dfGeolocX, &dfGeolocY, &z,
                            &bSuccess);
    GDALDestroyTransformer(hTransformer);
    if (bSuccess)
    {
        if (pdfPixel)
            *pdfPixel = dfGeolocX;
        if (pdfLine)
            *pdfLine = dfGeolocY;
        return CE_None;
    }
    else
    {
        return CE_Failure;
    }
}

/************************************************************************/
/*                  GDALDatasetGeolocationToPixelLine()                 */
/************************************************************************/

/** Transform georeferenced coordinates to pixel/line coordinates.
 *
 * @see GDALDataset::GeolocationToPixelLine()
 * @since GDAL 3.11
 */

CPLErr GDALDatasetGeolocationToPixelLine(GDALDatasetH hDS, double dfGeolocX,
                                         double dfGeolocY,
                                         OGRSpatialReferenceH hSRS,
                                         double *pdfPixel, double *pdfLine,
                                         CSLConstList papszTransformerOptions)
{
    VALIDATE_POINTER1(hDS, "GDALDatasetGeolocationToPixelLine", CE_Failure);

    GDALDataset *poDS = GDALDataset::FromHandle(hDS);
    return poDS->GeolocationToPixelLine(
        dfGeolocX, dfGeolocY, OGRSpatialReference::FromHandle(hSRS), pdfPixel,
        pdfLine, papszTransformerOptions);
}

/************************************************************************/
/*                               GetExtent()                            */
/************************************************************************/

/** Return extent of dataset in specified CRS.
 *
 * OGREnvelope.MinX/MaxX represents longitudes, and MinY/MaxY latitudes.
 *
 * For rasters, the base implementation of this method only succeeds if
 * GetGeoTransform() and GetSpatialRef() succeed.
 * For vectors, the base implementation of this method iterates over layers
 * and call their OGRLayer::GetExtent() method.
 *
 * TestCapability(GDsCFastGetExtent) can be used to test if the execution
 * time of this method is fast.
 *
 * This is the same as C function GDALGetExtent()
 *
 * @param[out] psExtent Pointer to output extent. Must NOT be null.
 * @param poCRS CRS in which to express the extent. If not specified, this will
 * be the raster CRS or the CRS of the first layer for a vector dataset.
 * @return CE_None in case of success, CE_Failure otherwise
 * @since GDAL 3.12
 */

CPLErr GDALDataset::GetExtent(OGREnvelope *psExtent,
                              const OGRSpatialReference *poCRS) const
{
    auto poThisDS = const_cast<GDALDataset *>(this);
    const OGRSpatialReference *poThisCRS = poThisDS->GetSpatialRef();
    int nLayerCount = 0;
    if (!poThisCRS)
    {
        nLayerCount = poThisDS->GetLayerCount();
        if (nLayerCount >= 1)
        {
            if (auto poLayer = poThisDS->GetLayer(0))
                poThisCRS = poLayer->GetSpatialRef();
        }
        if (poCRS && !poThisCRS)
            return CE_Failure;
    }
    if (!poCRS)
        poCRS = poThisCRS;

    *psExtent = OGREnvelope();

    GDALGeoTransform gt;
    const bool bHasGT = poThisDS->GetGeoTransform(gt) == CE_None;
    if (bHasGT)
    {
        std::unique_ptr<OGRCoordinateTransformation> poCT;
        if (poCRS)
        {
            poCT.reset(OGRCreateCoordinateTransformation(poThisCRS, poCRS));
        }

        constexpr int DENSIFY_POINT_COUNT = 21;
        double dfULX = gt[0];
        double dfULY = gt[3];
        double dfURX = 0, dfURY = 0;
        gt.Apply(nRasterXSize, 0, &dfURX, &dfURY);
        double dfLLX = 0, dfLLY = 0;
        gt.Apply(0, nRasterYSize, &dfLLX, &dfLLY);
        double dfLRX = 0, dfLRY = 0;
        gt.Apply(nRasterXSize, nRasterYSize, &dfLRX, &dfLRY);
        const double xmin = std::min({dfULX, dfURX, dfLLX, dfLRX});
        const double ymin = std::min({dfULY, dfURY, dfLLY, dfLRY});
        const double xmax = std::max({dfULX, dfURX, dfLLX, dfLRX});
        const double ymax = std::max({dfULY, dfURY, dfLLY, dfLRY});
        if (poCT)
        {
            OGREnvelope sEnvTmp;
            if (!poCT->TransformBounds(xmin, ymin, xmax, ymax, &(sEnvTmp.MinX),
                                       &(sEnvTmp.MinY), &(sEnvTmp.MaxX),
                                       &(sEnvTmp.MaxY), DENSIFY_POINT_COUNT))
            {
                return CE_Failure;
            }
            *psExtent = sEnvTmp;
        }
        else
        {
            psExtent->MinX = xmin;
            psExtent->MinY = ymin;
            psExtent->MaxX = xmax;
            psExtent->MaxY = ymax;
        }
    }

    if (nLayerCount > 0)
    {
        for (auto &&poLayer : poThisDS->GetLayers())
        {
            auto poLayerCRS = poLayer->GetSpatialRef();
            if (poLayerCRS)
            {
                OGREnvelope sLayerExtent;
                if (poLayer->GetExtent(&sLayerExtent) == OGRERR_NONE)
                {
                    auto poCT = std::unique_ptr<OGRCoordinateTransformation>(
                        OGRCreateCoordinateTransformation(poLayerCRS, poCRS));
                    if (poCT)
                    {
                        constexpr int DENSIFY_POINT_COUNT = 21;
                        OGREnvelope sEnvTmp;
                        if (poCT->TransformBounds(
                                sLayerExtent.MinX, sLayerExtent.MinY,
                                sLayerExtent.MaxX, sLayerExtent.MaxY,
                                &(sEnvTmp.MinX), &(sEnvTmp.MinY),
                                &(sEnvTmp.MaxX), &(sEnvTmp.MaxY),
                                DENSIFY_POINT_COUNT))
                        {
                            psExtent->Merge(sEnvTmp);
                        }
                    }
                }
            }
        }
    }

    return psExtent->IsInit() ? CE_None : CE_Failure;
}

/************************************************************************/
/*                           GDALGetExtent()                            */
/************************************************************************/

/** Return extent of dataset in specified CRS.
 *
 * OGREnvelope.MinX/MaxX represents longitudes, and MinY/MaxY latitudes.
 *
 * For rasters, the base implementation of this method only succeeds if
 * GetGeoTransform() and GetSpatialRef() succeed.
 * For vectors, the base implementation of this method iterates over layers
 * and call their OGRLayer::GetExtent() method.
 *
 * TestCapability(GDsCFastGetExtent) can be used to test if the execution
 * time of this method is fast.
 *
 * This is the same as C++ method GDALDataset::GetExtent()
 *
 * @param hDS Dataset handle. Must NOT be null.
 * @param[out] psExtent Pointer to output extent. Must NOT be null.
 * @param hCRS CRS in which to express the extent. If not specified, this will
 * be the raster CRS or the CRS of the first layer for a vector dataset.
 * @return extent in poCRS (valid only if IsInit() method returns true)
 * @since GDAL 3.12
 */

CPLErr GDALGetExtent(GDALDatasetH hDS, OGREnvelope *psExtent,
                     OGRSpatialReferenceH hCRS)
{
    VALIDATE_POINTER1(hDS, __func__, CE_Failure);
    VALIDATE_POINTER1(psExtent, __func__, CE_Failure);
    return GDALDataset::FromHandle(hDS)->GetExtent(
        psExtent, OGRSpatialReference::FromHandle(hCRS));
}

/************************************************************************/
/*                         GetExtentWGS84LongLat()                      */
/************************************************************************/

/** Return extent of dataset in WGS84 longitude/latitude
 *
 * OGREnvelope.MinX/MaxX represents longitudes, and MinY/MaxY latitudes.
 *
 * TestCapability(GDsCFastGetExtentWGS84LongLat) can be used to test if the execution
 * time of this method is fast.
 *
 * This is the same as C function GDALGetExtentWGS84LongLat()
 *
 * @return extent (valid only if IsInit() method returns true)
 * @since GDAL 3.12
 */

CPLErr GDALDataset::GetExtentWGS84LongLat(OGREnvelope *psExtent) const
{
    OGRSpatialReference oSRS_WGS84;
    oSRS_WGS84.SetFromUserInput("WGS84");
    oSRS_WGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return GetExtent(psExtent, &oSRS_WGS84);
}

/************************************************************************/
/*                    GDALGetExtentWGS84LongLat()                       */
/************************************************************************/

/** Return extent of dataset in WGS84 longitude/latitude
 *
 * OGREnvelope.MinX/MaxX represents longitudes, and MinY/MaxY latitudes.
 *
 * TestCapability(GDsCFastGetExtentWGS84LongLat) can be used to test if the execution
 * time of this method is fast.
 *
 * This is the same as C++ method GDALDataset::GetExtentWGS84LongLat()
 *
 * @param hDS Dataset handle. Must NOT be null.
 * @param[out] psExtent Pointer to output extent. Must NOT be null.
 * @return extent (valid only if IsInit() method returns true)
 * @since GDAL 3.12
 */

CPLErr GDALGetExtentWGS84LongLat(GDALDatasetH hDS, OGREnvelope *psExtent)
{
    VALIDATE_POINTER1(hDS, __func__, CE_Failure);
    VALIDATE_POINTER1(psExtent, __func__, CE_Failure);
    return GDALDataset::FromHandle(hDS)->GetExtentWGS84LongLat(psExtent);
}

/************************************************************************/
/*                  ReportUpdateNotSupportedByDriver()                  */
/************************************************************************/

//! @cond Doxygen_Suppress

/* static */
void GDALDataset::ReportUpdateNotSupportedByDriver(const char *pszDriverName)
{
    CPLError(CE_Failure, CPLE_NotSupported,
             "The %s driver does not support update access to existing "
             "datasets.",
             pszDriverName);
}

//! @endcond

/************************************************************************/
/*                         BuildFilename()                              */
/************************************************************************/

/** Generates a filename, potentially relative to another one.
 *
 * Given the path to a reference directory, and a path to a file
 * referenced from it, build a path to the file that the current application
 * can use. If the file path is already absolute, rather than relative, or if
 * bRelativeToReferencePath is false, then the filename of interest will be
 * returned unaltered.
 *
 * This is enhanced version of CPLProjectRelativeFilenameSafe() that takes
 * into account the subdataset syntax.
 *
 * Examples:
 * \code{.cpp}
 * BuildFilename("tmp/abc.gif", "abc/def", true) == "abc/def/tmp/abc.gif"
 * BuildFilename("../abc.gif", "/abc/def") == "/abc/abc.gif"
 * BuildFilename("abc.gif", "C:\WIN", true) == "C:\WIN\abc.gif"
 * BuildFilename("abc.gif", "C:\WIN", false) == "abc.gif"
 * BuildFilename("/home/even/foo.tif", "/home/even/workdir", true) == "/home/even/foo.tif"
 * \endcode
 *
 * @param pszFilename Filename of interest.
 * @param pszReferencePath Path to a reference directory.
 * @param bRelativeToReferencePath Whether pszFilename, if a relative path, is
 *                                 relative to pszReferencePath
 * @since 3.11
 */

/* static */
std::string GDALDataset::BuildFilename(const char *pszFilename,
                                       const char *pszReferencePath,
                                       bool bRelativeToReferencePath)
{
    std::string osSrcDSName;
    if (pszReferencePath != nullptr && bRelativeToReferencePath)
    {
        // Try subdatasetinfo API first
        // Note: this will become the only branch when subdatasetinfo will become
        //       available for NITF_IM, RASTERLITE and TILEDB
        const auto oSubDSInfo{GDALGetSubdatasetInfo(pszFilename)};
        if (oSubDSInfo && !oSubDSInfo->GetPathComponent().empty())
        {
            auto path{oSubDSInfo->GetPathComponent()};
            osSrcDSName = oSubDSInfo->ModifyPathComponent(
                CPLProjectRelativeFilenameSafe(pszReferencePath, path.c_str())
                    .c_str());
            GDALDestroySubdatasetInfo(oSubDSInfo);
        }
        else
        {
            bool bDone = false;
            for (const char *pszSyntax : apszSpecialSubDatasetSyntax)
            {
                CPLString osPrefix(pszSyntax);
                osPrefix.resize(strchr(pszSyntax, ':') - pszSyntax + 1);
                if (pszSyntax[osPrefix.size()] == '"')
                    osPrefix += '"';
                if (EQUALN(pszFilename, osPrefix, osPrefix.size()))
                {
                    if (STARTS_WITH_CI(pszSyntax + osPrefix.size(), "{ANY}"))
                    {
                        const char *pszLastPart = strrchr(pszFilename, ':') + 1;
                        // CSV:z:/foo.xyz
                        if ((pszLastPart[0] == '/' || pszLastPart[0] == '\\') &&
                            pszLastPart - pszFilename >= 3 &&
                            pszLastPart[-3] == ':')
                        {
                            pszLastPart -= 2;
                        }
                        CPLString osPrefixFilename = pszFilename;
                        osPrefixFilename.resize(pszLastPart - pszFilename);
                        osSrcDSName = osPrefixFilename +
                                      CPLProjectRelativeFilenameSafe(
                                          pszReferencePath, pszLastPart);
                        bDone = true;
                    }
                    else if (STARTS_WITH_CI(pszSyntax + osPrefix.size(),
                                            "{FILENAME}"))
                    {
                        CPLString osFilename(pszFilename + osPrefix.size());
                        size_t nPos = 0;
                        if (osFilename.size() >= 3 && osFilename[1] == ':' &&
                            (osFilename[2] == '\\' || osFilename[2] == '/'))
                            nPos = 2;
                        nPos = osFilename.find(
                            pszSyntax[osPrefix.size() + strlen("{FILENAME}")],
                            nPos);
                        if (nPos != std::string::npos)
                        {
                            const CPLString osSuffix = osFilename.substr(nPos);
                            osFilename.resize(nPos);
                            osSrcDSName = osPrefix +
                                          CPLProjectRelativeFilenameSafe(
                                              pszReferencePath, osFilename) +
                                          osSuffix;
                            bDone = true;
                        }
                    }
                    break;
                }
            }
            if (!bDone)
            {
                std::string osReferencePath = pszReferencePath;
                if (!CPLIsFilenameRelative(pszReferencePath))
                {
                    // Simplify path by replacing "foo/a/../b" with "foo/b"
                    while (STARTS_WITH(pszFilename, "../"))
                    {
                        osReferencePath =
                            CPLGetPathSafe(osReferencePath.c_str());
                        pszFilename += strlen("../");
                    }
                }

                osSrcDSName = CPLProjectRelativeFilenameSafe(
                    osReferencePath.c_str(), pszFilename);
            }
        }
    }
    else
    {
        osSrcDSName = pszFilename;
    }
    return osSrcDSName;
}
