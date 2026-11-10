/** Main-built immutable transport schema storage. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTSCHEMALEASE_H
#define LL_LLTRANSPORTSCHEMALEASE_H
#include "lltransportownerapi.h"
#include <map>

class LLMessageTemplate;

// Main only, before socket ownership transfers. No template pointer survives
// this call; the result owns the frozen schema and entry arrays. The caller
// retains its templates and handler registrations on main. Bans and later
// template-policy changes still require a separate ordered owner protocol;
// constructing this lease alone does not authorize starting the worker.
enum class LLTransportSchemaLeaseResult { Complete, Invalid, Capacity, AllocationFailed };
LLTransportSchemaLeaseResult llBuildTransportSchemaLeaseOnMain(
    const std::map<uint32_t, LLMessageTemplate*>& templates, uint64_t generation,
    uint64_t maxRetainedBytes, std::shared_ptr<const LLTransportSchemaLease>& out) noexcept;
#endif
