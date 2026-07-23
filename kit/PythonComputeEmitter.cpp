/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include "PythonComputeEmitter.hpp"
#include "ChildSession.hpp"
#include "Kit.hpp"

#include <common/Log.hpp>
#include <common/ProcUtil.hpp>

#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
using EmitFn = void (*)(void* userdata, const char* jsonUtf8, std::int32_t len);
using SetEmitterFn = void (*)(EmitFn fn, void* userdata);
using CompleteFn = int (*)(const char* jsonUtf8, std::int32_t len);
using ClearCachesFn = void (*)();

std::mutex gMutex;
ChildSession* gSession = nullptr;
std::unordered_set<ChildSession*> gSessions;
std::unordered_map<std::string, ChildSession*> gSessionsById;
SetEmitterFn gSetEmitter = nullptr;
CompleteFn gComplete = nullptr;
ClearCachesFn gClearCaches = nullptr;

void resolveSymbols()
{
    // Retry until all symbols are found — libpythoncomputelo.so may load late.
    if (gSetEmitter && gComplete && gClearCaches)
        return;

    // Symbols are in libpythoncomputelo.so once the Calc AddIn is loaded. Also
    // try RTLD_DEFAULT in case the component is already mapped.
    if (!gSetEmitter)
        gSetEmitter = reinterpret_cast<SetEmitterFn>(
            dlsym(RTLD_DEFAULT, "pythoncompute_set_emitter"));
    if (!gComplete)
        gComplete = reinterpret_cast<CompleteFn>(
            dlsym(RTLD_DEFAULT, "pythoncompute_complete_json"));
    if (!gClearCaches)
        gClearCaches = reinterpret_cast<ClearCachesFn>(
            dlsym(RTLD_DEFAULT, "pythoncompute_clear_caches"));

    if (!gSetEmitter || !gComplete || !gClearCaches)
    {
        // Explicit library name as built by LibreOffice gbuild (…lo.so).
        const char* candidates[] = {
            "libpythoncomputelo.so",
            "libpythoncompute.so",
        };
        for (const char* name : candidates)
        {
            void* lib = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
            if (!lib)
                lib = dlopen(name, RTLD_LAZY);
            if (!lib)
            {
                LOG_DBG("pythoncompute: dlopen(" << name << ") failed");
                continue;
            }
            LOG_DBG("pythoncompute: dlopen(" << name << ") ok");
            if (!gSetEmitter)
                gSetEmitter = reinterpret_cast<SetEmitterFn>(
                    dlsym(lib, "pythoncompute_set_emitter"));
            if (!gComplete)
                gComplete = reinterpret_cast<CompleteFn>(
                    dlsym(lib, "pythoncompute_complete_json"));
            if (!gClearCaches)
                gClearCaches = reinterpret_cast<ClearCachesFn>(
                    dlsym(lib, "pythoncompute_clear_caches"));
            if (gSetEmitter && gComplete && gClearCaches)
                break;
        }
    }

    LOG_DBG("pythoncompute: symbols set_emitter=" << (gSetEmitter ? "yes" : "no")
                                                  << " complete_json=" << (gComplete ? "yes" : "no")
                                                  << " clear_caches="
                                                  << (gClearCaches ? "yes" : "no"));
    if (!gSetEmitter)
        LOG_WRN("pythoncompute_set_emitter not found — =PY() AddIn emit will fail until "
                "libpythoncompute is loaded");
}

/// Prefer lowest session id among live views for a stable egress owner.
ChildSession* pickStableOwner_NoLock()
{
    ChildSession* best = nullptr;
    for (ChildSession* s : gSessions)
    {
        if (!s)
            continue;
        if (!best || s->getId() < best->getId())
            best = s;
    }
    return best;
}

bool ownerIsLive_NoLock()
{
    return gSession && gSessions.find(gSession) != gSessions.end();
}

void sendEmitOnKitThread(const std::string& sessionId, std::string payload)
{
    ChildSession* session = nullptr;
    {
        std::scoped_lock lock(gMutex);
        auto it = gSessionsById.find(sessionId);
        if (it == gSessionsById.end() || gSessions.find(it->second) == gSessions.end())
        {
            LOG_WRN("pythoncompute emit: session=[" << sessionId << "] no longer live");
            return;
        }
        session = it->second;
    }
    if (!session)
        return;
    LOG_DBG("pythoncompute emitThunk: session=[" << session->getId()
                                                 << "] bytes=" << payload.size());
    session->sendTextFrame("pythoncompute: " + payload);
}

