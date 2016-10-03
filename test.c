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

/*
 * Device classes seem to be constructed using type_init. This is a call to
 * module_init(*, MODULE_INIT_QOM)
 *
 * Macros in qemu/module.h
 * __attribute__((constructor_)), means the function will be called by default
 * before entering main. Horrible horrible horrible!
 *
 * So, register_dso_module_init(e1000_register_types, MODULE_INIT_QOM) will
 * get called. This sets up a list of "dso_inits".
 *
 * This places the function onto a list of functions, with MODULE_INIT_QOM
 * attached. At some point, this function is presumably called.
 *
 * Function for adding a device from the command line is qdev_device_add in
 * qdev-monitor.c
 *
 * I use a bunch of initialisation functions from hw/i386/pc_q35.c to get the
 * appropriate busses set up -- the main initialisation function is called
 * pc_q35_init, and I am slowly cannibalising it.
 */

#include <stdint.h>
#include <stdbool.h>
#include "pcie-debug.h"
#ifndef DUMMY
#include "hw/net/e1000_regs.h"
#endif

#define TARGET_BERI		1
#define TARGET_NATIVE	2

#ifndef BAREMETAL
#include <execinfo.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/endian.h>
#endif

#include <stdbool.h>

#include "block/coroutine.h"
#ifndef DUMMY
#include "qom/object.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/i386/pc.h"
#include "hw/pci-host/q35.h"
#include "qapi/qmp/qerror.h"
#include "qemu/config-file.h"
#include "sysemu/cpus.h"
#endif

#include "pcie-backend.h"
#include "log.h"

#include "baremetal/baremetalsupport.h"
#include "pcie.h"

#ifndef POSTGRES
#include "pciefpga.h"
#include "beri-io.h"
#endif

#include "mask.h"


#ifdef POSTGRES
bool ignore_next_postgres_completion;
bool mask_next_postgres_completion_data;
uint32_t postgres_completion_mask;

#define PG_STATUS_MASK \
	bswap32(~(E1000_STATUS_FD | E1000_STATUS_ASDV_100 | E1000_STATUS_ASDV_1000 \
		| E1000_STATUS_GIO_MASTER_ENABLE ))


/* The capbility list is different for many small reasons, which is why we
 * want this. */

void
print_last_recvd_packet_ids();

#endif

#ifndef DUMMY

static Object *qdev_get_peripheral(void)
{
    static Object *dev;

    if (dev == NULL) {
        dev = container_get(qdev_get_machine(), "/peripheral");
    }

    return dev;
}

static DeviceClass
*qdev_get_device_class(const char **driver, Error **errp)
{
    ObjectClass *oc;
    DeviceClass *dc;

    oc = object_class_by_name(*driver);

    if (!object_class_dynamic_cast(oc, TYPE_DEVICE)) {
        error_setg(errp, "'%s' is not a valid device model name", *driver);
        return NULL;
    }

    if (object_class_is_abstract(oc)) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE, "driver",
                   "non-abstract device type");
        return NULL;
    }

    dc = DEVICE_CLASS(oc);
    if (dc->cannot_instantiate_with_device_add_yet ||
        (qdev_hotplug && !dc->hotpluggable)) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE, "driver",
                   "pluggable device type");
        return NULL;
    }

    return dc;
}
#endif


#ifndef BAREMETAL
#ifndef DUMMY
MachineState *current_machine;
#endif
#endif
volatile uint8_t *led_phys_mem;


static inline void
write_leds(uint32_t data)
{
#ifdef BERI
	*led_phys_mem = ~data;
#endif
}

#ifndef DUMMY
long
timespec_diff_in_ns(struct timespec *left, struct timespec *right) {
	return 1000000000L * ((right->tv_sec) - (left->tv_sec)) +
		(right->tv_nsec - left->tv_nsec);
}
#endif

extern int last_packet;


enum packet_response {
	PR_NO_RESPONSE, PR_RESPONSE
};

struct PacketGeneratorState {
	PCIDevice *pci_dev;

	uint32_t next_read;
	int32_t device_id;
};

void
initialise_packet_generator_state(struct PacketGeneratorState *state)
{
	state->next_read = 0;
	state->device_id = -1;
}

enum packet_response
generate_packet(struct PacketGeneratorState *state, struct RawTLP *out)
{
	int i;

	if (state->device_id == -1) {
		return PR_NO_RESPONSE;
	}

	create_memory_read_header(out, 1, state->device_id, 0, 0, 0xF,
		state->next_read);

	state->next_read += 4;

	return PR_RESPONSE;
}

