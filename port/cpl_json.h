/******************************************************************************
 * Project:  Common Portability Library
 * Purpose:  Function wrapper for libjson-c access.
 * Author:   Dmitry Baryshnikov, dmitry.baryshnikov@nextgis.com
 *
 ******************************************************************************
 * Copyright (c) 2017-2018 NextGIS, <info@nextgis.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef CPL_JSON_H_INCLUDED
#define CPL_JSON_H_INCLUDED

#include "cpl_progress.h"
#include "cpl_string.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

/**
 * \file cpl_json.h
 *
 * Interface for read and write JSON documents
 */

/*! @cond Doxygen_Suppress */
typedef void *JSONObjectH;

class CPLJSONObject;
class CPLJSONArray;

class CPLJSONObjectProxy
{
    CPLJSONObject &oObj;
    const std::string osName;

  public:
    explicit inline CPLJSONObjectProxy(CPLJSONObject &oObjIn,
                                       const std::string &osNameIn)
        : oObj(oObjIn), osName(osNameIn)
    {
    }

    template <class T> inline CPLJSONObjectProxy &operator=(const T &val);
};

/*! @endcond */

/**
 * @brief The CPLJSONArray class holds JSON object from CPLJSONDocument
 */
class CPL_DLL CPLJSONObject
{
    friend class CPLJSONArray;
    friend class CPLJSONDocument;

  public:
    /**
     * Json object types
     */
    enum class Type
    {
        Unknown,
        Null,
        Object,
        Array,
        Boolean,
        String,
        Integer,
        Long,
        Double
    };

    /**
     * Json object format to string options
     */
    enum class PrettyFormat
    {
        Plain,   ///< No extra whitespace or formatting applied
        Spaced,  ///< Minimal whitespace inserted
        Pretty   ///< Formatted output
    };

  public:
    /*! @cond Doxygen_Suppress */
    CPLJSONObject();
    explicit CPLJSONObject(const std::string &osName,
                           const CPLJSONObject &oParent);
    explicit CPLJSONObject(std::nullptr_t);
    explicit CPLJSONObject(const std::string &osVal);
    explicit CPLJSONObject(const char *pszValue);
    explicit CPLJSONObject(bool bVal);
    explicit CPLJSONObject(int nVal);
    explicit CPLJSONObject(int64_t nVal);
    explicit CPLJSONObject(uint64_t nVal);
    explicit CPLJSONObject(double dfVal);
    ~CPLJSONObject();
    CPLJSONObject(const CPLJSONObject &other);
    CPLJSONObject(CPLJSONObject &&other);
    CPLJSONObject &operator=(const CPLJSONObject &other);
    CPLJSONObject &operator=(CPLJSONObject &&other);
    CPLJSONObject &operator=(CPLJSONArray &&other);

    // This method is not thread-safe
    CPLJSONObject Clone() const;

  private:
    explicit CPLJSONObject(const std::string &osName, JSONObjectH poJsonObject);
    /*! @endcond */

  public:
    // setters
    void Add(const std::string &osName, const std::string &osValue);
    void Add(const std::string &osName, const char *pszValue);
    void Add(const std::string &osName, double dfValue);
    void Add(const std::string &osName, int nValue);
    void Add(const std::string &osName, GInt64 nValue);
    void Add(const std::string &osName, uint64_t nValue);
    void Add(const std::string &osName, const CPLJSONArray &oValue);
    void Add(const std::string &osName, const CPLJSONObject &oValue);
    void AddNoSplitName(const std::string &osName, const CPLJSONObject &oValue);
    void Add(const std::string &osName, bool bValue);
    void AddNull(const std::string &osName);

    /** Change value by key */
    template <class T> void Set(const std::string &osName, const T &val)
    {
        Delete(osName);
        Add(osName, val);
    }

    void SetNull(const std::string &osName);

    CPLJSONObject operator[](const std::string &osName);

/*! @cond Doxygen_Suppress */

// GCC 9.4 seems to be confused by the template and doesn't realize it
// returns *this
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#endif
    template <class T> inline CPLJSONObject &operator=(const T &val)
    {
        CPLAssert(!m_osKeyForSet.empty());
        std::string osKeyForSet = m_osKeyForSet;
        m_osKeyForSet.clear();
        Set(osKeyForSet, val);
        return *this;
    }
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

    template <class T>
    inline CPLJSONObject &operator=(std::initializer_list<T> list);

    JSONObjectH GetInternalHandle() const
    {
        return m_poJsonObject;
    }

    /*! @endcond */

    // getters
    std::string GetString(const std::string &osName,
                          const std::string &osDefault = "") const;
    double GetDouble(const std::string &osName, double dfDefault = 0.0) const;
    int GetInteger(const std::string &osName, int nDefault = 0) const;
    GInt64 GetLong(const std::string &osName, GInt64 nDefault = 0) const;
    bool GetBool(const std::string &osName, bool bDefault = false) const;
    std::string ToString(const std::string &osDefault = "") const;
    double ToDouble(double dfDefault = 0.0) const;
    int ToInteger(int nDefault = 0) const;
    GInt64 ToLong(GInt64 nDefault = 0) const;
    bool ToBool(bool bDefault = false) const;
    CPLJSONArray ToArray() const;
    std::string Format(PrettyFormat eFormat) const;

