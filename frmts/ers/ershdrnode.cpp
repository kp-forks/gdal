/******************************************************************************
 *
 * Project:  ERMapper .ers Driver
 * Purpose:  Implementation of ERSHdrNode class for parsing/accessing .ers hdr.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_conv.h"
#include "cpl_string.h"
#include "ershdrnode.h"

/************************************************************************/
/*                            ~ERSHdrNode()                             */
/************************************************************************/

ERSHdrNode::~ERSHdrNode()

{
    for (int i = 0; i < nItemCount; i++)
    {
        if (papoItemChild[i] != nullptr)
            delete papoItemChild[i];
        if (papszItemValue[i] != nullptr)
            CPLFree(papszItemValue[i]);
        CPLFree(papszItemName[i]);
    }

    CPLFree(papszItemName);
    CPLFree(papszItemValue);
    CPLFree(papoItemChild);
}

/************************************************************************/
/*                             MakeSpace()                              */
/*                                                                      */
/*      Ensure we have room for at least one more entry in our item     */
/*      lists.                                                          */
/************************************************************************/

void ERSHdrNode::MakeSpace()

{
    if (nItemCount == nItemMax)
    {
        nItemMax = nItemMax + nItemMax / 3 + 10;
        papszItemName = static_cast<char **>(
            CPLRealloc(papszItemName, sizeof(char *) * nItemMax));
        papszItemValue = static_cast<char **>(
            CPLRealloc(papszItemValue, sizeof(char *) * nItemMax));
        papoItemChild = static_cast<ERSHdrNode **>(
            CPLRealloc(papoItemChild, sizeof(ERSHdrNode *) * nItemMax));
    }
}

/************************************************************************/
/*                              ReadLine()                              */
/*                                                                      */
/*      Read one virtual line from the input source.  Multiple lines    */
/*      will be appended for objects enclosed in {}.                    */
/************************************************************************/

int ERSHdrNode::ReadLine(VSILFILE *fp, CPLString &osLine)

{
    int nBracketLevel = 0;
    bool bInQuote = false;
    size_t i = 0;
    bool bLastCharWasSlashInQuote = false;

    osLine = "";
    do
    {
        const char *pszNewLine = CPLReadLineL(fp);

        if (pszNewLine == nullptr)
            return FALSE;

        osLine += pszNewLine;

        for (; i < osLine.length(); i++)
        {
            const char ch = osLine[i];
            if (bLastCharWasSlashInQuote)
            {
                bLastCharWasSlashInQuote = false;
            }
            else if (ch == '"')
                bInQuote = !bInQuote;
            else if (ch == '{' && !bInQuote)
                nBracketLevel++;
            else if (ch == '}' && !bInQuote)
                nBracketLevel--;
            // We have to ignore escaped quotes and backslashes in strings.
            else if (ch == '\\' && bInQuote)
            {
                bLastCharWasSlashInQuote = true;
            }
            // A comment is a '#' up to the end of the line.
            else if (ch == '#' && !bInQuote)
            {
                osLine = osLine.substr(0, i) + "\n";
            }
        }
    } while (nBracketLevel > 0);

    return TRUE;
}

/************************************************************************/
/*                            ParseHeader()                             */
/*                                                                      */
/*      We receive the FILE * positioned at the start of the file       */
/*      and read all children.  This allows reading comment lines       */
/*      at the start of the file.                                       */
/************************************************************************/

int ERSHdrNode::ParseHeader(VSILFILE *fp)

{
    while (true)
    {
        /* --------------------------------------------------------------------
         */
        /*      Read the next line */
        /* --------------------------------------------------------------------
         */
        CPLString osLine;
        size_t iOff;

        if (!ReadLine(fp, osLine))
            return FALSE;

        /* --------------------------------------------------------------------
         */
        /*      Got a DatasetHeader Begin */
        /* --------------------------------------------------------------------
         */
        else if ((iOff = osLine.ifind(" Begin")) != std::string::npos)
        {
            CPLString osName = osLine.substr(0, iOff);
            osName.Trim();

            if (osName.tolower() == CPLString("DatasetHeader").tolower())
            {
                return ParseChildren(fp);
            }
        }
    }
}

/************************************************************************/
/*                           ParseChildren()                            */
/*                                                                      */
/*      We receive the FILE * positioned after the "Object Begin"       */
/*      line for this object, and are responsible for reading all       */
/*      children.  We should return after consuming the                 */
/*      corresponding End line for this object.  Really the first       */
/*      unmatched End since we don't know what object we are.           */
/*                                                                      */
/*      This function is used recursively to read sub-objects.          */
/************************************************************************/