enum packet_response
respond_to_packet(struct PacketGeneratorState *state, struct RawTLP *in,
	struct RawTLP *out)
{
	int i, tlp_in_len = 0, bytecount;
	enum tlp_direction dir;
	enum tlp_completion_status completion_status;
	bool read_error = false;
	bool write_error = false;
	bool ignore_next_io_completion = false;
	bool mask_next_io_completion_data = false;
	uint16_t length, requester_id;
	uint32_t io_completion_mask, loweraddress;
	uint64_t addr, req_addr, data_buffer;

	struct TLP64DWord0 *dword0 = (struct TLP64DWord0 *)in->header;
	struct TLP64RequestDWord1 *request_dword1 =
		(struct TLP64RequestDWord1 *)(in->header + 1);
	struct TLP64ConfigRequestDWord2 *config_request_dword2 =
		(struct TLP64ConfigRequestDWord2 *)(in->header + 2);

	struct TLP64ConfigReq *config_req = (struct TLP64ConfigReq *)in->header;
	struct TLP64DWord0 *h0bits = &(config_req->header0);
	struct TLP64RequestDWord1 *req_bits = &(config_req->req_header);

	enum packet_response response = PR_NO_RESPONSE;

#ifndef DUMMY
	PCIIORegion *pci_io_region;
	MemoryRegion *target_region;
	hwaddr rel_addr;
#endif

	out->header_length = 0;
	out->data_length = 0;

	/* Has to be static due to the way reading over IO space works. */
	static int card_reg = -1;

	dir = ((dword0->fmt & 2) >> 1);
	const char *direction_string = (dir == TLPD_READ) ? "read" : "write";

