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

#pragma once

#include <com/sun/star/sheet/XVolatileResult.hpp>
#include <com/sun/star/sheet/XResultListener.hpp>
#include <com/sun/star/sheet/ResultEvent.hpp>
#include <com/sun/star/lang/XServiceInfo.hpp>
#include <cppuhelper/implbase.hxx>
#include <mutex>
#include <vector>

namespace collaboraoffice::pythoncompute
{
/** Async formula result: interim "#BUSY!", then modified() with the final Any. */
class PythonComputeVolatileResult final
    : public cppu::WeakImplHelper<css::sheet::XVolatileResult, css::lang::XServiceInfo>
{
public:
    PythonComputeVolatileResult();
    virtual ~PythonComputeVolatileResult() override;

    void finish(const cpo::uno::Any& aValue);

    // XVolatileResult
    virtual void SAL_CALL
    addResultListener(const css::uno::Reference<css::sheet::XResultListener>& xListener) override;
    virtual void SAL_CALL removeResultListener(
        const css::uno::Reference<css::sheet::XResultListener>& xListener) override;

    // XServiceInfo
    virtual OUString SAL_CALL getImplementationName() override;
    virtual bool SAL_CALL supportsService(const OUString& ServiceName) override;
    virtual cpo::uno::Sequence<OUString> SAL_CALL getSupportedServiceNames() override;

private:
    void notifyListeners(const cpo::uno::Any& aValue);

    std::mutex m_aMutex;
    std::vector<css::uno::Reference<css::sheet::XResultListener>> m_aListeners;
    cpo::uno::Any m_aCurrent;
    bool m_bFinished = false;
};

} // namespace collaboraoffice::pythoncompute

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
