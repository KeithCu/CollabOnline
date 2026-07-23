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

#include "pythoncompute_bridge.h"
#include "pythoncompute_anyjson.hxx"
#include "pythoncompute_volatile.hxx"

#include <rtl/ref.hxx>
#include <rtl/string.hxx>
#include <rtl/ustring.hxx>
#include <sal/log.hxx>
#include <tools/link.hxx>
#include <unotools/weakref.hxx>
#include <vcl/svapp.hxx>
#include <vcl/timer.hxx>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace css;
using namespace collaboraoffice::pythoncompute;

namespace
{
struct PendingEntry
{
    rtl::Reference<PythonComputeVolatileResult> xVolatile;
    std::chrono::steady_clock::time_point aDeadline;
};

// Process-wide param→volatile identity map (AddIn.idl: "same params must return
// the same object"). Entries are weak references so a formula cell dropping its
// XVolatileResult lets the entry lapse without us pinning kit memory. We never
// evict a still-live entry — that would break identity while the cell listens
// and force a duplicate HTTP emit on the next recalc. kParamCacheSoftCap only
// triggers a prune of lapsed weaks; if every entry is still live the map is
// allowed to grow past it (mirrors ScAddInAsync, whose live set is bounded by
// listeners, not a fixed cap). Cleared wholesale on kit teardown
// (pythoncompute_clear_caches) for memory hygiene, not identity.
constexpr size_t kParamCacheSoftCap = 256;
constexpr sal_Int32 kDefaultTimeoutMs = 90000;

using ParamCacheMap
    = std::unordered_map<std::string, unotools::WeakReference<PythonComputeVolatileResult>>;

std::mutex g_aMutex;
pythoncompute_emit_fn g_pEmit = nullptr;
void* g_pEmitUser = nullptr;
std::unordered_map<std::string, PendingEntry> g_aPending;
ParamCacheMap g_aParamCache;
std::atomic<sal_uInt64> g_nRequestSeq{ 0 };
std::atomic<sal_Int32> g_nPendingTimeoutMs{ kDefaultTimeoutMs };

/** Stable param key from code+data (fixed id so emits differ by request id only). */
std::string makeParamCacheKey(const OUString& sCode, const cpo::uno::Sequence<cpo::uno::Any>& aData)
{
    return buildExecuteRequestJson(u"_"_ustr, sCode, aData);
}

void expireStale_NoLock(const std::chrono::steady_clock::time_point& now,
                        std::vector<rtl::Reference<PythonComputeVolatileResult>>& rToFinish)
{
    for (auto it = g_aPending.begin(); it != g_aPending.end();)
    {
        if (now > it->second.aDeadline)
        {
            if (it->second.xVolatile.is())
            {
                SAL_INFO("scaddins.pythoncompute", "pending timeout -> #N/A");
                rToFinish.push_back(it->second.xVolatile);
            }
            it = g_aPending.erase(it);
        }
        else
            ++it;
    }
}

void finishExpired(std::vector<rtl::Reference<PythonComputeVolatileResult>>& rToFinish)
{
    for (auto& xVol : rToFinish)
    {
        if (xVol.is())
            xVol->finish(makeFormulaErrorAny(FormulaError::NotAvailable));
    }
    rToFinish.clear();
}

std::optional<std::chrono::steady_clock::time_point> earliestDeadline_NoLock()
{
    std::optional<std::chrono::steady_clock::time_point> aEarliest;
    for (const auto& e : g_aPending)
    {
        if (!aEarliest || e.second.aDeadline < *aEarliest)
            aEarliest = e.second.aDeadline;
    }
    return aEarliest;
}

sal_uInt64 remainingTimeoutMs(const std::chrono::steady_clock::time_point& aDeadline,
                              const std::chrono::steady_clock::time_point& now)
{
    if (aDeadline <= now)
        return 1;
    const auto nMs = std::chrono::duration_cast<std::chrono::milliseconds>(aDeadline - now).count();
    return nMs < 1 ? 1 : static_cast<sal_uInt64>(nMs);
}

/** One-shot VCL timer: active pending expiry without waiting for recalc/complete.
 *  Same pattern as sc SharedStringPoolPurge / ScTemporaryChartLock — Unipoll
 *  feeds SalTimer into kitPoll so Invoke runs under Solar on an idle sheet. */
class PendingTimeoutTimer final
{
public:
    PendingTimeoutTimer()
        : m_aTimer("pythoncompute pending timeout")
    {
        m_aTimer.SetInvokeHandler(LINK(this, PendingTimeoutTimer, TimeoutHdl));
    }