    //
    void Delete(const std::string &osName);
    void DeleteNoSplitName(const std::string &osName);
    CPLJSONArray GetArray(const std::string &osName) const;
    CPLJSONObject GetObj(const std::string &osName) const;
    CPLJSONObject operator[](const std::string &osName) const;
    Type GetType() const;

    /*! @cond Doxygen_Suppress */
    std::string GetName() const
    {
        return m_osKey;
    }

    /*! @endcond */

    std::vector<CPLJSONObject> GetChildren() const;
    bool IsValid() const;
    void Deinit();

  protected:
    /*! @cond Doxygen_Suppress */
    CPLJSONObject GetObjectByPath(const std::string &osPath,
                                  std::string &osName) const;
    /*! @endcond */

  private:
    JSONObjectH m_poJsonObject = nullptr;
    std::string m_osKey{};
    std::string m_osKeyForSet{};
};

/**
 * @brief The JSONArray class JSON array from JSONDocument
 */
class CPL_DLL CPLJSONArray : public CPLJSONObject
{
    friend class CPLJSONObject;
    friend class CPLJSONDocument;

  public:
    /*! @cond Doxygen_Suppress */
    CPLJSONArray();
    explicit CPLJSONArray(const std::string &osName);
    explicit CPLJSONArray(const CPLJSONObject &other);

    /** Constructor from a list of initial values */
    template <class T> static CPLJSONArray Build(std::initializer_list<T> list)
    {
        CPLJSONArray oArray;
        for (const auto &val : list)
            oArray.Add(val);
        return oArray;
    }

  private:
    explicit CPLJSONArray(const std::string &osName, JSONObjectH poJsonObject);

    class CPL_DLL ConstIterator
    {
        const CPLJSONArray &m_oSelf;
        int m_nIdx;
        mutable CPLJSONObject m_oObj{};

      public:
        ConstIterator(const CPLJSONArray &oSelf, bool bStart)
            : m_oSelf(oSelf), m_nIdx(bStart ? 0 : oSelf.Size())
        {
        }

        ~ConstIterator() = default;

        CPLJSONObject &operator*() const
        {
            m_oObj = m_oSelf[m_nIdx];
            return m_oObj;
        }

        ConstIterator &operator++()
        {
            m_nIdx++;
            return *this;
        }

        bool operator==(const ConstIterator &it) const
        {
            return m_nIdx == it.m_nIdx;
        }

        bool operator!=(const ConstIterator &it) const
        {
            return m_nIdx != it.m_nIdx;
        }
    };

    /*! @endcond */
  public:
    int Size() const;
    void AddNull();
    void Add(const CPLJSONObject &oValue);
    void Add(const std::string &osValue);
    void Add(const char *pszValue);
    void Add(double dfValue);
    void Add(int nValue);
    void Add(GInt64 nValue);
    void Add(uint64_t nValue);
    void Add(bool bValue);

    CPLJSONObject operator[](int nIndex);
    const CPLJSONObject operator[](int nIndex) const;

    /** Iterator to first element */
    ConstIterator begin() const
    {
        return ConstIterator(*this, true);
    }

    /** Iterator to after last element */
    ConstIterator end() const
    {
        return ConstIterator(*this, false);
    }
};

/**
 * @brief The CPLJSONDocument class Wrapper class around json-c library
 */
class CPL_DLL CPLJSONDocument
{
  public:
    /*! @cond Doxygen_Suppress */
    CPLJSONDocument();
    ~CPLJSONDocument();
    CPLJSONDocument(const CPLJSONDocument &other);
    CPLJSONDocument &operator=(const CPLJSONDocument &other);
    CPLJSONDocument(CPLJSONDocument &&other);
    CPLJSONDocument &operator=(CPLJSONDocument &&other);
    /*! @endcond */

    bool Save(const std::string &osPath) const;
    std::string SaveAsString() const;

    CPLJSONObject GetRoot();
    const CPLJSONObject GetRoot() const;
    void SetRoot(const CPLJSONObject &oRoot);
    bool Load(const std::string &osPath);
    bool LoadMemory(const std::string &osStr);
    bool LoadMemory(const GByte *pabyData, int nLength = -1);
    bool LoadChunks(const std::string &osPath, size_t nChunkSize = 16384,
                    GDALProgressFunc pfnProgress = nullptr,
                    void *pProgressArg = nullptr);
    bool LoadUrl(const std::string &osUrl, const char *const *papszOptions,
                 GDALProgressFunc pfnProgress = nullptr,
                 void *pProgressArg = nullptr);

  private:
    mutable JSONObjectH m_poRootJsonObject;
};

/*! @cond Doxygen_Suppress */
template <class T>
inline CPLJSONObject &CPLJSONObject::operator=(std::initializer_list<T> list)
{
    return operator=(CPLJSONArray::Build(list));
}

/*! @endcond */

CPLStringList CPLParseKeyValueJson(const char *pszJson);

#endif  // CPL_JSON_H_INCLUDED
