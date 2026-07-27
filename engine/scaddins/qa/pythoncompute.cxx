/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
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

#include <test/bootstrapfixture.hxx>

#include <com/sun/star/lang/IllegalArgumentException.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/sheet/ResultEvent.hpp>
#include <com/sun/star/sheet/XAddIn.hpp>
#include <com/sun/star/sheet/XResultListener.hpp>
#include <com/sun/star/sheet/XVolatileResult.hpp>
#include <org/collaboraoffice/sheet/addin/XPythonComputeFunctions.hpp>
#include <com/sun/star/uno/Reference.hxx>
#include <cpo/uno/TypeClass.hpp>
#include <com/sun/star/uno/XInterface.hpp>

#include <comphelper/processfactory.hxx>
#include <cppuhelper/implbase.hxx>
#include <formula/errorcodes.hxx>
#include <rtl/ref.hxx>
#include <vcl/scheduler.hxx>

#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pythoncompute_anyjson.hxx>
#include <pythoncompute_bridge.h>

using namespace css;

namespace
{
class CapturingResultListener final : public cppu::WeakImplHelper<sheet::XResultListener>
{
public:
    void SAL_CALL modified(const sheet::ResultEvent& aEvent) override
    {
        std::scoped_lock aGuard(m_aMutex);
        m_aValues.push_back(aEvent.Value);
    }

    void SAL_CALL disposing(const lang::EventObject&) override {}

    sal_Int32 count() const
    {
        std::scoped_lock aGuard(m_aMutex);
        return static_cast<sal_Int32>(m_aValues.size());
    }

    cpo::uno::Any at(sal_Int32 nIndex) const
    {
        std::scoped_lock aGuard(m_aMutex);
        CPPUNIT_ASSERT(nIndex >= 0 && static_cast<size_t>(nIndex) < m_aValues.size());
        return m_aValues[static_cast<size_t>(nIndex)];
    }