    void stop() { m_aTimer.Stop(); }

    void armTo(const std::chrono::steady_clock::time_point& aDeadline)
    {
        const auto now = std::chrono::steady_clock::now();
        m_aTimer.SetTimeout(remainingTimeoutMs(aDeadline, now));
        m_aTimer.Start();
    }

private:
    DECL_LINK(TimeoutHdl, Timer*, void);

    Timer m_aTimer;
};

std::unique_ptr<PendingTimeoutTimer> g_xPendingTimer;

PendingTimeoutTimer& ensurePendingTimer()
{
    if (!g_xPendingTimer)
        g_xPendingTimer = std::make_unique<PendingTimeoutTimer>();
    return *g_xPendingTimer;
}

/** Re-arm or stop the one-shot timer from the current pending map. */
void schedulePendingTimeout()
{
    std::optional<std::chrono::steady_clock::time_point> aEarliest;
    {
        std::scoped_lock aGuard(g_aMutex);
        aEarliest = earliestDeadline_NoLock();
    }

    SolarMutexGuard aSolar;
    PendingTimeoutTimer& rTimer = ensurePendingTimer();
    if (!aEarliest)
        rTimer.stop();
    else
        rTimer.armTo(*aEarliest);
}

IMPL_LINK_NOARG(PendingTimeoutTimer, TimeoutHdl, Timer*, void)
{
    std::vector<rtl::Reference<PythonComputeVolatileResult>> aExpired;
    {
        std::scoped_lock aGuard(g_aMutex);
        expireStale_NoLock(std::chrono::steady_clock::now(), aExpired);
    }
    // finish() may re-enter AddIn via Calc listeners — never hold g_aMutex.
    finishExpired(aExpired);
    // Re-read pending after finish() (listeners may startCompute); arm or stop.
    schedulePendingTimeout();
}

/** Resolve a live volatile for key, dropping the entry if its weak has lapsed. */
rtl::Reference<PythonComputeVolatileResult> paramCacheLookup_NoLock(const std::string& key)
{
    auto it = g_aParamCache.find(key);
    if (it == g_aParamCache.end())
        return {};
    rtl::Reference<PythonComputeVolatileResult> xVol = it->second.get();
    if (!xVol.is())
    {
        g_aParamCache.erase(it); // weak lapsed — no cell holds it anymore
        return {};
    }
    return xVol;
}

/** Drop entries whose weak reference has lapsed (no live XVolatileResult). */
void paramCachePruneDead_NoLock()
{
    for (auto it = g_aParamCache.begin(); it != g_aParamCache.end();)
    {
        if (it->second.get().is())
            ++it;
        else
            it = g_aParamCache.erase(it);
    }
}

void paramCachePut_NoLock(const std::string& key,
                          const rtl::Reference<PythonComputeVolatileResult>& xVol)
{
    // Overwrite any existing (possibly lapsed) entry for this key.
    g_aParamCache.erase(key);
    // Only prune lapsed weaks at the soft cap; never evict a live entry (a
    // formula cell may still hold it — AddIn.idl identity). Grow past the cap if
    // every entry is still live rather than break "same params → same object".
    if (g_aParamCache.size() >= kParamCacheSoftCap)
    {
        paramCachePruneDead_NoLock();
        if (g_aParamCache.size() >= kParamCacheSoftCap)
            SAL_INFO("scaddins.pythoncompute", "paramCache: " << g_aParamCache.size()
                                                              << " live entries above soft cap "
                                                              << kParamCacheSoftCap);
    }
    g_aParamCache.emplace(key, unotools::WeakReference<PythonComputeVolatileResult>(xVol));
}

void paramCacheClear_NoLock() { g_aParamCache.clear(); }
} // namespace

