/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (C) 2021-2023 Caleb Connolly <caleb@connolly.tech>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http:// www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gpt-utils.h"
#include "crc32.h"
#include "utils.h"

#include "bootctrl.h"

#define BOOTDEV_DIR	  "/dev/disk/by-partlabel"
#define BOOT_IMG_PTN_NAME "boot_"
#define LUN_NAME_END_LOC  14
#define BOOT_SLOT_PROP	  "slot_suffix"
#define MISC_DEVICE	  "/dev/disk/by-partlabel/misc"

#define MAX_CMDLINE_SIZE  4096
#define BOOTLOADER_CONTROL_OFFSET 2048
#define BOOT_CTRL_MAGIC   0x42414342
#define BOOT_CTRL_VERSION 1
#define DEFAULT_BOOT_ATTEMPTS 7
#define ACTIVE_BOOT_PRIORITY 15
#define ACTIVE_BOOT_TRIES 6

const char *slot_suffix_arr[] = { AB_SLOT_A_SUFFIX, AB_SLOT_B_SUFFIX, NULL };

unsigned get_number_slots();
static int get_current_or_active_slot();
void get_kernel_cmdline_arg(const char *arg, char *buf, const char *def);

struct slot_metadata {
	uint8_t priority : 4;
	uint8_t tries_remaining : 3;
	uint8_t successful_boot : 1;
	uint8_t verity_corrupted : 1;
	uint8_t reserved : 7;
} __attribute__((packed));

struct bootloader_control {
	char slot_suffix[4];
	uint32_t magic;
	uint8_t version;
	uint8_t nb_slot : 3;
	uint8_t recovery_tries_remaining : 3;
	uint8_t merge_status : 3;
	uint8_t reserved0[1];
	struct slot_metadata slot_info[4];
	uint8_t reserved1[8];
	uint32_t crc32_le;
} __attribute__((packed));

static uint32_t bootloader_control_crc(const struct bootloader_control *ctrl)
{
	return efi_crc32(ctrl, offsetof(struct bootloader_control, crc32_le));
}

static bool misc_ctrl_valid(const struct bootloader_control *ctrl)
{
	return ctrl->magic == BOOT_CTRL_MAGIC &&
	       ctrl->version == BOOT_CTRL_VERSION &&
	       ctrl->nb_slot > 0 &&
	       ctrl->nb_slot <= 4 &&
	       ctrl->crc32_le == bootloader_control_crc(ctrl);
}

static int read_misc_bootloader_control(struct bootloader_control *ctrl)
{
	int fd;
	ssize_t rc;

	fd = open(MISC_DEVICE, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", MISC_DEVICE, strerror(errno));
		return -1;
	}

	if (lseek(fd, BOOTLOADER_CONTROL_OFFSET, SEEK_SET) != BOOTLOADER_CONTROL_OFFSET) {
		fprintf(stderr, "Failed to seek %s: %s\n", MISC_DEVICE, strerror(errno));
		close(fd);
		return -1;
	}

	rc = read(fd, ctrl, sizeof(*ctrl));
	close(fd);
	if (rc != sizeof(*ctrl)) {
		fprintf(stderr, "Failed to read bootloader control from %s\n", MISC_DEVICE);
		return -1;
	}

	return 0;
}

static int write_misc_bootloader_control(struct bootloader_control *ctrl)
{
	int fd;
	ssize_t rc;

	ctrl->crc32_le = bootloader_control_crc(ctrl);

	fd = open(MISC_DEVICE, O_WRONLY | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", MISC_DEVICE, strerror(errno));
		return -1;
	}

	if (lseek(fd, BOOTLOADER_CONTROL_OFFSET, SEEK_SET) != BOOTLOADER_CONTROL_OFFSET) {
		fprintf(stderr, "Failed to seek %s: %s\n", MISC_DEVICE, strerror(errno));
		close(fd);
		return -1;
	}

	rc = write(fd, ctrl, sizeof(*ctrl));
	if (rc != sizeof(*ctrl)) {
		fprintf(stderr, "Failed to write bootloader control to %s\n", MISC_DEVICE);
		close(fd);
		return -1;
	}

	fsync(fd);
	close(fd);
	return 0;
}

