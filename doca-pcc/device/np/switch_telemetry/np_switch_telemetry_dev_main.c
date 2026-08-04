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

#include <doca_pcc_np_dev.h>
#include "pcc_common_dev.h"

/**< Log number of supported switch devices */
#define TELEMETRY_DB_LOG_MAX_DEVICES (10)
/**< Log number of supported ports per switch device */
#define TELEMETRY_DB_LOG_MAX_PORTS_PER_DEVICE (5)
/**< Log size of telemetry database */
#define TELEMETRY_DB_LOG_SIZE (TELEMETRY_DB_LOG_MAX_DEVICES + TELEMETRY_DB_LOG_MAX_PORTS_PER_DEVICE)
/**< Telemetry database mask */
#define TELEMETRY_DB_MASK ((1UL << TELEMETRY_DB_LOG_SIZE) - 1UL)
/**< Telemetry database size */
#define TELEMETRY_DB_SIZE (1UL << TELEMETRY_DB_LOG_SIZE)
/**< Time in microseconds for aged packet */
#define DB_ENTRY_AGING_TIME (5000000)
/**< Time interval in nanoseconds to filter out metadata that arrived earlier but were processed later due to
 * multithread */
#define INVALID_MD_TIME_INTERVAL (2000000000)

/**< Maximum hop limit value for IFA2 header */
uint8_t max_hop_limit;
/**< Flag to indicate that the mailbox operation has completed */
uint32_t mailbox_done = 0;

/**
 * Telemetry metadata database entry.
 * struct is aligned to half of DPA cacheline size for performance considerations.
 */
struct telemetry_db_entry_t {
	uint32_t tx_bytes0;	 /* last received TX bytes */
	uint32_t tx_ts_nano0;	 /* last received TX timestamp in nanoseconds */
	uint32_t tx_bytes1;	 /* last to last received TX bytes */
	uint32_t tx_ts_nano1;	 /* last to last received TX timestamp in nanoseconds */
	uint32_t valid;		 /* valid flag */
	uint32_t last_access_ts; /* timestamp of last access to entry in microseconds */
	uint32_t rsvd0;		 /* reserved */
	uint32_t rsvd1;		 /* reserved */
} __attribute__((aligned(32)));

/**< Database for storing telemetry metadata for each port and switch */
struct telemetry_db_entry_t telemetry_db[TELEMETRY_DB_SIZE];

/**< IFA2 metadata header */
struct ifa2_md_hdr {
	uint8_t request_vec; /* Request Vector (8 bits) */
	uint8_t action_vec;  /* Action Vector (8 bits) */
	uint8_t hop_limit;   /* Hop Limit (8 bits) */
	uint8_t cur_length;  /* Current Length (8 bits) */
};

/**< Switch metadata */
struct switch_md {
	uint32_t dev_id_high : 24; /* device ID - high bits */
	uint32_t pt : 2;	   /* port type */
	uint32_t dev_id_low : 6;   /* device ID - low bits */
	/* ----------------------------------- */
	uint8_t congestion : 5; /* congestion */
	uint8_t tid : 3;	/* TID */
	uint8_t tx_bytes_hi;	/* TX bytes - high bits */
	uint8_t ttl;		/* TTL */
	uint8_t queue_id;	/* queue ID */
	/* ----------------------------------- */
	uint32_t rx_ts_sec_hi; /* RX timestamp (sec) - high bits */
	/* ----------------------------------- */
	uint16_t rx_ts_sec_lo;	/* RX timestamp (sec) - low bits */
	uint16_t rx_ts_nano_hi; /* RX timestamp (nano) - high bits */
	/* ----------------------------------- */
	uint16_t rx_ts_nano_lo; /* RX timestamp (nano) - low bits */
	uint16_t tx_ts_nano_hi; /* TX timestamp (nano) - high bits */
	/* ----------------------------------- */
	uint16_t tx_ts_nano_lo;	    /* TX timestamp (nano) - low bits */
	uint16_t eg_queue_cell_cnt; /* queue cell count */
	/* ----------------------------------- */
	uint16_t src_port; /* source port */
	uint16_t dst_port; /* destination port */
	/* ----------------------------------- */
	uint32_t tx_bytes_lo; /* TX bytes - low bits */
};