int ERSHdrNode::ParseChildren(VSILFILE *fp, int nRecLevel)

{
    if (nRecLevel == 100)  // arbitrary limit
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too many recursion level while parsing .ers header");
        return FALSE;
    }

    while (true)
    {
        /* --------------------------------------------------------------------
         */
        /*      Read the next line (or multi-line for bracketed value). */
        /* --------------------------------------------------------------------
         */
        CPLString osLine;

        if (!ReadLine(fp, osLine))
            return FALSE;

        /* --------------------------------------------------------------------
         */
        /*      Got a Name=Value. */
        /* --------------------------------------------------------------------
         */
        size_t iOff;

        if ((iOff = osLine.find_first_of('=')) != std::string::npos)
        {
            CPLString osName =
                iOff == 0 ? std::string() : osLine.substr(0, iOff);
            osName.Trim();

            CPLString osValue = osLine.c_str() + iOff + 1;
            osValue.Trim();

            MakeSpace();
            papszItemName[nItemCount] = CPLStrdup(osName);
            papszItemValue[nItemCount] = CPLStrdup(osValue);
            papoItemChild[nItemCount] = nullptr;

            nItemCount++;
        }

        /* --------------------------------------------------------------------
         */
        /*      Got a Begin for an object. */
        /* --------------------------------------------------------------------
         */
        else if ((iOff = osLine.ifind(" Begin")) != std::string::npos)
        {
            CPLString osName = osLine.substr(0, iOff);
            osName.Trim();

            MakeSpace();
            papszItemName[nItemCount] = CPLStrdup(osName);
            papszItemValue[nItemCount] = nullptr;
            papoItemChild[nItemCount] = new ERSHdrNode();

            nItemCount++;

            if (!papoItemChild[nItemCount - 1]->ParseChildren(fp,
                                                              nRecLevel + 1))
                return FALSE;
        }

        /* --------------------------------------------------------------------
         */
        /*      Got an End for our object.  Well, at least we *assume* it */
        /*      must be for our object. */
        /* --------------------------------------------------------------------
         */
        else if (osLine.ifind(" End") != std::string::npos)
        {
            return TRUE;
        }

        /* --------------------------------------------------------------------
         */
        /*      Error? */
        /* --------------------------------------------------------------------
         */
        else if (osLine.Trim().length() > 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unexpected line parsing .ecw:\n%s", osLine.c_str());
            return FALSE;
        }
    }
}

/************************************************************************/
/*                             WriteSelf()                              */
/*                                                                      */
/*      Recursively write self and children to file.                    */
/************************************************************************/

int ERSHdrNode::WriteSelf(VSILFILE *fp, int nIndent)

{
    CPLString oIndent;

    oIndent.assign(nIndent, '\t');

    for (int i = 0; i < nItemCount; i++)
    {
        if (papszItemValue[i] != nullptr)
        {
            if (VSIFPrintfL(fp, "%s%s\t= %s\n", oIndent.c_str(),
                            papszItemName[i], papszItemValue[i]) < 1)
                return FALSE;
        }
        else
        {
            VSIFPrintfL(fp, "%s%s Begin\n", oIndent.c_str(), papszItemName[i]);
            if (!papoItemChild[i]->WriteSelf(fp, nIndent + 1))
                return FALSE;
            if (VSIFPrintfL(fp, "%s%s End\n", oIndent.c_str(),
                            papszItemName[i]) < 1)
                return FALSE;
        }
    }

    return TRUE;
}

/************************************************************************/
/*                                Find()                                */
/*                                                                      */
/*      Find the desired entry value.  The input is a path with         */
/*      components separated by dots, relative to the current node.     */
/************************************************************************/

const char *ERSHdrNode::Find(const char *pszPath, const char *pszDefault)

