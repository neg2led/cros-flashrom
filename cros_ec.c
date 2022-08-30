/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2012 The Chromium OS Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flashchips.h"
#include "flash.h"
#include "fmap.h"
#include "cros_ec.h"
#include "cros_ec_commands.h"
#include "programmer.h"
#include "spi.h"

struct cros_ec_priv *cros_ec_priv;

/* For region larger use async version for FLASH_ERASE */
#define FLASH_SMALL_REGION_THRESHOLD (16 * 1024)

/* 1 if we want the flashrom to call erase_and_write_flash() again. */
static int need_2nd_pass = 0;

/* 1 if EC firmware has RWSIG enabled. */
static int rwsig_enabled = 0;

/* The range of each firmware copy from the image file to update.
 * But re-define the .flags as the valid flag to indicate the firmware is
 * new or not (if flags = 1).
 */
static struct fmap_area fwcopy[4];  // [0] is not used.

/* The names of enum lpc_current_image to match in FMAP area names. */
static const char *sections[] = {
	"UNKNOWN SECTION",  // EC_IMAGE_UNKNOWN -- never matches
	"EC_RO",
	"EC_RW",
};

static struct ec_response_flash_region_info regions[EC_FLASH_REGION_COUNT];

/*
 * Delay after reboot before EC can respond to host command.
 * This value should be large enough for EC to initialize, but no larger than
 * CONFIG_RWSIG_JUMP_TIMEOUT. This way for EC using RWSIG task, we will be
 * able to abort RWSIG jump and stay in RO.
 */
#define EC_INIT_DELAY 800000

/*
 * Delay after a cold reboot which allows RWSIG enabled EC to jump to EC_RW.
 */
#define EC_RWSIG_JUMP_TO_RW_DELAY 3000000

/* Given the range not able to update, mark the corresponding
 * firmware as old.
 */
static void cros_ec_invalidate_copy(unsigned int addr, unsigned int len)
{
	unsigned i;

	for (i = EC_IMAGE_RO; i < ARRAY_SIZE(fwcopy); i++) {
		struct fmap_area *fw = &fwcopy[i];
		if ((addr >= fw->offset && (addr < fw->offset + fw->size)) ||
		    (fw->offset >= addr && (fw->offset < addr + len))) {
			msg_pdbg(" OLD[%s]", sections[i]);
			fw->flags = 0;  // mark as old
		}
	}
}


static int cros_ec_get_current_image(void)
{
	struct ec_response_get_version resp;
	int rc;

	rc = cros_ec_priv->ec_command(EC_CMD_GET_VERSION,
				0, NULL, 0, &resp, sizeof(resp));
	if (rc < 0) {
		msg_perr("CROS_EC cannot get the running copy: rc=%d\n", rc);
		return rc;
	}
	if (resp.current_image == EC_IMAGE_UNKNOWN) {
		msg_perr("CROS_EC gets unknown running copy\n");
		return -1;
	}

	return resp.current_image;
}


int cros_ec_get_region_info(enum ec_flash_region region,
			       struct ec_response_flash_region_info *info)
{
	struct ec_params_flash_region_info req;
	struct ec_response_flash_region_info resp;
	int rc;

	req.region = region;
	rc = cros_ec_priv->ec_command(EC_CMD_FLASH_REGION_INFO,
			      EC_VER_FLASH_REGION_INFO, &req, sizeof(req),
			      &resp, sizeof(resp));
	if (rc < 0) {
		msg_perr("Cannot get the WP_RO region info: %d\n", rc);
		return rc;
	}

	info->offset = resp.offset;
	info->size = resp.size;
	return 0;
}

/**
 * Check if a feature is supported by EC.
 *
 * @param feature	feature code
 * @return < 0 if error, 0 not supported, > 0 supported
 *
 * NOTE: Once it successfully runs, the feature bits are cached. So, if you
 *       want to query a feature that can be different per copy, you need to
 *       cache features per image copy.
 */
static int ec_check_features(int feature)
{
	static struct ec_response_get_features r;
	int rc = 0;

	if (feature < 0 || feature >= (int)sizeof(r.flags) * 8)
		return -1;

	/* We don't cache return code. We retry regardless the return code. */
	if (r.flags[0] == 0)
		rc = cros_ec_priv->ec_command(EC_CMD_GET_FEATURES,
					      0, NULL, 0, &r, sizeof(r));

	if (rc < 0)
		return rc;

	return !!(r.flags[feature / 32] & (1 << (feature % 32)));
}

