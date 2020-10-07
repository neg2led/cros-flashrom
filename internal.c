/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2009 Carl-Daniel Hailfinger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "flash.h"
#include "programmer.h"
#include "hwaccess.h"

#if NEED_PCI == 1
struct pci_dev *pci_dev_find_vendorclass(uint16_t vendor, uint16_t devclass)
{
	struct pci_dev *temp;
	struct pci_filter filter;
	uint16_t tmp2;

	pci_filter_init(NULL, &filter);
	filter.vendor = vendor;

	for (temp = pacc->devices; temp; temp = temp->next)
		if (pci_filter_match(&filter, temp)) {
			/* Read PCI class */
			tmp2 = pci_read_word(temp, 0x0a);
			if (tmp2 == devclass)
				return temp;
		}

	return NULL;
}

struct pci_dev *pci_dev_find(uint16_t vendor, uint16_t device)
{
	struct pci_dev *temp;
	struct pci_filter filter;

	pci_filter_init(NULL, &filter);
	filter.vendor = vendor;
	filter.device = device;

	for (temp = pacc->devices; temp; temp = temp->next)
		if (pci_filter_match(&filter, temp))
			return temp;

	return NULL;
}

struct pci_dev *pci_card_find(uint16_t vendor, uint16_t device,
			      uint16_t card_vendor, uint16_t card_device)
{
	struct pci_dev *temp;
	struct pci_filter filter;

	pci_filter_init(NULL, &filter);
	filter.vendor = vendor;
	filter.device = device;

	for (temp = pacc->devices; temp; temp = temp->next)
		if (pci_filter_match(&filter, temp)) {
			if ((card_vendor ==
			     pci_read_word(temp, PCI_SUBSYSTEM_VENDOR_ID))
			    && (card_device ==
				pci_read_word(temp, PCI_SUBSYSTEM_ID)))
				return temp;
		}

	return NULL;
}
#endif

#if CONFIG_INTERNAL == 1
int force_boardenable = 0;
int force_boardmismatch = 0;

#if IS_X86
void probe_superio(void)
{
	probe_superio_ite();
}

int superio_count = 0;
#define SUPERIO_MAX_COUNT 3

struct superio superios[SUPERIO_MAX_COUNT];

int register_superio(struct superio s)
{
	if (superio_count == SUPERIO_MAX_COUNT)
		return 1;
	superios[superio_count++] = s;
	return 0;
}

#endif

int is_laptop = 0;
int laptop_ok = 1;	/* FIXME: proper whitelisting hasn't been added yet */

static void internal_chip_writeb(const struct flashctx *flash, uint8_t val,
				 chipaddr addr);
static void internal_chip_writew(const struct flashctx *flash, uint16_t val,
				 chipaddr addr);
static void internal_chip_writel(const struct flashctx *flash, uint32_t val,
				 chipaddr addr);
static uint8_t internal_chip_readb(const struct flashctx *flash,
				   const chipaddr addr);
static uint16_t internal_chip_readw(const struct flashctx *flash,
				    const chipaddr addr);
static uint32_t internal_chip_readl(const struct flashctx *flash,
				    const chipaddr addr);
static void internal_chip_readn(const struct flashctx *flash, uint8_t *buf,
				const chipaddr addr, size_t len);

#if defined (__FLASHROM_LITTLE_ENDIAN__)
static const struct par_master par_master_internal = {
		.chip_readb		= internal_chip_readb,
		.chip_readw		= internal_chip_readw,
		.chip_readl		= internal_chip_readl,
		.chip_readn		= internal_chip_readn,
		.chip_writeb		= internal_chip_writeb,
		.chip_writew		= internal_chip_writew,
		.chip_writel		= internal_chip_writel,
		.chip_writen		= fallback_chip_writen,
};
#endif

enum chipbustype internal_buses_supported = BUS_NONE;

static int internal_shutdown(void *data)
{
	return 0;
}
enum chipbustype target_bus;

