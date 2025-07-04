/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Implementation of derived subdatasets
 * Author:   Julien Michel <julien dot michel at cnes dot fr>
 *
 ******************************************************************************
 * Copyright (c) 2016 Julien Michel <julien dot michel at cnes dot fr>
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/
#include "../vrt/vrtdataset.h"
#include "gdal_pam.h"
#include "derivedlist.h"

class DerivedDataset final : public VRTDataset
{
  public:
    DerivedDataset(int nXSize, int nYSize);

    ~DerivedDataset();

    static int Identify(GDALOpenInfo *);
    static GDALDataset *Open(GDALOpenInfo *);
};

DerivedDataset::DerivedDataset(int nXSize, int nYSize)
    : VRTDataset(nXSize, nYSize)
{
    poDriver = nullptr;
    SetWritable(FALSE);
}

DerivedDataset::~DerivedDataset() = default;

int DerivedDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    /* Try to open original dataset */
    CPLString filename(poOpenInfo->pszFilename);

    /* DERIVED_SUBDATASET should be first domain */
    const size_t dsds_pos = filename.find("DERIVED_SUBDATASET:");

    if (dsds_pos != 0)
    {
        /* Unable to Open in this case */
        return FALSE;
    }

    return TRUE;
}

GDALDataset *DerivedDataset::Open(GDALOpenInfo *poOpenInfo)
{
    /* Try to open original dataset */
    CPLString filename(poOpenInfo->pszFilename);

    /* DERIVED_SUBDATASET should be first domain */
    const size_t dsds_pos = filename.find("DERIVED_SUBDATASET:");
    const size_t nPrefixLen = strlen("DERIVED_SUBDATASET:");

    if (dsds_pos != 0)
    {
        /* Unable to Open in this case */
        return nullptr;
    }

    /* Next, we need to now which derived dataset to compute */
    const size_t alg_pos = filename.find(":", nPrefixLen + 1);
    if (alg_pos == std::string::npos)
    {
        /* Unable to Open if we do not find the name of the derived dataset */
        return nullptr;
    }

    CPLString odDerivedName = filename.substr(nPrefixLen, alg_pos - nPrefixLen);

    CPLDebug("DerivedDataset::Open", "Derived dataset requested: %s",
             odDerivedName.c_str());

    CPLString pixelFunctionName = "";
    bool datasetFound = false;

    unsigned int nbSupportedDerivedDS(0);
    GDALDataType type = GDT_Float64;

    const DerivedDatasetDescription *poDDSDesc =
        GDALGetDerivedDatasetDescriptions(&nbSupportedDerivedDS);

    for (unsigned int derivedId = 0; derivedId < nbSupportedDerivedDS;
         ++derivedId)
    {
        if (odDerivedName == poDDSDesc[derivedId].pszDatasetName)
        {
            datasetFound = true;
            pixelFunctionName = poDDSDesc[derivedId].pszPixelFunction;
            type =
                GDALGetDataTypeByName(poDDSDesc[derivedId].pszOutputPixelType);
        }
    }

    if (!datasetFound)
    {
        return nullptr;
    }

    CPLString odFilename =
        filename.substr(alg_pos + 1, filename.size() - alg_pos);

    auto poTmpDS = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(odFilename, GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR));

    if (poTmpDS == nullptr)
        return nullptr;

    int nbBands = poTmpDS->GetRasterCount();

    if (nbBands == 0)
    {
        return nullptr;
    }

    int nRows = poTmpDS->GetRasterYSize();
    int nCols = poTmpDS->GetRasterXSize();

    DerivedDataset *poDS = new DerivedDataset(nCols, nRows);

    // Transfer metadata
    poDS->SetMetadata(poTmpDS->GetMetadata());

    char **papszRPC = poTmpDS->GetMetadata("RPC");
    if (papszRPC)
    {
        poDS->SetMetadata(papszRPC, "RPC");
    }

    // Transfer projection
    poDS->SetProjection(poTmpDS->GetProjectionRef());

    // Transfer geotransform
    GDALGeoTransform gt;
    if (poTmpDS->GetGeoTransform(gt) == CE_None)
    {
        poDS->SetGeoTransform(gt);
    }

    // Transfer GCPs
    const char *gcpProjection = poTmpDS->GetGCPProjection();
    int nbGcps = poTmpDS->GetGCPCount();
    poDS->SetGCPs(nbGcps, poTmpDS->GetGCPs(), gcpProjection);

    // Map bands
    for (int nBand = 1; nBand <= nbBands; ++nBand)
    {
        VRTDerivedRasterBand *poBand =
            new VRTDerivedRasterBand(poDS, nBand, type, nCols, nRows);
        poDS->SetBand(nBand, poBand);

        poBand->SetPixelFunctionName(pixelFunctionName);
        poBand->SetSourceTransferType(
            poTmpDS->GetRasterBand(nBand)->GetRasterDataType());

        poBand->AddComplexSource(odFilename, nBand, 0, 0, nCols, nRows, 0, 0,
                                 nCols, nRows);
    }

    // If dataset is a real file, initialize overview manager
    VSIStatBufL sStat;
    if (VSIStatL(odFilename, &sStat) == 0)
    {
        CPLString path = CPLGetPathSafe(odFilename);
        CPLString ovrFileName = "DERIVED_DATASET_" + odDerivedName + "_" +
                                CPLGetFilename(odFilename);
        CPLString ovrFilePath = CPLFormFilenameSafe(path, ovrFileName, nullptr);

        poDS->oOvManager.Initialize(poDS, ovrFilePath);
    }

    return poDS;
}

void GDALRegister_Derived()
{
    if (GDALGetDriverByName("DERIVED") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("DERIVED");
#ifdef GDAL_DCAP_RASTER
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
#endif
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "Derived datasets using VRT pixel functions");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC,
                              "drivers/raster/derived.html");
    poDriver->SetMetadataItem(GDAL_DMD_SUBDATASETS, "NO");

    poDriver->pfnOpen = DerivedDataset::Open;
    poDriver->pfnIdentify = DerivedDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