/**
 * Disable EC rwsig jump.
 *
 * @return 0 if success, <0 if error
 */
static int ec_rwsig_abort()
{
	struct ec_params_rwsig_action p;

	p.action = RWSIG_ACTION_ABORT;
	return cros_ec_priv->ec_command(EC_CMD_RWSIG_ACTION,
				0, &p, sizeof(p), NULL, 0);
}

/**
 * Get the versions of the command supported by the EC.
 *
 * @param cmd		Command
 * @param pmask		Destination for version mask; will be set to 0 on
 *			error.
 * @return 0 if success, <0 if error
 */
static int ec_get_cmd_versions(int cmd, uint32_t *pmask)
{
	struct ec_params_get_cmd_versions pver;
	struct ec_response_get_cmd_versions rver;
	int rc;

	*pmask = 0;

	pver.cmd = cmd;
	rc = cros_ec_priv->ec_command(EC_CMD_GET_CMD_VERSIONS, 0,
			&pver, sizeof(pver), &rver, sizeof(rver));

	if (rc < 0)
		return rc;

	*pmask = rver.version_mask;
	return rc;
}

/* Perform a cold reboot.
 *
 * @param flags		flags to pass to EC_CMD_REBOOT_EC.
 * @return 0 for success, < 0 for command failure.
 */
int cros_ec_cold_reboot(int flags)
{
	struct ec_params_reboot_ec p;

	memset(&p, 0, sizeof(p));
	p.cmd = EC_REBOOT_COLD;
	p.flags = flags;
	return cros_ec_priv->ec_command(EC_CMD_REBOOT_EC, 0, &p, sizeof(p),
					NULL, 0);
}

/* Asks EC to jump to a firmware copy. If target is EC_IMAGE_UNKNOWN,
 * then this functions picks a NEW firmware copy and jumps to it. Note that
 * RO is preferred, then A, finally B.
 *
 * Returns 0 for success.
 */
static int cros_ec_jump_copy(enum ec_current_image target)
{
	struct ec_params_reboot_ec p;
	int rc;
	enum ec_current_image current_image;

	/* Since the EC may return EC_RES_SUCCESS twice if the EC doesn't
	 * jump to different firmware copy. The second EC_RES_SUCCESS would
	 * set the OBF=1 and the next command cannot be executed.
	 * Thus, we call EC to jump only if the target is different.
	 */
	current_image = cros_ec_get_current_image();
	if (current_image < 0)
		return 1;
	if (current_image == target)
		return 0;

	memset(&p, 0, sizeof(p));

	/* Translate target --> EC reboot command parameter */
	switch (target) {
	case EC_IMAGE_RO:
		/*
		 * Do a cold reset instead of JUMP_RO so board enabling
		 * EC_FLASH_PROTECT_ALL_NOW at runtime can clear the WP flag.
		 * This is true for EC enabling RWSIG, where
		 * EC_FLASH_PROTECT_ALL_NOW is applied before jumping into RW.
		 */
		if (rwsig_enabled)
			p.cmd = EC_REBOOT_COLD;
		else
			p.cmd = EC_REBOOT_JUMP_RO;
		break;
	case EC_IMAGE_RW:
		p.cmd = EC_REBOOT_JUMP_RW;
		break;
	default:
		/*
		 * If target is unspecified, set EC reboot command to use
		 * a new image. Also set "target" so that it may be used
		 * to update the priv->current_image if jump is successful.
		 */
		if (fwcopy[EC_IMAGE_RO].flags) {
			p.cmd = EC_REBOOT_JUMP_RO;
			target = EC_IMAGE_RO;
		} else if (fwcopy[EC_IMAGE_RW].flags) {
			p.cmd = EC_REBOOT_JUMP_RW;
			target = EC_IMAGE_RW;
		} else {
			return 1;
		}
		break;
	}

	if (p.cmd == EC_REBOOT_COLD)
		msg_pdbg("Doing a cold reboot instead of JUMP_RO/RW.\n");
	else
		msg_pdbg("CROS_EC is jumping to [%s]\n", sections[target]);

	if (current_image == p.cmd) {
		msg_pdbg("CROS_EC is already in [%s]\n", sections[target]);
		cros_ec_priv->current_image = target;
		return 0;
	}

	rc = cros_ec_priv->ec_command(EC_CMD_REBOOT_EC,
				      0, &p, sizeof(p), NULL, 0);
	if (rc < 0) {
		msg_perr("CROS_EC cannot jump/reboot to [%s]:%d\n",
			 sections[target], rc);
		return rc;
	}

	/* Sleep until EC can respond to host command, but just before
	 * CONFIG_RWSIG_JUMP_TIMEOUT if EC is using RWSIG task. */
	usleep(EC_INIT_DELAY);

	/* Abort RWSIG jump for EC that use it. Normal EC will ignore it. */
	if (target == EC_IMAGE_RO && rwsig_enabled) {
		msg_pdbg("Aborting RWSIG jump.\n");
		ec_rwsig_abort();
	}

	msg_pdbg("CROS_EC jumped/rebooted to [%s]\n", sections[target]);
	cros_ec_priv->current_image = target;

	return EC_RES_SUCCESS;
}