extern "C" SAL_DLLPUBLIC_EXPORT void pythoncompute_set_emitter(pythoncompute_emit_fn fn,
                                                               void* userdata)
{
    std::scoped_lock aGuard(g_aMutex);
    g_pEmit = fn;
    g_pEmitUser = userdata;
    SAL_INFO("scaddins.pythoncompute", "emitter " << (fn ? "installed" : "cleared"));
}

extern "C" SAL_DLLPUBLIC_EXPORT void
pythoncompute_set_pending_timeout_ms_for_tests(sal_Int32 timeoutMs)
{
    if (timeoutMs < 1)
        timeoutMs = 1;
    g_nPendingTimeoutMs.store(timeoutMs);
}

extern "C" SAL_DLLPUBLIC_EXPORT void pythoncompute_clear_caches()
{
    {
        std::scoped_lock aGuard(g_aMutex);
        g_aPending.clear();
        paramCacheClear_NoLock();
    }
    // Timer touches VCL — take Solar sequentially (never nested under g_aMutex,
    // and callers must not hold a mutex that Calc grabs under Solar).
    SolarMutexGuard aSolar;
    if (g_xPendingTimer)
        g_xPendingTimer->stop();
}

extern "C" SAL_DLLPUBLIC_EXPORT void pythoncompute_reset_for_tests()
{
    pythoncompute_clear_caches();
    g_nPendingTimeoutMs.store(kDefaultTimeoutMs);
}

extern "C" SAL_DLLPUBLIC_EXPORT int pythoncompute_complete_json(const char* jsonUtf8, int32_t len)
{
    if (!jsonUtf8 || len <= 0)
    {
        SAL_INFO("scaddins.pythoncompute", "complete_json: empty body len=" << len);
        return 0;
    }
    const std::string_view json(jsonUtf8, static_cast<size_t>(len));

    OUString sId;
    if (!extractRequestIdFromJson(json, sId) || sId.isEmpty())
    {
        SAL_INFO("scaddins.pythoncompute", "complete_json: missing id bytes=" << len);
        return 0;
    }

    const std::string idKey = std::string(OUStringToOString(sId, RTL_TEXTENCODING_UTF8));

    rtl::Reference<PythonComputeVolatileResult> xVol;
    std::vector<rtl::Reference<PythonComputeVolatileResult>> aExpired;
    bool bFound = false;
    {
        std::scoped_lock aGuard(g_aMutex);
        expireStale_NoLock(std::chrono::steady_clock::now(), aExpired);
        auto it = g_aPending.find(idKey);
        if (it == g_aPending.end())
        {
            SAL_INFO("scaddins.pythoncompute",
                     "complete_json: no pending id=[" << idKey << "] bytes=" << len);
        }
        else
        {
            xVol = it->second.xVolatile;
            g_aPending.erase(it);
            bFound = true;
        }
    }
    // finish() may re-enter AddIn via Calc listeners — never hold g_aMutex.
    finishExpired(aExpired);
    schedulePendingTimeout();
    if (!bFound)
        return 0;
    if (!xVol.is())
    {
        SAL_INFO("scaddins.pythoncompute", "complete_json: null volatile id=[" << idKey << ']');
        return 0;
    }

    cpo::uno::Any aResult;
    OUString sErr;
    jsonResultToAny(json, aResult, sErr);
    if (!sErr.isEmpty())
        SAL_INFO("scaddins.pythoncompute",
                 "complete_json: detail=[" << sErr << "] id=[" << idKey << ']');
    SAL_INFO("scaddins.pythoncompute",
             "complete_json: finishing id=[" << idKey << "] bytes=" << len);
    xVol->finish(aResult);
    return 1;
}

