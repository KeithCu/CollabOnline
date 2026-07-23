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

#include "pythoncompute_addin.hxx"
#include "pythoncompute_bridge.hxx"

#include <com/sun/star/lang/IllegalArgumentException.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <cppuhelper/supportsservice.hxx>
#include <sal/log.hxx>

using namespace css;
using namespace collaboraoffice::pythoncompute;

constexpr OUString ADDIN_SERVICE = u"com.sun.star.sheet.AddIn"_ustr;
constexpr OUString MY_SERVICE = u"org.collaboraoffice.sheet.addin.PythonComputeFunctions"_ustr;

extern "C" SAL_DLLPUBLIC_EXPORT css::uno::XInterface*
scaddins_CollaboraOfficePythonComputeAddIn_get_implementation(
    css::uno::XComponentContext*, cpo::uno::Sequence<cpo::uno::Any> const&)
{
    return cppu::acquire(new ScaPythonComputeAddIn());
}

ScaPythonComputeAddIn::ScaPythonComputeAddIn()
{
    m_aLocale.Language = u"en"_ustr;
    m_aLocale.Country = u"US"_ustr;
}

ScaPythonComputeAddIn::~ScaPythonComputeAddIn() = default;

cpo::uno::Any SAL_CALL ScaPythonComputeAddIn::getPy(const OUString& aCode,
                                                    const cpo::uno::Sequence<cpo::uno::Any>& aData)
{
    if (aCode.isEmpty())
        throw lang::IllegalArgumentException(u"PY() requires non-empty code"_ustr, getXWeak(), 0);
    SAL_INFO("scaddins.pythoncompute",
             "getPy codeLen=" << aCode.getLength() << " dataArity=" << aData.getLength());
    rtl::Reference<PythonComputeVolatileResult> xVol = startCompute(aCode, aData);
    // Prefer XVolatileResult (unambiguous); WeakImplHelper has multiple XInterface bases.
    cpo::uno::Any aRet;
    aRet <<= uno::Reference<sheet::XVolatileResult>(xVol);
    return aRet;
}

cpo::uno::Any SAL_CALL ScaPythonComputeAddIn::getPython(
    const OUString& aCode, const cpo::uno::Sequence<cpo::uno::Any>& aData)
{
    return getPy(aCode, aData);
}

OUString SAL_CALL ScaPythonComputeAddIn::getServiceName() { return MY_SERVICE; }

OUString SAL_CALL ScaPythonComputeAddIn::getImplementationName()
{
    return u"org.collaboraoffice.sheet.addin.PythonComputeFunctionsImpl"_ustr;
}

bool SAL_CALL ScaPythonComputeAddIn::supportsService(const OUString& aServiceName)
{
    return cppu::supportsService(this, aServiceName);
}

cpo::uno::Sequence<OUString> SAL_CALL ScaPythonComputeAddIn::getSupportedServiceNames()
{
    return { ADDIN_SERVICE, MY_SERVICE };
}

void SAL_CALL ScaPythonComputeAddIn::setLocale(const lang::Locale& eLocale) { m_aLocale = eLocale; }

lang::Locale SAL_CALL ScaPythonComputeAddIn::getLocale() { return m_aLocale; }

OUString SAL_CALL ScaPythonComputeAddIn::getProgrammaticFuntionName(const OUString& aDisplayName)
{
    const OUString s = aDisplayName.toAsciiUpperCase();
    if (s == "PY")
        return u"getPy"_ustr;
    if (s == "PYTHON")
        return u"getPython"_ustr;
    return {};
}

OUString SAL_CALL ScaPythonComputeAddIn::getDisplayFunctionName(const OUString& aProgrammaticName)
{
    if (aProgrammaticName == "getPy")
        return u"PY"_ustr;
    if (aProgrammaticName == "getPython")
        return u"PYTHON"_ustr;
    return aProgrammaticName;
}

OUString SAL_CALL ScaPythonComputeAddIn::getFunctionDescription(const OUString& aProgrammaticName)
{
    if (aProgrammaticName == "getPy" || aProgrammaticName == "getPython")
        return u"Executes Python via the remote compute service (interim #BUSY!). "
               "Assign output to result."_ustr;
    return {};
}

OUString SAL_CALL ScaPythonComputeAddIn::getDisplayArgumentName(const OUString& aProgrammaticName,
                                                                sal_Int32 nArgument)
{
    if (aProgrammaticName != "getPy" && aProgrammaticName != "getPython")
        return {};
    switch (nArgument)
    {
        case 0:
            return u"code"_ustr;
        case 1:
            return u"data"_ustr;
        default:
            return {};
    }
}

OUString SAL_CALL ScaPythonComputeAddIn::getArgumentDescription(const OUString& aProgrammaticName,
                                                                sal_Int32 nArgument)
{
    if (aProgrammaticName != "getPy" && aProgrammaticName != "getPython")
        return {};
    switch (nArgument)
    {
        case 0:
            return u"Python source. Assign the return value to result."_ustr;
        case 1:
            return u"Optional cell range(s) injected as data."_ustr;
        default:
            return {};
    }
}

OUString SAL_CALL ScaPythonComputeAddIn::getProgrammaticCategoryName(const OUString&)
{
    return u"Add-In"_ustr;
}

OUString SAL_CALL ScaPythonComputeAddIn::getDisplayCategoryName(const OUString& aProgrammaticName)
{
    return getProgrammaticCategoryName(aProgrammaticName);
}

cpo::uno::Sequence<sheet::LocalizedName>
    SAL_CALL ScaPythonComputeAddIn::getCompatibilityNames(const OUString& aProgrammaticName)
{
    sheet::LocalizedName aName;
    aName.Locale = m_aLocale;
    if (aProgrammaticName == "getPy")
        aName.Name = u"PY"_ustr;
    else if (aProgrammaticName == "getPython")
        aName.Name = u"PYTHON"_ustr;
    else
        return {};
    return { aName };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