static int cros_ec_restore_wp(void *data)
{
	msg_pdbg("Restoring EC soft WP.\n");

	struct flashctx *flash = data;

	struct flashrom_wp_cfg *cfg = NULL;
	if (flashrom_wp_cfg_new(&cfg) != FLASHROM_WP_OK)
		return 1;
	flashrom_wp_set_mode(cfg, FLASHROM_WP_MODE_HARDWARE);

	enum flashrom_wp_result ret = flashrom_wp_write_cfg(flash, cfg);
	flashrom_wp_cfg_release(cfg);

	return (ret != FLASHROM_WP_OK);
}

static int cros_ec_wp_is_enabled(void)
{
	struct ec_params_flash_protect p;
	struct ec_response_flash_protect r;
	int rc;

	memset(&p, 0, sizeof(p));
	rc = cros_ec_priv->ec_command(EC_CMD_FLASH_PROTECT,
			EC_VER_FLASH_PROTECT, &p, sizeof(p), &r, sizeof(r));
	if (rc < 0) {
		msg_perr("FAILED: Cannot get the write protection status: %d\n",
			 rc);
		return -1;
	} else if (rc < (int)sizeof(r)) {
		msg_perr("FAILED: Too little data returned (expected:%zd, "
			 "actual:%d)\n", sizeof(r), rc);
		return -1;
	}

	if (r.flags & (EC_FLASH_PROTECT_RO_NOW | EC_FLASH_PROTECT_ALL_NOW))
		return 1;

	return 0;
}

/*
 * Prepare EC for update:
 * - Disable soft WP if needed.
 * - Parse flashmap.
 * - Jump to RO firmware.
 */
int cros_ec_prepare(struct flashctx *flash, uint8_t *image, int size)
{
	struct fmap *fmap = NULL;
	unsigned i, j;
	int wp_status;

	if (!(cros_ec_priv && cros_ec_priv->detected)) return 0;

	if (ec_check_features(EC_FEATURE_RWSIG) > 0) {
		rwsig_enabled = 1;
		msg_pdbg("EC has RWSIG enabled.\n");
	}

	/*
	 * If HW WP is disabled we may still need to disable write protection
	 * that is active on the EC. Otherwise the EC can reject erase/write
	 * commands.
	 *
	 * Failure is OK since HW WP might be enabled or the EC needs to be
	 * rebooted for the change to take effect. We can still update RW
	 * portions.
	 *
	 * If disabled here, EC WP will be restored at the end so that
	 * "--wp-enable" does not need to be run later. This greatly
	 * simplifies logic for developers and scripts.
	 */
	wp_status = cros_ec_wp_is_enabled();
	if (wp_status < 0) {
		return 1;
	} else if (wp_status == 1) {
		msg_pdbg("Attempting to disable EC soft WP.\n");

		struct flashrom_wp_cfg *cfg = NULL;
		enum flashrom_wp_result ret = flashrom_wp_cfg_new(&cfg);

		if (ret == FLASHROM_WP_OK) {
			flashrom_wp_set_mode(cfg, FLASHROM_WP_MODE_HARDWARE);

			ret = flashrom_wp_write_cfg(flash, cfg);
			flashrom_wp_cfg_release(cfg);
		}

		if (ret == FLASHROM_WP_OK) {
			msg_pdbg("EC soft WP disabled successfully.\n");
			if (register_shutdown(cros_ec_restore_wp, flash))
				return 1;
		} else {
			msg_pdbg("Failed. Hardware WP might in effect or EC "
				"needs to be rebooted first.\n");
		}
	} else {
		msg_pdbg("EC soft WP is already disabled.\n");
	}

	// Parse the fmap in the image file and cache the firmware ranges.
	if (!fmap_read_from_buffer(&fmap, image, size)) {
		// Lookup RO/A/B sections in FMAP.
		for (i = 0; i < fmap->nareas; i++) {
			struct fmap_area *fa = &fmap->areas[i];
			for (j = EC_IMAGE_RO; j < ARRAY_SIZE(sections); j++) {
				if (!strcmp(sections[j],
						(const char *)fa->name)) {
					msg_pdbg("Found '%s' in image.\n",
						fa->name);
					memcpy(&fwcopy[j], fa, sizeof(*fa));
					fwcopy[j].flags = 1;  // mark as new
				}
			}
		}
		free(fmap);
	}

	if (ec_check_features(EC_FEATURE_EXEC_IN_RAM) > 0) {
		msg_pwarn("Skip jumping to RO\n");
		return 0;
	}
	/* Warning: before update, we jump the EC to RO copy. If you
	 * want to change this behavior, please also check the
	 * cros_ec_finish().
	 */
	return cros_ec_jump_copy(EC_IMAGE_RO);
}


