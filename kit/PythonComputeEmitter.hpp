/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Bridges coolkit ChildSession ↔ LibreOffice scaddins pythoncompute library
 * (libpythoncomputelo.so) via dlsym after LO is loaded.
 */

#pragma once

#include <cstddef>
#include <string>

class ChildSession;

namespace pythoncompute
{
/** Register a live view; keep a single egress owner (do not steal from a live owner).
 *  Emit is marshaled onto the kit Unipoll thread (Calc may call from workers).
 *  No-op on MOBILEAPP (no coolwsd broker). */
void installEmitter(ChildSession* session);

/** Drop this session; reinstall on another live view (lowest id) if any remain. */
void clearEmitter(ChildSession* session);

/**
 * Finish a pending XVolatileResult from pythoncomputeresult JSON.
 * @return true if the scaddins library handled it.
 */
bool completeFromJson(const std::string& json);
} // namespace pythoncompute

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