    cpo::uno::Any last() const
    {
        std::scoped_lock aGuard(m_aMutex);
        CPPUNIT_ASSERT(!m_aValues.empty());
        return m_aValues.back();
    }

private:
    mutable std::mutex m_aMutex;
    std::vector<cpo::uno::Any> m_aValues;
};

std::mutex g_aEmitMutex;
std::string g_aLastEmitJson;

void testEmitThunk(void* /*userdata*/, const char* jsonUtf8, int32_t len)
{
    std::scoped_lock aGuard(g_aEmitMutex);
    if (!jsonUtf8 || len <= 0)
        g_aLastEmitJson.clear();
    else
        g_aLastEmitJson.assign(jsonUtf8, static_cast<size_t>(len));
}

std::string takeLastEmitJson()
{
    std::scoped_lock aGuard(g_aEmitMutex);
    std::string s = g_aLastEmitJson;
    g_aLastEmitJson.clear();
    return s;
}

OUString extractJsonStringField(std::string_view json, std::string_view key)
{
    const std::string pattern = "\"" + std::string(key) + "\"";
    size_t p = json.find(pattern);
    CPPUNIT_ASSERT(p != std::string_view::npos);
    p = json.find(':', p + pattern.size());
    CPPUNIT_ASSERT(p != std::string_view::npos);
    p = json.find('"', p + 1);
    CPPUNIT_ASSERT(p != std::string_view::npos);
    size_t q = json.find('"', p + 1);
    while (q != std::string_view::npos && json[q - 1] == '\\')
        q = json.find('"', q + 1);
    CPPUNIT_ASSERT(q != std::string_view::npos);
    return OUString::fromUtf8(json.substr(p + 1, q - p - 1));
}

class Test : public test::BootstrapFixture
{
public:
    virtual void setUp() override;
    virtual void tearDown() override;

protected:
    css::uno::Reference<org::collaboraoffice::sheet::addin::XPythonComputeFunctions> mxPy;
    css::uno::Reference<css::sheet::XAddIn> mxAddIn;
};

void Test::setUp()
{
    test::BootstrapFixture::setUp();
    pythoncompute_reset_for_tests();
    pythoncompute_set_emitter(nullptr, nullptr);
    takeLastEmitJson();
    auto xFactory(comphelper::getProcessServiceFactory());
    auto xInst
        = xFactory->createInstance(u"org.collaboraoffice.sheet.addin.PythonComputeFunctions"_ustr);
    mxPy.set(xInst, css::uno::UNO_QUERY_THROW);
    mxAddIn.set(xInst, css::uno::UNO_QUERY_THROW);
}

void Test::tearDown()
{
    pythoncompute_set_emitter(nullptr, nullptr);
    pythoncompute_reset_for_tests();
    takeLastEmitJson();
    mxAddIn.clear();
    mxPy.clear();
    test::BootstrapFixture::tearDown();
}

CPPUNIT_TEST_FIXTURE(Test, test_emptyCodeThrows)
{
    CPPUNIT_ASSERT_THROW(mxPy->getPy(u""_ustr, {}), lang::IllegalArgumentException);
    CPPUNIT_ASSERT_THROW(mxPy->getPython(u""_ustr, {}), lang::IllegalArgumentException);
}

CPPUNIT_TEST_FIXTURE(Test, test_displayNames)
{
    CPPUNIT_ASSERT_EQUAL(u"getPy"_ustr, mxAddIn->getProgrammaticFuntionName(u"PY"_ustr));
    CPPUNIT_ASSERT_EQUAL(u"getPython"_ustr, mxAddIn->getProgrammaticFuntionName(u"PYTHON"_ustr));
    CPPUNIT_ASSERT_EQUAL(u"PY"_ustr, mxAddIn->getDisplayFunctionName(u"getPy"_ustr));
    CPPUNIT_ASSERT_EQUAL(u"PYTHON"_ustr, mxAddIn->getDisplayFunctionName(u"getPython"_ustr));
}

CPPUNIT_TEST_FIXTURE(Test, test_getPyReturnsVolatile)
{
    cpo::uno::Any aRet = mxPy->getPy(u"result=1"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol;
    CPPUNIT_ASSERT(aRet >>= xVol);
    CPPUNIT_ASSERT(xVol.is());

    cpo::uno::Any aRet2 = mxPy->getPython(u"result=1"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol2;
    CPPUNIT_ASSERT(aRet2 >>= xVol2);
    CPPUNIT_ASSERT(xVol2.is());
}

CPPUNIT_TEST_FIXTURE(Test, test_noEmitterUnavailable)
{
    pythoncompute_set_emitter(nullptr, nullptr);

    cpo::uno::Any aRet = mxPy->getPy(u"result=1"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol;
    CPPUNIT_ASSERT(aRet >>= xVol);

    rtl::Reference<CapturingResultListener> xLis(new CapturingResultListener);
    xVol->addResultListener(xLis);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), xLis->count());

    // void Any → #N/A in ScUnoAddInCall::SetResult
    CPPUNIT_ASSERT_EQUAL(cpo::uno::TypeClass_VOID, xLis->last().getValueTypeClass());
}

CPPUNIT_TEST_FIXTURE(Test, test_emitterCompleteRoundTrip)
{
    pythoncompute_set_emitter(testEmitThunk, nullptr);

    cpo::uno::Any aRet = mxPy->getPy(u"result=1+1"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol;
    CPPUNIT_ASSERT(aRet >>= xVol);

    rtl::Reference<CapturingResultListener> xLis(new CapturingResultListener);
    xVol->addResultListener(xLis);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), xLis->count());
    OUString sBusy;
    CPPUNIT_ASSERT(xLis->at(0) >>= sBusy);
    CPPUNIT_ASSERT_EQUAL(u"#BUSY!"_ustr, sBusy);

    const std::string emitted = takeLastEmitJson();
    CPPUNIT_ASSERT(!emitted.empty());
    CPPUNIT_ASSERT(emitted.find("\"mode\": \"isolated\"") != std::string::npos
                   || emitted.find("\"mode\":\"isolated\"") != std::string::npos);
    CPPUNIT_ASSERT_EQUAL(u"result=1+1"_ustr, extractJsonStringField(emitted, "code"));
    const OUString sId = extractJsonStringField(emitted, "id");
    CPPUNIT_ASSERT(!sId.isEmpty());

    const std::string badComplete = "{\"id\":\"not-this-id\",\"status\":\"ok\",\"result\":2}";
    CPPUNIT_ASSERT_EQUAL(0, pythoncompute_complete_json(
                                badComplete.data(), static_cast<sal_Int32>(badComplete.size())));

    const OString aIdUtf8 = OUStringToOString(sId, RTL_TEXTENCODING_UTF8);
    const std::string okComplete
        = std::string("{\"id\":\"") + aIdUtf8.getStr() + "\",\"status\":\"ok\",\"result\":2}";
    CPPUNIT_ASSERT_EQUAL(1, pythoncompute_complete_json(okComplete.data(),
                                                        static_cast<sal_Int32>(okComplete.size())));

    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), xLis->count());
    double f = 0.0;
    CPPUNIT_ASSERT(xLis->last() >>= f);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(2.0, f, 1e-12);

    CPPUNIT_ASSERT_EQUAL(0, pythoncompute_complete_json(okComplete.data(),
                                                        static_cast<sal_Int32>(okComplete.size())));
}

