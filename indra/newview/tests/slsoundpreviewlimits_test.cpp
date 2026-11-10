// Copyright (C) 2026 Slipstream Viewer contributors. SPDX-License-Identifier: LGPL-2.1-only
#include "linden_common.h"
#include "../../llaudio/llsoundpreviewlimits.h"
#include "../test/lltut.h"
namespace tut
{
struct preview_limits_data {};
using preview_limits_test = test_group<preview_limits_data>;
using preview_limits_object = preview_limits_test::object;
preview_limits_test preview_limits_group("SLSoundPreviewLimits");
template<> template<> void preview_limits_object::test<1>()
{
    using namespace LLSoundPreviewLimits;
    ensure("empty admission", fits(0, 0, PCM_BYTES));
    ensure("capacity refusal", !fits(PCM_BYTES, 0, 1));
    ensure("replacement releases old charge", fits(PCM_BYTES, 1024, 1024));
    ensure("invalid old charge", !fits(1, 2, 0));
    ensure("oversize replacement", !fits(0, 0, PCM_BYTES + 1));
    ensure("overflow input refused", !fits(UINT64_MAX, 0, 1));
}
template<> template<> void preview_limits_object::test<2>()
{
    using namespace LLSoundPreviewLimits;
    std::uint64_t bytes = 123;
    ensure("stereo source", sourceBytes(44100, 2, 44100, bytes));
    ensure_equals("checked byte count", bytes, std::uint64_t(176400));
    ensure("no channels", !sourceBytes(1, 0, 44100, bytes));
    ensure_equals("refusal clears output", bytes, std::uint64_t(0));
    ensure("overflow before ALsizei conversion", !sourceBytes(UINT32_MAX, 2, 44100, bytes));
    ensure("invalid signed rate", !sourceBytes(1, 1, UINT32_MAX, bytes));
}
template<> template<> void preview_limits_object::test<3>()
{
    using namespace LLSoundPreviewLimits;
    std::uint64_t bytes = 0;
    ensure("logical length small but physical reserve too large", !retainedBytes(2, PCM_BYTES, 1, 44100, bytes));
    ensure("physical reserve charged", retainedBytes(2, 100, 1, 44100, bytes));
    ensure_equals("capacity charge", bytes, std::uint64_t(200));
    ensure("incomplete stereo frame", !retainedBytes(3, 3, 2, 44100, bytes));
    ensure("invalid source rate", !retainedBytes(2, 2, 1, UINT32_MAX, bytes));
}

}
