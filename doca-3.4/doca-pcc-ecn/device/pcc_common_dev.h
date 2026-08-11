/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef PCC_COMMON_DEV_H_
#define PCC_COMMON_DEV_H_

#include <doca_pcc_dev_utils.h>
#include "utils.h"

/**< Unused attribute */
#define __unused __attribute__((__unused__))

/**< UDP header size in bytes */
#define UDP_HDR_SIZE (8)

/**< Struct holding bytes and timestamp */
struct bytes_ts_t {
	uint32_t bytes; /* bytes */
	uint32_t ts;	/* timestamp */
};

/*
 * Calculate wrap around in case of int overflow.
 *
 * @greater_num [in]: greater int
 * @smaller_num [in]: smaller int
 * @return: difference with wrap around
 */
ALWAYS_INLINE uint32_t diff_with_wrap32(uint32_t greater_num, uint32_t smaller_num)
{
	uint32_t diff_res;

	if (unlikely(greater_num < smaller_num))
		diff_res = UINT32_MAX - smaller_num + greater_num + 1; /* wrap around */
	else
		diff_res = greater_num - smaller_num;
	return diff_res;
}

#endif /* PCC_COMMON_DEV_H_ */
