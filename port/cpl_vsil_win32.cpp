/**********************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Implement VSI large file api for Win32.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 **********************************************************************
 * Copyright (c) 2000, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_vsi_virtual.h"

#if defined(_WIN32)

#include <windows.h>
#include <winioctl.h>  // for FSCTL_SET_SPARSE
#include <winternl.h>  // for NtCreateFile

#include "cpl_string.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>

#include <cwchar>
#include <type_traits>

/************************************************************************/
/* ==================================================================== */
/*                       VSIWin32FilesystemHandler                      */
/* ==================================================================== */
/************************************************************************/

// To avoid aliasing to GetDiskFreeSpace to GetDiskFreeSpaceA on Windows
#ifdef GetDiskFreeSpace
#undef GetDiskFreeSpace
#endif

static void VSIWin32TryLongFilename(wchar_t *&pwszFilename);

struct VSIDIRWin32;

class VSIWin32FilesystemHandler final : public VSIFilesystemHandler
{
    CPL_DISALLOW_COPY_ASSIGN(VSIWin32FilesystemHandler)

  public:
    // TODO(schwehr): Fix Open call to remove the need for this using call.
    using VSIFilesystemHandler::Open;

    VSIWin32FilesystemHandler() = default;

    virtual VSIVirtualHandle *Open(const char *pszFilename,
                                   const char *pszAccess, bool bSetError,
                                   CSLConstList /* papszOptions */) override;

    VSIVirtualHandle *
    CreateOnlyVisibleAtCloseTime(const char *pszFilename,
                                 bool bEmulationAllowed,
                                 CSLConstList papszOptions) override;

    virtual int Stat(const char *pszFilename, VSIStatBufL *pStatBuf,
                     int nFlags) override;
    virtual int Unlink(const char *pszFilename) override;
    virtual int Rename(const char *oldpath, const char *newpath,
                       GDALProgressFunc, void *) override;
    virtual int Mkdir(const char *pszDirname, long nMode) override;
    virtual int Rmdir(const char *pszDirname) override;
    virtual char **ReadDirEx(const char *pszDirname, int nMaxFiles) override;

    virtual int IsCaseSensitive(const char *pszFilename) override
    {
        (void)pszFilename;
        return FALSE;
    }

    virtual GIntBig GetDiskFreeSpace(const char *pszDirname) override;
    virtual int SupportsSparseFiles(const char *pszPath) override;
    virtual bool IsLocal(const char *pszPath) override;
    std::string
    GetCanonicalFilename(const std::string &osFilename) const override;

    VSIDIR *OpenDir(const char *pszPath, int nRecurseDepth,
                    const char *const *papszOptions) override;

    static std::unique_ptr<VSIDIRWin32>
    OpenDirInternal(const char *pszPath, int nRecurseDepth,
                    const char *const *papszOptions);

    const char *GetDirectorySeparator(const char *pszPath) override
    {
        // Return forward slash for paths of the form
        // "{drive_letter}:/{rest_of_the_path}", and backslash otherwise.
        return (pszPath[0] && pszPath[1] == ':' && pszPath[2] == '/') ? "/"
                                                                      : "\\";
    }
};

/************************************************************************/
/* ==================================================================== */
/*                            VSIWin32Handle                            */
/* ==================================================================== */
/************************************************************************/

class VSIWin32Handle final : public VSIVirtualHandle
{
    CPL_DISALLOW_COPY_ASSIGN(VSIWin32Handle)

  public:
    HANDLE hFile = nullptr;
    bool bEOF = false;
    bool bError = false;
    bool m_bWriteThrough = false;

    bool m_bCancelCreation = false;
    std::string m_osFilenameToSetAtCloseTime{};

    VSIWin32Handle() = default;
    ~VSIWin32Handle() override;

    int Seek(vsi_l_offset nOffset, int nWhence) override;
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
        return static_cast<void *>(hFile);
    }

    VSIRangeStatus GetRangeStatus(vsi_l_offset nOffset,
                                  vsi_l_offset nLength) override;

    void CancelCreation() override
    {
        m_bCancelCreation = true;
    }
};

/************************************************************************/
/*                      ErrnoFromGetLastError()                         */
/*                                                                      */
/* Private function translating Windows API error codes to POSIX errno. */
/*                                                                      */
/* TODO: If the function is going to be public CPL function, then       */
/* replace the switch with array of (Win32 code, errno code) tuples and */
/* complement it with missing codes.                                    */
/************************************************************************/

static int ErrnoFromGetLastError(DWORD dwError = 0)
{
    int err = 0;
    if (dwError == 0)
        dwError = GetLastError();

    switch (dwError)
    {
        case NO_ERROR:
            err = 0;
            break;
        case ERROR_FILE_NOT_FOUND:       /* Cannot find the file specified. */
        case ERROR_PATH_NOT_FOUND:       /* Cannot find the path specified. */
        case ERROR_INVALID_DRIVE:        /* Cannot find the drive specified. */
        case ERROR_NO_MORE_FILES:        /* There are no more files. */
        case ERROR_BAD_PATHNAME:         /* The specified path is invalid. */
        case ERROR_BAD_NETPATH:          /* The network path was not found. */
        case ERROR_FILENAME_EXCED_RANGE: /* The filename or extension is too
                                            long. */
            err = ENOENT;
            break;
        case ERROR_TOO_MANY_OPEN_FILES: /* The system cannot open the file. */
            err = EMFILE;
            break;
        case ERROR_ACCESS_DENIED:     /* Access denied. */
        case ERROR_CURRENT_DIRECTORY: /* The directory cannot be removed. */
        case ERROR_WRITE_PROTECT:     /* The media is write protected. */
        case ERROR_LOCK_VIOLATION:    /* Another process has locked a portion of
                                         the file. */
        case ERROR_WRONG_DISK:        /* The wrong diskette is in the drive. */
        case ERROR_SHARING_BUFFER_EXCEEDED: /* Too many files opened for
                                               sharing. */
        case ERROR_DRIVE_LOCKED:      /* The disk is in use or locked by another
                                         process. */
        case ERROR_LOCK_FAILED:       /* Unable to lock a region of a file. */
        case ERROR_SEEK_ON_DEVICE:    /* The file pointer cannot be set on the
                                         specified device or file. */
        case ERROR_SHARING_VIOLATION: /* The process cannot access the file
                                         because it is being used by another
                                         process. */
            err = EACCES;
            break;
        case ERROR_INVALID_HANDLE:        /* The handle is invalid. */
        case ERROR_INVALID_TARGET_HANDLE: /* The target internal file identifier
                                             is incorrect. */
        case ERROR_DIRECT_ACCESS_HANDLE:  /* Operation other than raw disk I/O
                                             not permitted. */
            err = EBADF;
            break;
        case ERROR_ARENA_TRASHED: /* The storage control blocks were destroyed.
                                   */
        case ERROR_NOT_ENOUGH_MEMORY: /* Not enough storage is available. */
        case ERROR_INVALID_BLOCK:     /* The storage control block address is
                                         invalid. */
        case ERROR_NOT_ENOUGH_QUOTA: /* Not enough quota is available to process
                                        this command. */
            err = ENOMEM;
            break;
        case ERROR_BAD_ENVIRONMENT: /* The environment is incorrect. */
            err = E2BIG;
            break;
        case ERROR_INVALID_ACCESS: /* The access code is invalid. */
        case ERROR_INVALID_DATA:   /* The data is invalid. */
            err = EINVAL;
            break;
        case ERROR_NOT_SAME_DEVICE: /* The system cannot move the file to a
                                       different disk drive. */
            err = EXDEV;
            break;
        case ERROR_DIR_NOT_EMPTY: /* The directory is not empty. */
            err = ENOTEMPTY;
            break;
        case ERROR_FILE_EXISTS:    /* The file exists. */
        case ERROR_ALREADY_EXISTS: /* Cannot create a file when that file
                                      already exists. */
            err = EEXIST;
            break;
        case ERROR_DISK_FULL: /* There is not enough space on the disk. */
            err = ENOSPC;
            break;
        case ERROR_HANDLE_EOF: /* Reached the end of the file. */
            err = 0;           /* There is no errno equivalent in the errno.h */
            break;
        default:
            err = 0;
    }
    CPLAssert(0 <= err);

    return err;
}