/* Returns >0 if we need 2nd pass of erase_and_write_flash().
 *         <0 if we cannot jump to any firmware copy.
 *        ==0 if no more pass is needed.
 *
 * This function also jumps to new-updated firmware copy before return >0.
 */
int cros_ec_need_2nd_pass(void)
{
	if (!(cros_ec_priv && cros_ec_priv->detected))
		return 0;

	if (!need_2nd_pass)
		return 0;

	if (ec_check_features(EC_FEATURE_EXEC_IN_RAM) > 0)
		/* EC_RES_ACCESS_DENIED is returned when the block is either
		 * protected or unsafe. Thus, theoretically, we shouldn't reach
		 * here because everywhere is safe for EXEC_IN_RAM chips and
		 * WP is disabled before erase/write cycle starts.
		 * We can still let the 2nd pass run (and it will probably
		 * fail again).
		 */
		return 1;

	if (cros_ec_jump_copy(EC_IMAGE_UNKNOWN))
		return -1;

	return 1;
}


/* Returns 0 for success.
 *
 * Try latest firmware: B > A > RO
 *
 * This function assumes the EC jumps to RO at cros_ec_prepare() so that
 * the fwcopy[RO].flags is old (0) and A/B are new. Please also refine
 * this code logic if you change the cros_ec_prepare() behavior.
 */
int cros_ec_finish(void)
{
	if (!(cros_ec_priv && cros_ec_priv->detected))
          return 0;

	/* For EC with RWSIG enabled. We need a cold reboot to enable
	 * EC_FLASH_PROTECT_ALL_NOW and make sure RWSIG check is performed.
	 */
	if (rwsig_enabled) {
		int rc;

		msg_pdbg("RWSIG enabled: doing a cold reboot to enable WP.\n");
		rc = cros_ec_cold_reboot(0);
		usleep(EC_RWSIG_JUMP_TO_RW_DELAY);
		return rc;
	}

	return 0;
}


int cros_ec_read(struct flashctx *flash, uint8_t *readarr,
             unsigned int blockaddr, unsigned int readcnt)
{
	int rc = 0;
	struct ec_params_flash_read p;
	int maxlen = flash->mst->opaque.max_data_read;
	uint8_t buf[maxlen];
	unsigned offset = 0, count;

	while (offset < readcnt) {
		count = min(maxlen, readcnt - offset);
		p.offset = blockaddr + offset;
		p.size = count;
		rc = cros_ec_priv->ec_command(EC_CMD_FLASH_READ,
					0, &p, sizeof(p), buf, count);
		if (rc < 0) {
			msg_perr("CROS_EC: Flash read error at offset 0x%x\n",
			         blockaddr + offset);
			return rc;
		} else {
			rc = EC_RES_SUCCESS;
		}

		memcpy(readarr + offset, buf, count);
		offset += count;
	}

	return rc;
}


/*
 * returns 0 to indicate area does not overlap current EC image
 * returns 1 to indicate area overlaps current EC image or error
 *
 * We can't get rid of this. The ECs should know what region is safe to erase
 * or write. We should let them decide (and return EC_RES_ACCESS_DENIED).
 * Not all existing EC firmware can do so.
 */