CPPUNIT_TEST_FIXTURE(Test, test_emitterCompleteServiceError)
{
    pythoncompute_set_emitter(testEmitThunk, nullptr);

    cpo::uno::Any aRet = mxPy->getPy(u"result=err"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol;
    CPPUNIT_ASSERT(aRet >>= xVol);

    rtl::Reference<CapturingResultListener> xLis(new CapturingResultListener);
    xVol->addResultListener(xLis);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), xLis->count());
    OUString sBusy;
    CPPUNIT_ASSERT(xLis->at(0) >>= sBusy);
    CPPUNIT_ASSERT_EQUAL(u"#BUSY!"_ustr, sBusy);

    const std::string emitted = takeLastEmitJson();
    const OUString sId = extractJsonStringField(emitted, "id");
    CPPUNIT_ASSERT(!sId.isEmpty());
    const OString aIdUtf8 = OUStringToOString(sId, RTL_TEXTENCODING_UTF8);
    const std::string errComplete = std::string("{\"id\":\"") + aIdUtf8.getStr()
                                    + "\",\"status\":\"error\",\"error\":\"boom\"}";
    CPPUNIT_ASSERT_EQUAL(1, pythoncompute_complete_json(
                                errComplete.data(), static_cast<sal_Int32>(errComplete.size())));

    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), xLis->count());
    double fErr = 0.0;
    CPPUNIT_ASSERT(xLis->last() >>= fErr);
    CPPUNIT_ASSERT_EQUAL(FormulaError::NoValue, GetDoubleErrorValue(fErr));
}

CPPUNIT_TEST_FIXTURE(Test, test_pendingTimeoutWithoutRecalc)
{
    // G6a: pending expiry must not require another startCompute / complete_json.
    pythoncompute_set_emitter(testEmitThunk, nullptr);
    pythoncompute_set_pending_timeout_ms_for_tests(50);

    cpo::uno::Any aRet = mxPy->getPy(u"result=timeout_idle"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol;
    CPPUNIT_ASSERT(aRet >>= xVol);

    rtl::Reference<CapturingResultListener> xLis(new CapturingResultListener);
    xVol->addResultListener(xLis);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), xLis->count());
    OUString sBusy;
    CPPUNIT_ASSERT(xLis->at(0) >>= sBusy);
    CPPUNIT_ASSERT_EQUAL(u"#BUSY!"_ustr, sBusy);

    const std::string emitted = takeLastEmitJson();
    CPPUNIT_ASSERT(!emitted.empty());
    const OUString sId = extractJsonStringField(emitted, "id");
    CPPUNIT_ASSERT(!sId.isEmpty());

    // Wait past the one-shot VCL timer deadline, then pump the scheduler.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    for (int i = 0; i < 20 && xLis->count() < 2; ++i)
    {
        Scheduler::ProcessEventsToIdle();
        if (xLis->count() < 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), xLis->count());
    CPPUNIT_ASSERT_EQUAL(cpo::uno::TypeClass_VOID, xLis->last().getValueTypeClass());

    const OString aIdUtf8 = OUStringToOString(sId, RTL_TEXTENCODING_UTF8);
    const std::string lateComplete
        = std::string("{\"id\":\"") + aIdUtf8.getStr() + "\",\"status\":\"ok\",\"result\":1}";
    CPPUNIT_ASSERT_EQUAL(0, pythoncompute_complete_json(
                                lateComplete.data(), static_cast<sal_Int32>(lateComplete.size())));
}

CPPUNIT_TEST_FIXTURE(Test, test_requestIdsUnique)
{
    pythoncompute_set_emitter(testEmitThunk, nullptr);

    std::set<OUString> aIds;
    constexpr int nCount = 64;
    for (int i = 0; i < nCount; ++i)
    {
        // Distinct code → distinct cache keys so each call still emits.
        const OUString sCode = "result=" + OUString::number(static_cast<sal_Int32>(i));
        cpo::uno::Any aRet = mxPy->getPy(sCode, {});
        uno::Reference<sheet::XVolatileResult> xVol;
        CPPUNIT_ASSERT(aRet >>= xVol);
        CPPUNIT_ASSERT(xVol.is());

        const std::string emitted = takeLastEmitJson();
        CPPUNIT_ASSERT(!emitted.empty());
        const OUString sId = extractJsonStringField(emitted, "id");
        CPPUNIT_ASSERT(!sId.isEmpty());
        CPPUNIT_ASSERT_MESSAGE("duplicate request id", aIds.insert(sId).second);
    }
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(nCount), aIds.size());
}

