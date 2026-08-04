/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef TELEM_TEMPLATE_CTXT_H_
#define TELEM_TEMPLATE_CTXT_H_

/* context filled by hardware and stored at the end */
typedef struct {
	uint8_t was_nack : 1;		 /* Signal the reception of a NACK */
	uint8_t was_cnp : 1;		 /* Signal the reception of a CNP */
	uint8_t rtt_req_to_rtt_sent : 3; /* Set between the algorithm's RTT request until the time at which the RTT
					    packet was sent */
	uint8_t reserved : 3;		 /* Reserved bits */
} telem_template_flags_t;

/* switch telemetry context */
typedef struct {
	uint32_t cur_rate;		   /* Current rate */
	uint32_t last_tx_bytes;		   /* Last switch TX bytes */
	uint32_t last_switch_tx_timestamp; /* Last switch TX timestamp */
	telem_template_flags_t flags;	   /* Flags struct */
	uint8_t abort_cnt;		   /* Counter of abort RTT requests */
	uint8_t rtt_meas_psn;		   /* RTT request sequence number */
	uint8_t reserved0;		   /* Reserved bits */
	uint32_t start_delay;		   /* The time at which the RTT packet was sent by the NIC's Tx pipe */
	uint32_t reserved[7];		   /* Reserved bits */
} cc_ctxt_telem_template_t;

/* Switch telemetry format */
typedef union {
	uint32_t _value; /* Value */
	struct {
		uint16_t qlen;	       /* Queue length in cells of 256B */
		uint8_t index;	       /* switch index */
		uint8_t reserved : 5;  /* Reserved bits */
		uint8_t valid : 1;     /* Valid flag */
		uint8_t port_type : 2; /* port type 0 - 25Gbps, 1 - 100Gbps */
	};
} doca_pcc_dev_switch_telem_extra_t;

#endif /* TELEM_TEMPLATE_CTXT_H_ */