/************************************************************************/
/*                          ~VSIWin32Handle()                           */
/************************************************************************/

VSIWin32Handle::~VSIWin32Handle()
{
    VSIWin32Handle::Close();
}

/************************************************************************/
/*                               Close()                                */
/************************************************************************/

int VSIWin32Handle::Close()

{
    if (!hFile)
        return 0;

    int ret = 0;

    if (!m_bCancelCreation && !m_osFilenameToSetAtCloseTime.empty())
    {
        // Rename the file
        wchar_t *finalPath =
            CPLRecodeToWChar(CPLString(m_osFilenameToSetAtCloseTime)
                                 .replaceAll('/', '\\')
                                 .c_str(),
                             CPL_ENC_UTF8, CPL_ENC_UCS2);

        if (!cpl::starts_with(m_osFilenameToSetAtCloseTime, "\\\\?\\"))
            VSIWin32TryLongFilename(finalPath);

#ifdef DEBUG_VERBOSE
        {
            char *pszWin32Filename =
                CPLRecodeFromWChar(finalPath, CPL_ENC_UCS2, CPL_ENC_UTF8);
            CPLDebug("CPL", "FileRenameInfo('%s')", pszWin32Filename);
            CPLFree(pszWin32Filename);
        }
#endif
        const size_t renameLen =
            sizeof(FILE_RENAME_INFO) + wcslen(finalPath) * sizeof(WCHAR);
        FILE_RENAME_INFO *renameInfo =
            static_cast<FILE_RENAME_INFO *>(CPLCalloc(1, renameLen));
        renameInfo->ReplaceIfExists = TRUE;
        renameInfo->FileNameLength =
            static_cast<DWORD>(wcslen(finalPath) * sizeof(WCHAR));
        wcscpy(renameInfo->FileName, finalPath);
        CPLFree(finalPath);

        const bool bRet = SetFileInformationByHandle(
            hFile, FileRenameInfo, renameInfo, static_cast<DWORD>(renameLen));
        CPLFree(renameInfo);

        if (!bRet)
        {
            CPLDebug("CPL",
                     "SetFileInformationByHandle FileRenameInfo failed: %lu",
                     GetLastError());
            ret = -1;
        }

        // Unhide the file
        FILE_BASIC_INFO basicInfo;
        if (ret == 0 &&
            !GetFileInformationByHandleEx(hFile, FileBasicInfo, &basicInfo,
                                          sizeof(basicInfo)))
        {
            CPLDebug("CPL", "GetFileInformationByHandleEx failed: %lu",
                     GetLastError());
            ret = -1;
        }
        basicInfo.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (ret == 0 &&
            !SetFileInformationByHandle(hFile, FileBasicInfo, &basicInfo,
                                        sizeof(basicInfo)))
        {
            CPLDebug("CPL",
                     "SetFileInformationByHandle FileBasicInfo failed: %lu",
                     GetLastError());
            ret = -1;
        }

        // Remove FILE_DELETE_ON_CLOSE
        FILE_DISPOSITION_INFO info = {FALSE};
        if (ret == 0 && !SetFileInformationByHandle(hFile, FileDispositionInfo,
                                                    &info, sizeof(info)))
        {
            CPLDebug(
                "CPL",
                "SetFileInformationByHandle FileDispositionInfo failed: %lu",
                GetLastError());
            ret = -1;
        }
    }

    int ret2 = CloseHandle(hFile) ? 0 : -1;
    if (ret == 0 && ret2 != 0)
        ret = ret2;
    hFile = nullptr;

    if (m_bCancelCreation && !m_osFilenameToSetAtCloseTime.empty())
        VSIUnlink(m_osFilenameToSetAtCloseTime.c_str());

    return ret;
}

/************************************************************************/
/*                                Seek()                                */
/************************************************************************/

int VSIWin32Handle::Seek(vsi_l_offset nOffset, int nWhence)

{
    LONG dwMoveMethod, dwMoveHigh;
    GUInt32 nMoveLow;
    LARGE_INTEGER li;

    bEOF = false;

    switch (nWhence)
    {
        case SEEK_CUR:
            dwMoveMethod = FILE_CURRENT;
            break;
        case SEEK_END:
            dwMoveMethod = FILE_END;
            break;
        case SEEK_SET:
        default:
            dwMoveMethod = FILE_BEGIN;
            break;
    }

    li.QuadPart = nOffset;
    nMoveLow = li.LowPart;
    dwMoveHigh = li.HighPart;

    SetLastError(0);
    SetFilePointer(hFile, nMoveLow, &dwMoveHigh, dwMoveMethod);

    if (GetLastError() != NO_ERROR)
    {
#ifdef notdef
        LPVOID lpMsgBuf = nullptr;

        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR)&lpMsgBuf, 0, nullptr);

        printf("[ERROR %d]\n %s\n", GetLastError(), (char *)lpMsgBuf); /*ok*/
        printf("nOffset=%u, nMoveLow=%u, dwMoveHigh=%u\n",             /*ok*/
               static_cast<GUInt32>(nOffset), nMoveLow, dwMoveHigh);
#endif
        errno = ErrnoFromGetLastError();
        return -1;
    }
    else
        return 0;
}

/************************************************************************/
/*                                Tell()                                */
/************************************************************************/

vsi_l_offset VSIWin32Handle::Tell()

{
    LARGE_INTEGER li;

    li.HighPart = 0;
    li.LowPart = SetFilePointer(hFile, 0, &(li.HighPart), FILE_CURRENT);

    return (static_cast<vsi_l_offset>(li.QuadPart));
}

/************************************************************************/
/*                               Flush()                                */
/************************************************************************/

int VSIWin32Handle::Flush()