CPPUNIT_TEST_FIXTURE(Test, test_volatileIdentitySameArgs)
{
    pythoncompute_set_emitter(testEmitThunk, nullptr);

    cpo::uno::Any aRet1 = mxPy->getPy(u"result=identity"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol1;
    CPPUNIT_ASSERT(aRet1 >>= xVol1);
    CPPUNIT_ASSERT(xVol1.is());
    const std::string emit1 = takeLastEmitJson();
    CPPUNIT_ASSERT(!emit1.empty());

    cpo::uno::Any aRet2 = mxPy->getPy(u"result=identity"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol2;
    CPPUNIT_ASSERT(aRet2 >>= xVol2);
    CPPUNIT_ASSERT(xVol2.is());
    CPPUNIT_ASSERT_EQUAL(xVol1.get(), xVol2.get());
    CPPUNIT_ASSERT(takeLastEmitJson().empty());

    cpo::uno::Sequence<cpo::uno::Any> aData{ cpo::uno::Any(sal_Int32(7)) };
    cpo::uno::Any aRet3 = mxPy->getPy(u"result=identity"_ustr, aData);
    uno::Reference<sheet::XVolatileResult> xVol3;
    CPPUNIT_ASSERT(aRet3 >>= xVol3);
    CPPUNIT_ASSERT(xVol3.is());
    CPPUNIT_ASSERT(xVol3.get() != xVol1.get());
    CPPUNIT_ASSERT(!takeLastEmitJson().empty());
}

CPPUNIT_TEST_FIXTURE(Test, test_paramCacheIdentityUnderPressure)
{
    // G6c: the weak-ref param cache must not evict a still-live entry. Hold more
    // live volatiles than the soft cap (256) — the old hard LRU would have
    // dropped the earliest and re-emitted on the next recalc, breaking
    // AddIn.idl "same params -> same object".
    pythoncompute_set_emitter(testEmitThunk, nullptr);

    constexpr int nKeys = 257;
    std::vector<uno::Reference<sheet::XVolatileResult>> aHeld;
    aHeld.reserve(nKeys);
    uno::Reference<sheet::XVolatileResult> xFirst;
    for (int i = 0; i < nKeys; ++i)
    {
        const OUString sCode = "result=pressure_" + OUString::number(static_cast<sal_Int32>(i));
        cpo::uno::Any aRet = mxPy->getPy(sCode, {});
        uno::Reference<sheet::XVolatileResult> xVol;
        CPPUNIT_ASSERT(aRet >>= xVol);
        CPPUNIT_ASSERT(xVol.is());
        // Distinct args -> a fresh emit each time (no cache hit).
        CPPUNIT_ASSERT(!takeLastEmitJson().empty());
        if (i == 0)
            xFirst = xVol;
        aHeld.push_back(xVol);
    }

    // The earliest key is still referenced: same object, and no duplicate emit.
    cpo::uno::Any aRet0 = mxPy->getPy(u"result=pressure_0"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol0;
    CPPUNIT_ASSERT(aRet0 >>= xVol0);
    CPPUNIT_ASSERT_EQUAL(xFirst.get(), xVol0.get());
    CPPUNIT_ASSERT(takeLastEmitJson().empty());
}

CPPUNIT_TEST_FIXTURE(Test, test_paramCacheDropsLapsedWeak)
{
    // Once the request has finished and no cell holds the volatile, the weak
    // lapses and the same params allocate a fresh object + re-emit.
    pythoncompute_set_emitter(testEmitThunk, nullptr);

    {
        cpo::uno::Any aRet = mxPy->getPy(u"result=lapse"_ustr, {});
        uno::Reference<sheet::XVolatileResult> xVol;
        CPPUNIT_ASSERT(aRet >>= xVol);
        CPPUNIT_ASSERT(xVol.is());
        const std::string emitted = takeLastEmitJson();
        CPPUNIT_ASSERT(!emitted.empty());

        // Complete the request so the pending map drops its strong reference;
        // an in-flight #BUSY! volatile is deliberately kept alive until then.
        const OUString sId = extractJsonStringField(emitted, "id");
        const OString aIdUtf8 = OUStringToOString(sId, RTL_TEXTENCODING_UTF8);
        const std::string okComplete
            = std::string("{\"id\":\"") + aIdUtf8.getStr() + "\",\"status\":\"ok\",\"result\":1}";
        CPPUNIT_ASSERT_EQUAL(1, pythoncompute_complete_json(
                                    okComplete.data(), static_cast<sal_Int32>(okComplete.size())));
    }

    // Cell reference dropped + request finished -> weak lapses; same params
    // miss the cache and re-emit a fresh request.
    cpo::uno::Any aRet2 = mxPy->getPy(u"result=lapse"_ustr, {});
    uno::Reference<sheet::XVolatileResult> xVol2;
    CPPUNIT_ASSERT(aRet2 >>= xVol2);
    CPPUNIT_ASSERT(xVol2.is());
    CPPUNIT_ASSERT(!takeLastEmitJson().empty());
}

CPPUNIT_TEST_FIXTURE(Test, test_anyToJsonFragment_scalars)
{
    using collaboraoffice::pythoncompute::anyToJsonFragment;

    CPPUNIT_ASSERT_EQUAL(u"null"_ustr, anyToJsonFragment(cpo::uno::Any()));
    CPPUNIT_ASSERT_EQUAL(u"true"_ustr, anyToJsonFragment(cpo::uno::Any(true)));
    CPPUNIT_ASSERT_EQUAL(u"false"_ustr, anyToJsonFragment(cpo::uno::Any(false)));
    CPPUNIT_ASSERT_EQUAL(u"42"_ustr, anyToJsonFragment(cpo::uno::Any(sal_Int32(42))));
    CPPUNIT_ASSERT_EQUAL(u"\"hi\""_ustr, anyToJsonFragment(cpo::uno::Any(u"hi"_ustr)));
    CPPUNIT_ASSERT_EQUAL(u"\"\""_ustr, anyToJsonFragment(cpo::uno::Any(u""_ustr)));
}

CPPUNIT_TEST_FIXTURE(Test, test_anyToJsonFragment_escapesAndNonFinite)
{
    using collaboraoffice::pythoncompute::anyToJsonFragment;

    CPPUNIT_ASSERT_EQUAL(u"\"a\\\"b\""_ustr, anyToJsonFragment(cpo::uno::Any(u"a\"b"_ustr)));
    CPPUNIT_ASSERT_EQUAL(u"\"a\\nb\""_ustr, anyToJsonFragment(cpo::uno::Any(u"a\nb"_ustr)));
    CPPUNIT_ASSERT_EQUAL(u"\"\\u0001\""_ustr,
                         anyToJsonFragment(cpo::uno::Any(OUString(sal_Unicode(1)))));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CPPUNIT_ASSERT_EQUAL(u"null"_ustr, anyToJsonFragment(cpo::uno::Any(nan)));
    CPPUNIT_ASSERT_EQUAL(u"null"_ustr, anyToJsonFragment(cpo::uno::Any(inf)));
    CPPUNIT_ASSERT_EQUAL(u"null"_ustr, anyToJsonFragment(cpo::uno::Any(-inf)));
}

CPPUNIT_TEST_FIXTURE(Test, test_anyToJsonFragment_grids)
{
    using collaboraoffice::pythoncompute::anyToJsonFragment;

    cpo::uno::Sequence<cpo::uno::Any> aFlat{ cpo::uno::Any(1.0), cpo::uno::Any(2.0),
                                             cpo::uno::Any(3.0) };
    // tools::JsonWriter inserts spaces after [ and commas.
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, anyToJsonFragment(cpo::uno::Any(aFlat)));

    // Typed matrix / vector paths — 1×N / N×1 flatten like Any grids / Classic.
    cpo::uno::Sequence<double> aDRow{ 1.0, 2.0, 3.0 };
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, anyToJsonFragment(cpo::uno::Any(aDRow)));

    cpo::uno::Sequence<cpo::uno::Sequence<double>> aDRowGrid{
        cpo::uno::Sequence<double>{ 1.0, 2.0, 3.0 },
    };
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, anyToJsonFragment(cpo::uno::Any(aDRowGrid)));

    cpo::uno::Sequence<cpo::uno::Sequence<double>> aDColGrid{
        cpo::uno::Sequence<double>{ 1.0 },
        cpo::uno::Sequence<double>{ 2.0 },
        cpo::uno::Sequence<double>{ 3.0 },
    };
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, anyToJsonFragment(cpo::uno::Any(aDColGrid)));

    cpo::uno::Sequence<cpo::uno::Sequence<double>> aDGrid{
        cpo::uno::Sequence<double>{ 1.0, 2.0 },
        cpo::uno::Sequence<double>{ 3.0, 4.0 },
    };
    CPPUNIT_ASSERT_EQUAL(u"[ [ 1, 2], [ 3, 4]]"_ustr, anyToJsonFragment(cpo::uno::Any(aDGrid)));

    // Calc ranges are Sequence<Sequence<Any>> — must not emit null.
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aRowGrid{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0), cpo::uno::Any(2.0),
                                           cpo::uno::Any(3.0) }
    };
    cpo::uno::Any aRowAny;
    aRowAny <<= aRowGrid;
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, anyToJsonFragment(aRowAny));

    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aColGrid{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0) },
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(2.0) },
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(3.0) },
    };
    cpo::uno::Any aColAny;
    aColAny <<= aColGrid;
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, anyToJsonFragment(aColAny));

    // Non-flattened 2×2 mixed Any grid (production Calc shape).
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aAnyGrid{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0), cpo::uno::Any(2.0) },
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(3.0), cpo::uno::Any(4.0) },
    };
    cpo::uno::Any aGridAny;
    aGridAny <<= aAnyGrid;
    CPPUNIT_ASSERT_EQUAL(u"[ [ 1, 2], [ 3, 4]]"_ustr, anyToJsonFragment(aGridAny));
}

