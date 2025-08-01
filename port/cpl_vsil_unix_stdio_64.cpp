/**********************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Implement VSI large file api for Unix platforms with fseek64()
 *           and ftell64() such as IRIX.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 **********************************************************************
 * Copyright (c) 2001, Frank Warmerdam
 * Copyright (c) 2010-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************
 *
 * NB: Note that in wrappers we are always saving the error state (errno
 * variable) to avoid side effects during debug prints or other possible
 * standard function calls (error states will be overwritten after such
 * a call).
 *
 ****************************************************************************/

//! @cond Doxygen_Suppress

// #define VSI_COUNT_BYTES_READ

// Some unusual filesystems do not work if _FORTIFY_SOURCE in GCC or
// clang is used within this source file, especially if techniques
// like those in vsipreload are used.  Fortify source interacts poorly with
// filesystems that use fread for forward seeks.  This leads to SIGSEGV within
// fread calls.
//
// See this for hardening background info: https://wiki.debian.org/Hardening
#undef _FORTIFY_SOURCE

// 64 bit IO is only available on 32-bit android since API 24 / Android 7.0
// See
// https://android.googlesource.com/platform/bionic/+/master/docs/32-bit-abi.md
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 24
#define _FILE_OFFSET_BITS 64
#endif

#include "cpl_port.h"

#if !defined(_WIN32)

#include "cpl_vsi.h"
#include "cpl_vsi_virtual.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <sys/stat.h>
#ifdef HAVE_STATVFS
#include <sys/statvfs.h>
#endif
#include <sys/types.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_PREAD_BSD
#include <sys/uio.h>
#endif

#if defined(__MACH__) && defined(__APPLE__)
#define HAS_CASE_INSENSITIVE_FILE_SYSTEM
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#endif

#include <limits>
#include <new>

#include "cpl_config.h"
#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "cpl_vsi_error.h"

#if defined(UNIX_STDIO_64)

#ifndef VSI_FTELL64
#define VSI_FTELL64 ftell64
#endif
#ifndef VSI_FSEEK64
#define VSI_FSEEK64 fseek64
#endif
#ifndef VSI_FOPEN64
#define VSI_FOPEN64 fopen64
#endif
#ifndef VSI_STAT64
#define VSI_STAT64 stat64
#endif
#ifndef VSI_STAT64_T
#define VSI_STAT64_T stat64
#endif
#ifndef VSI_FTRUNCATE64
#define VSI_FTRUNCATE64 ftruncate64
#endif

#else /* not UNIX_STDIO_64 */

#ifndef VSI_FTELL64
#define VSI_FTELL64 ftell
#endif
#ifndef VSI_FSEEK64
#define VSI_FSEEK64 fseek
#endif
#ifndef VSI_FOPEN64
#define VSI_FOPEN64 fopen
#endif
#ifndef VSI_STAT64
#define VSI_STAT64 stat
#endif
#ifndef VSI_STAT64_T
#define VSI_STAT64_T stat
#endif
#ifndef VSI_FTRUNCATE64
#define VSI_FTRUNCATE64 ftruncate
#endif

#endif /* ndef UNIX_STDIO_64 */

#ifndef BUILD_WITHOUT_64BIT_OFFSET
// Ensure we have working 64 bit API
static_assert(sizeof(VSI_FTELL64(stdout)) == sizeof(vsi_l_offset),
              "File API does not seem to support 64-bit offset. "
              "If you still want to build GDAL without > 4GB file support, "
              "add the -DBUILD_WITHOUT_64BIT_OFFSET define");
static_assert(sizeof(VSIStatBufL::st_size) == sizeof(vsi_l_offset),
              "File API does not seem to support 64-bit file size. "
              "If you still want to build GDAL without > 4GB file support, "
              "add the -DBUILD_WITHOUT_64BIT_OFFSET define");
#endif

/************************************************************************/
/* ==================================================================== */
/*                       VSIUnixStdioFilesystemHandler                  */
/* ==================================================================== */
/************************************************************************/

struct VSIDIRUnixStdio;

class VSIUnixStdioFilesystemHandler final : public VSIFilesystemHandler
{
    CPL_DISALLOW_COPY_ASSIGN(VSIUnixStdioFilesystemHandler)

#ifdef VSI_COUNT_BYTES_READ
    vsi_l_offset nTotalBytesRead = 0;
    CPLMutex *hMutex = nullptr;
#endif

  public:
    VSIUnixStdioFilesystemHandler() = default;
#ifdef VSI_COUNT_BYTES_READ
    ~VSIUnixStdioFilesystemHandler() override;
#endif

    VSIVirtualHandle *Open(const char *pszFilename, const char *pszAccess,
                           bool bSetError,
                           CSLConstList /* papszOptions */) override;

    VSIVirtualHandle *
    CreateOnlyVisibleAtCloseTime(const char *pszFilename,
                                 bool bEmulationAllowed,
                                 CSLConstList papszOptions) override;

    int Stat(const char *pszFilename, VSIStatBufL *pStatBuf,
             int nFlags) override;
    int Unlink(const char *pszFilename) override;
    int Rename(const char *oldpath, const char *newpath, GDALProgressFunc,
               void *) override;
    int Mkdir(const char *pszDirname, long nMode) override;
    int Rmdir(const char *pszDirname) override;
    char **ReadDirEx(const char *pszDirname, int nMaxFiles) override;
    GIntBig GetDiskFreeSpace(const char *pszDirname) override;
    int SupportsSparseFiles(const char *pszPath) override;