static void init_default_misc_bootloader_control(struct bootloader_control *ctrl)
{
	unsigned int num_slots = get_number_slots();
	char bootSlotProp[MAX_CMDLINE_SIZE] = { '\0' };
	int current_slot = 0;

	memset(ctrl, 0, sizeof(*ctrl));
	if (num_slots == 0 || num_slots > 4)
		num_slots = 2;

	get_kernel_cmdline_arg(BOOT_SLOT_PROP, bootSlotProp, "N/A");
	for (unsigned int i = 0; slot_suffix_arr[i] != NULL; i++) {
		if (!strncmp(bootSlotProp, slot_suffix_arr[i], strlen(slot_suffix_arr[i]))) {
			current_slot = i;
			break;
		}
	}

	if (current_slot < 0 || (unsigned)current_slot >= num_slots)
		current_slot = 0;

	strncpy(ctrl->slot_suffix, slot_suffix_arr[current_slot], sizeof(ctrl->slot_suffix) - 1);
	ctrl->magic = BOOT_CTRL_MAGIC;
	ctrl->version = BOOT_CTRL_VERSION;
	ctrl->nb_slot = num_slots;

	for (unsigned int i = 0; i < 4; i++) {
		if (i < num_slots) {
			ctrl->slot_info[i].priority = 7;
			ctrl->slot_info[i].tries_remaining = DEFAULT_BOOT_ATTEMPTS;
		}
	}

	ctrl->slot_info[current_slot].successful_boot = 1;
	ctrl->crc32_le = bootloader_control_crc(ctrl);
}

static int load_misc_bootloader_control(struct bootloader_control *ctrl)
{
	if (read_misc_bootloader_control(ctrl))
		return -1;

	if (misc_ctrl_valid(ctrl))
		return 0;

	fprintf(stderr, "Invalid /misc bootloader_control metadata; reinitializing it\n");
	init_default_misc_bootloader_control(ctrl);
	return write_misc_bootloader_control(ctrl);
}

void get_kernel_cmdline_arg(const char *arg, char *buf, const char *def)
{
	int fd;
	char pcmd[MAX_CMDLINE_SIZE];
	char *val, *found, *ptr = buf;
	fd = open("/proc/cmdline", O_RDONLY);
	int rc = read(fd, pcmd, MAX_CMDLINE_SIZE);
	if (rc < 0) {
		fprintf(stderr, "Couldn't open /proc/cmdline: %d (%s)\n", rc, strerror(errno));
		goto error;
	}
	close(fd);
	found = strstr(pcmd, arg);
	if (!found || !(val = strstr(found, "="))) {
		fprintf(stderr, "Couldn't find cmdline arg: '%s'\n", arg);
		goto error;
	}

	val++;
	// no this doesn't handle quotes lol
	while (*val != ' ') {
		*ptr++ = *val++;
	}

	return;

error:
	strcpy(buf, def);
}

/*
 * Returns 0 for no slots, or the number of slots found.
 * Fun semantic note: Having "1" slot (ie just a "boot" partition)
 * is the same as having "no slots".
 *
 * This function will never return 1.
 */
unsigned get_number_slots()
{
	struct dirent *de = NULL;
	DIR *dir_bootdev = NULL;
	static int slot_count = 0;

	// If we've already counted the slots, return the cached value.
	// If there are no slots then we'll always rerun the search...
	if (slot_count > 0)
		return slot_count;

	assert(AB_SLOT_A_SUFFIX[0] == '_');
	assert(AB_SLOT_B_SUFFIX[0] == '_');

	dir_bootdev = opendir(BOOTDEV_DIR);
	// Shouldn't this be an assert?
	if (!dir_bootdev) {
		fprintf(stderr, "%s: Failed to open bootdev dir (%s)\n", __func__, strerror(errno));
		return 0;
	}

	while ((de = readdir(dir_bootdev))) {
		if (de->d_name[0] == '.')
			continue;
		if (!strncmp(de->d_name, BOOT_IMG_PTN_NAME, strlen(BOOT_IMG_PTN_NAME)) &&
		    !!strncmp(de->d_name, "boot_aging\n", strlen("boot_aging"))) {
			slot_count++;
		}
	}

	if (slot_count < 0)
		slot_count = 0;

	closedir(dir_bootdev);

	return slot_count;
}

static int boot_control_check_slot_sanity(unsigned slot)
{
	uint32_t num_slots = get_number_slots();
	if ((num_slots < 1) || (slot > num_slots - 1)) {
		fprintf(stderr, "Invalid slot number %u\n", slot);
		return -1;
	}
	return 0;
}

unsigned get_active_boot_slot()
{
	struct bootloader_control ctrl;
	unsigned int active_slot = 0;
	unsigned int max_priority;

	if (load_misc_bootloader_control(&ctrl))
		return 0;

	max_priority = ctrl.slot_info[0].priority;
	for (uint32_t i = 1; i < ctrl.nb_slot; i++) {
		if (ctrl.slot_info[i].priority > max_priority) {
			max_priority = ctrl.slot_info[i].priority;
			active_slot = i;
		}
	}

	return active_slot;
}

/*
 * The current slot is usually made available via the kernel cmdline. If it isn't for some reason
 * (e.g. because we booted via a secondary bootloader that removes Android cmdline args) then we
 * assume that the active slot is the current slot
 */
