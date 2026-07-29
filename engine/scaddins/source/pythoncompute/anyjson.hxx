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

/*
 * Convert Calc AddIn Anys ↔ dumb JSON (numbers / strings / null / nested lists).
 */

#pragma once

#include <cpo/uno/Any.hxx>
#include <cpo/uno/Sequence.hxx>
#include <formula/errorcodes.hxx>
#include <rtl/ustring.hxx>
#include <sal/types.h>
#include <string>
#include <string_view>

namespace collaboraoffice::pythoncompute
{
/** Cell formula error for ScUnoAddInCall::SetResult: void → #N/A; else CreateDoubleError. */
SAL_DLLPUBLIC_EXPORT cpo::uno::Any makeFormulaErrorAny(FormulaError e);

/** Flatten 1×N / N×1 grids to 1D like Classic normalize_python_data_shape. */
SAL_DLLPUBLIC_EXPORT OUString anyToJsonFragment(const cpo::uno::Any& aValue);

/** Build body: {"id","code","mode", optional "data"}. */
SAL_DLLPUBLIC_EXPORT std::string
buildExecuteRequestJson(const OUString& sRequestId, const OUString& sCode,
                        const cpo::uno::Sequence<cpo::uno::Any>& aData);

/** Parse service result JSON into an Any suitable for ScUnoAddInCall::SetResult. */
SAL_DLLPUBLIC_EXPORT bool jsonResultToAny(std::string_view jsonUtf8, cpo::uno::Any& rOut,
                                          OUString& rError);

/** Extract the string "id" field from a JSON object body. */
SAL_DLLPUBLIC_EXPORT bool extractRequestIdFromJson(std::string_view jsonUtf8, OUString& rId);
} // namespace collaboraoffice::pythoncompute

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