int internal_init(void)
{
#if defined (__FLASHROM_LITTLE_ENDIAN__)
	int ret = 0;
#endif
	int force_laptop = 0;
	int not_a_laptop = 0;
	char *board_vendor = NULL;
	char *board_model = NULL;
#if IS_X86 || IS_ARM
	const char *cb_vendor = NULL;
	const char *cb_model = NULL;
	int probe_target_bus_later = 0;
#endif
	char *arg;

	arg = extract_programmer_param("boardenable");
	if (arg && !strcmp(arg,"force")) {
		force_boardenable = 1;
	} else if (arg && !strlen(arg)) {
		msg_perr("Missing argument for boardenable.\n");
		free(arg);
		return 1;
	} else if (arg) {
		msg_perr("Unknown argument for boardenable: %s\n", arg);
		free(arg);
		return 1;
	}
	free(arg);

	arg = extract_programmer_param("boardmismatch");
	if (arg && !strcmp(arg,"force")) {
		force_boardmismatch = 1;
	} else if (arg && !strlen(arg)) {
		msg_perr("Missing argument for boardmismatch.\n");
		free(arg);
		return 1;
	} else if (arg) {
		msg_perr("Unknown argument for boardmismatch: %s\n", arg);
		free(arg);
		return 1;
	}
	free(arg);

	arg = extract_programmer_param("laptop");
	if (arg && !strcmp(arg, "force_I_want_a_brick"))
		force_laptop = 1;
	else if (arg && !strcmp(arg, "this_is_not_a_laptop"))
		not_a_laptop = 1;
	else if (arg && !strlen(arg)) {
		msg_perr("Missing argument for laptop.\n");
		free(arg);
		return 1;
	} else if (arg) {
		msg_perr("Unknown argument for laptop: %s\n", arg);
		free(arg);
		return 1;
	}
	free(arg);

	arg = extract_programmer_param("mainboard");
	if (arg && strlen(arg)) {
		lb_vendor_dev_from_string(arg);
	} else if (arg && !strlen(arg)) {
		msg_perr("Missing argument for mainboard.\n");
		free(arg);
		return 1;
	}
	free(arg);

	arg = extract_programmer_param("bus");
	if (arg) {
		if (!strcasecmp(arg,"parallel")) {
			target_bus = BUS_PARALLEL;
		} else if (!strcasecmp(arg,"lpc")) {
			target_bus = BUS_LPC;
		} else if (!strcasecmp(arg,"fwh")) {
			target_bus = BUS_FWH;
		} else if (!strcasecmp(arg,"spi")) {
			target_bus = BUS_SPI;
		} else if (!strcasecmp(arg,"i2c")) {
			target_bus = BUS_PROG;
		} else {
			msg_perr("Unsupported bus: %s\n", arg);
			free(arg);
			return 1;
		}

		free(arg);
	} else {
#if IS_X86 || IS_ARM
		/* The pacc must be initialized before access pci devices. */
		probe_target_bus_later = 1;
#endif
	}

	if (rget_io_perms())
		return 1;
	if (register_shutdown(internal_shutdown, NULL))
		return 1;

#if IS_X86
	/* Default to Parallel/LPC/FWH flash devices. If a known host controller
	 * is found, the host controller init routine sets the
	 * internal_buses_supported bitfield.
	 */
	internal_buses_supported = BUS_NONSPI;

	/* Initialize PCI access for flash enables */
	if (pci_init_common() != 0)
		return 1;
#else
	internal_buses_supported = BUS_NONE;
#endif

#if IS_ARM
	/*
	 * FIXME: CrOS EC probing should not require this "IS_ARM"
	 * and should not depend on the target bus. This is only to satisfy
	 * users and scripts who currently depend on the old "-p internal:bus="
	 * syntax or some default behavior.
	 *
	 * Once everything is finally updated, we should only rely on
	 * alias == ALIAS_EC in order to call cros_ec_probe_*.
	 *
	 * Also, ensure probing does not get confused when removing the
	 * "#if IS_ARM" (see crbug.com/249568).
	 */
	if (!alias && probe_target_bus_later)
		target_bus = BUS_SPI;

	if (target_bus != BUS_SPI) {
		if (!cros_ec_probe_dev())
			return 0;
	}
#endif

	if (try_mtd() == 0) {
		return 0;
	}

#if IS_ARM || IS_MIPS && CONFIG_LINUX_SPI == 1
	/* On the ARM platform, we prefer /dev/spidev if it is supported.
	 * That means, if user specifies
	 *
	 *   1. -p internal programmer
	 *   2. without -p (the default programmer, which is internal too)
	 *
	 * This code would try to auto-detect the /dev/spidevX.Y.
	 * If failed, try processor_flash_enable() then.
	 *
	 * The -p linux_spi still works because the programmer_init() would
	 * call the linux_spi_init() in flashrom.c.
	 */
	if (!programmer_init(PROGRAMMER_LINUX_SPI, NULL)) {
		return 0;
	} else /* if failed, fall through */
#endif
	if (processor_flash_enable()) {
		msg_perr("Processor detection/init failed.\n"
			 "Aborting.\n");
		return 1;
	}

#if IS_X86 || IS_ARM
	/* We look at the cbtable first to see if we need a
	 * mainboard specific flash enable sequence.
	 */
	if ((cb_parse_table(&cb_vendor, &cb_model) == 0) && (board_vendor != NULL) && (board_model != NULL)) {
		if (strcasecmp(board_vendor, cb_vendor) || strcasecmp(board_model, cb_model)) {
			msg_pwarn("Warning: The mainboard IDs set by -p internal:mainboard (%s:%s) do not\n"
				  "         match the current coreboot IDs of the mainboard (%s:%s).\n",
				  board_vendor, board_model, cb_vendor, cb_model);
			if (!force_boardmismatch)
				return 1;
			msg_pinfo("Continuing anyway.\n");
		}
	}
#endif

#if IS_X86
	is_laptop = 2; /* Assume that we don't know by default. */

	dmi_init();

	if (probe_target_bus_later) {
		/* read the target bus value from register. */
		if (get_target_bus_from_chipset(&target_bus)) {
			msg_perr("Cannot get target bus from programmer.\n");
			return 1;
		}
		msg_pdbg("get_target_bus_from_chipset() returns 0x%x.\n",
		         target_bus);
	}

	/* In case Super I/O probing would cause pretty explosions. */
	board_handle_before_superio();

	/* Probe for the Super I/O chip and fill global struct superio. */
	probe_superio();
#else
	/* FIXME: Enable cbtable searching on all non-x86 platforms supported
	 *        by coreboot.
	 * FIXME: Find a replacement for DMI on non-x86.
	 * FIXME: Enable Super I/O probing once port I/O is possible.
	 */
#endif

	/* Check laptop whitelist. */
	board_handle_before_laptop();

	/*
	 * Disable all internal buses by default if we are not sure
	 * this isn't a laptop. Board-enables may override this,
	 * non-legacy buses (SPI and opaque atm) are probed anyway.
	 */
	if (force_laptop || (not_a_laptop && (is_laptop == 2)))
		internal_buses_supported = BUS_NONE;

#if defined (__FLASHROM_LITTLE_ENDIAN__)
#if IS_X86 || IS_MIPS || IS_ARM
	/* try to enable it. Failure IS an option, since not all motherboards
	 * really need this to be done, etc., etc.
	 */
	ret = chipset_flash_enable();
	if (ret == -2) {
		msg_perr("WARNING: No chipset found. Flash detection "
			 "will most likely fail.\n");
	} else if (ret == ERROR_FATAL)
		return ret;

	if (internal_buses_supported & BUS_NONSPI)
		register_par_master(&par_master_internal, internal_buses_supported);

	/* Report if a non-whitelisted laptop is detected that likely uses a legacy bus. */
	if (is_laptop && !laptop_ok) {
		msg_pinfo("========================================================================\n");
		if (is_laptop == 1) {
			msg_pinfo("You seem to be running flashrom on an unknown laptop. Some\n"
				  "internal buses have been disabled for safety reasons.\n\n");
		} else {
			msg_pinfo("You may be running flashrom on an unknown laptop. We could not\n"
				  "detect this for sure because your vendor has not set up the SMBIOS\n"
				  "tables correctly. Some internal buses have been disabled for\n"
				  "safety reasons. You can enforce using all buses by adding\n"
				  "  -p internal:laptop=this_is_not_a_laptop\n"
				  "to the command line, but please read the following warning if you\n"
				  "are not sure.\n\n");
		}
		msg_perr("Laptops, notebooks and netbooks are difficult to support and we\n"
			 "recommend to use the vendor flashing utility. The embedded controller\n"
			 "(EC) in these machines often interacts badly with flashing.\n"
			 "See the manpage and https://flashrom.org/Laptops for details.\n\n"
			 "If flash is shared with the EC, erase is guaranteed to brick your laptop\n"
			 "and write may brick your laptop.\n"
			 "Read and probe may irritate your EC and cause fan failure, backlight\n"
			 "failure and sudden poweroff.\n"
			 "You have been warned.\n"
			 "========================================================================\n");
	}
#if IS_X86

	/* probe for programmers that bridge LPC <--> SPI */
	if (target_bus == BUS_LPC || target_bus == BUS_FWH ||
	    (alias && alias->type == ALIAS_EC)) {
		/* Try to probe via kernel device first */
		if (!cros_ec_probe_dev()) {
			buses_supported &= ~(BUS_LPC|BUS_SPI);
			return 0;
		}
		if (wpce775x_probe_spi_flash(NULL) &&
			mec1308_probe_spi_flash() &&
			ene_probe_spi_flash() &&
			init_superio_ite())
			return 1;	/* EC not found */
		else
			return 0;
	}

#endif

	board_flash_enable(cb_vendor, cb_model);

	if (!(buses_supported & target_bus) &&
		(!alias || (alias && alias->type == ALIAS_NONE))) {
		/* User specified a target bus which is not supported on the
		 * platform or specified an alias which does not enable it.
		 */
		msg_perr("Programmer does not support specified bus\n");
		return 1;
	}

	/* Even if chipset init returns an error code, we don't want to abort.
	 * The error code might have been a warning only.
	 * Besides that, we don't check the board enable return code either.
	 */
	return 0;
#else
	msg_perr("Your platform is not supported yet for the internal "
		 "programmer due to missing\n"
		 "flash_base and top/bottom alignment information.\n"
		 "Aborting.\n");
	return 1;
#endif
#else
	/* FIXME: Remove this unconditional abort once all PCI drivers are
	 * converted to use little-endian accesses for memory BARs.
	 */
	msg_perr("Your platform is not supported yet for the internal "
		 "programmer because it has\n"
		 "not been converted from native endian to little endian "
		 "access yet.\n"
		 "Aborting.\n");
	return 1;
#endif
}
#endif

static void internal_chip_writeb(const struct flashctx *flash, uint8_t val,
				 chipaddr addr)
{
	mmio_writeb(val, (void *) addr);
}

static void internal_chip_writew(const struct flashctx *flash, uint16_t val,
				 chipaddr addr)
{
	mmio_writew(val, (void *) addr);
}

static void internal_chip_writel(const struct flashctx *flash, uint32_t val,
				 chipaddr addr)
{
	mmio_writel(val, (void *) addr);
}

static uint8_t internal_chip_readb(const struct flashctx *flash,
				   const chipaddr addr)
{
	return mmio_readb((void *) addr);
}

static uint16_t internal_chip_readw(const struct flashctx *flash,
				    const chipaddr addr)
{
	return mmio_readw((void *) addr);
}

static uint32_t internal_chip_readl(const struct flashctx *flash,
				    const chipaddr addr)
{
	return mmio_readl((void *) addr);
}

static void internal_chip_readn(const struct flashctx *flash, uint8_t *buf,
				const chipaddr addr, size_t len)
{
	mmio_readn((void *)addr, buf, len);
	return;
}