static int get_current_or_active_slot()
{
	uint32_t num_slots = 0;
	char bootSlotProp[MAX_CMDLINE_SIZE] = { '\0' };
	unsigned i = 0;
	num_slots = get_number_slots();
	if (num_slots == 0)
		return -ENOENT;
	if (num_slots == 1) {
		// Slot 0 is the only slot around.
		return 0;
	}

	get_kernel_cmdline_arg(BOOT_SLOT_PROP, bootSlotProp, "N/A");
	if (!strncmp(bootSlotProp, "N/A\n", strlen("N/A"))) {
		fprintf(stderr, "%s: Unable to read boot slot property\n", __func__);
		return get_active_boot_slot();
	}

	// Iterate through a list of partitons named as boot+suffix
	// and see which one is currently active.
	for (i = 0; slot_suffix_arr[i] != NULL; i++) {
		if (!strncmp(bootSlotProp, slot_suffix_arr[i], strlen(slot_suffix_arr[i]))) {
			// printf("%s current_slot = %d\n", __func__, i);
			return i;
		}
	}

	// The HAL spec requires that we return a number between
	// 0 to num_slots - 1. Since something went wrong here we
	// are just going to return the default slot.
	return 0;
}

int is_slot_bootable(unsigned slot)
{
	struct bootloader_control ctrl;

	if (boot_control_check_slot_sanity(slot) != 0)
		return -1;

	if (load_misc_bootloader_control(&ctrl))
		return -1;

	if (slot >= ctrl.nb_slot)
		return -1;

	return ctrl.slot_info[slot].tries_remaining != 0;
}

int mark_boot_successful(unsigned slot)
{
	struct bootloader_control ctrl;

	if (boot_control_check_slot_sanity(slot) != 0)
		return -1;

	if (load_misc_bootloader_control(&ctrl))
		return -1;

	if (slot >= ctrl.nb_slot)
		return -1;

	ctrl.slot_info[slot].successful_boot = 1;
	ctrl.slot_info[slot].tries_remaining = 1;
	return write_misc_bootloader_control(&ctrl);
}

const char *get_suffix(unsigned slot)
{
	if (boot_control_check_slot_sanity(slot) != 0)
		return "";
	else
		return slot_suffix_arr[slot];
}


int set_active_boot_slot(unsigned slot, bool ignore_missing_bsg)
{
	struct bootloader_control ctrl;

	(void)ignore_missing_bsg;
	if (boot_control_check_slot_sanity(slot)) {
		fprintf(stderr, "%s: Bad arguments\n", __func__);
		return -1;
	}

	if (load_misc_bootloader_control(&ctrl))
		return -1;

	if (slot >= ctrl.nb_slot)
		return -1;

	for (unsigned int i = 0; i < ctrl.nb_slot; i++) {
		if (i != slot && ctrl.slot_info[i].priority >= ACTIVE_BOOT_PRIORITY)
			ctrl.slot_info[i].priority = ACTIVE_BOOT_PRIORITY - 1;
	}

	ctrl.slot_info[slot].priority = ACTIVE_BOOT_PRIORITY;
	ctrl.slot_info[slot].tries_remaining = ACTIVE_BOOT_TRIES;

	return write_misc_bootloader_control(&ctrl);
}

int set_slot_as_unbootable(unsigned slot)
{
	struct bootloader_control ctrl;

	if (boot_control_check_slot_sanity(slot) != 0)
		return -1;

	if (load_misc_bootloader_control(&ctrl))
		return -1;

	if (slot >= ctrl.nb_slot)
		return -1;

	ctrl.slot_info[slot].successful_boot = 0;
	ctrl.slot_info[slot].tries_remaining = 0;
	return write_misc_bootloader_control(&ctrl);
}

int is_slot_marked_successful(unsigned slot)
{
	struct bootloader_control ctrl;

	if (boot_control_check_slot_sanity(slot) != 0)
		return -1;

	if (load_misc_bootloader_control(&ctrl))
		return -1;

	if (slot >= ctrl.nb_slot)
		return -1;

	return ctrl.slot_info[slot].successful_boot && ctrl.slot_info[slot].tries_remaining;
}

const struct boot_control_module bootctl = {
	.getCurrentSlot = get_current_or_active_slot,
	.markBootSuccessful = mark_boot_successful,
	.setActiveBootSlot = set_active_boot_slot,
	.setSlotAsUnbootable = set_slot_as_unbootable,
	.isSlotBootable = is_slot_bootable,
	.getSuffix = get_suffix,
	.isSlotMarkedSuccessful = is_slot_marked_successful,
	.getActiveBootSlot = get_active_boot_slot,
};
