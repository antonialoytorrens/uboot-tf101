/*
 * Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 */

#include <common.h>
#include <part.h>
#include <chromeos/gpio.h>
#include <chromeos/load_kernel_helper.h>
#include <chromeos/os_storage.h>

/* TODO For load fmap; remove when not used */
#include <chromeos/firmware_storage.h>

/* TODO For strcpy; remove when not used */
#include <linux/string.h>

/* TODO remove when not used */
extern uint64_t get_nvcxt_lba(void);

/* defined in common/cmd_bootm.c */
int do_bootm(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[]);

#include <load_kernel_fw.h>
#include <vboot_nvstorage.h>
#include <vboot_struct.h>

/* This is used to keep u-boot and kernel in sync */
#define SHARED_MEM_VERSION 1

#undef PREFIX
#define PREFIX "load_kernel_wrapper: "

int load_kernel_wrapper_core(LoadKernelParams *params,
			     void *gbb_data, uint64_t gbb_size,
			     uint64_t boot_flags, VbNvContext *nvcxt,
			     uint8_t *shared_data_blob,
			     int bypass_load_kernel)
{
	/*
	 * TODO(clchiou): Hack for bringing up factory; preserve recovery
	 * reason before LoadKernel destroys it. Remove when not needed.
	 */
	uint32_t reason = 0;
	VbNvGet(nvcxt, VBNV_RECOVERY_REQUEST, &reason);

	int status = LOAD_KERNEL_NOT_FOUND;
	block_dev_desc_t *dev_desc;

	memset(params, '\0', sizeof(*params));

	if (!bypass_load_kernel) {
		dev_desc = get_bootdev();
		if (!dev_desc) {
			debug(PREFIX "get_bootdev fail\n");
			goto EXIT;
		}
	}

	params->gbb_data = gbb_data;
	params->gbb_size = gbb_size;

	params->boot_flags = boot_flags;
	params->shared_data_blob = shared_data_blob ? shared_data_blob :
			(uint8_t *) CONFIG_VB_SHARED_DATA_BLOB;
	params->shared_data_size = CONFIG_VB_SHARED_DATA_SIZE;

	params->bytes_per_lba = get_bytes_per_lba();
	params->ending_lba = get_ending_lba();

	params->kernel_buffer = (uint8_t *) CONFIG_LOADADDR;
	params->kernel_buffer_size = CONFIG_MAX_KERNEL_SIZE;

	params->nv_context = nvcxt;

	debug(PREFIX "call LoadKernel() with parameters...\n");
	debug(PREFIX "shared_data_blob:     0x%p\n",
			params->shared_data_blob);
	debug(PREFIX "bytes_per_lba:        %d\n",
			(int) params->bytes_per_lba);
	debug(PREFIX "ending_lba:           0x%08x\n",
			(int) params->ending_lba);
	debug(PREFIX "kernel_buffer:        0x%p\n",
			params->kernel_buffer);
	debug(PREFIX "kernel_buffer_size:   0x%08x\n",
			(int) params->kernel_buffer_size);
	debug(PREFIX "boot_flags:           0x%08x\n",
			(int) params->boot_flags);

	if (!bypass_load_kernel) {
		status = LoadKernel(params);
	} else {
		status = LOAD_KERNEL_SUCCESS;
		params->partition_number = 2;
	}

EXIT:
	debug(PREFIX "LoadKernel status: %d\n", status);
	if (status == LOAD_KERNEL_SUCCESS) {
		debug(PREFIX "partition_number:   0x%08x\n",
				(int) params->partition_number);
		debug(PREFIX "bootloader_address: 0x%08x\n",
				(int) params->bootloader_address);
		debug(PREFIX "bootloader_size:    0x%08x\n",
				(int) params->bootloader_size);

		if (params->partition_number == 2) {
			setenv("kernelpart", "2");
			setenv("rootpart", "3");
		} else if (params->partition_number == 4) {
			setenv("kernelpart", "4");
			setenv("rootpart", "5");
		} else {
			debug(PREFIX "unknown kernel partition: %d\n",
					(int) params->partition_number);
			status = LOAD_KERNEL_NOT_FOUND;
		}
	}

	/*
	 * TODO(clchiou): This is an urgent hack for bringing up factory. We
	 * fill in data that will be used by kernel at last 1MB space.
	 *
	 * Rewrite this part after the protocol specification between
	 * Chrome OS firmware and kernel is finalized.
	 */
	if (status == LOAD_KERNEL_SUCCESS) {
		DECLARE_GLOBAL_DATA_PTR;

		void *kernel_shared_data = (void*)
			gd->bd->bi_dram[CONFIG_NR_DRAM_BANKS-1].start +
			gd->bd->bi_dram[CONFIG_NR_DRAM_BANKS-1].size - SZ_1M;

		struct {
			uint32_t total_size;
			uint8_t  signature[10];
			uint16_t version;
			uint64_t nvcxt_lba;
			uint16_t vbnv[2];
			uint8_t  nvcxt_cache[VBNV_BLOCK_SIZE];
			uint8_t  write_protect_sw;
			uint8_t  recovery_sw;
			uint8_t  developer_sw;
			uint8_t  binf[5];
			uint32_t chsw;
			uint8_t  hwid[256];
			uint8_t  fwid[256];
			uint8_t  frid[256];
			uint32_t fmap_base;
			uint8_t  shared_data_body[CONFIG_LENGTH_FMAP];
		} __attribute__((packed)) *sd = kernel_shared_data;

		int i;

		debug(PREFIX "kernel shared data at %p\n", kernel_shared_data);

		memset(sd, '\0', sizeof(*sd));

		strcpy((char*) sd->signature, "CHROMEOS");
		sd->version = SHARED_MEM_VERSION;

		/*
		 * chsw bit value
		 *   bit 0x00000002 : recovery button pressed
		 *   bit 0x00000020 : developer mode enabled
		 *   bit 0x00000200 : firmware write protect disabled
		 */
		if (params->boot_flags & BOOT_FLAG_RECOVERY)
			sd->chsw |= 0x002;
		if (params->boot_flags & BOOT_FLAG_DEVELOPER)
			sd->chsw |= 0x020;
		sd->chsw |= 0x200; /* so far write protect is disabled */

		strcpy((char*) sd->hwid, CONFIG_CHROMEOS_HWID);
		strcpy((char*) sd->fwid, "ARM Firmware ID");
		strcpy((char*) sd->frid, "ARM Read-Only Firmware ID");

		sd->binf[0] = 0; /* boot reason; always 0 */
		if (params->boot_flags & BOOT_FLAG_RECOVERY) {
			sd->binf[1] = 0; /* active main firmware */
			sd->binf[3] = 0; /* active firmware type */
		} else {
			sd->binf[1] = 1; /* active main firmware */
			sd->binf[3] = 1; /* active firmware type */
		}
		sd->binf[2] = 0; /* active EC firmware */
		sd->binf[4] = reason;

		sd->write_protect_sw =
			is_firmware_write_protect_gpio_asserted();
		sd->recovery_sw = is_recovery_mode_gpio_asserted();
		sd->developer_sw = is_developer_mode_gpio_asserted();

		sd->vbnv[0] = 0;
		sd->vbnv[1] = VBNV_BLOCK_SIZE;

		firmware_storage_t file;
		firmware_storage_init(&file);
		firmware_storage_read(&file,
				CONFIG_OFFSET_FMAP, CONFIG_LENGTH_FMAP,
				sd->shared_data_body);
		file.close(file.context);
		sd->fmap_base = (uint32_t)sd->shared_data_body;

		sd->total_size = sizeof(*sd);

		sd->nvcxt_lba = get_nvcxt_lba();

		memcpy(sd->nvcxt_cache,
				params->nv_context->raw, VBNV_BLOCK_SIZE);

		debug(PREFIX "chsw     %08x\n", sd->chsw);
		for (i = 0; i < 5; i++)
			debug(PREFIX "binf[%2d] %08x\n", i, sd->binf[i]);
		debug(PREFIX "vbnv[ 0] %08x\n", sd->vbnv[0]);
		debug(PREFIX "vbnv[ 1] %08x\n", sd->vbnv[1]);
		debug(PREFIX "fmap     %08llx\n", sd->fmap_start_address);
		debug(PREFIX "nvcxt    %08llx\n", sd->nvcxt_lba);
		debug(PREFIX "nvcxt_c  ");
		for (i = 0; i < VBNV_BLOCK_SIZE; i++)
			debug("%02x", sd->nvcxt_cache[i]);
		putc('\n');
	}

	return status;
}