static int in_current_image(unsigned int addr, unsigned int len)
{
	enum ec_current_image image;
	uint32_t region_offset;
	uint32_t region_size;

	image = cros_ec_priv->current_image;
	region_offset = cros_ec_priv->region[image].offset;
	region_size = cros_ec_priv->region[image].size;

	if ((addr + len - 1 < region_offset) ||
		(addr > region_offset + region_size - 1)) {
		return 0;
	}
	return 1;
}


int cros_ec_block_erase(struct flashctx *flash, unsigned int blockaddr,
                        unsigned int len)
{
	struct ec_params_flash_erase_v1 erase;
	uint32_t mask;
	int rc, cmd_version, timeout=0;

	if (ec_check_features(EC_FEATURE_EXEC_IN_RAM) <= 0 &&
			in_current_image(blockaddr, len)) {
		cros_ec_invalidate_copy(blockaddr, len);
		need_2nd_pass = 1;
		return SPI_ACCESS_DENIED;
	}

	erase.params.offset = blockaddr;
	erase.params.size = len;
	rc = ec_get_cmd_versions(EC_CMD_FLASH_ERASE, &mask);
	if (rc < 0) {
		msg_perr("Cannot determine erase command version\n");
		return 0;
	}
	cmd_version = 31 - __builtin_clz(mask);

	if (cmd_version == 0) {
		rc = cros_ec_priv->ec_command(EC_CMD_FLASH_ERASE, 0,
				&erase.params,
				sizeof(struct ec_params_flash_erase), NULL, 0);
		if (rc == -EC_RES_ACCESS_DENIED) {
			// this is active image.
			cros_ec_invalidate_copy(blockaddr, len);
			need_2nd_pass = 1;
			return SPI_ACCESS_DENIED;
		}
		if (rc < 0) {
			msg_perr("CROS_EC: Flash erase error at address 0x%x, rc=%d\n",
					blockaddr, rc);
			return rc;
		}
		goto end_flash_erase;
	}

	if (len >= FLASH_SMALL_REGION_THRESHOLD) {
		erase.cmd = FLASH_ERASE_SECTOR_ASYNC;
	} else {
		erase.cmd = FLASH_ERASE_SECTOR;
	}
	rc = cros_ec_priv->ec_command(EC_CMD_FLASH_ERASE, cmd_version,
			      &erase, sizeof(erase), NULL, 0);
	switch (rc) {
	case 0:
		break;
	case -EC_RES_ACCESS_DENIED:
		// this is active image.
		cros_ec_invalidate_copy(blockaddr, len);
		need_2nd_pass = 1;
		return SPI_ACCESS_DENIED;
	case -EC_RES_BUSY:
		msg_perr("CROS_EC: Flash erase command "
				" already in progress\n");
		return rc;
	default:
		return rc;
	}
	if (len < FLASH_SMALL_REGION_THRESHOLD)
		goto end_flash_erase;

	/* Wait for the erase command to complete */
	rc = -EC_RES_BUSY;

/* wait up to 10s to erase a flash sector */
#define CROS_EC_ERASE_ASYNC_TIMEOUT 10000000
/* wait .5 second between queries. */
#define CROS_EC_ERASE_ASYNC_WAIT 500000

	while (rc < 0 && timeout < CROS_EC_ERASE_ASYNC_TIMEOUT) {
		usleep(CROS_EC_ERASE_ASYNC_WAIT);
		timeout += CROS_EC_ERASE_ASYNC_WAIT;
		erase.cmd = FLASH_ERASE_GET_RESULT;
		rc = cros_ec_priv->ec_command(EC_CMD_FLASH_ERASE, cmd_version,
				&erase, sizeof(erase), NULL, 0);
	}
	if (rc < 0) {
		msg_perr("CROS_EC: Flash erase error at address 0x%x, rc=%d\n",
		         blockaddr, rc);
		return rc;
	}

end_flash_erase:
	if (rc > 0) {
		/*
		 * Can happen if the command with retried with
		 * EC_CMD_GET_COMMS_STATUS
		 */
		rc = -EC_RES_SUCCESS;
	}
	return rc;
}


