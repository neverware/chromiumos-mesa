/*
 * Copyright (C) 2020 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors (Collabora):
 *      Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

/**
 * Implements framebuffer format conversions in software for Midgard/Bifrost
 * blend shaders. This pass is designed for a single render target; Midgard
 * duplicates blend shaders for MRT to simplify everything. A particular
 * framebuffer format may be categorized as 1) typed load available, 2) typed
 * unpack available, or 3) software unpack only, and likewise for stores. The
 * first two types are handled in the compiler backend directly, so this module
 * is responsible for identifying type 3 formats (hardware dependent) and
 * inserting appropriate ALU code to perform the conversion from the packed
 * type to a designated unpacked type, and vice versa.
 *
 * The unpacked type depends on the format:
 *
 *      - For 32-bit float formats, 32-bit floats.
 *      - For other floats, 16-bit floats.
 *      - For 32-bit ints, 32-bit ints.
 *      - For 8-bit ints, 8-bit ints.
 *      - For other ints, 16-bit ints.
 *
 * The rationale is to optimize blending and logic op instructions by using the
 * smallest precision necessary to store the pixel losslessly.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_format_convert.h"
#include "util/format/u_format.h"
#include "pan_lower_framebuffer.h"
#include "panfrost-quirks.h"

/* Determines the unpacked type best suiting a given format, so the rest of the
 * pipeline may be adjusted accordingly */

nir_alu_type
pan_unpacked_type_for_format(const struct util_format_description *desc)
{
        int c = util_format_get_first_non_void_channel(desc->format);

        if (c == -1)
                unreachable("Void format not renderable");

        bool large = (desc->channel[c].size > 16);
        bool bit8 = (desc->channel[c].size == 8);
        assert(desc->channel[c].size <= 32);

        if (desc->channel[c].normalized)
                return large ? nir_type_float32 : nir_type_float16;

        switch (desc->channel[c].type) {
        case UTIL_FORMAT_TYPE_UNSIGNED:
                return bit8 ? nir_type_uint8 :
                        large ? nir_type_uint32 : nir_type_uint16;
        case UTIL_FORMAT_TYPE_SIGNED:
                return bit8 ? nir_type_int8 :
                        large ? nir_type_int32 : nir_type_int16;
        case UTIL_FORMAT_TYPE_FLOAT:
                return large ? nir_type_float32 : nir_type_float16;
        default:
                unreachable("Format not renderable");
        }
}

enum pan_format_class
pan_format_class_load(const struct util_format_description *desc, unsigned quirks)
{
        /* Check if we can do anything better than software architecturally */
        if (quirks & MIDGARD_NO_TYPED_BLEND_LOADS) {
                return (quirks & NO_BLEND_PACKS)
                        ? PAN_FORMAT_SOFTWARE : PAN_FORMAT_PACK;
        }

        /* Some formats are missing as typed on some GPUs but have unpacks */
        if (quirks & MIDGARD_MISSING_LOADS) {
                switch (desc->format) {
                case PIPE_FORMAT_R11G11B10_FLOAT:
                case PIPE_FORMAT_R10G10B10A2_UNORM:
                case PIPE_FORMAT_B10G10R10A2_UNORM:
                case PIPE_FORMAT_R10G10B10X2_UNORM:
                case PIPE_FORMAT_B10G10R10X2_UNORM:
                case PIPE_FORMAT_R10G10B10A2_UINT:
                        return PAN_FORMAT_PACK;
                default:
                        return PAN_FORMAT_NATIVE;
                }
        }

        /* Otherwise, we can do native */
        return PAN_FORMAT_NATIVE;
}