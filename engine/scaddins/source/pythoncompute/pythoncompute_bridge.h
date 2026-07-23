/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
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
 * C API: kit sets the emitter; AddIn posts dumb JSON; kit completes volatiles.
 */

#pragma once

#include <sal/types.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called by the AddIn (same LOKit process) to send a pythoncompute: request up. */
typedef void (*pythoncompute_emit_fn)(void* userdata, const char* jsonUtf8, int32_t len);

/** Installed by Online coolkit once LO is loaded (RTLD_DEFAULT findable). */
SAL_DLLPUBLIC_EXPORT void pythoncompute_set_emitter(pythoncompute_emit_fn fn, void* userdata);

/**
 * Clear pending map + param→volatile cache and stop the timeout timer.
 * Called by the kit on last-session teardown for memory hygiene; identity is
 * weak-reference based, so this does not affect live cells.
 */
SAL_DLLPUBLIC_EXPORT void pythoncompute_clear_caches();

/** Clear caches (via pythoncompute_clear_caches) and restore the default timeout. */
SAL_DLLPUBLIC_EXPORT void pythoncompute_reset_for_tests();

/** Override pending-map deadline (CppUnit only). Clamped to >= 1 ms. */
SAL_DLLPUBLIC_EXPORT void pythoncompute_set_pending_timeout_ms_for_tests(sal_Int32 timeoutMs);

/**
 * Finish a pending XVolatileResult from a pythoncomputeresult JSON body.
 * @return 1 if a pending request was completed, 0 otherwise.
 */
SAL_DLLPUBLIC_EXPORT int pythoncompute_complete_json(const char* jsonUtf8, int32_t len);

#ifdef __cplusplus
}
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
