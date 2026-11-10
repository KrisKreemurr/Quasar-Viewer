/**
 * @file slstraightalpha.h
 * @brief Straight-alpha 8-bit RGBA (#1328): premultiply before resampling a cut-out and
 *        unpremultiply after, so a resized transparent snapshot keeps its edge colour.
 *        Viewer-free and unit-tested (tests/slstraightalpha_test.cpp).
 *
 * WHY. A PNG carries straight (unpremultiplied) alpha: a fully transparent pixel's colour is
 * meaningless (black here). A resampler that averages straight RGBA mixes that black into
 * every partly covered pixel of the result -- a dark fringe around the cut-out. Averaging
 * premultiplied colour is exact (colour weighted by coverage); dividing by the averaged
 * alpha afterwards gives straight colour back.
 *
 * THE FLOOR. Alpha 0 unpremultiplies to transparent black (no stray colour where nothing
 * is). Colour above its alpha (light that carries no coverage) clamps to white. In 8 bits a
 * low alpha keeps few colour levels (premultiplied colour <= alpha): the colour error there
 * stays below one step once the cut-out is composited.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_SLSTRAIGHTALPHA_H
#define SL_SLSTRAIGHTALPHA_H

#include "stdtypes.h"

#include <cstddef>

// `pixels` RGBA pixels in place: colour x alpha / 255 (rounded to nearest).
void slPremultiplyRGBA(U8* rgba, size_t pixels);
// `pixels` RGBA pixels in place: colour x 255 / alpha (rounded, clamped to 255); alpha 0 ->
// transparent black.
void slUnpremultiplyRGBA(U8* rgba, size_t pixels);

#endif // SL_SLSTRAIGHTALPHA_H