	switch (dword0->type) {
	case M:
		assert(dword0->length == 1);
		/* This isn't in the spec, but seems to be all we've found in our
		 * trace. */

		bytecount = 0;
#ifndef DUMMY
		/* TODO: Different operation for flash? */
		pci_io_region = &(state->pci_dev->io_regions[0]);
		assert(pci_io_region->addr != PCI_BAR_UNMAPPED);
		assert(in->header[2] >= pci_io_region->addr);
		target_region = pci_io_region->memory;
		rel_addr = in->header[2] - pci_io_region->addr;
		loweraddress = rel_addr;
#endif

		if (dir == TLPD_READ) {
#ifdef DUMMY
			read_error = false;
			out->data[0] = 0xBEDEBEDE;
#else
			read_error = io_mem_read(target_region, rel_addr, &data_buffer, 4);
			out->data[0] = data_buffer;
			response = PR_RESPONSE;
#endif

#ifdef POSTGRES
			if (read_error) {
				print_last_recvd_packet_ids();
			}

			if (rel_addr == 0x0) {
				mask_next_postgres_completion_data = true;
				postgres_completion_mask =
					bswap32(~uint32_mask_enable_bits(19, 19));
				/* 19 is apparently a software controllable IO pin, so I
				 * don't think we particularly care. */
			} else if (rel_addr == 0x8) {
				mask_next_postgres_completion_data = true;
				postgres_completion_mask = PG_STATUS_MASK;
			} else if (rel_addr == 0x10 || rel_addr == 0x5B58) {
				/* 1) EEPROM or Flash
				 * 2) Second software semaphore, not present on this
				 * card.
				 */
				ignore_next_postgres_completion = true;
			} else if (rel_addr == 0x8) {
				mask_next_postgres_completion_data = true;
				postgres_completion_mask = PG_STATUS_MASK;
			}
#endif
			assert(!read_error);

			for (i = 0; i < 4; ++i) {
				if ((request_dword1->firstbe >> i) & 1) {
					if (bytecount == 0) {
						loweraddress += i;
					}
					++bytecount;
				}
			}

			out->header_length = 12;
			out->data_length = 4;
			create_completion_header(out, dir, state->device_id,
				TLPCS_SUCCESSFUL_COMPLETION, bytecount, requester_id,
				req_bits->tag, loweraddress);
		} else { /* dir == TLPD_WRITE */
			io_mem_write(target_region, rel_addr, bswap32(in->data[0]), 4);
		}

		break;
	case CFG_0:
		assert(dword0->length == 1);
		response = PR_RESPONSE;
		requester_id = request_dword1->requester_id;

		req_addr = config_request_dword2->ext_reg_num;
		req_addr = (req_addr << 8) | config_request_dword2->reg_num;

		if ((config_request_dword2->device_id & uint32_mask(3)) == 0) {
			/* Mask to get function num -- we are 0 */
			completion_status = TLPCS_SUCCESSFUL_COMPLETION;
			state->device_id = config_request_dword2->device_id;

			if (dir == TLPD_READ) {
				out->data_length = 4;
#ifdef DUMMY
				out->data[0] = 0xBEDEBEDE;
#else
				out->data[0] = pci_host_config_read_common(
					state->pci_dev, req_addr, req_addr + 4, 4);
#endif

#ifdef POSTGRES
				if (req_addr == 0 || req_addr == 0xC) {
					/* Model number and ?cacheline size? */
					mask_next_postgres_completion_data = true;
					postgres_completion_mask = 0xFFFF00FF;
				} else if (req_addr == 4) {
					mask_next_postgres_completion_data = true;
					postgres_completion_mask = 0x00FFFFFF;
				} else if (req_addr == 8) {
					/* Revision ID */
					mask_next_postgres_completion_data = true;
					postgres_completion_mask = 0x00FFFFFF;
				} else if (req_addr == 0x2C) {
					/* Subsystem ID and Subsystem vendor ID */
					ignore_next_postgres_completion = true;
				}
#endif

			} else {
				out->data_length = 0;
#ifndef DUMMY
				for (i = 0; i < 4; ++i) {
					if ((request_dword1->firstbe >> i) & 1) {
						pci_host_config_write_common(
							state->pci_dev, req_addr + i, req_addr + 4,
							(in->data[0] >> ((3 - i) * 8)) & 0xFF, 1);
					}
				}
#endif
			}
		}
		else {
			completion_status = TLPCS_UNSUPPORTED_REQUEST;
			out->data_length = 0;
		}

		out->header_length = 12;
		create_completion_header(out, dir, state->device_id, completion_status,
			4, requester_id, req_bits->tag, 0);

		break;
	case IO:
		assert(request_dword1->firstbe == 0xf); /* Only seen trace. */

		response = PR_RESPONSE;
		out->header_length = 12;

		/*
		 * The process for interacting with the device over IO is rather
		 * convoluted.
		 *
		 * 1) A packet is sent writing an address to a register.
		 * 2) A completion happens.
		 *
		 * 3) A packet is then sent reading or writing another register.
		 * 4) The completion for this is effectively for the address that
		 * was written in 1).
		 *
		 * So we need to ignore the completion for the IO packet after the
		 * completion for 2)
		 *
		 */
#ifndef DUMMY
		req_addr = in->header[2];
		pci_io_region = &(state->pci_dev->io_regions[2]);
		assert(pci_io_region->addr != PCI_BAR_UNMAPPED);
		if (req_addr < pci_io_region->addr) {
			PDBG("Trying to map req with addr %lx in BAR with addr %lx.",
				req_addr, pci_io_region->addr);
			PDBG("Last packet: %d", last_packet);
		}
		assert(req_addr >= pci_io_region->addr);
		target_region = pci_io_region->memory;
		rel_addr = req_addr - pci_io_region->addr;
#endif

		if (dir == TLPD_WRITE) {
			out->data_length = 0;
#ifndef DUMMY
			assert(io_mem_write(target_region, rel_addr, in->data[0], 4)
				== false);
#endif
		} else {
			out->data_length = 4;
#ifdef DUMMY
			out->data[0] = 0xBEDEBEDE;
#else
			assert(io_mem_read(target_region, rel_addr, &data_buffer, 4)
				== false);
			out->data[0] = data_buffer;
#endif
		}

#ifdef POSTGRES
		if (ignore_next_io_completion) {
			ignore_next_io_completion = false;
			ignore_next_postgres_completion = true;
		}
#endif

		create_completion_header(out, dir, state->device_id,
			TLPCS_SUCCESSFUL_COMPLETION, 4, requester_id, req_bits->tag, 0);

#ifdef POSTGRES
		if (dir == TLPD_WRITE && card_reg == 0x10) {
			ignore_next_io_completion = true;
		} else if (dir == TLPD_READ && card_reg == 0x8) {
			mask_next_postgres_completion_data = true;
			postgres_completion_mask = PG_STATUS_MASK;
		}
#endif

		break;
	case CPL:
		break;
	default:
		log_log(LS_RECV_UNKNOWN, LIF_NONE, 0, LOG_NEWLINE);
	}