{
    /* Nothing needed to offer the same guarantee as POSIX fflush() */
    /* FlushFileBuffers() would be closer to fsync() */
    /* See http://trac.osgeo.org/gdal/ticket/5556 */

    // Add this as a hack to make ogr_mitab_30 and _31 tests pass
    if (!m_bWriteThrough &&
        CPLTestBool(CPLGetConfigOption("VSI_FLUSH", "FALSE")))
    {
        if (!FlushFileBuffers(hFile))
        {
            errno = ErrnoFromGetLastError();
            CPLDebug("CPL", "VSIWin32Handle::Flush() failed with errno=%d (%s)",
                     errno, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/************************************************************************/
/*                                Read()                                */
/************************************************************************/

size_t VSIWin32Handle::Read(void *pBuffer, size_t nSize, size_t nCount)

{
    GByte *const pabyBuffer = static_cast<GByte *>(pBuffer);
    size_t nTotalRead = 0;
    size_t nRemaining = nSize * nCount;
    while (nRemaining > 0)
    {
        DWORD dwSizeRead = 0;
        DWORD dwToRead = static_cast<DWORD>(
            nRemaining > UINT32_MAX ? UINT32_MAX : nRemaining);

        if (!ReadFile(hFile, pabyBuffer + nTotalRead, dwToRead, &dwSizeRead,
                      nullptr))
        {
            bError = true;
            errno = ErrnoFromGetLastError();
            return 0;
        }
        else
        {
            nTotalRead += dwSizeRead;
            nRemaining -= dwSizeRead;
            if (dwSizeRead < dwToRead)
                break;
        }
    }

    size_t nResult = 0;
    if (nSize)
    {
        nResult = nTotalRead / nSize;
        if (nResult != nCount)
            bEOF = true;
    }

    return nResult;
}

/************************************************************************/
/*                               Write()                                */
/************************************************************************/

size_t VSIWin32Handle::Write(const void *pBuffer, size_t nSize, size_t nCount)

{
    DWORD dwSizeWritten = 0;
    size_t nResult = 0;

    if (nSize > 0 && nCount > UINT32_MAX / nSize)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Too many bytes to write at once");
        return 0;
    }

    if (!WriteFile(hFile, pBuffer, static_cast<DWORD>(nSize * nCount),
                   &dwSizeWritten, nullptr))
    {
        nResult = 0;
        errno = ErrnoFromGetLastError();
        CPLDebug("CPL", "VSIWin32Handle::Write() failed with errno=%d (%s)",
                 errno, strerror(errno));
    }
    else if (nSize == 0)
        nResult = 0;
    else
        nResult = dwSizeWritten / nSize;

    return nResult;
}

/************************************************************************/
/*                             ClearErr()                               */
/************************************************************************/

void VSIWin32Handle::ClearErr()

{
    bEOF = false;
    bError = false;
}

/************************************************************************/
/*                              Error()                                 */
/************************************************************************/

int VSIWin32Handle::Error()

{
    return bError ? TRUE : FALSE;
}

/************************************************************************/
/*                                Eof()                                 */
/************************************************************************/

int VSIWin32Handle::Eof()

{
    return bEOF ? TRUE : FALSE;
}

/************************************************************************/
/*                             Truncate()                               */
/************************************************************************/

int VSIWin32Handle::Truncate(vsi_l_offset nNewSize)
{
    vsi_l_offset nCur = Tell();
    Seek(0, SEEK_END);
    if (nNewSize > Tell())
    {
        // Enable sparse files if growing size
        DWORD dwTemp;
        DeviceIoControl(hFile, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                        &dwTemp, nullptr);
    }
    Seek(nNewSize, SEEK_SET);
    BOOL bRes = SetEndOfFile(hFile);
    Seek(nCur, SEEK_SET);

    if (bRes)
        return 0;
    else
        return -1;
}

/************************************************************************/
/*                           GetRangeStatus()                           */
/************************************************************************/

VSIRangeStatus VSIWin32Handle::GetRangeStatus(vsi_l_offset
#ifdef FSCTL_QUERY_ALLOCATED_RANGES
                                                  nOffset
#endif
                                              ,
                                              vsi_l_offset
#ifdef FSCTL_QUERY_ALLOCATED_RANGES
                                                  nLength
#endif
)
{
    // Not available on mingw includes
#ifdef FSCTL_QUERY_ALLOCATED_RANGES
    FILE_ALLOCATED_RANGE_BUFFER sQueryRange;
    FILE_ALLOCATED_RANGE_BUFFER asOutputRange[1];
    DWORD nOutputBytes = 0;

    sQueryRange.FileOffset.QuadPart = nOffset;
    sQueryRange.Length.QuadPart = nOffset + nLength;

    if (!DeviceIoControl(hFile, FSCTL_QUERY_ALLOCATED_RANGES, &sQueryRange,
                         sizeof(sQueryRange), asOutputRange,
                         sizeof(asOutputRange), &nOutputBytes, nullptr))
    {
        if (GetLastError() == ERROR_MORE_DATA)
        {
            return VSI_RANGE_STATUS_DATA;
        }
        else
        {
            return VSI_RANGE_STATUS_UNKNOWN;
        }
    }

    if (nOutputBytes)
        return VSI_RANGE_STATUS_DATA;
    else
        return VSI_RANGE_STATUS_HOLE;
#else
    return VSI_RANGE_STATUS_UNKNOWN;
#endif
}

/************************************************************************/
/* ==================================================================== */
/*                       VSIWin32FilesystemHandler                      */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                          CPLGetWineVersion()                         */
/************************************************************************/

const char *CPLGetWineVersion();  // also used by cpl_aws.cpp

const char *CPLGetWineVersion()
{
    HMODULE hntdll = GetModuleHandle("ntdll.dll");
    if (hntdll == nullptr)
    {
        CPLDebug("CPLGetWineVersion", "Can't get handle to ntdll.dll.");
        return nullptr;
    }

    const char *(CDECL * pwine_get_version)(void);
    const auto ret = GetProcAddress(hntdll, "wine_get_version");
    static_assert(sizeof(pwine_get_version) == sizeof(ret),
                  "sizeof(pwine_get_version) == sizeof(ret)");
    memcpy(&pwine_get_version, &ret, sizeof(ret));
    if (pwine_get_version == nullptr)
    {
        return nullptr;
    }

    return pwine_get_version();
}

/************************************************************************/
/*                           VSIWin32StrlenW()                          */
/************************************************************************/

static size_t VSIWin32StrlenW(const wchar_t *pwszString)
{
    size_t nLen = 0;
    while (pwszString[nLen] != 0)
        nLen++;
    return nLen;
}

/************************************************************************/
/*                        VSIWin32TryLongFilename()                     */
/************************************************************************/

static void VSIWin32TryLongFilename(wchar_t *&pwszFilename)
{
    const size_t nLen = VSIWin32StrlenW(pwszFilename);
    constexpr const wchar_t LONG_FILENAME_PREFIX[] = L"\\\\?\\";
    constexpr size_t LONG_FILENAME_PREFIX_LEN =
        std::char_traits<wchar_t>::length(LONG_FILENAME_PREFIX);
    static_assert(LONG_FILENAME_PREFIX_LEN == 4);

    // <drive_letter>:\ or <drive_letter>:/
    if (pwszFilename[0] != 0 && pwszFilename[1] == ':' &&
        (pwszFilename[2] == '\\' || pwszFilename[2] == '/'))
    {
        pwszFilename = static_cast<wchar_t *>(
            CPLRealloc(pwszFilename, (LONG_FILENAME_PREFIX_LEN + nLen + 1) *
                                         sizeof(wchar_t)));
        memmove(pwszFilename + LONG_FILENAME_PREFIX_LEN, pwszFilename,
                (nLen + 1) * sizeof(wchar_t));
        memcpy(pwszFilename, LONG_FILENAME_PREFIX,
               LONG_FILENAME_PREFIX_LEN * sizeof(wchar_t));
    }
    // \\network_path or //network_path
    else if (pwszFilename[0] != 0 &&
             ((pwszFilename[0] == '\\' && pwszFilename[1] == '\\') ||
              (pwszFilename[0] == '/' && pwszFilename[1] == '/')))
    {
        constexpr const wchar_t UNC_PREFIX[] = L"\\\\?\\UNC\\";
        constexpr size_t UNC_PREFIX_LEN =
            std::char_traits<wchar_t>::length(UNC_PREFIX);
        static_assert(UNC_PREFIX_LEN == 8);
        constexpr const wchar_t NETWORK_PATH_PREFIX[] = L"\\\\";
        constexpr size_t NETWORK_PATH_PREFIX_LEN =
            std::char_traits<wchar_t>::length(NETWORK_PATH_PREFIX);
        static_assert(NETWORK_PATH_PREFIX_LEN == 2);
        constexpr size_t EXTRA_ALLOC_SIZE =
            UNC_PREFIX_LEN - NETWORK_PATH_PREFIX_LEN;

        pwszFilename = static_cast<wchar_t *>(CPLRealloc(
            pwszFilename, (EXTRA_ALLOC_SIZE + nLen + 1) * sizeof(wchar_t)));
        memmove(pwszFilename + UNC_PREFIX_LEN,
                pwszFilename + NETWORK_PATH_PREFIX_LEN,
                (nLen - NETWORK_PATH_PREFIX_LEN + 1) * sizeof(wchar_t));
        memcpy(pwszFilename, UNC_PREFIX, UNC_PREFIX_LEN * sizeof(wchar_t));
    }
    else
    {
        constexpr size_t MAX_LONG_FILENAME_SIZE = 32768;
        wchar_t *pwszCurDir = static_cast<wchar_t *>(
            CPLMalloc(MAX_LONG_FILENAME_SIZE * sizeof(wchar_t)));
        DWORD nCurDirLen =
            GetCurrentDirectoryW(MAX_LONG_FILENAME_SIZE, pwszCurDir);
        CPLAssert(nCurDirLen < MAX_LONG_FILENAME_SIZE);
        pwszFilename = static_cast<wchar_t *>(
            CPLRealloc(pwszFilename,
                       (LONG_FILENAME_PREFIX_LEN + nCurDirLen + 1 + nLen + 1) *
                           sizeof(wchar_t)));
        int nOffset = 0;
        if (pwszFilename[0] == '.' &&
            (pwszFilename[1] == '/' || pwszFilename[1] == '\\'))
            nOffset = 2;
        /* \\$\c:\a\b ..\c --> \\$\c:\a\c */
        while (pwszFilename[nOffset + 0] == '.' &&
               pwszFilename[nOffset + 1] == '.' &&
               (pwszFilename[nOffset + 2] == '/' ||
                pwszFilename[nOffset + 2] == '\\'))
        {
            DWORD nCurDirLenBefore = nCurDirLen;
            while (nCurDirLen > 0 && pwszCurDir[nCurDirLen - 1] != '\\')
                nCurDirLen--;
            if (nCurDirLen <= 2)
            {
                nCurDirLen = nCurDirLenBefore;
                break;
            }
            nCurDirLen--;
            nOffset += 3;
        }
        memmove(pwszFilename + LONG_FILENAME_PREFIX_LEN + nCurDirLen + 1,
                pwszFilename + nOffset, (nLen - nOffset + 1) * sizeof(wchar_t));
        memmove(pwszFilename + LONG_FILENAME_PREFIX_LEN, pwszCurDir,
                nCurDirLen * sizeof(wchar_t));
        pwszFilename[LONG_FILENAME_PREFIX_LEN + nCurDirLen] = '\\';
        memcpy(pwszFilename, LONG_FILENAME_PREFIX,
               LONG_FILENAME_PREFIX_LEN * sizeof(wchar_t));
        CPLFree(pwszCurDir);
    }

    for (size_t i = LONG_FILENAME_PREFIX_LEN; pwszFilename[i] != 0; i++)
    {
        if (pwszFilename[i] == '/')
            pwszFilename[i] = '\\';
    }
#if notdef
    CPLString osFilename;
    for (size_t i = 0; pwszFilename[i] != 0; i++)
        osFilename += (char)pwszFilename[i];
    CPLDebug("VSI", "Trying %s", osFilename.c_str());
#endif
}

/************************************************************************/
/*                         VSIWin32IsLongFilename()                     */
/************************************************************************/

static bool VSIWin32IsLongFilename(const wchar_t *pwszFilename)
{
    return (pwszFilename[0] == '\\' && pwszFilename[1] == '\\' &&
            pwszFilename[2] == '?' && pwszFilename[3] == '\\');
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

VSIVirtualHandle *VSIWin32FilesystemHandler::Open(const char *pszFilename,
                                                  const char *pszAccess,
                                                  bool bSetError,
                                                  CSLConstList papszOptions)

{
    DWORD dwDesiredAccess;
    DWORD dwCreationDisposition;
    DWORD dwFlagsAndAttributes;
    HANDLE hFile;

    // GENERICs are used instead of FILE_GENERIC_READ.
    if (strcmp(pszAccess, "w") == 0 || strcmp(pszAccess, "wb") == 0)
    {
        dwDesiredAccess = GENERIC_WRITE;
    }
    else
    {
        dwDesiredAccess = GENERIC_READ;
        if (strchr(pszAccess, '+') != nullptr ||
            strchr(pszAccess, 'w') != nullptr)
            dwDesiredAccess |= GENERIC_WRITE;
    }

    // Append mode only makes sense on files and pipes, have to use FILE_ access
    // these are very different from the GENERICs
    // Append is read and write but not overwrite data (only append data)
    if (strchr(pszAccess, 'a') != nullptr)
    {
        dwDesiredAccess =
            FILE_GENERIC_READ | (FILE_GENERIC_WRITE ^ FILE_WRITE_DATA);

        // Wine < 1.7.4 doesn't work properly without FILE_WRITE_DATA bit
        // (it refuses to write at all), so we'd better re-add it even if the
        // resulting semantics isn't completely conformant.
        // See https://bugs.winehq.org/show_bug.cgi?id=33232
        const char *pszWineVersion = CPLGetWineVersion();
        if (pszWineVersion != nullptr)
        {
            int nVersion = atoi(pszWineVersion) * 10000;
            const char *pszDot = strchr(pszWineVersion, '.');
            if (pszDot)
            {
                nVersion += atoi(pszDot + 1) * 100;
                pszDot = strchr(pszDot + 1, '.');
                if (pszDot)
                {
                    nVersion += atoi(pszDot + 1);
                }
            }
            if (nVersion < 1 * 10000 + 7 * 100 + 4)
            {
#if DEBUG_VERBOSE
                CPLDebug("VSI",
                         "Wine %s detected. Append mode needs FILE_WRITE_DATA",
                         pszWineVersion);
#endif
                dwDesiredAccess |= FILE_WRITE_DATA;
            }
        }
    }

    if (strstr(pszAccess, "w") != nullptr)
        dwCreationDisposition = CREATE_ALWAYS;
    else if (strstr(pszAccess, "a") != nullptr)
        dwCreationDisposition = OPEN_ALWAYS;
    else
        dwCreationDisposition = OPEN_EXISTING;

    dwFlagsAndAttributes = (dwDesiredAccess == GENERIC_READ)
                               ? FILE_ATTRIBUTE_READONLY
                               : FILE_ATTRIBUTE_NORMAL;

    const bool bWriteThrough =
        CPLTestBool(CSLFetchNameValueDef(papszOptions, "WRITE_THROUGH", "NO"));
    if (bWriteThrough)
    {
        dwFlagsAndAttributes |= FILE_FLAG_WRITE_THROUGH;
    }

    /* -------------------------------------------------------------------- */
    /*      On Win32 consider treating the filename as utf-8 and            */
    /*      converting to wide characters to open.                          */
    /* -------------------------------------------------------------------- */
    DWORD nLastError = 0;
    bool bShared = CPLTestBool(CPLGetConfigOption("GDAL_SHARED_FILE", "YES"));
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(pszFilename, CPL_ENC_UTF8, CPL_ENC_UCS2);

        hFile = CreateFileW(pwszFilename, dwDesiredAccess,
                            bShared ? FILE_SHARE_READ | FILE_SHARE_WRITE : 0,
                            nullptr, dwCreationDisposition,
                            dwFlagsAndAttributes, nullptr);
        if (hFile == INVALID_HANDLE_VALUE &&
            !VSIWin32IsLongFilename(pwszFilename))
        {
            nLastError = GetLastError();
#ifdef notdef
            switch (nLastError)
            {
                case ERROR_FILE_NOT_FOUND:
                    CPLDebug("VSI", "ERROR_FILE_NOT_FOUND");
                    break;
                case ERROR_PATH_NOT_FOUND:
                    CPLDebug("VSI", "ERROR_PATH_NOT_FOUND");
                    break;
                case ERROR_INVALID_DRIVE:
                    CPLDebug("VSI", "ERROR_INVALID_DRIVE");
                    break;
                case ERROR_NO_MORE_FILES:
                    CPLDebug("VSI", "ERROR_NO_MORE_FILES");
                    break;
                case ERROR_BAD_PATHNAME:
                    CPLDebug("VSI", "ERROR_BAD_PATHNAME");
                    break;
                case ERROR_BAD_NETPATH:
                    CPLDebug("VSI", "ERROR_BAD_NETPATH");
                    break;
                case ERROR_FILENAME_EXCED_RANGE:
                    CPLDebug("VSI", "ERROR_FILENAME_EXCED_RANGE");
                    break;
                case ERROR_SHARING_VIOLATION:
                    CPLDebug("VSI", "ERROR_SHARING_VIOLATION");
                    break;
                default:
                    CPLDebug("VSI", "other error %d", nLastError);
                    break;
            }
#endif
        }
        if (nLastError == ERROR_PATH_NOT_FOUND ||
            nLastError == ERROR_FILENAME_EXCED_RANGE)
        {
            VSIWin32TryLongFilename(pwszFilename);
            nLastError = 0;
            hFile = CreateFileW(
                pwszFilename, dwDesiredAccess,
                bShared ? FILE_SHARE_READ | FILE_SHARE_WRITE : 0, nullptr,
                dwCreationDisposition, dwFlagsAndAttributes, nullptr);
        }
        CPLFree(pwszFilename);
    }
    else
    {
        hFile = CreateFile(pszFilename, dwDesiredAccess,
                           bShared ? FILE_SHARE_READ | FILE_SHARE_WRITE : 0,
                           nullptr, dwCreationDisposition, dwFlagsAndAttributes,
                           nullptr);
    }

    if (hFile == INVALID_HANDLE_VALUE)
    {
        nLastError = GetLastError();
        const int nError = ErrnoFromGetLastError(nLastError);
        if (bSetError && nError != 0)
        {
            VSIError(VSIE_FileError, "%s: %s", pszFilename,
                     (nLastError == ERROR_SHARING_VIOLATION)
                         ? "file used by other process"
                         : strerror(nError));
        }
        errno = nError;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a VSI file handle.                                       */
    /* -------------------------------------------------------------------- */
    VSIWin32Handle *poHandle = new VSIWin32Handle;

    poHandle->hFile = hFile;
    poHandle->m_bWriteThrough = bWriteThrough;

    if (strchr(pszAccess, 'a') != nullptr)
        poHandle->Seek(0, SEEK_END);

    /* -------------------------------------------------------------------- */
    /*      If VSI_CACHE is set we want to use a cached reader instead      */
    /*      of more direct io on the underlying file.                       */
    /* -------------------------------------------------------------------- */
    if ((EQUAL(pszAccess, "r") || EQUAL(pszAccess, "rb")) &&
        CPLTestBool(CPLGetConfigOption("VSI_CACHE", "FALSE")))
    {
        return VSICreateCachedFile(poHandle);
    }
    else
    {
        return poHandle;
    }
}

/************************************************************************/
/*                        GetNTStatusMessage()                          */
/************************************************************************/

static std::string GetNTStatusMessage(NTSTATUS status)
{
    typedef ULONG(WINAPI * CPLRtlNtStatusToDosError_t)(NTSTATUS Status);

    HMODULE ntdll = GetModuleHandle("ntdll.dll");
    if (!ntdll)
    {
        CPLDebugOnce("CPL", "ntdll.dll not found");
        return CPLSPrintf("NTSTATUS %ld", status);
    }

    CPLRtlNtStatusToDosError_t hRtlNtStatusToDosError;
    auto handle = GetProcAddress(ntdll, "RtlNtStatusToDosError");
    static_assert(sizeof(hRtlNtStatusToDosError) == sizeof(handle));
    memcpy(&hRtlNtStatusToDosError, &handle, sizeof(handle));
    if (!hRtlNtStatusToDosError)
    {
        CPLDebugOnce("CPL", "hRtlNtStatusToDosError not found");
        return CPLSPrintf("NTSTATUS %ld", status);
    }

    const DWORD winError = hRtlNtStatusToDosError(status);

    wchar_t *msg = NULL;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, winError,
        0,  // use system default language
        reinterpret_cast<LPWSTR>(&msg), 0, nullptr);

    if (len && msg)
    {
        char *pszMsg = CPLRecodeFromWChar(msg, CPL_ENC_UCS2, CPL_ENC_UTF8);
        std::string ret = pszMsg;
        CPLFree(pszMsg);
        LocalFree(msg);
        return ret;
    }
    else
    {
        return CPLSPrintf("NTSTATUS %ld, WinError %lu", status, winError);
    }
}

/************************************************************************/
/*                            IsPathNTFS()                              */
/************************************************************************/

static bool IsPathNTFS(const char *pszPath)
{
    wchar_t volumePath[32];

    wchar_t *pszWPath = CPLRecodeToWChar(pszPath, CPL_ENC_UTF8, CPL_ENC_UCS2);
    if (!GetVolumePathNameW(pszWPath, volumePath,
                            static_cast<DWORD>(sizeof(volumePath))))
    {
        CPLFree(pszWPath);
        wprintf(L"GetVolumePathNameW failed: %lu\n", GetLastError());
        return false;
    }
    CPLFree(pszWPath);

    // Get the filesystem name
    wchar_t fileSystemName[32];
    if (!GetVolumeInformationW(volumePath, nullptr, 0, nullptr, nullptr,
                               nullptr, fileSystemName,
                               static_cast<DWORD>(sizeof(fileSystemName))))
    {
        CPLDebug("CPL", "GetVolumeInformationW failed: %lu", GetLastError());
        return false;
    }

    return _wcsicmp(fileSystemName, L"NTFS") == 0;
}

/************************************************************************/
/*                      CreateOnlyVisibleAtCloseTime()                  */
/************************************************************************/

VSIVirtualHandle *VSIWin32FilesystemHandler::CreateOnlyVisibleAtCloseTime(
    const char *pszFilename, bool bEmulationAllowed, CSLConstList papszOptions)
{
    typedef NTSTATUS(WINAPI * CPLNtCreateFile_t)(
        PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
        PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
        ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
        ULONG EaLength);

    typedef void(WINAPI * CPLRtlInitUnicodeString_t)(
        PUNICODE_STRING DestinationString, PCWSTR SourceString);

    std::string osFullFilename;
    if ((pszFilename[0] && pszFilename[1] == ':' &&
         (pszFilename[2] == '\\' || pszFilename[2] == '/')) ||
        STARTS_WITH(pszFilename, "\\\\"))
    {
        osFullFilename = pszFilename;
    }
    else
    {
        WCHAR wszCWD[MAX_PATH];
        DWORD length = GetCurrentDirectoryW(MAX_PATH, wszCWD);
        if (length > 0 && length < MAX_PATH)
        {
            char *pszCWD =
                CPLRecodeFromWChar(wszCWD, CPL_ENC_UCS2, CPL_ENC_UTF8);
            osFullFilename =
                std::string(pszCWD).append("\\").append(pszFilename);
            CPLFree(pszCWD);
        }
    }

    if (!osFullFilename.empty() && IsPathNTFS(osFullFilename.c_str()))
    {
        do
        {
            HMODULE ntdll = GetModuleHandle("ntdll.dll");
            if (!ntdll)
            {
                CPLDebugOnce("CPL", "ntdll.dll not found");
                break;
            }

            CPLNtCreateFile_t hNtCreateFile;
            {
                auto handle = GetProcAddress(ntdll, "NtCreateFile");
                static_assert(sizeof(hNtCreateFile) == sizeof(handle));
                memcpy(&hNtCreateFile, &handle, sizeof(handle));
            }

            CPLRtlInitUnicodeString_t hRtlInitUnicodeString;
            {
                auto handle = GetProcAddress(ntdll, "RtlInitUnicodeString");
                static_assert(sizeof(hRtlInitUnicodeString) == sizeof(handle));
                memcpy(&hRtlInitUnicodeString, &handle, sizeof(handle));
            }

            if (!hNtCreateFile || !hRtlInitUnicodeString)
            {
                CPLDebugOnce("CPL",
                             "NtCreateFile or RtlInitUnicodeString not found");
                break;
            }

            wchar_t *pwszFilename = CPLRecodeToWChar(
                CPLString(osFullFilename).replaceAll('/', '\\').c_str(),
                CPL_ENC_UTF8, CPL_ENC_UCS2);

            std::vector<WCHAR> fileNameBuffer;
            fileNameBuffer.resize(wcslen(pwszFilename) +
                                  strlen("\\??\\.tmp_hidden") + 1);
            // Use NT Kernel long filename convention whose prefix is
            // "backslash question_mark question_mark backslash",
            // whereas Win32 API long filename convention is
            // "backslash backslash question_mark backslash" ...
            if (pwszFilename[0] == '\\' && pwszFilename[1] == '\\' &&
                pwszFilename[2] == '?' && pwszFilename[3] == '\\')
            {
                swprintf(fileNameBuffer.data(), fileNameBuffer.size(),
                         L"\\??\\%s.tmp_hidden",
                         pwszFilename + strlen("\\\\?\\"));
            }
            else
            {
                swprintf(fileNameBuffer.data(), fileNameBuffer.size(),
                         L"\\??\\%s.tmp_hidden", pwszFilename);
            }
            CPLFree(pwszFilename);
            fileNameBuffer.resize(wcslen(fileNameBuffer.data()));

#ifdef DEBUG_VERBOSE
            {
                char *pszNtFilename = CPLRecodeFromWChar(
                    fileNameBuffer.data(), CPL_ENC_UCS2, CPL_ENC_UTF8);
                CPLDebug("CPL", "NtCreateFile('%s')", pszNtFilename);
                CPLFree(pszNtFilename);
            }
#endif

            // Define NT path
            UNICODE_STRING fileName;
            hRtlInitUnicodeString(&fileName, fileNameBuffer.data());

            OBJECT_ATTRIBUTES fileAttr;
            InitializeObjectAttributes(&fileAttr, &fileName,
                                       OBJ_CASE_INSENSITIVE, nullptr, nullptr);

            HANDLE hFile = nullptr;
            IO_STATUS_BLOCK ioStatus;
            memset(&ioStatus, 0, sizeof(ioStatus));

            DWORD creationOptions =
                FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT;
            const bool bWriteThrough = CPLTestBool(
                CSLFetchNameValueDef(papszOptions, "WRITE_THROUGH", "NO"));
            if (bWriteThrough)
            {
                creationOptions |= FILE_WRITE_THROUGH;
            }

            NTSTATUS status = hNtCreateFile(
                &hFile, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE,
                &fileAttr, &ioStatus, nullptr,
                FILE_ATTRIBUTE_HIDDEN | FILE_DELETE_ON_CLOSE, 0, FILE_SUPERSEDE,
                creationOptions, nullptr, 0);

            if (status != 0)
            {
                CPLDebug("CPL", "NtCreateFile() failed: %s",
                         GetNTStatusMessage(status).c_str());
                break;
            }

            VSIWin32Handle *poHandle = new VSIWin32Handle;

            poHandle->hFile = hFile;
            poHandle->m_bWriteThrough = bWriteThrough;
            poHandle->m_osFilenameToSetAtCloseTime = osFullFilename;

            return poHandle;

        } while (false);
    }
    return VSIFilesystemHandler::CreateOnlyVisibleAtCloseTime(
        pszFilename, bEmulationAllowed, papszOptions);
}

/************************************************************************/
/*                                Stat()                                */
/************************************************************************/

int VSIWin32FilesystemHandler::Stat(const char *pszFilename,
                                    VSIStatBufL *pStatBuf, int nFlags)

{
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(pszFilename, CPL_ENC_UTF8, CPL_ENC_UCS2);

        if (nFlags == VSI_STAT_EXISTS_FLAG)
        {
            memset(pStatBuf, 0, sizeof(VSIStatBufL));
            const int nResult =
                (GetFileAttributesW(pwszFilename) == INVALID_FILE_ATTRIBUTES)
                    ? -1
                    : 0;
            CPLFree(pwszFilename);
            return nResult;
        }

#if defined(__MINGW32__)
        // MinGW runtime for _wstat64() apparently doesn't like trailing slashes
        // for directories.
        const size_t nLen = wcslen(pwszFilename);
        if (nLen > 0 &&
            (pwszFilename[nLen - 1] == '/' || pwszFilename[nLen - 1] == '\\'))
            pwszFilename[nLen - 1] = 0;
#endif

        int nResult = _wstat64(pwszFilename, pStatBuf);

        // If _wstat64() fails and the original name is not an extended one,
        // then retry with an extended filename
        if (nResult < 0 && !VSIWin32IsLongFilename(pwszFilename))
        {
            DWORD nLastError = GetLastError();
            if (nLastError == ERROR_PATH_NOT_FOUND ||
                nLastError == ERROR_FILENAME_EXCED_RANGE)
            {
                VSIWin32TryLongFilename(pwszFilename);
                nResult = _wstat64(pwszFilename, pStatBuf);
            }
        }

        // There are some issues with mingw64 runtime with extended file names.
        // In that situation try a poor-man implementation with Open()
        if (nResult < 0 && VSIWin32IsLongFilename(pwszFilename))
        {
            VSIVirtualHandle *poHandle = Open(pszFilename, "rb");
            if (poHandle != nullptr)
            {
                nResult = 0;
                memset(pStatBuf, 0, sizeof(VSIStatBufL));
                CPL_IGNORE_RET_VAL(poHandle->Seek(0, SEEK_END));
                pStatBuf->st_mode = S_IFREG;
                pStatBuf->st_size = poHandle->Tell();
                poHandle->Close();
                delete poHandle;
            }
            else
                nResult = -1;
        }

        CPLFree(pwszFilename);

        return nResult;
    }
    else
    {
        (void)nFlags;
        return (VSI_STAT64(pszFilename, pStatBuf));
    }
}

/************************************************************************/

/*                               Unlink()                               */
/************************************************************************/

int VSIWin32FilesystemHandler::Unlink(const char *pszFilename)

{
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(pszFilename, CPL_ENC_UTF8, CPL_ENC_UCS2);

        const int nResult = _wunlink(pwszFilename);
        CPLFree(pwszFilename);
        return nResult;
    }
    else
    {
        return unlink(pszFilename);
    }
}

/************************************************************************/
/*                               Rename()                               */
/************************************************************************/

int VSIWin32FilesystemHandler::Rename(const char *oldpath, const char *newpath,
                                      GDALProgressFunc, void *)

{
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszOldPath =
            CPLRecodeToWChar(oldpath, CPL_ENC_UTF8, CPL_ENC_UCS2);
        wchar_t *pwszNewPath =
            CPLRecodeToWChar(newpath, CPL_ENC_UTF8, CPL_ENC_UCS2);

        const int nResult = _wrename(pwszOldPath, pwszNewPath);
        CPLFree(pwszOldPath);
        CPLFree(pwszNewPath);
        return nResult;
    }
    else
    {
        return rename(oldpath, newpath);
    }
}