/**< intelemetry response data */
struct int_response {
	uint16_t qlen;		/* queue length */
	uint8_t index : 3;	/* index (3 bits) */
	uint8_t reserved_1 : 5; /* reserved (5 bits) */
	uint8_t pt : 2;		/* port type (2 bits) */
	uint8_t valid : 1;	/* valid flag */
	uint8_t reserved_2 : 5; /* reserved (5 bits) */
	uint32_t tx_bytes;	/* TX bytes */
	uint32_t tx_ts;		/* TX timestamp */
};

/*
 * Get number of metadatas added by switches metadata header
 *
 * @md_hdr [in]: switch metadata header
 * @return: number of metadatas
 */
static inline uint8_t get_md_num_from_md_hdr(struct ifa2_md_hdr *md_hdr)
{
	uint8_t num_mds = max_hop_limit - md_hdr->hop_limit;
	return num_mds;
}

/*
 * Hash function to index telemetry database
 *
 * @device_id [in]: switch device ID
 * @port [in]: port number
 * @return: index in telemetry DB
 */
static inline uint16_t telemetry_db_hash(uint32_t device_id, uint16_t port)
{
	uint16_t idx = (((uint16_t)device_id) & 0x3FFu) | ((((uint16_t)port) & 0x1Fu) << 10);
	return (idx & TELEMETRY_DB_MASK);
}

/*
 * Get TX timestamp (nanoseconds) from switch metadata
 *
 * @md [in]: switch metadata
 * @return: TX timestamp
 */
static inline uint32_t telemetry_get_tx_ts_nano(struct switch_md *md)
{
	return ((uint32_t)(__builtin_bswap16(md->tx_ts_nano_hi))) << 16 |
	       ((uint32_t)(__builtin_bswap16(md->tx_ts_nano_lo)));
}

/*
 * Get TX bytes from switch metadata
 *
 * @md [in]: switch metadata
 * @return: TX bytes
 */
static inline uint32_t telemetry_get_tx_bytes(struct switch_md *md)
{
	/* tx_bytes_hi aren't in use */
	return __builtin_bswap32(((uint32_t)md->tx_bytes_lo));
}

/*
 * Get queue length from switch metadata
 *
 * @md [in]: switch metadata
 * @return: switch queue length
 */
static inline uint32_t telemetry_get_q_len(struct switch_md *md)
{
	/* check switch format type */
	if (((__builtin_bswap32((md->rx_ts_sec_hi))) >> 16) == 0x8000u) {
		/* Mellanox format */
		return ((__builtin_bswap32(md->rx_ts_sec_hi)) & 0xFFFFu) << 16 |
		       (uint32_t)(__builtin_bswap16(md->eg_queue_cell_cnt));
	} else {
		/* Broadcom format */
		return ((uint32_t)(__builtin_bswap16(md->eg_queue_cell_cnt))) << 8;
	}
}

/*
 * process switch metadata and prepare response packet
 *
 * @md [in]: switch metadata
 * @response_pkt [in]: response packet
 * @return: 0 on success, negative value otherwise
 */