	for (i = 0; i < out->data_length / 4; ++i) {
		out->data[i] = bswap32(out->data[i]);
	}

	return response;
}

void coroutine_fn process_packet(void *opaque)
{
	while (true) {
		printf("Hello, coroutine world!\n");
		qemu_coroutine_yield();
	}
}

void enter_co_bh(void *opaque) {
	Coroutine *co = opaque;
	qemu_coroutine_enter(co, NULL);
}

int
main(int argc, char *argv[])
{
	log_set_strings(log_strings);
	puts("Starting.");
	/*const char *driver = "e1000-82540em";*/
#ifndef DUMMY
	const char *driver = "e1000e";
	const char *nic_id = "the-e1000e";
	const char *netdev_id = "the-netdev";

	MachineClass *machine_class;
    DeviceClass *dc;
    DeviceState *dev;
    Error *err = NULL;

	/* Initiliase main loop, which has to run to shuttle data between NIC and
	 * client. */

	qemu_init_main_loop(&err);
	assert(err == NULL);

	/* This sets up a load of mutexes and condition variables for the main
	 * loop. Locking of the iothread seems to have to happen directly after
	 * it. I have no idea why. */
	qemu_init_cpu_loop();
    qemu_mutex_lock_iothread();

	/* This needs to be called, otherwise the types are never registered. */
	module_call_init(MODULE_INIT_QOM);

    qemu_add_opts(&qemu_netdev_opts);
    qemu_add_opts(&qemu_net_opts);

	/* Stuff needs to exist within the context of a mchine, apparently. The
	 * device attempts to realize the machine within the course of getting
	 * realized itself
	 */
    module_call_init(MODULE_INIT_MACHINE);
    machine_class = find_default_machine();

	printf("Initialised modules, found default machine.\n");

	current_machine = MACHINE(object_new(object_class_get_name(
                          OBJECT_CLASS(machine_class))));

	printf("Created machine, attached to root object.\n");

    object_property_add_child(object_get_root(), "machine",
                              OBJECT(current_machine), &error_abort);

	printf("Attached machine to root object.\n");

	/* This sets up the appropriate address spaces. */
	cpu_exec_init_all();

	printf("Done cpu init.\n");

	MemoryRegion *pci_memory;
	pci_memory = g_new(MemoryRegion, 1);
	memory_region_init(pci_memory, NULL, "my-pci-memory", UINT64_MAX);

	printf("Created pci memory region.\n");

	// Something to do with interrupts
	GSIState *gsi_state = g_malloc0(sizeof(*gsi_state));
	qemu_irq *gsi = qemu_allocate_irqs(gsi_handler, gsi_state, GSI_NUM_PINS);

	printf("Done gsi stuff.\n");

	Q35PCIHost *q35_host;
	q35_host = Q35_HOST_DEVICE(qdev_create(NULL, TYPE_Q35_HOST_DEVICE));
    /*q35_host->mch.ram_memory = ram_memory;*/
    q35_host->mch.pci_address_space = pci_memory;
    q35_host->mch.system_memory = get_system_memory();
    q35_host->mch.address_space_io = get_system_io();
	PDBG("System IO name: %s", get_system_io()->name);
    /*q35_host->mch.below_4g_mem_size = below_4g_mem_size;*/
    /*q35_host->mch.above_4g_mem_size = above_4g_mem_size;*/
    /*q35_host->mch.guest_info = guest_info;*/

	printf("Created q35.\n");

	// Actually get round to creating the bus!
	PCIHostState *phb;
	PCIBus *pci_bus;

    qdev_init_nofail(DEVICE(q35_host));
    phb = PCI_HOST_BRIDGE(q35_host);
    pci_bus = phb->bus;

	printf("Created bus.\n");

	if (net_init_clients() < 0) {
		printf("Failed to initialise network clients :(\n");
		exit(1);
	}
	printf("Network clients initialised.\n");

	/* Create a client netdev */
	struct Netdev netdev;
	struct NetClientOptions net_client_options;
	struct NetdevUserOptions nuo;

	netdev.id = (char *)netdev_id;
	netdev.opts = &net_client_options;

	net_client_options.kind = NET_CLIENT_OPTIONS_KIND_USER;
	net_client_options.user = &nuo;

	memset(&nuo, 0, sizeof(nuo));
	nuo.has_hostname = false;
	nuo.has_q_restrict = true;
	nuo.has_q_restrict = false;
	nuo.has_ip = false;
	nuo.has_net = false;
	nuo.has_host = false;
	nuo.has_tftp = false;
	nuo.has_bootfile = false;
	nuo.has_dhcpstart = false;
	nuo.has_dns = false;
	nuo.has_dnssearch = false;
	nuo.has_smb = false;

	net_client_netdev_init(&netdev, &err);

	assert(err == NULL);

    /* find driver */
    dc = qdev_get_device_class(&driver, &err);
    if (!dc) {
		printf("Didn't find NIC device class -- failing :(\n");
        return 1;
    }

	printf("Found device class.\n");

    /* find bus */
	if (!pci_bus /*|| qbus_is_full(bus)*/) {
		error_setg(&err, "No '%s' bus found for device '%s'",
			dc->bus_type, driver);
		return 2;
	}

	printf("Creating device...\n");
    /* create device */
    dev = DEVICE(object_new(driver));

	printf("Setting parent bus...\n");

    if (pci_bus) {
        qdev_set_parent_bus(dev, &(pci_bus->qbus));
    }

	printf("Setting device nic_id...\n");
	dev->id = nic_id;

	if (dev->id) {
		object_property_add_child(qdev_get_peripheral(), dev->id,
								  OBJECT(dev), &err);
		assert(err == NULL);
	}

	object_property_set_str(OBJECT(dev), netdev_id, "netdev", &err);
	if (err != NULL) {
		qerror_report_err(err);
		error_free(err);
		assert(false);
	}

	printf("Setting device realized...\n");
	// This will realize the device if it isn't already, shockingly.
	object_property_set_bool(OBJECT(dev), true, "realized", &err);

	PCIDevice *pci_dev = PCI_DEVICE(dev);
	/* Use pci_host_config read common to reply to read responses.
	 * This calls the config_read function on the device.
	 * For the e1000e, this is a thin wrapper over pci_default_read_config,
	 * from hw/pci/pci.c
	 */
	printf("%x.\n", pci_host_config_read_common(pci_dev, 0, 4, 4));
#endif // not DUMMY

    int init = pcie_hardware_init(argc, argv, &physmem);
    if (init)
    	return init;

	int i, send_result;
	int header_length, data_length;
	bool ignore_next_io_completion = false;
	bool mask_next_io_completion_data = false;
	uint16_t length, device_id, requester_id;
	uint32_t io_completion_mask, loweraddress;
	uint64_t addr, req_addr;

	enum packet_response response;
	enum tlp_data_alignment alignment;

	TLPQuadWord tlp_in_quadword[32];
	TLPQuadWord tlp_out_header[2];
	TLPQuadWord tlp_out_data[16];

	struct RawTLP raw_tlp_in;
	struct RawTLP raw_tlp_out;
	raw_tlp_out.header = (TLPDoubleWord *)tlp_out_header;
	raw_tlp_out.data = (TLPDoubleWord *)tlp_out_data;

	int received_count = 0;
	write_leds(received_count);

	struct PacketGeneratorState packet_generator_state;
	initialise_packet_generator_state(&packet_generator_state);
	packet_generator_state.pci_dev = pci_dev;

	drain_pcie_core();
	puts("PCIe Core Drained. Let's go.");

	Coroutine *co = qemu_coroutine_create(process_packet);
	QEMUBH *start_bh = qemu_bh_new(enter_co_bh, co);

	while (1) {
		qemu_bh_schedule(start_bh);
		main_loop_wait(false);
	}

	while (1) {
		wait_for_tlp(tlp_in_quadword, sizeof(tlp_in_quadword), &raw_tlp_in);

#ifdef POSTGRES
		if (is_raw_tlp_trace_finished(&raw_tlp_in)) {
			PDBG("Reached end of trace! Checked %d TLPs.", TLPS_CHECKED);
			exit(0);
		}
#endif

		if (is_raw_tlp_valid(&raw_tlp_in)) {
			response = respond_to_packet(&packet_generator_state, &raw_tlp_in,
				&raw_tlp_out);
		} else {
			main_loop_wait(true); /* true means non blocking. */
			/*response = generate_packet(&packet_generator_state, &raw_tlp_out);*/
			response = PR_NO_RESPONSE;
		}

		if (response != PR_NO_RESPONSE) {
			send_result = send_tlp(&raw_tlp_out);
			assert(send_result != -1);
		}
	}

	return 0;
}