/************************************************************************/
/*                               Mkdir()                                */
/************************************************************************/

int VSIWin32FilesystemHandler::Mkdir(const char *pszPathname, long /* nMode */)

{
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(pszPathname, CPL_ENC_UTF8, CPL_ENC_UCS2);

        const int nResult = _wmkdir(pwszFilename);
        CPLFree(pwszFilename);
        return nResult;
    }
    else
    {
        return mkdir(pszPathname);
    }
}

/************************************************************************/
/*                               Rmdir()                                */
/************************************************************************/

int VSIWin32FilesystemHandler::Rmdir(const char *pszPathname)

{
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(pszPathname, CPL_ENC_UTF8, CPL_ENC_UCS2);

        const int nResult = _wrmdir(pwszFilename);
        CPLFree(pwszFilename);
        return nResult;
    }
    else
    {
        return rmdir(pszPathname);
    }
}

/************************************************************************/
/*                              ReadDir()                               */
/************************************************************************/

char **VSIWin32FilesystemHandler::ReadDirEx(const char *pszPath, int nMaxFiles)

{
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        struct _wfinddata_t c_file;
        intptr_t hFile;
        char *pszFileSpec;
        CPLStringList oDir;

        if (strlen(pszPath) == 0)
            pszPath = ".";

        pszFileSpec = CPLStrdup(CPLSPrintf("%s\\*.*", pszPath));
        wchar_t *pwszFileSpec =
            CPLRecodeToWChar(pszFileSpec, CPL_ENC_UTF8, CPL_ENC_UCS2);

        if ((hFile = _wfindfirst(pwszFileSpec, &c_file)) != -1L)
        {
            do
            {
                oDir.AddStringDirectly(CPLRecodeFromWChar(
                    c_file.name, CPL_ENC_UCS2, CPL_ENC_UTF8));
                if (nMaxFiles > 0 && oDir.Count() > nMaxFiles)
                    break;
            } while (_wfindnext(hFile, &c_file) == 0);

            _findclose(hFile);
        }
        else
        {
            /* Should we generate an error???
             * For now we'll just return NULL (at the end of the function)
             */
        }

        CPLFree(pszFileSpec);
        CPLFree(pwszFileSpec);

        return oDir.StealList();
    }
    else
    {
        struct _finddata_t c_file;
        intptr_t hFile;
        char *pszFileSpec;
        CPLStringList oDir;

        if (strlen(pszPath) == 0)
            pszPath = ".";

        pszFileSpec = CPLStrdup(CPLSPrintf("%s\\*.*", pszPath));

        if ((hFile = _findfirst(pszFileSpec, &c_file)) != -1L)
        {
            do
            {
                oDir.AddString(c_file.name);
                if (nMaxFiles > 0 && oDir.Count() > nMaxFiles)
                    break;
            } while (_findnext(hFile, &c_file) == 0);

            _findclose(hFile);
        }
        else
        {
            /* Should we generate an error???
             * For now we'll just return NULL (at the end of the function)
             */
        }

        CPLFree(pszFileSpec);

        return oDir.StealList();
    }
}