static inline int telemetry_process_md(struct switch_md *md, struct int_response *response_pkt)
{
	////###### Put your algorithm code in here ######/////
	////###### The following is an example code for Switch Telemetry handling using last hop metadata ######////

	uint16_t db_idx;
	struct telemetry_db_entry_t *db_entry;
	uint8_t first_packet = 0;
	struct bytes_ts_t new_bytes_ts, old_bytes_ts;
	uint32_t last_access_ts, curr_ts;

	db_idx = telemetry_db_hash(__builtin_bswap32(md->dev_id_high) << 6 | md->dev_id_low,
				   __builtin_bswap16(md->dst_port));
	new_bytes_ts.ts = telemetry_get_tx_ts_nano(md);
	new_bytes_ts.bytes = telemetry_get_tx_bytes(md);
	db_entry = &telemetry_db[db_idx];
	/* check for first use of entry */
	if (db_entry->valid == 0) {
		db_entry->tx_bytes0 = 0;
		db_entry->tx_ts_nano0 = 0;
		db_entry->tx_bytes1 = 0;
		db_entry->tx_ts_nano1 = 0;
		db_entry->last_access_ts = doca_pcc_dev_get_timer_lo();
		db_entry->valid = 1;
		first_packet = 1;
	} else { /* consider case of aging for database entry */
		last_access_ts = db_entry->last_access_ts;
		curr_ts = doca_pcc_dev_get_timer_lo();
		/* consider packet as first if arrival of new packet passed pre-defined time period */
		if (diff_with_wrap32(curr_ts, last_access_ts) > DB_ENTRY_AGING_TIME)
			first_packet = 1;
		db_entry->last_access_ts = curr_ts;
	}

	/* get last metadata from entry */
	old_bytes_ts.bytes = db_entry->tx_bytes0;
	old_bytes_ts.ts = db_entry->tx_ts_nano0;

	/* consider case of invalid metadata timestamp */
	if (!first_packet && diff_with_wrap32(new_bytes_ts.ts, old_bytes_ts.ts) > INVALID_MD_TIME_INTERVAL) {
		response_pkt->valid = 0;
		return -1;
	}

	/* check to handle duplicate metadata by switch */
	if (new_bytes_ts.bytes != old_bytes_ts.bytes) {
		/* store last metadata */
		db_entry->tx_bytes1 = db_entry->tx_bytes0;
		db_entry->tx_ts_nano1 = db_entry->tx_ts_nano0;
	}

	/* store new metadata in database entry */
	db_entry->tx_bytes0 = new_bytes_ts.bytes;
	db_entry->tx_ts_nano0 = new_bytes_ts.ts;
	old_bytes_ts.bytes = db_entry->tx_bytes1;
	old_bytes_ts.ts = db_entry->tx_ts_nano1;

	response_pkt->tx_bytes = diff_with_wrap32(new_bytes_ts.bytes, old_bytes_ts.bytes);
	response_pkt->tx_ts = diff_with_wrap32(new_bytes_ts.ts, old_bytes_ts.ts);
	response_pkt->tx_bytes = __builtin_bswap32(response_pkt->tx_bytes);
	response_pkt->tx_ts = __builtin_bswap32(response_pkt->tx_ts);
	response_pkt->qlen = __builtin_bswap16(telemetry_get_q_len(md) >> 8);
	response_pkt->pt = md->pt & 3;
	response_pkt->valid = !first_packet;
	return 0;
}

/*
 * User callback - packet handler
 */
doca_pcc_dev_error_t doca_pcc_dev_np_user_packet_handler(struct doca_pcc_np_dev_request_packet *in,
							 struct doca_pcc_np_dev_response_packet *out)
{
	struct switch_md *md;
	struct int_response *response_pkt = (struct int_response *)(out->data);
	uint8_t *ifa2_md_hdr = doca_pcc_np_dev_get_l4_header(in) + UDP_HDR_SIZE;
	uint32_t num_mds = get_md_num_from_md_hdr((struct ifa2_md_hdr *)ifa2_md_hdr);

	if (num_mds > 0 && mailbox_done) {
		md = (struct switch_md *)(ifa2_md_hdr + sizeof(struct ifa2_md_hdr));
		telemetry_process_md(md, response_pkt);
		response_pkt->index = (num_mds - 1);
	}

	return DOCA_PCC_DEV_STATUS_OK;
}

/*
 * Called when host sends a mailbox send request.
 * Used to save the hop limit that was set by user in host.
 */
doca_pcc_dev_error_t doca_pcc_dev_user_mailbox_handle(void *request,
						      uint32_t request_size,
						      uint32_t max_response_size,
						      void *response,
						      uint32_t *response_size)
{
	if (request_size != sizeof(uint32_t))
		return DOCA_PCC_DEV_STATUS_FAIL;

	max_hop_limit = *(uint8_t *)(request);
	doca_pcc_dev_printf("Mailbox initiated hop limit = %d\n", max_hop_limit);

	mailbox_done = 1;
	__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);

	(void)(max_response_size);
	(void)(response);
	(void)(response_size);

	return DOCA_PCC_DEV_STATUS_OK;
}