    bool IsLocal(const char *pszPath) override;
    bool SupportsSequentialWrite(const char *pszPath,
                                 bool /* bAllowLocalTempFile */) override;
    bool SupportsRandomWrite(const char *pszPath,
                             bool /* bAllowLocalTempFile */) override;

    VSIDIR *OpenDir(const char *pszPath, int nRecurseDepth,
                    const char *const *papszOptions) override;

    static std::unique_ptr<VSIDIRUnixStdio>
    OpenDirInternal(const char *pszPath, int nRecurseDepth,
                    const char *const *papszOptions);

#ifdef HAS_CASE_INSENSITIVE_FILE_SYSTEM
    std::string
    GetCanonicalFilename(const std::string &osFilename) const override;
#endif

#ifdef VSI_COUNT_BYTES_READ
    void AddToTotal(vsi_l_offset nBytes);
#endif
};

/************************************************************************/
/* ==================================================================== */
/*                        VSIUnixStdioHandle                            */
/* ==================================================================== */
/************************************************************************/

class VSIUnixStdioHandle final : public VSIVirtualHandle
{
    friend class VSIUnixStdioFilesystemHandler;
    CPL_DISALLOW_COPY_ASSIGN(VSIUnixStdioHandle)

    FILE *fp = nullptr;
    vsi_l_offset m_nOffset = 0;
    bool bReadOnly = true;
    bool bLastOpWrite = false;
    bool bLastOpRead = false;
    bool bAtEOF = false;
    bool bError = false;
    // In a+ mode, disable any optimization since the behavior of the file
    // pointer on Mac and other BSD system is to have a seek() to the end of
    // file and thus a call to our Seek(0, SEEK_SET) before a read will be a
    // no-op.
    bool bModeAppendReadWrite = false;
#ifdef VSI_COUNT_BYTES_READ
    vsi_l_offset nTotalBytesRead = 0;
    VSIUnixStdioFilesystemHandler *poFS = nullptr;
#endif

    bool m_bCancelCreation = false;
    std::string m_osFilenameToSetAtCloseTime{};
#if !defined(__linux)
    std::string m_osTmpFilename{};
#endif

  public:
    VSIUnixStdioHandle(VSIUnixStdioFilesystemHandler *poFSIn, FILE *fpIn,
                       bool bReadOnlyIn, bool bModeAppendReadWriteIn);
    ~VSIUnixStdioHandle() override;

    int Seek(vsi_l_offset nOffsetIn, int nWhence) override;
    vsi_l_offset Tell() override;
    size_t Read(void *pBuffer, size_t nSize, size_t nMemb) override;
    size_t Write(const void *pBuffer, size_t nSize, size_t nMemb) override;
    void ClearErr() override;
    int Eof() override;
    int Error() override;
    int Flush() override;
    int Close() override;
    int Truncate(vsi_l_offset nNewSize) override;

    void *GetNativeFileDescriptor() override
    {
        return reinterpret_cast<void *>(static_cast<uintptr_t>(fileno(fp)));
    }

    VSIRangeStatus GetRangeStatus(vsi_l_offset nOffset,
                                  vsi_l_offset nLength) override;
#if defined(HAVE_PREAD64) || (defined(HAVE_PREAD_BSD) && SIZEOF_OFF_T == 8)
    bool HasPRead() const override;
    size_t PRead(void * /*pBuffer*/, size_t /* nSize */,
                 vsi_l_offset /*nOffset*/) const override;
#endif

    void CancelCreation() override
    {
        m_bCancelCreation = true;
    }
};

/************************************************************************/
/*                       VSIUnixStdioHandle()                           */
/************************************************************************/

VSIUnixStdioHandle::VSIUnixStdioHandle(
#ifndef VSI_COUNT_BYTES_READ
    CPL_UNUSED
#endif
        VSIUnixStdioFilesystemHandler *poFSIn,
    FILE *fpIn, bool bReadOnlyIn, bool bModeAppendReadWriteIn)
    : fp(fpIn), bReadOnly(bReadOnlyIn),
      bModeAppendReadWrite(bModeAppendReadWriteIn)
#ifdef VSI_COUNT_BYTES_READ
      ,
      poFS(poFSIn)
#endif
{
}

/************************************************************************/
/*                         ~VSIUnixStdioHandle()                        */
/************************************************************************/

VSIUnixStdioHandle::~VSIUnixStdioHandle()
{
    VSIUnixStdioHandle::Close();
}

/************************************************************************/
/*                               Close()                                */
/************************************************************************/

int VSIUnixStdioHandle::Close()