/************************************************************************/
/*                              VSIDIRWin32                             */
/************************************************************************/

struct VSIDIRWin32 final : public VSIDIR
{
    struct DIR
    {
        intptr_t handle = -1;

        ~DIR()
        {
            close();
        }

        void close()
        {
            if (handle != -1)
                _findclose(handle);
            handle = -1;
        }

        DIR(const DIR &) = delete;
        DIR &operator=(const DIR &) = delete;
        DIR(DIR &&) = delete;

        DIR &operator=(DIR &&other)
        {
            close();
            std::swap(handle, other.handle);
            return *this;
        }
    };

    explicit VSIDIRWin32(const CPLString &osRootPathIn)
        : osRootPath(osRootPathIn),
          SEP(VSIGetDirectorySeparator(osRootPathIn.c_str())[0])
    {
    }

    CPLString osRootPath{};
    const char SEP;
    bool bUTF8 = false;
    bool bFirstEntry = true;
    CPLString osBasePath{};
    DIR m_sDir{};
    int nRecurseDepth = 0;
    VSIDIREntry entry{};
    std::vector<std::unique_ptr<VSIDIR>> aoStackSubDir{};
    std::string m_osFilterPrefix{};
    CPLStringList m_aosOptions{};

    template <typename T> void FillEntry(const T &c_file)
    {
        CPLString osName(osBasePath);
        if (!osName.empty())
            osName += SEP;
        if constexpr (std::is_same_v<T, struct _wfinddata_t>)
        {
            char *pwszName =
                CPLRecodeFromWChar(c_file.name, CPL_ENC_UCS2, CPL_ENC_UTF8);
            osName += pwszName;
            CPLFree(pwszName);
        }
        else
        {
            osName += c_file.name;
        }

        CPLFree(entry.pszName);
        entry.pszName = CPLStrdup(osName);
        entry.nMode = (c_file.attrib & _A_SUBDIR) != 0 ? S_IFDIR : S_IFREG;
        entry.nSize = c_file.size;
        entry.nMTime = c_file.time_write;
        entry.bModeKnown = true;
        entry.bSizeKnown = true;
        entry.bMTimeKnown = true;
    }

