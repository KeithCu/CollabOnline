/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the Collabora Office project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include "pythoncompute_anyjson.hxx"

#include <cpo/uno/Type.hxx>
#include <rtl/math.hxx>
#include <rtl/ustrbuf.hxx>
#include <rtl/ustring.hxx>
#include <sal/log.hxx>
#include <tools/json_writer.hxx>

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace collaboraoffice::pythoncompute;

cpo::uno::Any collaboraoffice::pythoncompute::makeFormulaErrorAny(FormulaError e)
{
    // Prefer void for #N/A — SetResult maps TypeClass_VOID → NotAvailable.
    if (e == FormulaError::NotAvailable)
        return cpo::uno::Any();
    return cpo::uno::Any(CreateDoubleError(e));
}

namespace
{
/** Detect 1×N or N×1 nested sequences and flatten to a 1D JSON array. */
bool tryFlattenRowOrCol(const cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>>& aGrid,
                        cpo::uno::Sequence<cpo::uno::Any>& rFlat)
{
    const sal_Int32 nRows = aGrid.getLength();
    if (nRows <= 0)
        return false;
    if (nRows == 1)
    {
        rFlat = aGrid[0];
        return true;
    }
    bool bSingleCol = true;
    for (sal_Int32 r = 0; r < nRows; ++r)
    {
        if (aGrid[r].getLength() != 1)
        {
            bSingleCol = false;
            break;
        }
    }
    if (!bSingleCol)
        return false;
    rFlat.realloc(nRows);
    auto* p = rFlat.getArray();
    for (sal_Int32 r = 0; r < nRows; ++r)
        p[r] = aGrid[r][0];
    return true;
}

bool tryFlattenDoubleRowOrCol(const cpo::uno::Sequence<cpo::uno::Sequence<double>>& aGrid,
                              cpo::uno::Sequence<double>& rFlat)
{
    const sal_Int32 nRows = aGrid.getLength();
    if (nRows <= 0)
        return false;
    if (nRows == 1)
    {
        rFlat = aGrid[0];
        return true;
    }
    for (sal_Int32 r = 0; r < nRows; ++r)
    {
        if (aGrid[r].getLength() != 1)
            return false;
    }
    rFlat.realloc(nRows);
    auto* p = rFlat.getArray();
    for (sal_Int32 r = 0; r < nRows; ++r)
        p[r] = aGrid[r][0];
    return true;
}

void writeAnyElement(tools::JsonWriter& rWriter, const cpo::uno::Any& aValue);
void writeAnyProperty(tools::JsonWriter& rWriter, std::string_view sName,
                      const cpo::uno::Any& aValue);

void writeNumberElement(tools::JsonWriter& rWriter, double f)
{
    if (std::isnan(f) || std::isinf(f))
        rWriter.putRaw("null");
    else
        rWriter.putSimpleValue(f);
}

void writeNumberProperty(tools::JsonWriter& rWriter, std::string_view sName, double f)
{
    if (std::isnan(f) || std::isinf(f))
    {
        std::string field = "\"";
        field += sName;
        field += "\": null";
        rWriter.putRaw(field);
    }
    else
        rWriter.put(sName, f);
}

void writeSequenceElements(tools::JsonWriter& rWriter,
                           const cpo::uno::Sequence<cpo::uno::Any>& aSeq)
{
    for (sal_Int32 i = 0; i < aSeq.getLength(); ++i)
        writeAnyElement(rWriter, aSeq[i]);
}

void writeAnyGrid(tools::JsonWriter& rWriter,
                  const cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>>& aGrid,
                  std::optional<std::string_view> oName)
{
    cpo::uno::Sequence<cpo::uno::Any> aRowCol;
    if (tryFlattenRowOrCol(aGrid, aRowCol))
    {
        if (oName)
        {
            auto aArr = rWriter.startArray(*oName);
            writeSequenceElements(rWriter, aRowCol);
        }
        else
        {
            auto aArr = rWriter.startAnonArray();
            writeSequenceElements(rWriter, aRowCol);
        }
        return;
    }
    if (oName)
    {
        auto aOuter = rWriter.startArray(*oName);
        for (sal_Int32 r = 0; r < aGrid.getLength(); ++r)
        {
            auto aInner = rWriter.startAnonArray();
            writeSequenceElements(rWriter, aGrid[r]);
        }
    }
    else
    {
        auto aOuter = rWriter.startAnonArray();
        for (sal_Int32 r = 0; r < aGrid.getLength(); ++r)
        {
            auto aInner = rWriter.startAnonArray();
            writeSequenceElements(rWriter, aGrid[r]);
        }
    }
}

void writeDoubleGrid(tools::JsonWriter& rWriter,
                     const cpo::uno::Sequence<cpo::uno::Sequence<double>>& aGrid,
                     std::optional<std::string_view> oName)
{
    cpo::uno::Sequence<double> aRowCol;
    if (tryFlattenDoubleRowOrCol(aGrid, aRowCol))
    {
        if (oName)
        {
            auto aArr = rWriter.startArray(*oName);
            for (sal_Int32 i = 0; i < aRowCol.getLength(); ++i)
                writeNumberElement(rWriter, aRowCol[i]);
        }
        else
        {
            auto aArr = rWriter.startAnonArray();
            for (sal_Int32 i = 0; i < aRowCol.getLength(); ++i)
                writeNumberElement(rWriter, aRowCol[i]);
        }
        return;
    }
    if (oName)
    {
        auto aOuter = rWriter.startArray(*oName);
        for (sal_Int32 r = 0; r < aGrid.getLength(); ++r)
        {
            auto aInner = rWriter.startAnonArray();
            for (sal_Int32 c = 0; c < aGrid[r].getLength(); ++c)
                writeNumberElement(rWriter, aGrid[r][c]);
        }
    }
    else
    {
        auto aOuter = rWriter.startAnonArray();
        for (sal_Int32 r = 0; r < aGrid.getLength(); ++r)
        {
            auto aInner = rWriter.startAnonArray();
            for (sal_Int32 c = 0; c < aGrid[r].getLength(); ++c)
                writeNumberElement(rWriter, aGrid[r][c]);
        }
    }
}

void writeAnyElement(tools::JsonWriter& rWriter, const cpo::uno::Any& aValue)
{
    if (!aValue.hasValue())
    {
        rWriter.putRaw("null");
        return;
    }

    switch (aValue.getValueTypeClass())
    {
        case cpo::uno::TypeClass_VOID:
            rWriter.putRaw("null");
            return;
        case cpo::uno::TypeClass_BOOLEAN:
        {
            bool b = false;
            aValue >>= b;
            rWriter.putRaw(b ? "true" : "false");
            return;
        }
        case cpo::uno::TypeClass_BYTE:
        case cpo::uno::TypeClass_SHORT:
        case cpo::uno::TypeClass_UNSIGNED_SHORT:
        case cpo::uno::TypeClass_LONG:
        case cpo::uno::TypeClass_UNSIGNED_LONG:
        case cpo::uno::TypeClass_HYPER:
        case cpo::uno::TypeClass_UNSIGNED_HYPER:
        case cpo::uno::TypeClass_FLOAT:
        case cpo::uno::TypeClass_DOUBLE:
        {
            double f = 0.0;
            aValue >>= f;
            writeNumberElement(rWriter, f);
            return;
        }
        case cpo::uno::TypeClass_STRING:
        {
            OUString s;
            aValue >>= s;
            rWriter.putSimpleValue(s);
            return;
        }
        case cpo::uno::TypeClass_SEQUENCE:
        {
            // Calc ranges arrive as Sequence<Sequence<Any>> — try grids first.
            // Sequence<Any> is a distinct UNO type and must not gate grids.
            cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aGrid;
            if (aValue >>= aGrid)
            {
                writeAnyGrid(rWriter, aGrid, std::nullopt);
                return;
            }
            cpo::uno::Sequence<cpo::uno::Sequence<double>> aDGrid;
            if (aValue >>= aDGrid)
            {
                writeDoubleGrid(rWriter, aDGrid, std::nullopt);
                return;
            }
            cpo::uno::Sequence<cpo::uno::Any> aFlat;
            if (aValue >>= aFlat)
            {
                auto aArr = rWriter.startAnonArray();
                writeSequenceElements(rWriter, aFlat);
                return;
            }
            cpo::uno::Sequence<double> aDRow;
            if (aValue >>= aDRow)
            {
                auto aArr = rWriter.startAnonArray();
                for (sal_Int32 i = 0; i < aDRow.getLength(); ++i)
                    writeNumberElement(rWriter, aDRow[i]);
                return;
            }
            cpo::uno::Sequence<OUString> aSRow;
            if (aValue >>= aSRow)
            {
                auto aArr = rWriter.startAnonArray();
                for (sal_Int32 i = 0; i < aSRow.getLength(); ++i)
                    rWriter.putSimpleValue(aSRow[i]);
                return;
            }
            break;
        }
        default:
            break;
    }
    SAL_INFO("scaddins.pythoncompute", "anyToJson: unsupported type -> null");
    rWriter.putRaw("null");
}

void writeAnyProperty(tools::JsonWriter& rWriter, std::string_view sName,
                      const cpo::uno::Any& aValue)
{
    auto putNamedNull = [&]() {
        std::string field = "\"";
        field += sName;
        field += "\": null";
        rWriter.putRaw(field);
    };

    if (!aValue.hasValue())
    {
        putNamedNull();
        return;
    }

    switch (aValue.getValueTypeClass())
    {
        case cpo::uno::TypeClass_VOID:
            putNamedNull();
            return;
        case cpo::uno::TypeClass_BOOLEAN:
        {
            bool b = false;
            aValue >>= b;
            rWriter.put(sName, b);
            return;
        }
        case cpo::uno::TypeClass_BYTE:
        case cpo::uno::TypeClass_SHORT:
        case cpo::uno::TypeClass_UNSIGNED_SHORT:
        case cpo::uno::TypeClass_LONG:
        case cpo::uno::TypeClass_UNSIGNED_LONG:
        case cpo::uno::TypeClass_HYPER:
        case cpo::uno::TypeClass_UNSIGNED_HYPER:
        case cpo::uno::TypeClass_FLOAT:
        case cpo::uno::TypeClass_DOUBLE:
        {
            double f = 0.0;
            aValue >>= f;
            writeNumberProperty(rWriter, sName, f);
            return;
        }
        case cpo::uno::TypeClass_STRING:
        {
            OUString s;
            aValue >>= s;
            rWriter.put(sName, s);
            return;
        }
        case cpo::uno::TypeClass_SEQUENCE:
        {
            // Named array field: "name": [ … ] — grids before Sequence<Any>.
            cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aGrid;
            if (aValue >>= aGrid)
            {
                writeAnyGrid(rWriter, aGrid, sName);
                return;
            }
            cpo::uno::Sequence<cpo::uno::Sequence<double>> aDGrid;
            if (aValue >>= aDGrid)
            {
                writeDoubleGrid(rWriter, aDGrid, sName);
                return;
            }
            cpo::uno::Sequence<cpo::uno::Any> aFlat;
            if (aValue >>= aFlat)
            {
                auto aArr = rWriter.startArray(sName);
                writeSequenceElements(rWriter, aFlat);
                return;
            }
            cpo::uno::Sequence<double> aDRow;
            if (aValue >>= aDRow)
            {
                auto aArr = rWriter.startArray(sName);
                for (sal_Int32 i = 0; i < aDRow.getLength(); ++i)
                    writeNumberElement(rWriter, aDRow[i]);
                return;
            }
            cpo::uno::Sequence<OUString> aSRow;
            if (aValue >>= aSRow)
            {
                auto aArr = rWriter.startArray(sName);
                for (sal_Int32 i = 0; i < aSRow.getLength(); ++i)
                    rWriter.putSimpleValue(aSRow[i]);
                return;
            }
            break;
        }
        default:
            break;
    }
    SAL_INFO("scaddins.pythoncompute", "anyToJson property: unsupported type -> null");
    putNamedNull();
}

// --- Dumb-JSON hand parser: preserve string vs number vs bool vs null. ---

struct JsonCursor
{
    std::string_view s;
    size_t i = 0;