{
    if (!fp)
        return 0;

    VSIDebug1("VSIUnixStdioHandle::Close(%p)", fp);

#ifdef VSI_COUNT_BYTES_READ
    poFS->AddToTotal(nTotalBytesRead);
#endif

    int ret = 0;

#ifdef __linux
    if (!m_bCancelCreation && !m_osFilenameToSetAtCloseTime.empty())
    {
        ret = fflush(fp);
        if (ret == 0)
        {
            // As advised by "man 2 open" if the caller doesn't have the
            // CAP_DAC_READ_SEARCH capability, which seems to be the default

            char szPath[32];
            const int fd = fileno(fp);
            snprintf(szPath, sizeof(szPath), "/proc/self/fd/%d", fd);
            ret =
                linkat(AT_FDCWD, szPath, AT_FDCWD,
                       m_osFilenameToSetAtCloseTime.c_str(), AT_SYMLINK_FOLLOW);
            if (ret != 0)
                CPLDebug("CPL", "linkat() failed with errno=%d", errno);
        }
    }
#endif

    int ret2 = fclose(fp);
    if (ret == 0 && ret2 != 0)
        ret = ret2;

#if !defined(__linux)
    if (!m_osFilenameToSetAtCloseTime.empty())
    {
        if (m_bCancelCreation)
        {
            ret = unlink(m_osFilenameToSetAtCloseTime.c_str());
        }
        else
        {
            ret = rename(m_osTmpFilename.c_str(),
                         m_osFilenameToSetAtCloseTime.c_str());
        }
    }
#endif

    fp = nullptr;
    return ret;
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int VSIUnixStdioHandle::Seek(vsi_l_offset nOffsetIn, int nWhence)
{
    bAtEOF = false;

    // Seeks that do nothing are still surprisingly expensive with MSVCRT.
    // try and short circuit if possible.
    if (!bModeAppendReadWrite && nWhence == SEEK_SET && nOffsetIn == m_nOffset)
        return 0;

    // On a read-only file, we can avoid a lseek() system call to be issued
    // if the next position to seek to is within the buffered page.
    if (bReadOnly && nWhence == SEEK_SET)
    {
        const int l_PAGE_SIZE = 4096;
        if (nOffsetIn > m_nOffset && nOffsetIn < l_PAGE_SIZE + m_nOffset)
        {
            const int nDiff = static_cast<int>(nOffsetIn - m_nOffset);
            // Do not zero-initialize the buffer. We don't read from it
            GByte abyTemp[l_PAGE_SIZE];
            const int nRead = static_cast<int>(fread(abyTemp, 1, nDiff, fp));
            if (nRead == nDiff)
            {
                m_nOffset = nOffsetIn;
                bLastOpWrite = false;
                bLastOpRead = false;
                return 0;
            }
        }
    }

#if !defined(UNIX_STDIO_64) && SIZEOF_UNSIGNED_LONG == 4
    if (nOffsetIn > static_cast<vsi_l_offset>(std::numeric_limits<long>::max()))
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt at seeking beyond long extent. Lack of 64-bit file I/O");
        return -1;
    }
#endif

    const int nResult = VSI_FSEEK64(fp, nOffsetIn, nWhence);
    const int nError = errno;

#ifdef VSI_DEBUG

    if (nWhence == SEEK_SET)
    {
        VSIDebug3("VSIUnixStdioHandle::Seek(%p," CPL_FRMT_GUIB
                  ",SEEK_SET) = %d",
                  fp, nOffsetIn, nResult);
    }
    else if (nWhence == SEEK_END)
    {
        VSIDebug3("VSIUnixStdioHandle::Seek(%p," CPL_FRMT_GUIB
                  ",SEEK_END) = %d",
                  fp, nOffsetIn, nResult);
    }
    else if (nWhence == SEEK_CUR)
    {
        VSIDebug3("VSIUnixStdioHandle::Seek(%p," CPL_FRMT_GUIB
                  ",SEEK_CUR) = %d",
                  fp, nOffsetIn, nResult);
    }
    else
    {
        VSIDebug4("VSIUnixStdioHandle::Seek(%p," CPL_FRMT_GUIB
                  ",%d-Unknown) = %d",
                  fp, nOffsetIn, nWhence, nResult);
    }

#endif

    if (nResult != -1)
    {
        if (nWhence == SEEK_SET)
        {
            m_nOffset = nOffsetIn;
        }
        else if (nWhence == SEEK_END)
        {
            m_nOffset = VSI_FTELL64(fp);
        }
        else if (nWhence == SEEK_CUR)
        {
            if (nOffsetIn > INT_MAX)
            {
                // printf("likely negative offset intended\n");
            }
            m_nOffset += nOffsetIn;
        }
    }

    bLastOpWrite = false;
    bLastOpRead = false;

    errno = nError;
    return nResult;
}

/************************************************************************/
/*                                Tell()                                */
/************************************************************************/

vsi_l_offset VSIUnixStdioHandle::Tell()

{
#if 0
    const vsi_l_offset nOffset = VSI_FTELL64( fp );
    const int nError = errno;

    VSIDebug2( "VSIUnixStdioHandle::Tell(%p) = %ld",
               fp, static_cast<long>(nOffset) );

    errno = nError;
#endif

    return m_nOffset;
}

/************************************************************************/
/*                               Flush()                                */
/************************************************************************/

int VSIUnixStdioHandle::Flush()

{
    VSIDebug1("VSIUnixStdioHandle::Flush(%p)", fp);

    return fflush(fp);
}

/************************************************************************/
/*                                Read()                                */
/************************************************************************/

size_t VSIUnixStdioHandle::Read(void *pBuffer, size_t nSize, size_t nCount)