/* Maximum kernel command-line size */
#define CROS_CONFIG_SIZE 4096

/* Size of the x86 zeropage table */
#define CROS_PARAMS_SIZE 4096

int load_kernel_config(uint64_t bootloader_address)
{
	char buf[80 + CROS_CONFIG_SIZE];

	strcpy(buf, "setenv bootargs ${bootargs} ");

	/* Use the bootloader address to find the kernel config location. */
	strcat(buf, (char *)(bootloader_address - CROS_PARAMS_SIZE -
			CROS_CONFIG_SIZE));

	/*
	 * Use run_command instead of setenv because we need variable
	 * substitutions.
	 * TODO: Do more variable substitutions for the bug:
	 * http://crosbug.com/14022
	 */
	if (run_command(buf, 0)) {
		debug(PREFIX "run_command(%s) fail\n", buf);
		return 1;
	}
	return 0;
}

int load_kernel_wrapper(LoadKernelParams *params,
			void *gbb_data, uint64_t gbb_size,
			uint64_t boot_flags, VbNvContext *nvcxt,
			uint8_t *shared_data_blob)
{
	return load_kernel_wrapper_core(params, gbb_data, gbb_size, boot_flags,
					nvcxt, shared_data_blob, 0);
}

void boot_kernel(LoadKernelParams *params)
{
	char load_address[32];
	char *argv[2] = { "bootm", load_address };

	debug(PREFIX "boot_kernel\n");
	debug(PREFIX "kernel_buffer:      0x%p\n",
			params->kernel_buffer);
	debug(PREFIX "bootloader_address: 0x%08x\n",
			(int) params->bootloader_address);

	if (load_kernel_config(params->bootloader_address)) {
		debug(PREFIX "error: load kernel config failed\n");
		return;
	}

	/*
	 * FIXME: So far bootloader in kernel partition isn't really a
	 * bootloader; instead, it is merely a u-boot scripts that sets kernel
	 * parameters. And therefore we still have to boot kernel to here
	 * by calling do_bootm.
	 */
	sprintf(load_address, "0x%p", params->kernel_buffer);
	debug(PREFIX "run command: %s %s\n", argv[0], argv[1]);
	do_bootm(NULL, 0, sizeof(argv)/sizeof(*argv), argv);

	debug(PREFIX "error: do_bootm() returned\n");
	while (1);
}