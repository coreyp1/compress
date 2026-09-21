/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file dict.h
 *
 * Building a dictionary from samples.
 *
 * ## What a dictionary is for
 *
 * Compression works by finding repetition, and a short input has almost none
 * to find: a 300-byte JSON record, log line or HTTP header block compresses
 * badly not because the data is incompressible but because there is no room
 * for the encoder to learn it. A dictionary is history supplied up front, so
 * the first byte of a message can match against content the encoder has
 * already been shown.
 *
 * That only helps if the dictionary holds what the messages actually have in
 * common, which is what this finds.
 *
 * ## What it does
 *
 * FASTCOVER, the variant of COVER (Liao, Petri, Moffat and Wirth, "Effective
 * Construction of Relative Lempel-Ziv Dictionaries", WWW 2016) that libzstd's
 * dictionary builder uses. The idea is short:
 *
 * 1. Count how often every @c d -byte sequence occurs across the samples.
 * 2. Score each @c k -byte segment by the total frequency of the *distinct*
 *    sequences in it - so a segment made of common pieces scores highly, and a
 *    segment that repeats one common piece does not score highly twice.
 * 3. Take the best segment, add it to the dictionary, and set the frequency of
 *    everything in it to zero so that the next pick covers something new.
 * 4. Repeat until the dictionary is full.
 *
 * The result is raw content: no header, no entropy tables. That is a valid
 * dictionary for every method here - RFC 8878 section 5 calls it a
 * content-only dictionary, and for deflate and LZ4 it is the only kind there
 * is.
 *
 * ## What it is not
 *
 * It is not a formatted Zstandard dictionary. Those carry pre-trained Huffman
 * and FSE tables as well as content, which help most on very short messages
 * where even the entropy tables cost more than the data. Content is the larger
 * half of the benefit and the portable half.
 */

#ifndef GHOTI_IO_GCOMP_DICT_H
#define GHOTI_IO_GCOMP_DICT_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build a dictionary from sample messages.
 *
 * The samples should be what the dictionary will be used on: a few hundred
 * real records beat a megabyte of one. They are not copied and are not
 * retained.
 *
 * A dictionary bigger than about a hundredth of the total sample size mostly
 * holds content that appears once, which is content the encoder would have
 * found on its own. The usual shape is a 100 KB dictionary trained on several
 * megabytes of samples.
 *
 * ## Options
 *
 * - `dict.segment_size` (uint64, default 256): @c k, the length of the pieces
 *   the dictionary is built from. Longer pieces capture longer shared
 *   structure and waste more of the budget when they are only partly useful.
 * - `dict.dmer_size` (uint64, 6 to 8, default 8): @c d, the length of the
 *   sequences whose frequency is counted. Smaller finds shorter commonality
 *   and is noisier.
 *
 * @param registry Registry to find the method in (NULL for the default)
 * @param method_name Method the dictionary is for; the content is the same for
 *        all of them, and this is checked only so that a name nobody
 *        implements is refused rather than quietly accepted
 * @param options Configuration options (may be NULL)
 * @param samples Array of @p n_samples pointers to sample bytes
 * @param sample_sizes Their lengths
 * @param n_samples How many
 * @param dict_out Where the dictionary goes
 * @param dict_capacity How much room there is; also the size asked for, since
 *        a trainer fills its budget
 * @param dict_size_out Receives how much was written, which may be less than
 *        @p dict_capacity when the samples do not contain enough repetition to
 *        fill it
 * @return ::GCOMP_OK; ::GCOMP_ERR_INVALID_ARG for missing arguments;
 *         ::GCOMP_ERR_UNSUPPORTED for an unknown method or samples too small
 *         to learn anything from; ::GCOMP_ERR_MEMORY
 */
GCOMP_API gcomp_status_t gcomp_dict_train(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * const * samples, const size_t * sample_sizes, size_t n_samples,
    void * dict_out, size_t dict_capacity, size_t * dict_size_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_DICT_H