{
    /* -------------------------------------------------------------------- */
    /*      If a fwrite() is followed by an fread(), the POSIX rules are    */
    /*      that some of the write may still be buffered and lost.  We      */
    /*      are required to do a seek between to force flushing.   So we    */
    /*      keep careful track of what happened last to know if we          */
    /*      skipped a flushing seek that we may need to do now.             */
    /* -------------------------------------------------------------------- */
    if (!bModeAppendReadWrite && bLastOpWrite)
    {
        if (VSI_FSEEK64(fp, m_nOffset, SEEK_SET) != 0)
        {
            VSIDebug1("Write calling seek failed. %d", m_nOffset);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Perform the read.                                               */
    /* -------------------------------------------------------------------- */
    const size_t nResult = fread(pBuffer, nSize, nCount, fp);

#ifdef VSI_DEBUG
    const int nError = errno;
    VSIDebug4("VSIUnixStdioHandle::Read(%p,%ld,%ld) = %ld", fp,
              static_cast<long>(nSize), static_cast<long>(nCount),
              static_cast<long>(nResult));
    errno = nError;
#endif

    /* -------------------------------------------------------------------- */
    /*      Update current offset.                                          */
    /* -------------------------------------------------------------------- */

#ifdef VSI_COUNT_BYTES_READ
    nTotalBytesRead += nSize * nResult;
#endif

    m_nOffset += nSize * nResult;
    bLastOpWrite = false;
    bLastOpRead = true;

    if (nResult != nCount)
    {
        if (ferror(fp))
            bError = true;
        else
        {
            CPLAssert(feof(fp));
            bAtEOF = true;
        }

        errno = 0;
        vsi_l_offset nNewOffset = VSI_FTELL64(fp);
        if (errno == 0)  // ftell() can fail if we are end of file with a pipe.
            m_nOffset = nNewOffset;
        else
            CPLDebug("VSI", "%s", VSIStrerror(errno));
    }

    return nResult;
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

size_t VSIUnixStdioHandle::Write(const void *pBuffer, size_t nSize,
                                 size_t nCount)

{
    /* -------------------------------------------------------------------- */
    /*      If a fwrite() is followed by an fread(), the POSIX rules are    */
    /*      that some of the write may still be buffered and lost.  We      */
    /*      are required to do a seek between to force flushing.   So we    */
    /*      keep careful track of what happened last to know if we          */
    /*      skipped a flushing seek that we may need to do now.             */
    /* -------------------------------------------------------------------- */
    if (!bModeAppendReadWrite && bLastOpRead)
    {
        if (VSI_FSEEK64(fp, m_nOffset, SEEK_SET) != 0)
        {
            VSIDebug1("Write calling seek failed. %d", m_nOffset);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Perform the write.                                              */
    /* -------------------------------------------------------------------- */
    const size_t nResult = fwrite(pBuffer, nSize, nCount, fp);

#if VSI_DEBUG
    const int nError = errno;

    VSIDebug4("VSIUnixStdioHandle::Write(%p,%ld,%ld) = %ld", fp,
              static_cast<long>(nSize), static_cast<long>(nCount),
              static_cast<long>(nResult));

    errno = nError;
#endif

    /* -------------------------------------------------------------------- */
    /*      Update current offset.                                          */
    /* -------------------------------------------------------------------- */
    m_nOffset += nSize * nResult;
    bLastOpWrite = true;
    bLastOpRead = false;

    return nResult;
}

/************************************************************************/
/*                             ClearErr()                               */
/************************************************************************/

void VSIUnixStdioHandle::ClearErr()

{
    clearerr(fp);
    bAtEOF = false;
    bError = false;
}

/************************************************************************/
/*                              Error()                                 */
/************************************************************************/

int VSIUnixStdioHandle::Error()

{
    return bError ? TRUE : FALSE;
}

/************************************************************************/
/*                                Eof()                                 */
/************************************************************************/

int VSIUnixStdioHandle::Eof()

{
    return bAtEOF ? TRUE : FALSE;
}

/************************************************************************/
/*                             Truncate()                               */
/************************************************************************/

int VSIUnixStdioHandle::Truncate(vsi_l_offset nNewSize)
{
    fflush(fp);
    return VSI_FTRUNCATE64(fileno(fp), nNewSize);
}

/************************************************************************/
/*                          GetRangeStatus()                            */
/************************************************************************/

#ifdef __linux
#if !defined(MISSING_LINUX_FS_H)
#include <linux/fs.h>  // FS_IOC_FIEMAP
#endif
#ifdef FS_IOC_FIEMAP
#include <linux/types.h>   // for types used in linux/fiemap.h
#include <linux/fiemap.h>  // struct fiemap
#endif
#include <sys/ioctl.h>
#include <errno.h>
#endif

VSIRangeStatus VSIUnixStdioHandle::GetRangeStatus(vsi_l_offset
#ifdef FS_IOC_FIEMAP
                                                      nOffset
#endif
                                                  ,
                                                  vsi_l_offset
#ifdef FS_IOC_FIEMAP
                                                      nLength
#endif
)
{
#ifdef FS_IOC_FIEMAP
    // fiemap IOCTL documented at
    // https://www.kernel.org/doc/Documentation/filesystems/fiemap.txt

    // The fiemap struct contains a "variable length" array at its end
    // As we are interested in only one extent, we allocate the base size of
    // fiemap + one fiemap_extent.
    GByte abyBuffer[sizeof(struct fiemap) + sizeof(struct fiemap_extent)];
    int fd = fileno(fp);
    struct fiemap *psExtentMap = reinterpret_cast<struct fiemap *>(&abyBuffer);
    memset(psExtentMap, 0,
           sizeof(struct fiemap) + sizeof(struct fiemap_extent));
    psExtentMap->fm_start = nOffset;
    psExtentMap->fm_length = nLength;
    psExtentMap->fm_extent_count = 1;
    int ret = ioctl(fd, FS_IOC_FIEMAP, psExtentMap);
    if (ret < 0)
        return VSI_RANGE_STATUS_UNKNOWN;
    if (psExtentMap->fm_mapped_extents == 0)
        return VSI_RANGE_STATUS_HOLE;
    // In case there is one extent with unknown status, retry after having
    // asked the kernel to sync the file.
    const fiemap_extent *pasExtent = &(psExtentMap->fm_extents[0]);
    if (psExtentMap->fm_mapped_extents == 1 &&
        (pasExtent[0].fe_flags & FIEMAP_EXTENT_UNKNOWN) != 0)
    {
        psExtentMap->fm_flags = FIEMAP_FLAG_SYNC;
        psExtentMap->fm_start = nOffset;
        psExtentMap->fm_length = nLength;
        psExtentMap->fm_extent_count = 1;
        ret = ioctl(fd, FS_IOC_FIEMAP, psExtentMap);
        if (ret < 0)
            return VSI_RANGE_STATUS_UNKNOWN;
        if (psExtentMap->fm_mapped_extents == 0)
            return VSI_RANGE_STATUS_HOLE;
    }
    return VSI_RANGE_STATUS_DATA;
#else
    static bool bMessageEmitted = false;
    if (!bMessageEmitted)
    {
        CPLDebug("VSI", "Sorry: GetExtentStatus() not implemented for "
                        "this operating system");
        bMessageEmitted = true;
    }
    return VSI_RANGE_STATUS_UNKNOWN;
#endif
}

/************************************************************************/
/*                             HasPRead()                               */
/************************************************************************/

#if defined(HAVE_PREAD64) || (defined(HAVE_PREAD_BSD) && SIZEOF_OFF_T == 8)
bool VSIUnixStdioHandle::HasPRead() const
{
    return true;
}

/************************************************************************/
/*                              PRead()                                 */
/************************************************************************/

size_t VSIUnixStdioHandle::PRead(void *pBuffer, size_t nSize,
                                 vsi_l_offset nOffset) const
{
#ifdef HAVE_PREAD64
    return pread64(fileno(fp), pBuffer, nSize, nOffset);
#else
    return pread(fileno(fp), pBuffer, nSize, static_cast<off_t>(nOffset));
#endif
}
#endif

/************************************************************************/
/* ==================================================================== */
/*                       VSIUnixStdioFilesystemHandler                  */
/* ==================================================================== */
/************************************************************************/

#ifdef VSI_COUNT_BYTES_READ
/************************************************************************/
/*                     ~VSIUnixStdioFilesystemHandler()                 */
/************************************************************************/

VSIUnixStdioFilesystemHandler::~VSIUnixStdioFilesystemHandler()
{
    CPLDebug(
        "VSI",
        "~VSIUnixStdioFilesystemHandler() : nTotalBytesRead = " CPL_FRMT_GUIB,
        nTotalBytesRead);

    if (hMutex != nullptr)
        CPLDestroyMutex(hMutex);
    hMutex = nullptr;
}
#endif

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

VSIVirtualHandle *
VSIUnixStdioFilesystemHandler::Open(const char *pszFilename,
                                    const char *pszAccess, bool bSetError,
                                    CSLConstList /* papszOptions */)

{
    FILE *fp = VSI_FOPEN64(pszFilename, pszAccess);
    const int nError = errno;

    VSIDebug3("VSIUnixStdioFilesystemHandler::Open(\"%s\",\"%s\") = %p",
              pszFilename, pszAccess, fp);

    if (fp == nullptr)
    {
        if (bSetError)
        {
            VSIError(VSIE_FileError, "%s: %s", pszFilename, strerror(nError));
        }
        errno = nError;
        return nullptr;
    }

    const bool bReadOnly =
        strcmp(pszAccess, "rb") == 0 || strcmp(pszAccess, "r") == 0;
    const bool bModeAppendReadWrite =
        strcmp(pszAccess, "a+b") == 0 || strcmp(pszAccess, "a+") == 0;
    VSIUnixStdioHandle *poHandle = new (std::nothrow)
        VSIUnixStdioHandle(this, fp, bReadOnly, bModeAppendReadWrite);
    if (poHandle == nullptr)
    {
        fclose(fp);
        return nullptr;
    }

    errno = nError;

    /* -------------------------------------------------------------------- */
    /*      If VSI_CACHE is set we want to use a cached reader instead      */
    /*      of more direct io on the underlying file.                       */
    /* -------------------------------------------------------------------- */
    if (bReadOnly && CPLTestBool(CPLGetConfigOption("VSI_CACHE", "FALSE")))
    {
        return VSICreateCachedFile(poHandle);
    }

    return poHandle;
}

/************************************************************************/
/*                      CreateOnlyVisibleAtCloseTime()                  */
/************************************************************************/

VSIVirtualHandle *VSIUnixStdioFilesystemHandler::CreateOnlyVisibleAtCloseTime(
    const char *pszFilename, bool bEmulationAllowed, CSLConstList papszOptions)
{
#ifdef __linux
    static bool bIsLinkatSupported = []()
    {
        // Check that /proc is accessible, since we will need it to run linkat()
        struct stat statbuf;
        return stat("/proc/self/fd", &statbuf) == 0;
    }();

    const int fd = bIsLinkatSupported
                       ? open(CPLGetDirnameSafe(pszFilename).c_str(),
                              O_TMPFILE | O_RDWR, 0666)
                       : -1;
    if (fd < 0)
        return VSIFilesystemHandler::CreateOnlyVisibleAtCloseTime(
            pszFilename, bEmulationAllowed, papszOptions);

    FILE *fp = fdopen(fd, "wb+");
    if (!fp)
    {
        close(fd);
        return nullptr;
    }

    VSIUnixStdioHandle *poHandle = new (std::nothrow) VSIUnixStdioHandle(
        this, fp, /* bReadOnly = */ false, /* bModeAppendReadWrite = */ false);
    if (poHandle)
    {
        poHandle->m_osFilenameToSetAtCloseTime = pszFilename;
    }
    return poHandle;
#else
    if (!bEmulationAllowed)
        return nullptr;

    std::string osTmpFilename = std::string(pszFilename).append("XXXXXX");
    int fd = mkstemp(osTmpFilename.data());
    if (fd < 0)
    {
        return VSIFilesystemHandler::CreateOnlyVisibleAtCloseTime(
            pszFilename, bEmulationAllowed, papszOptions);
    }

    FILE *fp = fdopen(fd, "wb+");
    if (!fp)
    {
        close(fd);
        return nullptr;
    }

    VSIUnixStdioHandle *poHandle = new (std::nothrow) VSIUnixStdioHandle(
        this, fp, /* bReadOnly = */ false, /* bModeAppendReadWrite = */ false);
    if (poHandle)
    {
        poHandle->m_osTmpFilename = std::move(osTmpFilename);
        poHandle->m_osFilenameToSetAtCloseTime = pszFilename;
    }
    return poHandle;
#endif
}

/************************************************************************/
/*                                Stat()                                */
/************************************************************************/

int VSIUnixStdioFilesystemHandler::Stat(const char *pszFilename,
                                        VSIStatBufL *pStatBuf, int /* nFlags */)
{
    return (VSI_STAT64(pszFilename, pStatBuf));
}

/************************************************************************/
/*                               Unlink()                               */
/************************************************************************/

int VSIUnixStdioFilesystemHandler::Unlink(const char *pszFilename)

{
    return unlink(pszFilename);
}

/************************************************************************/
/*                               Rename()                               */
/************************************************************************/

int VSIUnixStdioFilesystemHandler::Rename(const char *oldpath,
                                          const char *newpath, GDALProgressFunc,
                                          void *)

{
    return rename(oldpath, newpath);
}

/************************************************************************/
/*                               Mkdir()                                */
/************************************************************************/

int VSIUnixStdioFilesystemHandler::Mkdir(const char *pszPathname, long nMode)

{
    return mkdir(pszPathname, static_cast<int>(nMode));
}

/************************************************************************/
/*                               Rmdir()                                */
/************************************************************************/

int VSIUnixStdioFilesystemHandler::Rmdir(const char *pszPathname)

{
    return rmdir(pszPathname);
}

/************************************************************************/
/*                              ReadDirEx()                             */
/************************************************************************/

char **VSIUnixStdioFilesystemHandler::ReadDirEx(const char *pszPath,
                                                int nMaxFiles)

{
    if (strlen(pszPath) == 0)
        pszPath = ".";

    CPLStringList oDir;
    DIR *hDir = opendir(pszPath);
    if (hDir != nullptr)
    {
        // We want to avoid returning NULL for an empty list.
        oDir.Assign(static_cast<char **>(CPLCalloc(2, sizeof(char *))));

        struct dirent *psDirEntry = nullptr;
        while ((psDirEntry = readdir(hDir)) != nullptr)
        {
            oDir.AddString(psDirEntry->d_name);
            if (nMaxFiles > 0 && oDir.Count() > nMaxFiles)
                break;
        }

        closedir(hDir);
    }
    else
    {
        // Should we generate an error?
        // For now we'll just return NULL (at the end of the function).
    }

    return oDir.StealList();
}

/************************************************************************/
/*                        GetDiskFreeSpace()                            */
/************************************************************************/

GIntBig VSIUnixStdioFilesystemHandler::GetDiskFreeSpace(const char *
#ifdef HAVE_STATVFS
                                                            pszDirname
#endif
)
{
    GIntBig nRet = -1;
#ifdef HAVE_STATVFS

#ifdef HAVE_STATVFS64
    struct statvfs64 buf;
    if (statvfs64(pszDirname, &buf) == 0)
    {
        nRet = static_cast<GIntBig>(buf.f_frsize *
                                    static_cast<GUIntBig>(buf.f_bavail));
    }
#else
    struct statvfs buf;
    if (statvfs(pszDirname, &buf) == 0)
    {
        nRet = static_cast<GIntBig>(buf.f_frsize *
                                    static_cast<GUIntBig>(buf.f_bavail));
    }
#endif

#endif
    return nRet;
}

/************************************************************************/
/*                      SupportsSparseFiles()                           */
/************************************************************************/

#ifdef __linux
#include <sys/vfs.h>
#endif

int VSIUnixStdioFilesystemHandler::SupportsSparseFiles(const char *
#ifdef __linux
                                                           pszPath
#endif
)
{
#ifdef __linux
    struct statfs sStatFS;
    if (statfs(pszPath, &sStatFS) == 0)
    {
        // Add here any missing filesystem supporting sparse files.
        // See http://en.wikipedia.org/wiki/Comparison_of_file_systems
        switch (static_cast<unsigned>(sStatFS.f_type))
        {
            // Codes from http://man7.org/linux/man-pages/man2/statfs.2.html
            case 0xef53U:      // ext2, 3, 4
            case 0x52654973U:  // reiser
            case 0x58465342U:  // xfs
            case 0x3153464aU:  // jfs
            case 0x5346544eU:  // ntfs
            case 0x9123683eU:  // brfs
            // nfs: NFS < 4.2 supports creating sparse files (but reading them
            // not efficiently).
            case 0x6969U:
            case 0x01021994U:  // tmpfs
                return TRUE;

            case 0x4d44U:  // msdos
                return FALSE;

            case 0x53464846U:  // Windows Subsystem for Linux fs
            {
                static bool bUnknownFSEmitted = false;
                if (!bUnknownFSEmitted)
                {
                    CPLDebug("VSI",
                             "Windows Subsystem for Linux FS is at "
                             "the time of writing not known to support sparse "
                             "files");
                    bUnknownFSEmitted = true;
                }
                return FALSE;
            }

            default:
            {
                static bool bUnknownFSEmitted = false;
                if (!bUnknownFSEmitted)
                {
                    CPLDebug("VSI",
                             "Filesystem with type %X unknown. "
                             "Assuming it does not support sparse files",
                             static_cast<int>(sStatFS.f_type));
                    bUnknownFSEmitted = true;
                }
                return FALSE;
            }
        }
    }
    return FALSE;
#else
    static bool bMessageEmitted = false;
    if (!bMessageEmitted)
    {
        CPLDebug("VSI", "Sorry: SupportsSparseFiles() not implemented "
                        "for this operating system");
        bMessageEmitted = true;
    }
    return FALSE;
#endif
}

/************************************************************************/
/*                          IsLocal()                                   */
/************************************************************************/

bool VSIUnixStdioFilesystemHandler::IsLocal(const char *
#ifdef __linux
                                                pszPath
#endif
)
{
#ifdef __linux
    struct statfs sStatFS;
    if (statfs(pszPath, &sStatFS) == 0)
    {
        // See http://en.wikipedia.org/wiki/Comparison_of_file_systems
        switch (static_cast<unsigned>(sStatFS.f_type))
        {
            // Codes from http://man7.org/linux/man-pages/man2/statfs.2.html
            case 0x6969U:      // NFS
            case 0x517bU:      // SMB
            case 0xff534d42U:  // CIFS
            case 0xfe534d42U:  // SMB2
                // (https://github.com/libuv/libuv/blob/97dcdb1926f6aca43171e1614338bcef067abd59/src/unix/fs.c#L960)
                return false;
        }
    }
#else
    static bool bMessageEmitted = false;
    if (!bMessageEmitted)
    {
        CPLDebug("VSI", "Sorry: IsLocal() not implemented "
                        "for this operating system");
        bMessageEmitted = true;
    }
#endif
    return true;
}

/************************************************************************/
/*                    SupportsSequentialWrite()                         */
/************************************************************************/

bool VSIUnixStdioFilesystemHandler::SupportsSequentialWrite(
    const char *pszPath, bool /* bAllowLocalTempFile */)
{
    VSIStatBufL sStat;
    if (VSIStatL(pszPath, &sStat) == 0)
        return access(pszPath, W_OK) == 0;
    return access(CPLGetDirnameSafe(pszPath).c_str(), W_OK) == 0;
}

/************************************************************************/
/*                     SupportsRandomWrite()                            */
/************************************************************************/

bool VSIUnixStdioFilesystemHandler::SupportsRandomWrite(
    const char *pszPath, bool /* bAllowLocalTempFile */)
{
    return SupportsSequentialWrite(pszPath, false);
}

/************************************************************************/
/*                            VSIDIRUnixStdio                           */
/************************************************************************/

struct VSIDIRUnixStdio final : public VSIDIR
{
    struct DIRCloser
    {
        void operator()(DIR *d)
        {
            if (d)
                closedir(d);
        }
    };

    CPLString osRootPath{};
    CPLString osBasePath{};
    std::unique_ptr<DIR, DIRCloser> m_psDir{};
    int nRecurseDepth = 0;
    VSIDIREntry entry{};
    std::vector<std::unique_ptr<VSIDIR>> aoStackSubDir{};
    std::string m_osFilterPrefix{};
    bool m_bNameAndTypeOnly = false;

    const VSIDIREntry *NextDirEntry() override;
};

/************************************************************************/
/*                        OpenDirInternal()                             */
/************************************************************************/

/* static */
std::unique_ptr<VSIDIRUnixStdio> VSIUnixStdioFilesystemHandler::OpenDirInternal(
    const char *pszPath, int nRecurseDepth, const char *const *papszOptions)
{
    std::unique_ptr<DIR, VSIDIRUnixStdio::DIRCloser> psDir(opendir(pszPath));
    if (psDir == nullptr)
    {
        return nullptr;
    }
    auto dir = std::make_unique<VSIDIRUnixStdio>();
    dir->osRootPath = pszPath;
    dir->nRecurseDepth = nRecurseDepth;
    dir->m_psDir = std::move(psDir);
    dir->m_osFilterPrefix = CSLFetchNameValueDef(papszOptions, "PREFIX", "");
    dir->m_bNameAndTypeOnly = CPLTestBool(
        CSLFetchNameValueDef(papszOptions, "NAME_AND_TYPE_ONLY", "NO"));
    return dir;
}

/************************************************************************/
/*                            OpenDir()                                 */
/************************************************************************/

VSIDIR *VSIUnixStdioFilesystemHandler::OpenDir(const char *pszPath,
                                               int nRecurseDepth,
                                               const char *const *papszOptions)
{
    return OpenDirInternal(pszPath, nRecurseDepth, papszOptions).release();
}

/************************************************************************/
/*                           NextDirEntry()                             */
/************************************************************************/

const VSIDIREntry *VSIDIRUnixStdio::NextDirEntry()
{
begin:
    if (VSI_ISDIR(entry.nMode) && nRecurseDepth != 0)
    {
        CPLString osCurFile(osRootPath);
        if (!osCurFile.empty())
            osCurFile += '/';
        osCurFile += entry.pszName;
        auto subdir = VSIUnixStdioFilesystemHandler::OpenDirInternal(
            osCurFile, nRecurseDepth - 1, nullptr);
        if (subdir)
        {
            subdir->osRootPath = osRootPath;
            subdir->osBasePath = entry.pszName;
            subdir->m_osFilterPrefix = m_osFilterPrefix;
            subdir->m_bNameAndTypeOnly = m_bNameAndTypeOnly;
            aoStackSubDir.push_back(std::move(subdir));
        }
        entry.nMode = 0;
    }

    while (!aoStackSubDir.empty())
    {
        auto l_entry = aoStackSubDir.back()->NextDirEntry();
        if (l_entry)
        {
            return l_entry;
        }
        aoStackSubDir.pop_back();
    }

    while (const auto *psEntry = readdir(m_psDir.get()))
    {
        // Skip . and ..entries
        if (psEntry->d_name[0] == '.' &&
            (psEntry->d_name[1] == '\0' ||
             (psEntry->d_name[1] == '.' && psEntry->d_name[2] == '\0')))
        {
            // do nothing
        }
        else
        {
            CPLFree(entry.pszName);
            CPLString osName(osBasePath);
            if (!osName.empty())
                osName += '/';
            osName += psEntry->d_name;

            entry.pszName = CPLStrdup(osName);
            entry.nMode = 0;
            entry.nSize = 0;
            entry.nMTime = 0;
            entry.bModeKnown = false;
            entry.bSizeKnown = false;
            entry.bMTimeKnown = false;

            CPLString osCurFile(osRootPath);
            if (!osCurFile.empty())
                osCurFile += '/';
            osCurFile += entry.pszName;

#if !defined(__sun) && !defined(__HAIKU__)
            if (psEntry->d_type == DT_REG)
                entry.nMode = S_IFREG;
            else if (psEntry->d_type == DT_DIR)
                entry.nMode = S_IFDIR;
            else if (psEntry->d_type == DT_LNK)
                entry.nMode = S_IFLNK;
#endif

            const auto StatFile = [&osCurFile, this]()
            {
                VSIStatBufL sStatL;
                if (VSIStatL(osCurFile, &sStatL) == 0)
                {
                    entry.nMode = sStatL.st_mode;
                    entry.nSize = sStatL.st_size;
                    entry.nMTime = sStatL.st_mtime;
                    entry.bModeKnown = true;
                    entry.bSizeKnown = true;
                    entry.bMTimeKnown = true;
                }
            };

            if (!m_osFilterPrefix.empty() &&
                m_osFilterPrefix.size() > osName.size())
            {
                if (STARTS_WITH(m_osFilterPrefix.c_str(), osName.c_str()) &&
                    m_osFilterPrefix[osName.size()] == '/')
                {
#if !defined(__sun) && !defined(__HAIKU__)
                    if (psEntry->d_type == DT_UNKNOWN)
#endif
                    {
                        StatFile();
                    }
                    if (VSI_ISDIR(entry.nMode))
                    {
                        goto begin;
                    }
                }
                continue;
            }
            if (!m_osFilterPrefix.empty() &&
                !STARTS_WITH(osName.c_str(), m_osFilterPrefix.c_str()))
            {
                continue;
            }

            if (!m_bNameAndTypeOnly
#if !defined(__sun) && !defined(__HAIKU__)
                || psEntry->d_type == DT_UNKNOWN
#endif
            )
            {
                StatFile();
            }

            return &(entry);
        }
    }

    return nullptr;
}

#ifdef VSI_COUNT_BYTES_READ
/************************************************************************/
/*                            AddToTotal()                              */
/************************************************************************/

void VSIUnixStdioFilesystemHandler::AddToTotal(vsi_l_offset nBytes)
{
    CPLMutexHolder oHolder(&hMutex);
    nTotalBytesRead += nBytes;
}

#endif

/************************************************************************/
/*                      GetCanonicalFilename()                          */
/************************************************************************/

#ifdef HAS_CASE_INSENSITIVE_FILE_SYSTEM
std::string VSIUnixStdioFilesystemHandler::GetCanonicalFilename(
    const std::string &osFilename) const
{
    char szResolvedPath[PATH_MAX];
    const char *pszFilename = osFilename.c_str();
    if (realpath(pszFilename, szResolvedPath))
    {
        const char *pszFilenameLastPart = strrchr(pszFilename, '/');
        const char *pszResolvedFilenameLastPart = strrchr(szResolvedPath, '/');
        if (pszFilenameLastPart && pszResolvedFilenameLastPart &&
            EQUAL(pszFilenameLastPart, pszResolvedFilenameLastPart))
        {
            std::string osRet;
            osRet.assign(pszFilename, pszFilenameLastPart - pszFilename);
            osRet += pszResolvedFilenameLastPart;
            return osRet;
        }
        return szResolvedPath;
    }
    return osFilename;
}
#endif

/************************************************************************/
/*                     VSIInstallLargeFileHandler()                     */
/************************************************************************/

void VSIInstallLargeFileHandler()

{
    VSIFileManager::InstallHandler("", new VSIUnixStdioFilesystemHandler());
}

#endif  // ndef WIN32

//! @endcond