namespace collaboraoffice::pythoncompute
{
OUString makeRequestId()
{
    const sal_uInt64 n = ++g_nRequestSeq;
    return "py-" + OUString::number(static_cast<sal_Int64>(n));
}

rtl::Reference<PythonComputeVolatileResult>
startCompute(const OUString& sCode, const cpo::uno::Sequence<cpo::uno::Any>& aData)
{
    const std::string paramKey = makeParamCacheKey(sCode, aData);

    pythoncompute_emit_fn pEmit = nullptr;
    void* pUser = nullptr;
    rtl::Reference<PythonComputeVolatileResult> xVol;
    std::vector<rtl::Reference<PythonComputeVolatileResult>> aExpired;
    bool bCacheHit = false;
    {
        std::scoped_lock aGuard(g_aMutex);
        expireStale_NoLock(std::chrono::steady_clock::now(), aExpired);
        xVol = paramCacheLookup_NoLock(paramKey);
        if (xVol.is())
        {
            SAL_INFO("scaddins.pythoncompute",
                     "startCompute: cache hit codeLen=" << sCode.getLength()
                                                        << " dataArity=" << aData.getLength());
            bCacheHit = true;
        }
        else
        {
            pEmit = g_pEmit;
            pUser = g_pEmitUser;
            xVol = rtl::Reference<PythonComputeVolatileResult>(new PythonComputeVolatileResult);
            paramCachePut_NoLock(paramKey, xVol);
        }
    }
    // finish() may re-enter AddIn via Calc listeners — never hold g_aMutex.
    finishExpired(aExpired);
    if (!aExpired.empty())
        schedulePendingTimeout();
    if (bCacheHit)
        return xVol;

    if (!pEmit)
    {
        SAL_INFO("scaddins.pythoncompute", "startCompute: no emitter -> unavailable (#N/A)");
        xVol->finish(makeFormulaErrorAny(FormulaError::NotAvailable));
        return xVol;
    }

    const OUString sId = makeRequestId();
    const std::string json = buildExecuteRequestJson(sId, sCode, aData);
    const std::string idKey = std::string(OUStringToOString(sId, RTL_TEXTENCODING_UTF8));

    rtl::Reference<PythonComputeVolatileResult> xDisplaced;
    std::vector<rtl::Reference<PythonComputeVolatileResult>> aExpired2;
    const auto nTimeoutMs = std::chrono::milliseconds(g_nPendingTimeoutMs.load());
    {
        std::scoped_lock aGuard(g_aMutex);
        expireStale_NoLock(std::chrono::steady_clock::now(), aExpired2);
        auto it = g_aPending.find(idKey);
        if (it != g_aPending.end())
        {
            xDisplaced = it->second.xVolatile;
            g_aPending.erase(it);
        }
        PendingEntry e;
        e.xVolatile = xVol;
        e.aDeadline = std::chrono::steady_clock::now() + nTimeoutMs;
        g_aPending[idKey] = std::move(e);
    }
    finishExpired(aExpired2);
    if (xDisplaced.is())
    {
        SAL_INFO("scaddins.pythoncompute", "startCompute: superseded pending -> #N/A");
        xDisplaced->finish(makeFormulaErrorAny(FormulaError::NotAvailable));
    }
    schedulePendingTimeout();

    SAL_INFO("scaddins.pythoncompute", "startCompute: emit id=["
                                           << idKey << "] bytes=" << json.size()
                                           << " codeLen=" << sCode.getLength());
    pEmit(pUser, json.data(), static_cast<int32_t>(json.size()));
    return xVol;
}
} // namespace collaboraoffice::pythoncompute

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