void emitThunk(void* /*userdata*/, const char* jsonUtf8, std::int32_t len)
{
#if MOBILEAPP
    // No coolwsd broker on mobile/WASM/Qt — never emit upward.
    (void)jsonUtf8;
    (void)len;
    return;
#else
    std::string sessionId;
    {
        std::scoped_lock lock(gMutex);
        if (!gSession)
        {
            LOG_WRN("pythoncompute emitThunk: no active ChildSession");
            return;
        }
        sessionId = gSession->getId();
    }
    if (!jsonUtf8 || len <= 0)
    {
        LOG_WRN("pythoncompute emitThunk: empty payload len=" << len);
        return;
    }
    std::string payload(jsonUtf8, static_cast<size_t>(len));

    // Calc may call the emitter off the kit Unipoll thread — marshal send.
    KitSocketPoll* poll = KitSocketPoll::getMainPoll();
    if (poll && poll->getThreadOwner() != ProcUtil::getThreadId())
    {
        poll->addCallback(
            [sessionId = std::move(sessionId), payload = std::move(payload)]() mutable {
                sendEmitOnKitThread(sessionId, std::move(payload));
            });
        return;
    }
    sendEmitOnKitThread(sessionId, std::move(payload));
#endif
}
} // namespace

namespace pythoncompute
{
void installEmitter(ChildSession* session)
{
#if MOBILEAPP
    // Desktop Online only — mobile/WASM/Qt have no pythoncompute broker.
    (void)session;
    return;
#else
    std::scoped_lock lock(gMutex);
    resolveSymbols();
    if (session)
    {
        gSessions.insert(session);
        gSessionsById[session->getId()] = session;
    }

    // One egress owner per kit: do not steal from a live owner when a second view loads.
    if (!ownerIsLive_NoLock())
        gSession = session ? session : pickStableOwner_NoLock();

    LOG_DBG("pythoncompute installEmitter: session=["
            << (session ? session->getId() : std::string("null"))
            << "] owner=[" << (gSession ? gSession->getId() : std::string("null"))
            << "] liveViews=" << gSessions.size()
            << " haveSetEmitter=" << (gSetEmitter ? 1 : 0));
    if (gSetEmitter && gSession)
        gSetEmitter(&emitThunk, nullptr);
#endif
}

void clearEmitter(ChildSession* session)
{
#if MOBILEAPP
    (void)session;
    return;
#else
    ClearCachesFn pClearCaches = nullptr;
    {
        std::scoped_lock lock(gMutex);
        gSessions.erase(session);
        if (session)
            gSessionsById.erase(session->getId());
        if (gSession != session)
        {
            LOG_DBG("pythoncompute clearEmitter: session=["
                    << (session ? session->getId() : std::string("null"))
                    << "] was not owner; liveViews=" << gSessions.size());
            return;
        }

        if (!gSessions.empty())
        {
            // Keep AddIn emission alive via another live view in this kit.
            gSession = pickStableOwner_NoLock();
            LOG_DBG("pythoncompute clearEmitter: reinstalled on session=["
                    << (gSession ? gSession->getId() : std::string("null"))
                    << "] liveViews=" << gSessions.size());
            if (gSetEmitter && gSession)
                gSetEmitter(&emitThunk, nullptr);
            return;
        }

        gSession = nullptr;
        LOG_DBG("pythoncompute clearEmitter: no sessions left; clearing AddIn emitter");
        if (gSetEmitter)
            gSetEmitter(nullptr, nullptr);
        pClearCaches = gClearCaches;
    }

    // Last view gone: drop the AddIn param/pending caches for memory hygiene.
    // Call outside gMutex — clear_caches takes SolarMutex, and emitThunk takes
    // gMutex under Solar, so holding gMutex across Solar here would deadlock.
    if (pClearCaches)
        pClearCaches();
#endif
}

bool completeFromJson(const std::string& json)
{
    CompleteFn complete = nullptr;
    {
        std::scoped_lock lock(gMutex);
        resolveSymbols();
        complete = gComplete;
        if (!complete)
        {
            LOG_WRN("pythoncompute completeFromJson: complete_json symbol missing; bytes="
                    << json.size());
            return false;
        }
    }

    // complete_json finishes Calc listeners under SolarMutex and can re-enter
    // emitThunk. Never hold gMutex across the call into the AddIn.
    const bool ok = complete(json.c_str(), static_cast<std::int32_t>(json.size())) != 0;
    LOG_DBG("pythoncompute completeFromJson: handled=" << (ok ? 1 : 0)
                                                       << " bytes=" << json.size());
    return ok;
}
} // namespace pythoncompute

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
