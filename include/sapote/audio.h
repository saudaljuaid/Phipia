/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_AUDIO_H
#define SAPOTE_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The first Sapote driver that lets a device write into kernel memory on its
 * own initiative for a reason other than storage or networking.
 *
 * High Definition Audio does not have registers a driver can ask a codec
 * through. It has two rings in memory: the controller reads commands out of
 * one and writes the codecs' answers into the other, both by bus-mastering
 * DMA. Sapote has no IOMMU, so a device that can write one ring can write
 * anything, and the whole of this driver's shape follows from that: the rings
 * are typed DMA allocations, bus mastering is enabled only with those exact
 * allocations declared, the engines are stopped and the controller is reset
 * before bus mastering is withdrawn, and the memory is only reclaimed after
 * that.
 *
 * What it proves is a real conversation: every codec the controller reports is
 * asked who it is, and answers with a vendor and device identifier, a
 * revision, and the node numbering of its function groups.
 */

/* HD Audio 1.0a section 3.3.9: fifteen codec addresses on the link. */
#define AUDIO_MAX_CODECS 15U

/*
 * Ring sizes. The specification allows 2, 16 or 256 entries and reports which
 * of them a controller supports; the driver takes the largest it offers, which
 * is what every real driver does and what makes the size-capability field
 * worth reading at all.
 */
#define AUDIO_CORB_MAX_ENTRIES 256U
#define AUDIO_RIRB_MAX_ENTRIES 256U
#define AUDIO_CORB_ENTRY_BYTES 4U
#define AUDIO_RIRB_ENTRY_BYTES 8U

/*
 * How long any one device handshake may take. The specification gives codec
 * enumeration 521 microseconds after the controller leaves reset and a command
 * a bounded response time; a second is far beyond both and turns a device that
 * never answers into a named status rather than a hang.
 */
#define AUDIO_TIMEOUT_NS UINT64_C(1000000000)

/* Controls the pure foundation stage exercises before any device is touched. */
#define AUDIO_CONTROLLED_CONTROLS 10U

enum audio_status {
    AUDIO_STATUS_OK = 0,
    AUDIO_STATUS_NULL_ARGUMENT,
    AUDIO_STATUS_BUSY,
    AUDIO_STATUS_PREREQUISITE,
    AUDIO_STATUS_ABSENT,
    AUDIO_STATUS_CLAIM_FAILURE,
    AUDIO_STATUS_MAPPING_FAILURE,
    AUDIO_STATUS_REGISTER_WINDOW,
    AUDIO_STATUS_VERSION,
    AUDIO_STATUS_RESET_TIMEOUT,
    AUDIO_STATUS_DMA_FAILURE,
    AUDIO_STATUS_BUS_MASTER_GUARD_FAILURE,
    AUDIO_STATUS_BUS_MASTER_FAILURE,
    AUDIO_STATUS_RING_SIZE,
    AUDIO_STATUS_RING_RESET,
    AUDIO_STATUS_RING_START,
    AUDIO_STATUS_CODEC_ABSENT,
    AUDIO_STATUS_VERB_TIMEOUT,
    AUDIO_STATUS_RESPONSE_AUTHENTICATION,
    AUDIO_STATUS_IDENTITY,
    AUDIO_STATUS_TEARDOWN,
    AUDIO_STATUS_RESOURCE_CENSUS,
    AUDIO_STATUS_COUNT
};

struct audio_codec {
    uint8_t address;
    uint8_t first_group_node;
    uint8_t group_node_count;
    uint8_t function_group_type;
    uint32_t vendor_device;
    uint32_t revision;
    bool audio_function_group;
    bool identified;
};

struct audio_proof_result {
    uint32_t version;
    uint32_t capability;
    uint32_t output_streams;
    uint32_t input_streams;
    uint32_t bidirectional_streams;
    uint32_t serial_data_out_signals;
    uint32_t corb_entries;
    uint32_t rirb_entries;
    uint32_t codecs_present;
    uint32_t codecs_identified;
    uint32_t verbs_issued;
    uint32_t responses_received;
    uint32_t controls;
    bool controller_reset;
    bool rings_running;
    bool audio_function_group_found;
    bool device_wrote_response_ring;
    bool bus_master_withdrawn_before_release;
    bool teardown_complete;
    bool resource_census_equal;
    struct audio_codec codecs[AUDIO_MAX_CODECS];
};

bool audio_foundation_self_test(size_t *completed_tests);
enum audio_status audio_prove(struct audio_proof_result *result);
struct audio_proof_result audio_get_proof_result(void);
bool audio_resources_released(void);
const char *audio_status_string(enum audio_status status);

#endif