CPPUNIT_TEST_FIXTURE(Test, test_buildExecuteRequestJson)
{
    using collaboraoffice::pythoncompute::buildExecuteRequestJson;

    const std::string noData = buildExecuteRequestJson(u"py-1"_ustr, u"result=1"_ustr, {});
    CPPUNIT_ASSERT(noData.find("\"data\"") == std::string::npos);
    CPPUNIT_ASSERT(noData.find("\"mode\": \"isolated\"") != std::string::npos
                   || noData.find("\"mode\":\"isolated\"") != std::string::npos);
    CPPUNIT_ASSERT_EQUAL(u"py-1"_ustr, extractJsonStringField(noData, "id"));
    CPPUNIT_ASSERT_EQUAL(u"result=1"_ustr, extractJsonStringField(noData, "code"));

    cpo::uno::Sequence<cpo::uno::Any> aOne{ cpo::uno::Any(sal_Int32(7)) };
    const std::string withScalar = buildExecuteRequestJson(u"py-2"_ustr, u"x"_ustr, aOne);
    CPPUNIT_ASSERT(withScalar.find("\"data\": 7") != std::string::npos
                   || withScalar.find("\"data\":7") != std::string::npos);

    cpo::uno::Sequence<cpo::uno::Any> aTwo{ cpo::uno::Any(1.0), cpo::uno::Any(2.0) };
    const std::string withArr = buildExecuteRequestJson(u"py-3"_ustr, u"x"_ustr, aTwo);
    CPPUNIT_ASSERT(withArr.find("\"data\": [ 1, 2]") != std::string::npos
                   || withArr.find("\"data\":[1,2]") != std::string::npos);

    // Single AddIn arg that is a Calc range (Sequence<Sequence<Any>>).
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aRange{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0), cpo::uno::Any(2.0) },
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(3.0), cpo::uno::Any(4.0) },
    };
    cpo::uno::Sequence<cpo::uno::Any> aOneRange{ cpo::uno::Any(aRange) };
    const std::string withRange
        = buildExecuteRequestJson(u"py-4"_ustr, u"result=data"_ustr, aOneRange);
    CPPUNIT_ASSERT(withRange.find("\"data\": null") == std::string::npos);
    CPPUNIT_ASSERT(withRange.find("\"data\":null") == std::string::npos);
    CPPUNIT_ASSERT(withRange.find("[ 1, 2]") != std::string::npos
                   || withRange.find("[1,2]") != std::string::npos);

    // Multiple AddIn ranges → JSON array of grids (each range may flatten).
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aRangeA{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0), cpo::uno::Any(2.0) },
    };
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aRangeB{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(3.0) },
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(4.0) },
    };
    cpo::uno::Sequence<cpo::uno::Any> aTwoRanges{ cpo::uno::Any(aRangeA), cpo::uno::Any(aRangeB) };
    const std::string withMulti
        = buildExecuteRequestJson(u"py-5"_ustr, u"result=data"_ustr, aTwoRanges);
    CPPUNIT_ASSERT(withMulti.find("\"data\": [") != std::string::npos
                   || withMulti.find("\"data\":[") != std::string::npos);
    CPPUNIT_ASSERT(withMulti.find("[ 1, 2]") != std::string::npos
                   || withMulti.find("[1,2]") != std::string::npos);
    CPPUNIT_ASSERT(withMulti.find("[ 3, 4]") != std::string::npos
                   || withMulti.find("[3,4]") != std::string::npos);
}