{
    /* -------------------------------------------------------------------- */
    /*      If this is the final component of the path, search for a        */
    /*      matching child and return the value.                            */
    /* -------------------------------------------------------------------- */
    if (strchr(pszPath, '.') == nullptr)
    {
        for (int i = 0; i < nItemCount; i++)
        {
            if (EQUAL(pszPath, papszItemName[i]))
            {
                if (papszItemValue[i] != nullptr)
                {
                    if (papszItemValue[i][0] == '"')
                    {
                        // strip off quotes.
                        osTempReturn = papszItemValue[i];
                        if (osTempReturn.length() < 2)
                            osTempReturn.clear();
                        else
                            osTempReturn = osTempReturn.substr(
                                1, osTempReturn.length() - 2);
                        return osTempReturn;
                    }
                    else
                        return papszItemValue[i];
                }
                else
                    return pszDefault;
            }
        }
        return pszDefault;
    }

    /* -------------------------------------------------------------------- */
    /*      This is a dot path - extract the first element, find a match    */
    /*      and recurse.                                                    */
    /* -------------------------------------------------------------------- */
    CPLString osPathFirst, osPathRest, osPath = pszPath;

    size_t iDot = osPath.find_first_of('.');
    osPathFirst = osPath.substr(0, iDot);
    osPathRest = osPath.substr(iDot + 1);

    for (int i = 0; i < nItemCount; i++)
    {
        if (EQUAL(osPathFirst, papszItemName[i]))
        {
            if (papoItemChild[i] != nullptr)
                return papoItemChild[i]->Find(osPathRest, pszDefault);

            return pszDefault;
        }
    }

    return pszDefault;
}

/************************************************************************/
/*                              FindElem()                              */
/*                                                                      */
/*      Find a particular element from an array valued item.            */
/************************************************************************/

const char *ERSHdrNode::FindElem(const char *pszPath, int iElem,
                                 const char *pszDefault)

{
    const char *pszArray = Find(pszPath, nullptr);

    if (pszArray == nullptr)
        return pszDefault;

    bool bDefault = true;
    char **papszTokens =
        CSLTokenizeStringComplex(pszArray, "{ \t}", TRUE, FALSE);
    if (iElem >= 0 && iElem < CSLCount(papszTokens))
    {
        osTempReturn = papszTokens[iElem];
        bDefault = false;
    }

    CSLDestroy(papszTokens);

    if (bDefault)
        return pszDefault;

    return osTempReturn;
}

/************************************************************************/
/*                              FindNode()                              */
/*                                                                      */
/*      Find the desired node.                                          */
/************************************************************************/

ERSHdrNode *ERSHdrNode::FindNode(const char *pszPath)

{
    std::string osPathFirst, osPathRest;
    std::string osPath = pszPath;
    const size_t iDot = osPath.find('.');
    if (iDot == std::string::npos)
    {
        osPathFirst = std::move(osPath);
    }
    else
    {
        osPathFirst = osPath.substr(0, iDot);
        osPathRest = osPath.substr(iDot + 1);
    }

    for (int i = 0; i < nItemCount; i++)
    {
        if (EQUAL(osPathFirst.c_str(), papszItemName[i]))
        {
            if (papoItemChild[i] != nullptr)
            {
                if (osPathRest.length() > 0)
                    return papoItemChild[i]->FindNode(osPathRest.c_str());
                else
                    return papoItemChild[i];
            }
            else
                return nullptr;
        }
    }

    return nullptr;
}

/************************************************************************/
/*                                Set()                                 */
/*                                                                      */
/*      Set a value item.                                               */
/************************************************************************/

void ERSHdrNode::Set(const char *pszPath, const char *pszValue)

{
    CPLString osPath = pszPath;
    size_t iDot = osPath.find_first_of('.');

    /* -------------------------------------------------------------------- */
    /*      We have an intermediate node, find or create it and             */
    /*      recurse.                                                        */
    /* -------------------------------------------------------------------- */
    if (iDot != std::string::npos)
    {
        CPLString osPathFirst = osPath.substr(0, iDot);
        CPLString osPathRest = osPath.substr(iDot + 1);
        ERSHdrNode *poFirst = FindNode(osPathFirst);

        if (poFirst == nullptr)
        {
            poFirst = new ERSHdrNode();

            MakeSpace();
            papszItemName[nItemCount] = CPLStrdup(osPathFirst);
            papszItemValue[nItemCount] = nullptr;
            papoItemChild[nItemCount] = poFirst;
            nItemCount++;
        }

        poFirst->Set(osPathRest, pszValue);
        return;
    }

    /* -------------------------------------------------------------------- */
    /*      This is the final item name.  Find or create it.                */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < nItemCount; i++)
    {
        if (EQUAL(osPath, papszItemName[i]) && papszItemValue[i] != nullptr)
        {
            CPLFree(papszItemValue[i]);
            papszItemValue[i] = CPLStrdup(pszValue);
            return;
        }
    }

    MakeSpace();
    papszItemName[nItemCount] = CPLStrdup(osPath);
    papszItemValue[nItemCount] = CPLStrdup(pszValue);
    papoItemChild[nItemCount] = nullptr;
    nItemCount++;
}