    void skipWs()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    bool consume(char c)
    {
        skipWs();
        if (i < s.size() && s[i] == c)
        {
            ++i;
            return true;
        }
        return false;
    }

    bool peek(char c)
    {
        skipWs();
        return i < s.size() && s[i] == c;
    }
};

bool parseValue(JsonCursor& cur, cpo::uno::Any& rOut);
bool parseArray(JsonCursor& cur, cpo::uno::Any& rOut);

bool parseStringToken(JsonCursor& cur, OUString& rOut)
{
    cur.skipWs();
    if (cur.i >= cur.s.size() || cur.s[cur.i] != '"')
        return false;
    ++cur.i;
    OUStringBuffer buf;
    while (cur.i < cur.s.size())
    {
        const char c = cur.s[cur.i++];
        if (c == '"')
        {
            rOut = buf.makeStringAndClear();
            return true;
        }
        if (c == '\\')
        {
            if (cur.i >= cur.s.size())
                return false;
            const char e = cur.s[cur.i++];
            switch (e)
            {
                case '"':
                case '\\':
                case '/':
                    buf.append(sal_Unicode(e));
                    break;
                case 'b':
                    buf.append(u'\b');
                    break;
                case 'f':
                    buf.append(u'\f');
                    break;
                case 'n':
                    buf.append(u'\n');
                    break;
                case 'r':
                    buf.append(u'\r');
                    break;
                case 't':
                    buf.append(u'\t');
                    break;
                case 'u':
                {
                    auto readCodeUnit = [&cur](sal_uInt32& rCode) {
                        if (cur.i + 4 > cur.s.size())
                            return false;
                        rCode = 0;
                        for (int n = 0; n < 4; ++n)
                        {
                            const char h = cur.s[cur.i++];
                            rCode <<= 4;
                            if (h >= '0' && h <= '9')
                                rCode |= static_cast<sal_uInt32>(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                rCode |= static_cast<sal_uInt32>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                rCode |= static_cast<sal_uInt32>(h - 'A' + 10);
                            else
                                return false;
                        }
                        return true;
                    };

                    sal_uInt32 code = 0;
                    if (!readCodeUnit(code))
                        return false;
                    if (code >= 0xD800 && code <= 0xDBFF)
                    {
                        if (cur.i + 2 > cur.s.size() || cur.s[cur.i] != '\\'
                            || cur.s[cur.i + 1] != 'u')
                            return false;
                        cur.i += 2;
                        sal_uInt32 low = 0;
                        if (!readCodeUnit(low) || low < 0xDC00 || low > 0xDFFF)
                            return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else if (code >= 0xDC00 && code <= 0xDFFF)
                        return false;
                    buf.appendUtf32(code);
                    break;
                }
                default:
                    return false;
            }
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
        // UTF-8: copy through as Unicode code units via fromUtf8 of the rest
        // is heavy; for ASCII-heavy dumb JSON append bytes as Latin-1-safe
        // and decode multi-byte sequences.
        if (static_cast<unsigned char>(c) < 0x80)
        {
            buf.append(sal_Unicode(c));
            continue;
        }
        // Multi-byte UTF-8: back up and decode a full character.
        --cur.i;
        const size_t start = cur.i;
        unsigned char lead = static_cast<unsigned char>(cur.s[cur.i]);
        size_t need = 0;
        if ((lead & 0xE0) == 0xC0)
            need = 2;
        else if ((lead & 0xF0) == 0xE0)
            need = 3;
        else if ((lead & 0xF8) == 0xF0)
            need = 4;
        else
            return false;
        if (cur.i + need > cur.s.size())
            return false;
        for (size_t k = 1; k < need; ++k)
        {
            if ((static_cast<unsigned char>(cur.s[cur.i + k]) & 0xC0) != 0x80)
                return false;
        }
        const OUString piece = OUString::fromUtf8(cur.s.substr(start, need));
        if (piece.isEmpty())
            return false;
        buf.append(piece);
        cur.i += need;
    }
    return false;
}

bool parseNumberToken(JsonCursor& cur, double& rOut)
{
    cur.skipWs();
    if (cur.i >= cur.s.size())
        return false;
    const size_t start = cur.i;
    if (cur.s[cur.i] == '-')
        ++cur.i;
    if (cur.i >= cur.s.size() || !(cur.s[cur.i] >= '0' && cur.s[cur.i] <= '9'))
        return false;
    if (cur.s[cur.i] == '0')
        ++cur.i;
    else
    {
        while (cur.i < cur.s.size() && cur.s[cur.i] >= '0' && cur.s[cur.i] <= '9')
            ++cur.i;
    }
    if (cur.i < cur.s.size() && cur.s[cur.i] == '.')
    {
        ++cur.i;
        if (cur.i >= cur.s.size() || !(cur.s[cur.i] >= '0' && cur.s[cur.i] <= '9'))
            return false;
        while (cur.i < cur.s.size() && cur.s[cur.i] >= '0' && cur.s[cur.i] <= '9')
            ++cur.i;
    }
    if (cur.i < cur.s.size() && (cur.s[cur.i] == 'e' || cur.s[cur.i] == 'E'))
    {
        ++cur.i;
        if (cur.i < cur.s.size() && (cur.s[cur.i] == '+' || cur.s[cur.i] == '-'))
            ++cur.i;
        if (cur.i >= cur.s.size() || !(cur.s[cur.i] >= '0' && cur.s[cur.i] <= '9'))
            return false;
        while (cur.i < cur.s.size() && cur.s[cur.i] >= '0' && cur.s[cur.i] <= '9')
            ++cur.i;
    }
    const std::string_view tok = cur.s.substr(start, cur.i - start);
    rtl_math_ConversionStatus eStatus = rtl_math_ConversionStatus_Ok;
    sal_Int32 nEnd = 0;
    const OUString sData = OUString::fromUtf8(tok);
    rOut = rtl::math::stringToDouble(sData, '.', ',', &eStatus, &nEnd);
    return eStatus == rtl_math_ConversionStatus_Ok && nEnd == sData.getLength();
}

bool consumeLiteral(JsonCursor& cur, std::string_view lit)
{
    cur.skipWs();
    if (cur.i + lit.size() > cur.s.size())
        return false;
    if (cur.s.substr(cur.i, lit.size()) != lit)
        return false;
    cur.i += lit.size();
    return true;
}

bool promoteFlatNumeric(const cpo::uno::Sequence<cpo::uno::Any>& aFlat, cpo::uno::Any& rOut)
{
    if (aFlat.getLength() <= 1)
        return false;
    for (sal_Int32 i = 0; i < aFlat.getLength(); ++i)
    {
        const cpo::uno::TypeClass e = aFlat[i].getValueTypeClass();
        if (e != cpo::uno::TypeClass_DOUBLE && e != cpo::uno::TypeClass_LONG
            && e != cpo::uno::TypeClass_HYPER && e != cpo::uno::TypeClass_FLOAT)
            return false;
    }
    cpo::uno::Sequence<cpo::uno::Sequence<double>> row(1);
    row.getArray()[0].realloc(aFlat.getLength());
    auto* pr = row.getArray()[0].getArray();
    for (sal_Int32 i = 0; i < aFlat.getLength(); ++i)
    {
        double f = 0;
        aFlat[i] >>= f;
        pr[i] = f;
    }
    rOut <<= row;
    return true;
}

bool elemsToAny(std::vector<cpo::uno::Any> elems, cpo::uno::Any& rOut)
{
    if (elems.empty())
    {
        rOut <<= cpo::uno::Sequence<cpo::uno::Any>();
        return true;
    }

    // Nested numeric rows: recursive convert promotes each inner list to a
    // 1×N double matrix; stack those into one R×C sequence<sequence<double>>.
    {
        bool bAll1xN = true;
        sal_Int32 nCols = -1;
        for (const auto& e : elems)
        {
            cpo::uno::Sequence<cpo::uno::Sequence<double>> rowMat;
            if (!(e >>= rowMat) || rowMat.getLength() != 1)
            {
                bAll1xN = false;
                break;
            }
            const sal_Int32 n = rowMat[0].getLength();
            if (nCols < 0)
                nCols = n;
            else if (n != nCols)
            {
                bAll1xN = false;
                break;
            }
        }
        if (bAll1xN && nCols >= 0)
        {
            cpo::uno::Sequence<cpo::uno::Sequence<double>> grid(
                static_cast<sal_Int32>(elems.size()));
            auto* p = grid.getArray();
            for (size_t i = 0; i < elems.size(); ++i)
            {
                cpo::uno::Sequence<cpo::uno::Sequence<double>> rowMat;
                elems[i] >>= rowMat;
                p[static_cast<sal_Int32>(i)] = rowMat[0];
            }
            rOut <<= grid;
            return true;
        }
    }

    // Nested arrays → sequence<sequence<any>>. Accept bare sequence<any> rows
    // or recursive 1×N matrix wraps. Enforce a rectangular shape because Calc
    // matrices cannot represent ragged JSON arrays.
    {
        bool bAllRows = true;
        sal_Int32 nCols = -1;
        std::vector<cpo::uno::Sequence<cpo::uno::Any>> aRows;
        aRows.reserve(elems.size());
        for (const auto& e : elems)
        {
            cpo::uno::Sequence<cpo::uno::Any> aRow;
            cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aMat;
            if ((e >>= aMat) && aMat.getLength() == 1)
                aRow = aMat[0];
            else
            {
                cpo::uno::Sequence<cpo::uno::Sequence<double>> aDoubleMat;
                if ((e >>= aDoubleMat) && aDoubleMat.getLength() == 1)
                {
                    aRow.realloc(aDoubleMat[0].getLength());
                    for (sal_Int32 i = 0; i < aDoubleMat[0].getLength(); ++i)
                        aRow.getArray()[i] <<= aDoubleMat[0][i];
                }
                else if (!(e >>= aRow))
                {
                    bAllRows = false;
                    break;
                }
            }

            if (nCols < 0)
                nCols = aRow.getLength();
            else if (aRow.getLength() != nCols)
            {
                bAllRows = false;
                break;
            }
            aRows.push_back(std::move(aRow));
        }
        if (bAllRows)
        {
            cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> grid(
                static_cast<sal_Int32>(aRows.size()));
            auto* p = grid.getArray();
            for (size_t i = 0; i < aRows.size(); ++i)
                p[static_cast<sal_Int32>(i)] = aRows[i];
            rOut <<= grid;
            return true;
        }
    }

    // A JSON array containing array values reached here only when the nested
    // rows were ragged or otherwise not representable as one Calc matrix.
    for (const auto& e : elems)
    {
        if (e.getValueTypeClass() == cpo::uno::TypeClass_SEQUENCE)
            return false;
    }

    cpo::uno::Sequence<cpo::uno::Any> flat(static_cast<sal_Int32>(elems.size()));
    auto* pf = flat.getArray();
    for (size_t i = 0; i < elems.size(); ++i)
        pf[static_cast<sal_Int32>(i)] = elems[i];

    if (promoteFlatNumeric(flat, rOut))
        return true;
    if (flat.getLength() > 1)
    {
        cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> row(1);
        row.getArray()[0] = flat;
        rOut <<= row;
        return true;
    }
    if (flat.getLength() == 1)
    {
        // Keep a one-element JSON list as a 1×1 matrix for every scalar type.
        // Bare JSON scalar values still use the leaf path in parseValue().
        const cpo::uno::TypeClass e = flat[0].getValueTypeClass();
        if (e == cpo::uno::TypeClass_DOUBLE || e == cpo::uno::TypeClass_LONG
            || e == cpo::uno::TypeClass_HYPER || e == cpo::uno::TypeClass_FLOAT)
        {
            double f = 0;
            flat[0] >>= f;
            cpo::uno::Sequence<cpo::uno::Sequence<double>> cell(1);
            cell.getArray()[0].realloc(1);
            cell.getArray()[0].getArray()[0] = f;
            rOut <<= cell;
            return true;
        }
        cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> cell(1);
        cell.getArray()[0] = flat;
        rOut <<= cell;
        return true;
    }
    rOut <<= flat;
    return true;
}

bool parseArray(JsonCursor& cur, cpo::uno::Any& rOut)
{
    if (!cur.consume('['))
        return false;
    std::vector<cpo::uno::Any> elems;
    cur.skipWs();
    if (cur.consume(']'))
        return elemsToAny(std::move(elems), rOut);
    for (;;)
    {
        cpo::uno::Any item;
        if (!parseValue(cur, item))
            return false;
        elems.push_back(std::move(item));
        cur.skipWs();
        if (cur.consume(']'))
            return elemsToAny(std::move(elems), rOut);
        if (!cur.consume(','))
            return false;
    }
}

bool skipObject(JsonCursor& cur)
{
    // Object-as-cell is unsupported; skip without retaining keys.
    if (!cur.consume('{'))
        return false;
    cur.skipWs();
    if (cur.consume('}'))
        return true;
    for (;;)
    {
        OUString key;
        if (!parseStringToken(cur, key))
            return false;
        if (!cur.consume(':'))
            return false;
        cpo::uno::Any ignored;
        if (!parseValue(cur, ignored))
            return false;
        cur.skipWs();
        if (cur.consume('}'))
            return true;
        if (!cur.consume(','))
            return false;
    }
}

bool parseValue(JsonCursor& cur, cpo::uno::Any& rOut)
{
    cur.skipWs();
    if (cur.i >= cur.s.size())
        return false;
    const char c = cur.s[cur.i];
    if (c == '"')
    {
        OUString s;
        if (!parseStringToken(cur, s))
            return false;
        rOut <<= s;
        return true;
    }
    if (c == '[')
        return parseArray(cur, rOut);
    if (c == '{')
    {
        if (!skipObject(cur))
            return false;
        // Classic: unexpected object → empty string (same as prior ptree path).
        rOut <<= OUString();
        return true;
    }
    if (c == 't')
    {
        if (!consumeLiteral(cur, "true"))
            return false;
        rOut <<= true;
        return true;
    }
    if (c == 'f')
    {
        if (!consumeLiteral(cur, "false"))
            return false;
        rOut <<= false;
        return true;
    }
    if (c == 'n')
    {
        if (!consumeLiteral(cur, "null"))
            return false;
        // Classic: None → ""
        rOut <<= OUString();
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9'))
    {
        double f = 0;
        if (!parseNumberToken(cur, f))
            return false;
        rOut <<= f;
        return true;
    }
    return false;
}

struct EnvelopeFields
{
    std::optional<OUString> id;
    std::optional<OUString> status;
    std::optional<OUString> error;
    std::optional<cpo::uno::Any> result;
    bool hasImages = false;
};

bool parseEnvelope(std::string_view jsonUtf8, EnvelopeFields& rOut)
{
    JsonCursor cur{ jsonUtf8, 0 };
    if (!cur.consume('{'))
        return false;
    cur.skipWs();
    if (cur.consume('}'))
        return true;
    for (;;)
    {
        OUString key;
        if (!parseStringToken(cur, key))
            return false;
        if (!cur.consume(':'))
            return false;
        const OString aKeyUtf8 = OUStringToOString(key, RTL_TEXTENCODING_UTF8);
        const std::string_view k(aKeyUtf8);

        if (k == "id" || k == "status" || k == "error")
        {
            cur.skipWs();
            if (cur.peek('"'))
            {
                OUString s;
                if (!parseStringToken(cur, s))
                    return false;
                if (k == "id")
                    rOut.id = s;
                else if (k == "status")
                    rOut.status = s;
                else
                    rOut.error = s;
            }
            else
            {
                // Non-string values for these keys: skip as a generic value.
                cpo::uno::Any ignored;
                if (!parseValue(cur, ignored))
                    return false;
            }
        }
        else if (k == "result")
        {
            cpo::uno::Any result;
            if (!parseValue(cur, result))
                return false;
            rOut.result = std::move(result);
        }
        else if (k == "images")
        {
            cpo::uno::Any ignored;
            if (!parseValue(cur, ignored))
                return false;
            rOut.hasImages = true;
        }
        else
        {
            cpo::uno::Any ignored;
            if (!parseValue(cur, ignored))
                return false;
        }

        cur.skipWs();
        if (cur.consume('}'))
        {
            cur.skipWs();
            return cur.i == cur.s.size();
        }
        if (!cur.consume(','))
            return false;
    }
}
} // namespace

OUString collaboraoffice::pythoncompute::anyToJsonFragment(const cpo::uno::Any& aValue)
{
    // JsonWriter always builds an object document. Wrap as {"_": VALUE} then strip.
    tools::JsonWriter aWriter;
    writeAnyProperty(aWriter, "_", aValue);
    const OString aDoc = aWriter.finishAndGetAsOString();
    const std::string_view s(aDoc);
    const size_t nColon = s.find(':');
    if (nColon == std::string_view::npos || s.size() < 3)
        return u"null"_ustr;
    size_t nStart = nColon + 1;
    while (nStart < s.size() && s[nStart] == ' ')
        ++nStart;
    size_t nEnd = s.size();
    if (nEnd > 0 && s[nEnd - 1] == '}')
        --nEnd;
    while (nEnd > nStart && s[nEnd - 1] == ' ')
        --nEnd;
    return OUString::fromUtf8(s.substr(nStart, nEnd - nStart));
}

std::string collaboraoffice::pythoncompute::buildExecuteRequestJson(
    const OUString& sRequestId, const OUString& sCode,
    const cpo::uno::Sequence<cpo::uno::Any>& aData)
{
    tools::JsonWriter aWriter;
    aWriter.put("id", sRequestId);
    aWriter.put("code", sCode);
    aWriter.put("mode", std::string_view("isolated"));
    if (aData.getLength() == 1)
        writeAnyProperty(aWriter, "data", aData[0]);
    else if (aData.getLength() > 1)
    {
        auto aArr = aWriter.startArray("data");
        writeSequenceElements(aWriter, aData);
    }
    const OString aDoc = aWriter.finishAndGetAsOString();
    return std::string(aDoc);
}

bool collaboraoffice::pythoncompute::jsonResultToAny(std::string_view jsonUtf8, cpo::uno::Any& rOut,
                                                     OUString& rError)
{
    rError.clear();
    EnvelopeFields env;
    if (!parseEnvelope(jsonUtf8, env))
    {
        rError = u"Python compute result JSON parse failed"_ustr;
        rOut = makeFormulaErrorAny(FormulaError::NoValue);
        return true;
    }

    if (env.status && *env.status == u"error")
    {
        if (env.error && !env.error->isEmpty())
            rError = *env.error;
        else
            rError = u"Python compute error"_ustr;
        // Admin-off path from coolwsd — show a clear cell marker (like #BUSY!),
        // not opaque #VALUE!, so operators can see the feature flag without logs.
        if (rError == u"Python compute is disabled")
            rOut <<= u"#DISABLED"_ustr;
        else
            rOut = makeFormulaErrorAny(FormulaError::NoValue);
        return true;
    }

    if (!env.result)
    {
        rError = u"Python compute response missing result"_ustr;
        rOut = makeFormulaErrorAny(FormulaError::NoValue);
        return true;
    }

    // images[] only — Classic inserts plots; Online v1: short message
    const cpo::uno::Any& aResult = *env.result;
    const bool bResultNull
        = !aResult.hasValue()
          || (aResult.getValueTypeClass() == cpo::uno::TypeClass_STRING && [&]() {
                 OUString s;
                 return (aResult >>= s) && s.isEmpty();
             }());
    // JSON null maps to empty OUString above; treat that as null result.
    if (env.hasImages && bResultNull)
    {
        rOut <<= u"Image generated (plot insert not supported yet)"_ustr;
        return true;
    }

    rOut = aResult;
    return true;
}

bool collaboraoffice::pythoncompute::extractRequestIdFromJson(std::string_view jsonUtf8,
                                                              OUString& rId)
{
    EnvelopeFields env;
    if (!parseEnvelope(jsonUtf8, env) || !env.id || env.id->isEmpty())
        return false;
    rId = *env.id;
    return true;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
