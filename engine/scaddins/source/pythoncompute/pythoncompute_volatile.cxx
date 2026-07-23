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

#include "pythoncompute_volatile.hxx"

#include <cppuhelper/supportsservice.hxx>
#include <sal/log.hxx>
#include <vcl/svapp.hxx>
#include <algorithm>

using namespace css;
using namespace collaboraoffice::pythoncompute;

PythonComputeVolatileResult::PythonComputeVolatileResult()
{
    // Interim value shown in the formula cell until HTTP completes.
    m_aCurrent <<= u"#BUSY!"_ustr;
}

PythonComputeVolatileResult::~PythonComputeVolatileResult() = default;

void PythonComputeVolatileResult::finish(const cpo::uno::Any& aValue)
{
    {
        std::unique_lock aGuard(m_aMutex);
        if (m_bFinished)
        {
            SAL_INFO("scaddins.pythoncompute", "finish: already finished, ignoring");
            return;
        }
        m_bFinished = true;
        m_aCurrent = aValue;
    }

    // Kit Unipoll delivers pythoncomputeresult inside kitPoll with Solar released
    // (SolarMutexReleaser). Always re-acquire so ScAddInListener::modified →
    // TrackFormulas can invalidate tiles. Do not trust IsCurrentThread() alone
    // (m_bNoYieldLock false positives) or PostUserEvent (can sit undispatched).
    SolarMutexGuard aGuard;
    size_t nListeners = 0;
    {
        std::scoped_lock aLock(m_aMutex);
        nListeners = m_aListeners.size();
    }
    SAL_INFO("scaddins.pythoncompute",
             "finish: notify under SolarMutexGuard listeners=" << nListeners);
    notifyListeners(aValue);
}

void PythonComputeVolatileResult::notifyListeners(const cpo::uno::Any& aValue)
{
    std::vector<uno::Reference<sheet::XResultListener>> aCopy;
    {
        std::scoped_lock aGuard(m_aMutex);
        aCopy = m_aListeners;
    }
    sheet::ResultEvent aEvent;
    aEvent.Source = getXWeak();
    aEvent.Value = aValue;
    for (const auto& xLis : aCopy)
    {
        if (xLis.is())
            xLis->modified(aEvent);
    }
}

void SAL_CALL PythonComputeVolatileResult::addResultListener(
    const uno::Reference<sheet::XResultListener>& xListener)
{
    if (!xListener.is())
        return;
    bool bNeedImmediate = false;
    cpo::uno::Any aCurrent;
    {
        std::scoped_lock aGuard(m_aMutex);
        m_aListeners.push_back(xListener);
        aCurrent = m_aCurrent;
        // Always push current value (including "#BUSY!") so the cell paints interim text.
        bNeedImmediate = true;
    }
    if (bNeedImmediate)
    {
        // Same Solar requirement as finish(): ScAddInListener::modified → TrackFormulas.
        SolarMutexGuard aSolar;
        sheet::ResultEvent aEvent;
        aEvent.Source = getXWeak();
        aEvent.Value = aCurrent;
        xListener->modified(aEvent);
    }
}

void SAL_CALL PythonComputeVolatileResult::removeResultListener(
    const uno::Reference<sheet::XResultListener>& xListener)
{
    std::scoped_lock aGuard(m_aMutex);
    auto& v = m_aListeners;
    v.erase(std::remove_if(
                v.begin(), v.end(),
                [&](const uno::Reference<sheet::XResultListener>& x) { return x == xListener; }),
            v.end());
}

OUString SAL_CALL PythonComputeVolatileResult::getImplementationName()
{
    return u"org.collaboraoffice.sheet.addin.PythonComputeVolatileResult"_ustr;
}

bool SAL_CALL PythonComputeVolatileResult::supportsService(const OUString& ServiceName)
{
    return cppu::supportsService(this, ServiceName);
}

cpo::uno::Sequence<OUString> SAL_CALL PythonComputeVolatileResult::getSupportedServiceNames()
{
    return { u"com.sun.star.sheet.VolatileResult"_ustr };
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