int cros_ec_write(struct flashctx *flash, const uint8_t *buf, unsigned int addr,
                  unsigned int nbytes)
{
	unsigned i;
	int rc = 0;
	unsigned int written = 0, real_write_size;
	struct ec_params_flash_write p;
	uint8_t *packet;

	/*
	 * For b:35542013, to workaround the undersized
	 * outdata buffer issue in kernel.
	 * chunk size should exclude the packet header ec_params_flash_write.
	 */
	real_write_size = min(flash->mst->opaque.max_data_write - sizeof(p),
			      cros_ec_priv->ideal_write_size);
	assert(real_write_size > 0);

	packet = malloc(sizeof(p) + real_write_size);
	if (!packet)
		return -1;

	for (i = 0; i < nbytes; i += written) {
		written = min(nbytes - i, real_write_size);
		p.offset = addr + i;
		p.size = written;

		if (ec_check_features(EC_FEATURE_EXEC_IN_RAM) <= 0 &&
				in_current_image(p.offset, p.size)) {
			cros_ec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return SPI_ACCESS_DENIED;
		}

		memcpy(packet, &p, sizeof(p));
		memcpy(packet + sizeof(p), &buf[i], written);
		rc = cros_ec_priv->ec_command(EC_CMD_FLASH_WRITE,
				0, packet, sizeof(p) + p.size, NULL, 0);

		if (rc == -EC_RES_ACCESS_DENIED) {
			// this is active image.
			cros_ec_invalidate_copy(addr, nbytes);
			need_2nd_pass = 1;
			return SPI_ACCESS_DENIED;
		}

		if (rc < 0) break;
		rc = EC_RES_SUCCESS;
	}

	free(packet);
	return rc;
}