CPPUNIT_TEST_FIXTURE(Test, test_jsonResultToAny)
{
    using collaboraoffice::pythoncompute::jsonResultToAny;

    cpo::uno::Any aOut;
    OUString sErr;

    CPPUNIT_ASSERT(jsonResultToAny(
        R"({"id":"1","status":"error","error":"boom"})", aOut, sErr));
    CPPUNIT_ASSERT_EQUAL(u"boom"_ustr, sErr);
    double fErr = 0.0;
    CPPUNIT_ASSERT(aOut >>= fErr);
    CPPUNIT_ASSERT_EQUAL(FormulaError::NoValue, GetDoubleErrorValue(fErr));

    // coolwsd feature-flag reject → literal #DISABLED (not FormulaError / #VALUE!)
    CPPUNIT_ASSERT(jsonResultToAny(
        R"({"id":"1","status":"error","error":"Python compute is disabled"})", aOut, sErr));
    CPPUNIT_ASSERT_EQUAL(u"Python compute is disabled"_ustr, sErr);
    OUString sDisabled;
    CPPUNIT_ASSERT(aOut >>= sDisabled);
    CPPUNIT_ASSERT_EQUAL(u"#DISABLED"_ustr, sDisabled);

    OUString s;

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":3.5})", aOut, sErr));
    double f = 0.0;
    CPPUNIT_ASSERT(aOut >>= f);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(3.5, f, 1e-12);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":true})", aOut, sErr));
    bool b = false;
    CPPUNIT_ASSERT(aOut >>= b);
    CPPUNIT_ASSERT(b);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":false})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= b);
    CPPUNIT_ASSERT(!b);

    // Quoted strings must not be coerced to bool/number/null.
    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":"true"})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT_EQUAL(u"true"_ustr, s);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":"false"})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT_EQUAL(u"false"_ustr, s);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":"42"})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT_EQUAL(u"42"_ustr, s);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":"null"})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT_EQUAL(u"null"_ustr, s);

    CPPUNIT_ASSERT(
        jsonResultToAny(R"({"id":"1","status":"ok","result":"\uD83D\uDE00"})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT_EQUAL(OUString::fromUtf8("\xF0\x9F\x98\x80"), s);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":42})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= f);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(42.0, f, 1e-12);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":null})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT(s.isEmpty());

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":[1,2,3]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aMatrix;
    CPPUNIT_ASSERT(aOut >>= aMatrix);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aMatrix.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(3), aMatrix[0].getLength());
    CPPUNIT_ASSERT_DOUBLES_EQUAL(1.0, aMatrix[0][0], 1e-12);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(3.0, aMatrix[0][2], 1e-12);

    // Rectangular nested numeric lists → R×C sequence<sequence<double>>.
    CPPUNIT_ASSERT(
        jsonResultToAny(R"({"id":"1","status":"ok","result":[[1,2],[3,4]]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aGrid;
    CPPUNIT_ASSERT(aOut >>= aGrid);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aGrid.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aGrid[0].getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aGrid[1].getLength());
    CPPUNIT_ASSERT_DOUBLES_EQUAL(1.0, aGrid[0][0], 1e-12);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(2.0, aGrid[0][1], 1e-12);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(3.0, aGrid[1][0], 1e-12);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(4.0, aGrid[1][1], 1e-12);

    // Mixed grid keeps strings.
    CPPUNIT_ASSERT(
        jsonResultToAny(R"({"id":"1","status":"ok","result":[[1,"a"],[2,"b"]]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aMixed;
    CPPUNIT_ASSERT(aOut >>= aMixed);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aMixed.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aMixed[0].getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aMixed[1].getLength());
    double f00 = 0;
    CPPUNIT_ASSERT(aMixed[0][0] >>= f00);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(1.0, f00, 1e-12);
    OUString s01;
    CPPUNIT_ASSERT(aMixed[0][1] >>= s01);
    CPPUNIT_ASSERT_EQUAL(u"a"_ustr, s01);
    double f10 = 0;
    CPPUNIT_ASSERT(aMixed[1][0] >>= f10);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(2.0, f10, 1e-12);
    OUString s11;
    CPPUNIT_ASSERT(aMixed[1][1] >>= s11);
    CPPUNIT_ASSERT_EQUAL(u"b"_ustr, s11);

    // Column vector: [[1],[2],[3]] → 3×1 (not a collapsed 1×3 row).
    CPPUNIT_ASSERT(
        jsonResultToAny(R"({"id":"1","status":"ok","result":[[1],[2],[3]]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aCol;
    CPPUNIT_ASSERT(aOut >>= aCol);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(3), aCol.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aCol[0].getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aCol[1].getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aCol[2].getLength());
    CPPUNIT_ASSERT_DOUBLES_EQUAL(1.0, aCol[0][0], 1e-12);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(2.0, aCol[1][0], 1e-12);
    CPPUNIT_ASSERT_DOUBLES_EQUAL(3.0, aCol[2][0], 1e-12);

    // Single-element list stays 1×1 matrix (not bare scalar) for stack consistency.
    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":[5]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aOne;
    CPPUNIT_ASSERT(aOut >>= aOne);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOne.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOne[0].getLength());
    CPPUNIT_ASSERT_DOUBLES_EQUAL(5.0, aOne[0][0], 1e-12);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":["one"]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aOneString;
    CPPUNIT_ASSERT(aOut >>= aOneString);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOneString.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOneString[0].getLength());
    CPPUNIT_ASSERT(aOneString[0][0] >>= s);
    CPPUNIT_ASSERT_EQUAL(u"one"_ustr, s);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":[true]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aOneBool;
    CPPUNIT_ASSERT(aOut >>= aOneBool);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOneBool.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOneBool[0].getLength());
    CPPUNIT_ASSERT(aOneBool[0][0] >>= b);
    CPPUNIT_ASSERT(b);

    CPPUNIT_ASSERT(
        jsonResultToAny(R"({"id":"1","status":"ok","result":[["a"],["b"]]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aStringCol;
    CPPUNIT_ASSERT(aOut >>= aStringCol);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aStringCol.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aStringCol[0].getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aStringCol[1].getLength());

    CPPUNIT_ASSERT(
        jsonResultToAny(R"({"id":"1","status":"ok","result":[[1,2],["a","b"]]})", aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aMixedRows;
    CPPUNIT_ASSERT(aOut >>= aMixedRows);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aMixedRows.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aMixedRows[0].getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aMixedRows[1].getLength());

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":[[1,2],[3]]})", aOut, sErr));
    CPPUNIT_ASSERT_EQUAL(u"Python compute result JSON parse failed"_ustr, sErr);
    CPPUNIT_ASSERT(aOut >>= fErr);
    CPPUNIT_ASSERT_EQUAL(FormulaError::NoValue, GetDoubleErrorValue(fErr));

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok","result":"\uD83D"})", aOut, sErr));
    CPPUNIT_ASSERT_EQUAL(u"Python compute result JSON parse failed"_ustr, sErr);
    CPPUNIT_ASSERT(jsonResultToAny(
        R"({"id":"1","status":"ok","result":1} trailing)", aOut, sErr));
    CPPUNIT_ASSERT_EQUAL(u"Python compute result JSON parse failed"_ustr, sErr);

    CPPUNIT_ASSERT(jsonResultToAny(
        R"({"id":"1","status":"ok","result":null,"images":["a"]})", aOut, sErr));
    CPPUNIT_ASSERT(aOut >>= s);
    CPPUNIT_ASSERT_EQUAL(u"Image generated (plot insert not supported yet)"_ustr, s);

    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"1","status":"ok"})", aOut, sErr));
    CPPUNIT_ASSERT_EQUAL(u"Python compute response missing result"_ustr, sErr);
    CPPUNIT_ASSERT(aOut >>= fErr);
    CPPUNIT_ASSERT_EQUAL(FormulaError::NoValue, GetDoubleErrorValue(fErr));
}