    const VSIDIREntry *NextDirEntry() override;
};

/************************************************************************/
/*                        OpenDirInternal()                             */
/************************************************************************/

/* static */
std::unique_ptr<VSIDIRWin32> VSIWin32FilesystemHandler::OpenDirInternal(
    const char *pszPath, int nRecurseDepth, const char *const *papszOptions)
{
    if (strlen(pszPath) == 0)
        pszPath = ".";
    auto dir = std::make_unique<VSIDIRWin32>(pszPath);
    dir->bUTF8 = CPLTestBool(CSLFetchNameValueDef(
        papszOptions, "GDAL_FILENAME_IS_UTF8",
        CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")));
    const std::string osFileSpec = std::string(pszPath).append("\\*.*");
    if (dir->bUTF8)
    {
        wchar_t *pwszFileSpec =
            CPLRecodeToWChar(osFileSpec.c_str(), CPL_ENC_UTF8, CPL_ENC_UCS2);

        struct _wfinddata_t c_file;
        dir->m_sDir.handle = _wfindfirst(pwszFileSpec, &c_file);
        CPLFree(pwszFileSpec);
        if (dir->m_sDir.handle != -1)
            dir->FillEntry(c_file);
    }
    else
    {
        struct _finddata_t c_file;
        dir->m_sDir.handle = _findfirst(osFileSpec.c_str(), &c_file);
        if (dir->m_sDir.handle != -1)
            dir->FillEntry(c_file);
    }
    if (dir->m_sDir.handle == -1)
    {
        return nullptr;
    }
    dir->nRecurseDepth = nRecurseDepth;
    dir->m_osFilterPrefix =
        CPLString(CSLFetchNameValueDef(papszOptions, "PREFIX", ""))
            .replaceAll('\\', '/');
    dir->m_aosOptions.SetNameValue("GDAL_FILENAME_IS_UTF8",
                                   dir->bUTF8 ? "YES" : "NO");
    return dir;
}

/************************************************************************/
/*                            OpenDir()                                 */
/************************************************************************/

VSIDIR *VSIWin32FilesystemHandler::OpenDir(const char *pszPath,
                                           int nRecurseDepth,
                                           const char *const *papszOptions)
{
    return OpenDirInternal(pszPath, nRecurseDepth, papszOptions).release();
}

/************************************************************************/
/*                           NextDirEntry()                             */
/************************************************************************/

const VSIDIREntry *VSIDIRWin32::NextDirEntry()
{
begin:
    if (!bFirstEntry && VSI_ISDIR(entry.nMode) && nRecurseDepth != 0)
    {
        CPLString osCurFile(osRootPath);
        if (!osCurFile.empty())
            osCurFile += SEP;
        osCurFile += entry.pszName;
        auto subdir = VSIWin32FilesystemHandler::OpenDirInternal(
            osCurFile, nRecurseDepth - 1, m_aosOptions.List());
        if (subdir)
        {
            subdir->osRootPath = osRootPath;
            subdir->osBasePath = entry.pszName;
            subdir->m_osFilterPrefix = m_osFilterPrefix;
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

    while (true)
    {
        if (bFirstEntry)
        {
            bFirstEntry = false;
        }
        else
        {
            bool bHasNext;
            if (bUTF8)
            {
                struct _wfinddata_t c_file;
                bHasNext = _wfindnext(m_sDir.handle, &c_file) == 0;
                if (bHasNext)
                    FillEntry(c_file);
            }
            else
            {
                struct _finddata_t c_file;
                bHasNext = _findnext(m_sDir.handle, &c_file) == 0;
                if (bHasNext)
                    FillEntry(c_file);
            }
            if (!bHasNext)
                break;
        }

        const char *pszFilename = CPLGetFilename(entry.pszName);
        // Skip . and ..entries
        if (pszFilename[0] == '.' &&
            (pszFilename[1] == '\0' ||
             (pszFilename[1] == '.' && pszFilename[2] == '\0')))
        {
            continue;
        }

        if (!m_osFilterPrefix.empty())
        {
            const CPLString osName =
                CPLString(entry.pszName).replaceAll('\\', '/');
            if (m_osFilterPrefix.size() > osName.size())
            {
                if (STARTS_WITH(m_osFilterPrefix.c_str(), osName.c_str()) &&
                    m_osFilterPrefix[osName.size()] == '/')
                {
                    if (VSI_ISDIR(entry.nMode))
                    {
                        goto begin;
                    }
                }
                continue;
            }
            if (!STARTS_WITH(osName.c_str(), m_osFilterPrefix.c_str()))
            {
                continue;
            }
        }

        return &entry;
    }

    return nullptr;
}

/************************************************************************/
/*                        GetDiskFreeSpace()                            */
/************************************************************************/

GIntBig VSIWin32FilesystemHandler::GetDiskFreeSpace(const char *pszDirname)
{
    GIntBig nRet = -1;
    ULARGE_INTEGER nFreeBytesAvailable;
    if (GetDiskFreeSpaceEx(pszDirname, &nFreeBytesAvailable, nullptr, nullptr))
    {
        nRet = static_cast<GIntBig>(nFreeBytesAvailable.QuadPart);
    }
    return nRet;
}

/************************************************************************/
/*                      SupportsSparseFiles()                           */
/************************************************************************/

int VSIWin32FilesystemHandler::SupportsSparseFiles(const char *pszPath)
{
    CPLString osPath(pszPath);
    DWORD dwVolFlags = 0;
    if (CPLIsFilenameRelative(pszPath))
    {
        char *pszTmp = CPLGetCurrentDir();
        osPath = pszTmp;
        CPLFree(pszTmp);
    }
    if (osPath.size() >= 3 && osPath[1] == ':' &&
        (osPath[2] == '/' || osPath[2] == '\\'))
    {
        osPath.resize(3);
    }

    GetVolumeInformation(osPath.c_str(), nullptr, 0, nullptr, nullptr,
                         &dwVolFlags, nullptr, 0);
    return (dwVolFlags & FILE_SUPPORTS_SPARSE_FILES);
}

/************************************************************************/
/*                          IsLocal()                                   */
/************************************************************************/

bool VSIWin32FilesystemHandler::IsLocal(const char *pszPath)
{
    if (STARTS_WITH(pszPath, "\\\\") || STARTS_WITH(pszPath, "//"))
        return false;
    std::string osPath(pszPath);
    if (osPath.size() >= 3 && osPath[1] == ':' &&
        (osPath[2] == '\\' || osPath[2] == '/'))
    {
        osPath.resize(3);
        return GetDriveType(osPath.c_str()) != DRIVE_REMOTE;
    }
    return true;
}

/************************************************************************/
/*                      GetCanonicalFilename()                          */
/************************************************************************/

std::string VSIWin32FilesystemHandler::GetCanonicalFilename(
    const std::string &osFilename) const
{
    constexpr int MAX_ITERS = 4;
    if (CPLTestBool(CPLGetConfigOption("GDAL_FILENAME_IS_UTF8", "YES")))
    {
        wchar_t *pwszFilename =
            CPLRecodeToWChar(osFilename.c_str(), CPL_ENC_UTF8, CPL_ENC_UCS2);
        std::wstring longPath;
        longPath.resize(std::wcslen(pwszFilename) + 256);
        for (int i = 0; i < MAX_ITERS; ++i)
        {
            DWORD result =
                GetLongPathNameW(pwszFilename, longPath.data(),
                                 static_cast<DWORD>(longPath.size()));
            if (result <= longPath.size())
            {
                longPath.resize(result);
                break;
            }
            if (result == 0 || i == MAX_ITERS - 1)
            {
                CPLFree(pwszFilename);
                return osFilename;
            }
            longPath.resize(longPath.size() * 2);
        }
        CPLFree(pwszFilename);
        char *pszTmp =
            CPLRecodeFromWChar(longPath.data(), CPL_ENC_UCS2, CPL_ENC_UTF8);
        std::string osRet(pszTmp);
        CPLFree(pszTmp);
        return osRet;
    }
    else
    {
        std::string longPath;
        longPath.resize(osFilename.size() + 256);
        for (int i = 0; i < MAX_ITERS; ++i)
        {
            DWORD result =
                GetLongPathNameA(osFilename.c_str(), longPath.data(),
                                 static_cast<DWORD>(longPath.size()));
            if (result <= longPath.size())
            {
                longPath.resize(result);
                break;
            }
            if (result == 0 || i == MAX_ITERS - 1)
            {
                return osFilename;
            }
            longPath.resize(longPath.size() * 2);
        }
        return longPath;
    }
}

/************************************************************************/
/*                     VSIInstallLargeFileHandler()                     */
/************************************************************************/

void VSIInstallLargeFileHandler()

{
    VSIFileManager::InstallHandler("", new VSIWin32FilesystemHandler);
}

#endif /* def WIN32 */