int cros_ec_probe_size(struct flashctx *flash)
{
	int rc = 0, cmd_version;
	struct ec_response_flash_spi_info spi_info;
	struct ec_response_get_chip_info chip_info;
	struct block_eraser *eraser;
	uint32_t mask;

	rc = cros_ec_get_current_image();
	if (rc < 0) {
		msg_perr("%s(): Failed to probe (no current image): %d\n",
			 __func__, rc);
		return 0;
	}
	cros_ec_priv->current_image = rc;
	cros_ec_priv->region = &regions[0];

	rc = ec_get_cmd_versions(EC_CMD_FLASH_INFO, &mask);
	if (rc < 0) {
		msg_perr("Cannot determine write command version\n");
		return 0;
	}
	cmd_version = 31 - __builtin_clz(mask);

	eraser = &flash->chip->block_erasers[0];
	flash->chip->page_size = flash->mst->opaque.max_data_read;

	if (cmd_version < 2) {
		struct ec_response_flash_info_1 info;
		/* Request general information about flash (v1 or below). */
		rc = cros_ec_priv->ec_command(EC_CMD_FLASH_INFO, cmd_version,
				NULL, 0, &info,
				(cmd_version > 0 ? sizeof(info) :
				 sizeof(struct ec_response_flash_info)));
		if (rc < 0) {
			msg_perr("%s(): FLASH_INFO v%d returns %d.\n", __func__,
					cmd_version, rc);
			return 0;
		}
		if (cmd_version == 0) {
			cros_ec_priv->ideal_write_size =
				EC_FLASH_WRITE_VER0_SIZE;
		} else {
			cros_ec_priv->ideal_write_size = info.write_ideal_size;
			if (info.flags & EC_FLASH_INFO_ERASE_TO_0)
				flash->chip->feature_bits |=
					FEATURE_ERASED_ZERO;
		}
		flash->chip->total_size = info.flash_size / 1024;

		/* Allow overriding the erase block size in case EC is incorrect */
		if (cros_ec_priv->erase_block_size > 0)
			eraser->eraseblocks[0].size =
				cros_ec_priv->erase_block_size;
		else
			eraser->eraseblocks[0].size = info.erase_block_size;

		eraser->eraseblocks[0].count = info.flash_size /
			eraser->eraseblocks[0].size;
	} else {
		struct ec_response_flash_info_2 info_2;
		struct ec_params_flash_info_2 params_2;
		struct ec_response_flash_info_2 *info_2_p = &info_2;
		int size_info_v2 = sizeof(info_2), i;

		params_2.num_banks_desc = 0;
		/*
		 * Call FLASH_INFO twice, second time with all banks
		 * information.
		 */
		for (i = 0; i < 2; i++) {
			rc = cros_ec_priv->ec_command(EC_CMD_FLASH_INFO,
					cmd_version, &params_2,
					sizeof(params_2),
					info_2_p, size_info_v2);
			if (rc < 0) {
				msg_perr("%s(): FLASH_INFO(%d) v%d returns %d.\n",
						__func__,
						params_2.num_banks_desc,
						cmd_version, rc);
				if (info_2_p != &info_2)
					free(info_2_p);
				return 0;
			} else if (i > 0) {
				break;
			}
			params_2.num_banks_desc = info_2_p->num_banks_total;
			size_info_v2 += info_2_p->num_banks_total *
				sizeof(struct ec_flash_bank);

			info_2_p = malloc(size_info_v2);
			if (!info_2_p) {
				msg_perr("%s(): malloc of %d banks failed\n",
					 __func__, info_2_p->num_banks_total);
				return 0;
			}
		}
		flash->chip->total_size = info_2_p->flash_size / 1024;
		for (i = 0; i < info_2_p->num_banks_desc; i++) {
			/* Allow overriding the erase block size in case EC is incorrect */
			eraser->eraseblocks[i].size =
				(cros_ec_priv->erase_block_size > 0 ?
				 cros_ec_priv->erase_block_size :
				 (unsigned) 1 << info_2_p->banks[i].erase_size_exp);
			eraser->eraseblocks[i].count =
				info_2_p->banks[i].count <<
				(info_2_p->banks[i].size_exp -
				 info_2_p->banks[i].erase_size_exp);
		}
		cros_ec_priv->ideal_write_size = info_2_p->write_ideal_size;
#if 0
		/*
		 * TODO(b/38506987)Comment out, as some firmware were not
		 * setting this flag properly.
		 */
		if (info_2_p->flags & EC_FLASH_INFO_ERASE_TO_0)
			flash->chip->feature_bits |= FEATURE_ERASED_ZERO;
#endif
		free(info_2_p);
	}
	eraser->block_erase = cros_ec_block_erase;
	/*
	 * Some STM32 variants erase bits to 0. For now, assume that this
	 * applies to STM32L parts.
	 *
	 * FIXME: This info will eventually be exposed via some EC command.
	 * See chrome-os-partner:20973.
	 */
	rc = cros_ec_priv->ec_command(EC_CMD_GET_CHIP_INFO,
			0, NULL, 0, &chip_info, sizeof(chip_info));
	if (rc < 0) {
		msg_perr("%s(): CHIP_INFO returned %d.\n", __func__, rc);
		return 0;
	}
	if (!strncmp(chip_info.name, "stm32l1", 7))
		flash->chip->feature_bits |= FEATURE_ERASED_ZERO;



	rc = cros_ec_priv->ec_command(EC_CMD_FLASH_SPI_INFO,
				0, NULL, 0, &spi_info, sizeof(spi_info));
	if (rc < 0) {
		static char chip_vendor[32];
		static char chip_name[32];

		memcpy(chip_vendor, chip_info.vendor, sizeof(chip_vendor));
		memcpy(chip_name, chip_info.name, sizeof(chip_name));
		flash->chip->vendor = chip_vendor;
		flash->chip->name = chip_name;
		flash->chip->tested = TEST_OK_PREW;
	} else {
		const struct flashchip *f;
		uint32_t mfg = spi_info.jedec[0];
		uint32_t model = (spi_info.jedec[1] << 8) | spi_info.jedec[2];

		for (f = flashchips; f && f->name; f++) {
			if (f->bustype != BUS_SPI)
				continue;
			if ((f->manufacture_id == mfg) &&
				f->model_id == model) {
				flash->chip->vendor = f->vendor;
				flash->chip->name = f->name;
				flash->chip->tested = f->tested;
				break;
			}
		}
	}

	/* FIXME: EC_IMAGE_* is ordered differently from EC_FLASH_REGION_*,
	 * so we need to be careful about using these enums as array indices */
	rc = cros_ec_get_region_info(EC_FLASH_REGION_RO,
				 &cros_ec_priv->region[EC_IMAGE_RO]);
	if (rc) {
		msg_perr("%s(): Failed to probe (cannot find RO region): %d\n",
			 __func__, rc);
		return 0;
	}

	rc = cros_ec_get_region_info(EC_FLASH_REGION_RW,
				 &cros_ec_priv->region[EC_IMAGE_RW]);
	if (rc) {
		msg_perr("%s(): Failed to probe (cannot find RW region): %d\n",
			 __func__, rc);
		return 0;
	}

	return 1;
};