CPPUNIT_TEST_FIXTURE(Test, test_anyJsonFragment_roundTrip)
{
    using collaboraoffice::pythoncompute::anyToJsonFragment;
    using collaboraoffice::pythoncompute::jsonResultToAny;

    auto wrapOk = [](const OUString& sFragment) {
        return std::string("{\"id\":\"t\",\"status\":\"ok\",\"result\":")
               + std::string(sFragment.toUtf8()) + "}";
    };

    cpo::uno::Any aOut;
    OUString sErr;

    // Row vector Any grid → flat JSON → 1×N double matrix.
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aRow{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0), cpo::uno::Any(2.0),
                                           cpo::uno::Any(3.0) },
    };
    const OUString sRow = anyToJsonFragment(cpo::uno::Any(aRow));
    CPPUNIT_ASSERT_EQUAL(u"[ 1, 2, 3]"_ustr, sRow);
    CPPUNIT_ASSERT(jsonResultToAny(wrapOk(sRow), aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aRowMat;
    CPPUNIT_ASSERT(aOut >>= aRowMat);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aRowMat.getLength());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(3), aRowMat[0].getLength());
    CPPUNIT_ASSERT_DOUBLES_EQUAL(2.0, aRowMat[0][1], 1e-12);

    // Typed double 2×2 stays nested through emit → parse.
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aDGrid{
        cpo::uno::Sequence<double>{ 1.0, 2.0 },
        cpo::uno::Sequence<double>{ 3.0, 4.0 },
    };
    const OUString sGrid = anyToJsonFragment(cpo::uno::Any(aDGrid));
    CPPUNIT_ASSERT_EQUAL(u"[ [ 1, 2], [ 3, 4]]"_ustr, sGrid);
    CPPUNIT_ASSERT(jsonResultToAny(wrapOk(sGrid), aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<double>> aParsedGrid;
    CPPUNIT_ASSERT(aOut >>= aParsedGrid);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aParsedGrid.getLength());
    CPPUNIT_ASSERT_DOUBLES_EQUAL(4.0, aParsedGrid[1][1], 1e-12);

    // Mixed grid keeps string cells.
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aMixed{
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(1.0), cpo::uno::Any(u"a"_ustr) },
        cpo::uno::Sequence<cpo::uno::Any>{ cpo::uno::Any(2.0), cpo::uno::Any(u"b"_ustr) },
    };
    const OUString sMixed = anyToJsonFragment(cpo::uno::Any(aMixed));
    CPPUNIT_ASSERT(jsonResultToAny(wrapOk(sMixed), aOut, sErr));
    cpo::uno::Sequence<cpo::uno::Sequence<cpo::uno::Any>> aMixedOut;
    CPPUNIT_ASSERT(aOut >>= aMixedOut);
    OUString s01;
    CPPUNIT_ASSERT(aMixedOut[0][1] >>= s01);
    CPPUNIT_ASSERT_EQUAL(u"a"_ustr, s01);

    // Quoted numeric string stays a string through the envelope.
    CPPUNIT_ASSERT(jsonResultToAny(R"({"id":"t","status":"ok","result":"42"})", aOut, sErr));
    OUString sQuoted;
    CPPUNIT_ASSERT(aOut >>= sQuoted);
    CPPUNIT_ASSERT_EQUAL(u"42"_ustr, sQuoted);
}
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab cinoptions=b1,g0,N-s cinkeys+=0=break: */
