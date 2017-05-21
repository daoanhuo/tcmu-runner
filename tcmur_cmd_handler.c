/*
 * Copyright 2017, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define _GNU_SOURCE
#include <scsi/scsi.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ccan/list/list.h"

#include "darray.h"
#include "libtcmu.h"
#include "libtcmu_log.h"
#include "libtcmu_priv.h"
#include "tcmur_aio.h"
#include "tcmur_device.h"
#include "tcmur_cmd_handler.h"
#include "tcmu-runner.h"
#include "alua.h"

void tcmur_command_complete(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			    int rc)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);

	pthread_cleanup_push(_cleanup_spin_lock, (void *)&rdev->lock);
	pthread_spin_lock(&rdev->lock);

	tcmulib_command_complete(dev, cmd, rc);

	pthread_spin_unlock(&rdev->lock);
	pthread_cleanup_pop(0);
}

static void aio_command_finish(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			       int rc)
{
	int wakeup;

	track_aio_request_finish(tcmu_get_daemon_dev_private(dev), &wakeup);
	tcmur_command_complete(dev, cmd, rc);
	if (wakeup)
		tcmulib_processing_complete(dev);
}

static int alloc_iovec(struct tcmulib_cmd *cmd, size_t length)
{
	struct iovec *iov;

	assert(!cmd->iovec);

	iov = calloc(1, sizeof(*iov));
	if (!iov)
		goto out;
	iov->iov_base = calloc(1, length);
	if (!iov->iov_base)
		goto free_iov;
	iov->iov_len = length;

	cmd->iovec = iov;
	cmd->iov_cnt = 1;
	return 0;

free_iov:
	free(iov);
out:
	return -ENOMEM;
}

static void free_iovec(struct tcmulib_cmd *cmd)
{
	assert(cmd->iovec);
	assert(cmd->iovec->iov_base);

	free(cmd->iovec->iov_base);
	free(cmd->iovec);

	cmd->iov_cnt = 0;
	cmd->iovec = NULL;
}

static int check_lba_and_length(struct tcmu_device *dev,
				struct tcmulib_cmd *cmd, uint32_t sectors)
{
	uint8_t *cdb = cmd->cdb;
	uint64_t lba = tcmu_get_lba(cdb);
	uint64_t num_lbas = tcmu_get_dev_num_lbas(dev);
	size_t iov_length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);

	if (iov_length != sectors * tcmu_get_dev_block_size(dev)) {
		tcmu_dev_err(dev, "iov len mismatch: iov len %zu, xfer len %lu, block size %lu\n",
			     iov_length, sectors, tcmu_get_dev_block_size(dev));

		return tcmu_set_sense_data(cmd->sense_buf, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE, NULL);
	}

	if (lba + sectors > num_lbas || lba + sectors < lba) {
		tcmu_dev_err(dev, "cmd exceeds last lba %llu (lba %llu, xfer len %lu)\n",
			     num_lbas, lba, sectors);
		return tcmu_set_sense_data(cmd->sense_buf, ILLEGAL_REQUEST,
					   ASC_LBA_OUT_OF_RANGE, NULL);
	}

	return SAM_STAT_GOOD;
}

static void handle_generic_cbk(struct tcmu_device *dev,
			       struct tcmulib_cmd *cmd, int ret)
{
	aio_command_finish(dev, cmd, ret);
}

static int read_work_fn(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	uint32_t block_size = tcmu_get_dev_block_size(dev);

	return rhandler->read(dev, cmd, cmd->iovec, cmd->iov_cnt,
			      tcmu_iovec_length(cmd->iovec, cmd->iov_cnt),
			      block_size * tcmu_get_lba(cmd->cdb));
}

static int write_work_fn(struct tcmu_device *dev,
				struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	uint32_t block_size = tcmu_get_dev_block_size(dev);

	return rhandler->write(dev, cmd, cmd->iovec, cmd->iov_cnt,
				tcmu_iovec_length(cmd->iovec, cmd->iov_cnt),
				block_size * tcmu_get_lba(cmd->cdb));
}

struct write_same {
	pthread_cond_t cond;
	pthread_mutex_t lock;
	bool cbk_finished;
};

static void handle_write_same_sync(struct tcmu_device *dev,
				   struct tcmulib_cmd *cmd,
				   int ret)
{
	struct write_same *write_same = cmd->cmdstate;

	pthread_mutex_lock(&write_same->lock);
	write_same->cbk_finished = true;
	pthread_cond_signal(&write_same->cond);
	pthread_mutex_unlock(&write_same->lock);
}

static int write_same_16_work_fn(struct tcmu_device *dev,
				 struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	uint32_t block_size = tcmu_get_dev_block_size(dev);
	uint32_t lba_cnt = tcmu_get_xfer_length(cmd->cdb);
	uint64_t start_lba = tcmu_get_lba(cmd->cdb);
	struct write_same *write_same = cmd->cmdstate;
	void *iov_base;
	size_t iov_len;
	int ret;

	tcmu_dev_dbg(dev, "Start lba: %llu, number of lba:: %hu, end lba: %llu\n",
		     start_lba, lba_cnt, start_lba + lba_cnt);


	iov_base = cmd->iovec->iov_base;
	iov_len = cmd->iovec->iov_len;

	write_same->cbk_finished = false;
	cmd->done = handle_write_same_sync;

	/*
	 * Write contents of the logical block data(from the Data-Out Buffer)
	 * to each LBA in the specified LBA range.
	 */
	while (lba_cnt--) {
		cmd->iovec->iov_base = iov_base;
		cmd->iovec->iov_len = iov_len;
		ret = rhandler->write(dev, cmd, cmd->iovec, cmd->iov_cnt,
				      tcmu_iovec_length(cmd->iovec, cmd->iov_cnt),
				      block_size * start_lba);
		if (ret)
			goto out;

		pthread_mutex_lock(&write_same->lock);
		if (!write_same->cbk_finished)
			pthread_cond_wait(&write_same->cond, &write_same->lock);
		write_same->cbk_finished = false;
		pthread_mutex_unlock(&write_same->lock);

		start_lba++;
	}

	ret = SAM_STAT_GOOD;

out:
	aio_command_finish(dev, cmd, ret);
	pthread_cond_destroy(&write_same->cond);
	pthread_mutex_destroy(&write_same->lock);
	free(write_same);

	return ret;
}

static int handle_write_same_16(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	uint8_t *sense = cmd->sense_buf;
	uint32_t blocks = tcmu_get_xfer_length(cdb);
	uint32_t block_size = tcmu_get_dev_block_size(dev);
	uint64_t start_lba = tcmu_get_lba(cmd->cdb);
	uint64_t dev_last_lba = tcmu_get_dev_num_lbas(dev);
	struct write_same *write_same;

	if (cmd->iov_cnt != 1 || cmd->iovec->iov_len != block_size) {
		tcmu_dev_err(dev, "Illegal Data-Out: iov_cnt %u length: %u\n",
			     cmd->iov_cnt, cmd->iovec->iov_len);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_CDB,
					   NULL);
	}

	/*
	 * From sbc4r13, section 5.50 WRITE SAME (16) command
	 *
	 * A write same (WSNZ) bit has beed set to one, so the device server
	 * won't support a value of zero here.
	 */
	if (!blocks) {
		tcmu_dev_err(dev, "The WSNZ = 1 & WRITE_SAME blocks = 0 is not supported!\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_CDB,
					   NULL);
	}

	/*
	 * The MAXIMUM WRITE SAME LENGTH field in Block Limits VPD page (B0h)
	 * limit the maximum block number for the WRITE SAME.
	 */
	if (blocks > VPD_MAX_WRITE_SAME_LENGTH) {
		tcmu_dev_err(dev, "blocks: %u exceeds MAXIMUM WRITE SAME LENGTH: %u\n",
			     blocks, VPD_MAX_WRITE_SAME_LENGTH);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_CDB,
					   NULL);
	}

	/*
	 * The logical block address plus the number of blocks shouldn't
	 * exceeds the capacity of the medium
	 */
	if (start_lba + blocks > dev_last_lba) {
		tcmu_dev_err(dev, "lba %llu plus blocks %lu exceeds the dev capacity %llu\n",
			     start_lba, blocks, dev_last_lba);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_LBA_OUT_OF_RANGE,
					   NULL);
	}

	/*
	 * For now not support UNMAP bit and ANCHOR bit.
	 */
	if (cdb[1] & 0x10 || cdb[1] & 0x08) {
		tcmu_dev_err(dev, "The ANCHOR or UNMAP bit is set not support!\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_CDB,
					   NULL);
	}

	write_same = calloc(1, sizeof(struct write_same));
	if (!write_same) {
		return tcmu_set_sense_data(sense, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE,
					   NULL);
	}

	pthread_cond_init(&write_same->cond, NULL);
	pthread_mutex_init(&write_same->lock, NULL);

	cmd->cmdstate = write_same;
	return async_handle_cmd(dev, cmd, write_same_16_work_fn);
}

/* async write verify */

struct write_verify_state {
	size_t requested;
	struct iovec *w_iovec;
	size_t w_iov_cnt;
	void *read_buf;
	struct tcmulib_cmd *readcmd;
};

static int write_verify_init(struct tcmulib_cmd *origcmd, size_t length)
{
	struct tcmulib_cmd *readcmd;
	struct write_verify_state *state;
	int i;

	readcmd = calloc(1, sizeof(*readcmd));
	if (!readcmd)
		goto out;
	readcmd->cmdstate = origcmd;
	readcmd->cdb = origcmd->cdb;

	if (alloc_iovec(readcmd, length))
		goto free_cmd;

	state = calloc(1, sizeof(*state));
	if (!state)
		goto free_iov;

	/* use @origcmd as writecmd */
	state->read_buf = readcmd->iovec->iov_base;
	state->requested = length;
	state->readcmd = readcmd;

	state->w_iovec = calloc(origcmd->iov_cnt, sizeof(struct iovec));
	if (!state->w_iovec)
		goto free_state;

	state->w_iov_cnt = origcmd->iov_cnt;
	for (i = 0; i < origcmd->iov_cnt; i++) {
		state->w_iovec[i].iov_base = origcmd->iovec[i].iov_base;
		state->w_iovec[i].iov_len = origcmd->iovec[i].iov_len;
	}
	origcmd->cmdstate = state;

	return 0;

free_state:
	free(state);
free_iov:
	free_iovec(readcmd);
free_cmd:
	free(readcmd);
out:
	return -ENOMEM;
}

static void write_verify_free(struct tcmulib_cmd *origcmd)
{
	struct write_verify_state *state = origcmd->cmdstate;
	struct tcmulib_cmd *readcmd = state->readcmd;

	/* some handlers update iov_base */
	readcmd->iovec->iov_base = state->read_buf;
	free_iovec(readcmd);
	free(readcmd);
	free(state->w_iovec);
	free(state);
}

static void handle_write_verify_read_cbk(struct tcmu_device *dev,
					 struct tcmulib_cmd *readcmd, int ret)
{
	uint32_t cmp_offset;
	struct tcmulib_cmd *writecmd = readcmd->cmdstate;
	struct write_verify_state *state = writecmd->cmdstate;
	uint8_t *sense = writecmd->sense_buf;

	/* failed read - bail out */
	if (ret != SAM_STAT_GOOD) {
		memcpy(writecmd->sense_buf, readcmd->sense_buf,
		       sizeof(writecmd->sense_buf));
		goto done;
	}

	ret = SAM_STAT_GOOD;
	cmp_offset = tcmu_compare_with_iovec(state->read_buf, state->w_iovec,
					     state->requested);
	if (cmp_offset != -1) {
		tcmu_dev_err(dev, "Verify failed at offset %lu\n", cmp_offset);
		ret =  tcmu_set_sense_data(sense, MISCOMPARE,
					   ASC_MISCOMPARE_DURING_VERIFY_OPERATION,
					   &cmp_offset);
	}

done:
	write_verify_free(writecmd);
	aio_command_finish(dev, writecmd, ret);
}

static void handle_write_verify_write_cbk(struct tcmu_device *dev,
					  struct tcmulib_cmd *writecmd,
					  int ret)
{
	struct write_verify_state *state = writecmd->cmdstate;

	/* write error - bail out */
	if (ret != SAM_STAT_GOOD)
		goto finish_err;

	state->readcmd->done = handle_write_verify_read_cbk;
	ret = async_handle_cmd(dev, state->readcmd, read_work_fn);
	if (ret != TCMU_ASYNC_HANDLED)
		goto finish_err;
	return;

finish_err:
	write_verify_free(writecmd);
	aio_command_finish(dev, writecmd, ret);
}

static int handle_write_verify(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret = SAM_STAT_TASK_SET_FULL;
	uint8_t *cdb = cmd->cdb;
	size_t length = tcmu_get_xfer_length(cdb) * tcmu_get_dev_block_size(dev);

	ret = check_lba_and_length(dev, cmd, tcmu_get_xfer_length(cmd->cdb));
	if (ret)
		return ret;

	if (write_verify_init(cmd, length)) {
		ret = SAM_STAT_TASK_SET_FULL;
		goto out;
	}

	cmd->done = handle_write_verify_write_cbk;

	ret = async_handle_cmd(dev, cmd, write_work_fn);
	if (ret != TCMU_ASYNC_HANDLED)
		goto free_write_verify;

	return TCMU_ASYNC_HANDLED;

free_write_verify:
	write_verify_free(cmd);
out:
	return ret;
}

#define XCOPY_HDR_LEN                   16
#define XCOPY_TARGET_DESC_LEN           32
#define XCOPY_SEGMENT_DESC_B2B_LEN      28
#define XCOPY_NAA_IEEE_REGEX_LEN        16
#define XCOPY_MAX_SECTORS               1024

struct xcopy {
	pthread_cond_t cond;
	pthread_mutex_t lock;
	bool cbk_finished;

	struct tcmu_device *origdev;
	struct tcmu_device *src_dev;
	uint8_t src_tid_wwn[XCOPY_NAA_IEEE_REGEX_LEN];
	struct tcmu_device *dst_dev;
	uint8_t dst_tid_wwn[XCOPY_NAA_IEEE_REGEX_LEN];

	uint64_t src_lba;
	uint64_t dst_lba;
	unsigned long stdi;
	unsigned long dtdi;
	unsigned long lba_cnt;
};

/* For now only supports block -> block type */
static int xcopy_parse_segment_descs(uint8_t *seg_descs, struct xcopy *xcopy,
				     uint8_t sdll, uint8_t *sense)
{
	uint8_t *seg_desc = seg_descs;
	uint8_t desc_len;

	/*
	 * From spc4r31, section 6.3.7.5 Block device to block device
	 * operations
	 *
	 * The segment descriptor size should be 28 bytes
	 */
	if (sdll % XCOPY_SEGMENT_DESC_B2B_LEN != 0) {
		tcmu_err("Illegal block --> block type segment descriptor length %u\n",
			 sdll);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	if (sdll > RCR_OP_MAX_SEGMENT_DESC_COUNT * XCOPY_SEGMENT_DESC_B2B_LEN) {
		tcmu_err("Only %u segment descriptor(s) supported, but there are %u\n",
			 RCR_OP_MAX_SEGMENT_DESC_COUNT,
			 sdll / XCOPY_SEGMENT_DESC_B2B_LEN);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	/* EXTENDED COPY segment descriptor type codes block --> block */
	if (seg_desc[0] != XCOPY_SEG_DESC_TYPE_CODE_B2B) {
		tcmu_err("Unsupport segment descriptor type code 0x%x\n",
			 seg_desc[0]);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_UNSUPPORTED_SEGMENT_DESC_TYPE_CODE,
					   NULL);
	}

	/*
	 * For block -> block type the length is 4-byte header + 0x18-byte
	 * data.
	 */
	desc_len = be16toh(*(uint16_t *)&seg_desc[2]);
	if (desc_len != 0x18) {
		tcmu_err("Invalid length for block->block type 0x%x\n",
			 desc_len);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	/*
	 * From spc4r31, section 6.3.7.1 Segment descriptors introduction
	 *
	 * The SOURCE TARGET DESCRIPTOR INDEX field contains an index into
	 * the target descriptor list (see 6.3.1) identifying the source
	 * copy target device. The DESTINATION TARGET DESCRIPTOR INDEX field
	 * contains an index into the target descriptor list (see 6.3.1)
	 * identifying the destination copy target device.
	 */
	xcopy->stdi = be16toh(*(uint16_t *)&seg_desc[4]);
	xcopy->dtdi = be16toh(*(uint16_t *)&seg_desc[6]);
	tcmu_dbg("Segment descriptor: stdi: %hu dtdi: %hu\n", xcopy->stdi,
		 xcopy->dtdi);

	xcopy->lba_cnt = be16toh(*(uint16_t *)&seg_desc[10]);
	xcopy->src_lba = be64toh(*(uint64_t *)&seg_desc[12]);
	xcopy->dst_lba = be64toh(*(uint64_t *)&seg_desc[20]);
	tcmu_dbg("Segment descriptor: lba_cnt: %hu src_lba: %llu dst_lba: %llu\n",
		 xcopy->lba_cnt, xcopy->src_lba, xcopy->dst_lba);

	return SAM_STAT_GOOD;
}

static int xcopy_gen_naa_ieee(struct tcmu_device *udev, uint8_t *wwn)
{
	char *buf, *p, ch;
	bool next = true;
	int ind = 0;

	/* Set type 6 and use OpenFabrics IEEE Company ID: 00 14 05 */
	wwn[ind++] = 0x60;
	wwn[ind++] = 0x01;
	wwn[ind++] = 0x40;
	wwn[ind] = 0x50;

	/* Parse the udev vpd unit serial number */
	buf = tcmu_get_wwn(udev);
	if (!buf)
		return -1;
	p = buf;

	/*
	 * Generate up to 36 bits of VENDOR SPECIFIC IDENTIFIER starting on
	 * byte 3 bit 3-0 for NAA IEEE Registered Extended DESIGNATOR field
	 * format, followed by 64 bits of VENDOR SPECIFIC IDENTIFIER EXTENSION
	 * to complete the payload.  These are based from VPD=0x80 PRODUCT SERIAL
	 * NUMBER set via vpd_unit_serial in target_core_configfs.c to ensure
	 * per device uniqeness.
	 */
	for (; *p && ind < XCOPY_NAA_IEEE_REGEX_LEN; p++) {
		int val;

		ch = *p;
		if ((ch >= '0') && (ch <= '9'))
			val = ch - '0';
		else if ((ch >= 'a') && (ch <= 'f'))
			val = ch - 'a' + 10;
		else if ((ch >= 'A') && (ch <= 'F'))
			val = ch - 'A' + 10;
		else
			continue;

		if (next) {
			next = false;
			wwn[ind++] |= val;
		} else {
			next = true;
			wwn[ind] = val << 4;
		}
	}

	free(buf);
	return SAM_STAT_GOOD;
}

static int xcopy_locate_udev(struct tcmulib_context *ctx,
			     const uint8_t *dev_wwn,
			     struct tcmu_device **udev)
{
	struct tcmu_device **dev_ptr;
	struct tcmu_device *dev;
	uint8_t wwn[XCOPY_NAA_IEEE_REGEX_LEN];

	darray_foreach(dev_ptr, ctx->devices) {
		dev = *dev_ptr;

		memset(wwn, 0, XCOPY_NAA_IEEE_REGEX_LEN);
		if (xcopy_gen_naa_ieee(dev, wwn))
			return -1;

		if (memcmp(wwn, dev_wwn, XCOPY_NAA_IEEE_REGEX_LEN))
			continue;

		*udev = dev;
		tcmu_dev_dbg(dev, "Located tcmu devivce: %s\n", dev->dev_name);

		return 0;
	}

	return -1;
}

/* Identification descriptor target */
static int xcopy_parse_target_id(struct tcmu_device *udev,
				  struct xcopy *xcopy,
				  uint8_t *tgt_desc,
				  int32_t index,
				  uint8_t *sense)
{
	uint8_t wwn[XCOPY_NAA_IEEE_REGEX_LEN];

	/*
	 * Generate an IEEE Registered Extended designator based upon the
	 * device the XCOPY specified.
	 */
	memset(wwn, 0, XCOPY_NAA_IEEE_REGEX_LEN);
	if (xcopy_gen_naa_ieee(udev, wwn))
		return tcmu_set_sense_data(sense, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE,
					   NULL);

	/*
	 * CODE SET: for now only binary type code is supported.
	 */
	if ((tgt_desc[4] & 0x0f) != 0x1) {
		tcmu_dev_err(udev, "Id target CODE DET only support binary type!\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	/*
	 * ASSOCIATION: for now only LUN type code is supported.
	 */
	if ((tgt_desc[5] & 0x30) != 0x00) {
		tcmu_dev_err(udev, "Id target ASSOCIATION other than LUN not supported!\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	/*
	 * DESIGNATOR TYPE: for now only NAA type code is supported.
	 *
	 * The designator type define please see: such as
	 * From spc4r31, section 7.8.6.1 Device Identification VPD page
	 * overview
	 */
	if ((tgt_desc[5] & 0x0f) != 0x3) {
		tcmu_dev_err(udev, "Id target DESIGNATOR TYPE other than NAA not supported!\n");
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}
	/*
	 * Check for matching 16 byte length for NAA IEEE Registered Extended
	 * Assigned designator
	 */
	if (tgt_desc[7] != 16) {
		tcmu_dev_err(udev, "Id target DESIGNATOR LENGTH should be 16, but it's: %d\n",
			     tgt_desc[7]);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	/*
	 * Check for NAA IEEE Registered Extended Assigned header.
	 */
	if ((tgt_desc[8] >> 4) != 0x06) {
		tcmu_dev_err(udev, "Id target NAA designator type: 0x%x\n",
			     tgt_desc[8] >> 4);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	/*
	 * Source designator matches the local device
	 */
	if (index == xcopy->stdi) {
		memcpy(&xcopy->src_tid_wwn[0], &tgt_desc[8],
		       XCOPY_NAA_IEEE_REGEX_LEN);

		if (!memcmp(wwn, xcopy->src_tid_wwn, XCOPY_NAA_IEEE_REGEX_LEN))
			xcopy->src_dev = udev;
	}

	/*
	 * Destination designator matches the local device.
	 */
	if (index == xcopy->dtdi) {
		memcpy(xcopy->dst_tid_wwn, &tgt_desc[8],
		       XCOPY_NAA_IEEE_REGEX_LEN);

		if (!memcmp(wwn, xcopy->dst_tid_wwn, XCOPY_NAA_IEEE_REGEX_LEN))
			xcopy->dst_dev = udev;
	}

	return SAM_STAT_GOOD;
}

static int xcopy_parse_target_descs(struct tcmu_device *udev,
				    struct xcopy *xcopy,
				    uint8_t *tgt_desc,
				    uint16_t tdll,
				    uint8_t *sense)
{
	int i, ret;

	if (tdll > RCR_OP_MAX_TARGET_DESC_COUNT * XCOPY_TARGET_DESC_LEN) {
		tcmu_dev_err(udev, "Only %u target descriptor(s) supported, but there are %u\n",
			     RCR_OP_MAX_TARGET_DESC_COUNT, tdll / XCOPY_TARGET_DESC_LEN);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					   NULL);
	}

	for (i = 0; i < RCR_OP_MAX_TARGET_DESC_COUNT; i++) {
		/*
		 * Only Identification Descriptor Target Descriptor support
		 * for now.
		 */
		if (tgt_desc[0] == XCOPY_TARGET_DESC_TYPE_CODE_ID) {
			ret = xcopy_parse_target_id(udev, xcopy, tgt_desc, i, sense);
			if (ret != SAM_STAT_GOOD)
				return ret;

			tgt_desc += XCOPY_TARGET_DESC_LEN;
		} else {
			tcmu_dev_err(udev, "Unsupport target descriptor type code 0x%x\n",
				     tgt_desc[0]);
			return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
						   ASC_UNSUPPORTED_TARGET_DESC_TYPE_CODE,
						   NULL);
		}
	}

	if (xcopy->src_dev)
		ret = xcopy_locate_udev(udev->ctx, xcopy->dst_tid_wwn,
					&xcopy->dst_dev);
	else if (xcopy->dst_dev)
		ret = xcopy_locate_udev(udev->ctx, xcopy->src_tid_wwn,
					&xcopy->src_dev);

	if (ret) {
		tcmu_err("Target device not found, the index are %hu and %hu\n",
			 xcopy->stdi, xcopy->dtdi);
		return tcmu_set_sense_data(sense, COPY_ABORTED,
					   ASC_COPY_TARGET_DEVICE_NOT_REACHABLE,
					   NULL);
	}

	tcmu_dev_dbg(xcopy->src_dev, "Source device NAA IEEE WWN: 0x%16phN\n",
		     xcopy->src_tid_wwn);
	tcmu_dev_dbg(xcopy->dst_dev, "Destination device NAA IEEE WWN: 0x%16phN\n",
		     xcopy->dst_tid_wwn);

	return SAM_STAT_GOOD;
}

static int xcopy_parse_parameter_list(struct tcmu_device *dev,
				      struct tcmulib_cmd *cmd,
				      struct xcopy *xcopy)
{
	uint8_t *cdb = cmd->cdb;
	size_t data_length = tcmu_get_xfer_length(cdb);
	struct iovec *iovec = cmd->iovec;
	size_t iov_cnt = cmd->iov_cnt;
	uint8_t *sense = cmd->sense_buf;
	uint32_t inline_dl;
	uint8_t *seg_desc, *tgt_desc, *par;
	uint16_t sdll, tdll;
	uint64_t num_lbas;
	int ret;

	/*
	 * The PARAMETER LIST LENGTH field specifies the length in bytes
	 * of the parameter data that shall be contained in the Data-Out
	 * Buffer.
	*/
	par = calloc(1, data_length);
	if (!par) {
		tcmu_dev_err(dev, "calloc parameter list buffer error\n");
		return tcmu_set_sense_data(sense, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE,
					   NULL);
	}

	tcmu_memcpy_from_iovec(par, data_length, iovec, iov_cnt);

	/*
	 * From spc4r31, section 6.18.4 OPERATING PARAMETERS service action
	 *
	 * A supports no list identifier (SNLID) bit set to one indicates
	 * the copy manager supports an EXTENDED COPY (see 6.3) command
	 * parameter list in which the LIST ID USAGE field is set to 11b
	 * and the LIST IDENTIFIER field is set to zero as described in
	 * table 105 (see 6.3.1).
	 *
	 * From spc4r31, section 6.3.1 EXTENDED COPY command introduction
	 *
	 * LIST ID USAGE == 11b, then the LIST IDENTIFIER field should be
	 * as zero.
	 */
	tcmu_dev_dbg(dev, "LIST ID USAGE: 0x%x, LIST IDENTIFIER: 0x%x\n",
		     (par[1] & 0x18) >> 3, par[0]);
	if ((par[1] & 0x18) != 0x18 || par[0]) {
		tcmu_dev_err(dev, "LIST ID USAGE: 0x%x, LIST IDENTIFIER: 0x%x\n",
			     (par[1] & 0x18) >> 3, par[0]);
		ret = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					  ASC_INVALID_FIELD_IN_PARAMETER_LIST,
					  NULL);
		goto err;
	}

	/*
	 * From spc4r31, section 6.3.6.1 Target descriptors introduction
	 *
	 * All target descriptors (see table 108) are 32 bytes or 64 bytes
	 * in length
	 */
	tdll = be16toh(*(uint16_t *)&par[2]);
	if (tdll % 32 != 0) {
		tcmu_dev_err(dev, "Illegal target descriptor length %u\n",
			     tdll);
		ret = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					  ASC_PARAMETER_LIST_LENGTH_ERROR,
					  NULL);
		goto err;
	}

	/*
	 * From spc4r31, section 6.3.7.1 Segment descriptors introduction
	 *
	 * Segment descriptors (see table 120) begin with an eight byte header.
	 */
	sdll = be32toh(*(uint32_t *)&par[8]);
	if (sdll < 8) {
		tcmu_dev_err(dev, "Illegal segment descriptor length %u\n",
			     tdll);
		ret = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					  ASC_PARAMETER_LIST_LENGTH_ERROR,
					  NULL);
		goto err;
	}

	/*
	 * The maximum length of the target and segment descriptors permitted
	 * within a parameter list is indicated by the MAXIMUM DESCRIPTOR LIST
	 * LENGTH field in the copy managers operating parameters.
	 */
	if (tdll + sdll > RCR_OP_MAX_DESC_LIST_LEN) {
		tcmu_dev_err(dev, "descriptor list length %u exceeds maximum %u\n",
			     tdll + sdll, RCR_OP_MAX_DESC_LIST_LEN);
		ret = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					  ASC_PARAMETER_LIST_LENGTH_ERROR,
					  NULL);
		goto err;
	}

	/*
	 * The INLINE DATA LENGTH field contains the number of bytes of inline
	 * data, after the last segment descriptor.
	 * */
	inline_dl = be32toh(*(uint32_t *)&par[12]);

	/* From spc4r31, section 6.3.1 EXTENDED COPY command introduction
	 *
	 * The EXTENDED COPY parameter list (see table 104) begins with a 16
	 * byte header.
	 *
	 * The data length in CDB should be equal to tdll + sdll + inline_dl
	 * + parameter list header length
	 */
	if (data_length < (XCOPY_HDR_LEN + tdll + sdll + inline_dl)) {
		tcmu_dev_err(dev, "Illegal list length: length from CDB is %u,"
			     " but here the length is %u\n",
			     data_length, tdll + sdll + inline_dl);
		ret = tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					  ASC_PARAMETER_LIST_LENGTH_ERROR,
					  NULL);
		goto err;
	}

	tcmu_dev_dbg(dev, "Processing XCOPY with tdll: %hu sdll: %u inline_dl: %u\n",
		     tdll, sdll, inline_dl);

	/*
	 * Parse the segment descripters and for now we only support block
	 * -> block type.
	 *
	 * The max seg_desc number support is 1(see RCR_OP_MAX_SG_DESC_COUNT)
	 */
	seg_desc = par + XCOPY_HDR_LEN + tdll;
	ret = xcopy_parse_segment_descs(seg_desc, xcopy, sdll, sense);
	if (ret != SAM_STAT_GOOD)
		goto err;

	/*
	 * Parse the target descripter
	 *
	 * The max seg_desc number support is 2(see RCR_OP_MAX_TARGET_DESC_COUNT)
	 */
	tgt_desc = par + XCOPY_HDR_LEN;
	ret = xcopy_parse_target_descs(dev, xcopy, tgt_desc, tdll, sense);
	if (ret != SAM_STAT_GOOD)
		goto err;

	if (tcmu_get_dev_block_size(xcopy->src_dev) !=
	    tcmu_get_dev_block_size(xcopy->dst_dev)) {
		tcmu_dev_err(dev, "The block size of src dev %u != dst dev %u\n",
			     tcmu_get_dev_block_size(xcopy->src_dev),
			     tcmu_get_dev_block_size(xcopy->dst_dev));
		ret = tcmu_set_sense_data(sense, COPY_ABORTED,
					  ASC_INCORRECT_COPY_TARGET_DEVICE_TYPE,
					  NULL);
		goto err;
	}

	num_lbas = tcmu_get_dev_num_lbas(xcopy->src_dev);
	if (xcopy->src_lba + xcopy->lba_cnt > num_lbas) {
		tcmu_dev_err(xcopy->src_dev,
			     "src target exceeds last lba %lld (lba %lld, copy len %lld)\n",
			     num_lbas, xcopy->src_lba, xcopy->lba_cnt);
		return tcmu_set_sense_data(cmd->sense_buf, ILLEGAL_REQUEST,
					   ASC_LBA_OUT_OF_RANGE, NULL);
	}

	num_lbas = tcmu_get_dev_num_lbas(xcopy->dst_dev);
	if (xcopy->dst_lba + xcopy->lba_cnt > num_lbas) {
		tcmu_dev_err(xcopy->dst_dev,
			     "dst target exceeds last lba %lld (lba %lld, copy len %lld)\n",
			     num_lbas, xcopy->dst_lba, xcopy->lba_cnt);
		return tcmu_set_sense_data(cmd->sense_buf, ILLEGAL_REQUEST,
					   ASC_LBA_OUT_OF_RANGE, NULL);
	}

	return SAM_STAT_GOOD;

err:
	free(par);

	return ret;
}

static void handle_xcopy_rw_sync(struct tcmu_device *dev,
				 struct tcmulib_cmd *cmd,
				 int ret)
{
	struct xcopy *xcopy = cmd->cmdstate;

	pthread_mutex_lock(&xcopy->lock);
	xcopy->cbk_finished = true;
	pthread_cond_signal(&xcopy->cond);
	pthread_mutex_unlock(&xcopy->lock);
}

static int xcopy_work_fn(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	uint32_t block_size = tcmu_get_dev_block_size(dev);
	struct xcopy *xcopy = cmd->cmdstate;
	struct tcmu_device *src_dev = xcopy->src_dev, *dst_dev = xcopy->dst_dev;
	uint64_t src_lba = xcopy->src_lba, dst_lba = xcopy->dst_lba, end_lba;
	unsigned short lba_cnt = xcopy->lba_cnt, copy_lbas, max_lbas;
	uint32_t max_sectors, src_max_sectors, dst_max_sectors;
	void *iov_base;
	size_t iov_len;
	struct iovec iovec;
	size_t iov_cnt;
	int ret;

	end_lba = src_lba + lba_cnt;

	src_max_sectors = tcmu_get_attribute(src_dev, "hw_max_sectors");
	dst_max_sectors = tcmu_get_attribute(dst_dev, "hw_max_sectors");

	max_sectors = min(src_max_sectors, dst_max_sectors);
	max_sectors = min(max_sectors, (uint32_t)XCOPY_MAX_SECTORS);
	max_lbas = min(max_sectors, ((uint32_t)(~0U)));

	iov_len = min(lba_cnt, max_lbas) * block_size;
	iov_base = calloc(1, iov_len);
	iov_cnt = 1;

	tcmu_dev_dbg(dev, "Source lba: %llu Number of lba:: %hu, Destination lba: %llu\n",
		     src_lba, lba_cnt, dst_lba);

	xcopy->cbk_finished = false;
	cmd->done = handle_xcopy_rw_sync;
	while (src_lba < end_lba) {
		copy_lbas = min(lba_cnt, max_lbas);

		iovec.iov_base = iov_base;
		iovec.iov_len = copy_lbas * block_size;
		ret = rhandler->read(src_dev, cmd, &iovec, iov_cnt,
				     tcmu_iovec_length(&iovec, iov_cnt),
				     block_size * src_lba);
		if (ret)
			goto out;
		pthread_mutex_lock(&xcopy->lock);
		if (!xcopy->cbk_finished)
			pthread_cond_wait(&xcopy->cond, &xcopy->lock);
		xcopy->cbk_finished = false;
		pthread_mutex_unlock(&xcopy->lock);

		src_lba += copy_lbas;

		iovec.iov_base = iov_base;
		iovec.iov_len = copy_lbas * block_size;
		ret = rhandler->write(dst_dev, cmd, &iovec, iov_cnt,
				      tcmu_iovec_length(&iovec, iov_cnt),
				      block_size * dst_lba);
		if (ret)
			goto out;
		pthread_mutex_lock(&xcopy->lock);
		if (!xcopy->cbk_finished)
			pthread_cond_wait(&xcopy->cond, &xcopy->lock);
		xcopy->cbk_finished = false;
		pthread_mutex_unlock(&xcopy->lock);

		dst_lba += copy_lbas;
		lba_cnt -= copy_lbas;
	}

	ret = SAM_STAT_GOOD;
out:
	free(iov_base);
	aio_command_finish(xcopy->origdev, cmd, ret);
	pthread_cond_destroy(&xcopy->cond);
	pthread_mutex_destroy(&xcopy->lock);
	free(xcopy);

	return ret;
}

/* async xcopy */
static int handle_xcopy(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	size_t data_length = tcmu_get_xfer_length(cdb);
	uint8_t *sense = cmd->sense_buf;
	struct xcopy *xcopy;
	int ret;

	/*
	 * A parameter list length of zero specifies that copy manager
	 * shall not transfer any data or alter any internal state.
	 */
	if (data_length == 0)
		return SAM_STAT_GOOD;

	/*
	 * The EXTENDED COPY parameter list begins with a 16 byte header
	 * that contains the LIST IDENTIFIER field.
	 */
	if (data_length < XCOPY_HDR_LEN) {
		tcmu_dev_err(dev, "Illegal parameter list: length %u < hdr_len %u\n",
			     data_length, XCOPY_HDR_LEN);
		return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
					   ASC_PARAMETER_LIST_LENGTH_ERROR,
					   NULL);
	}

	xcopy = calloc(1, sizeof(struct xcopy));
	if (!xcopy) {
		tcmu_dev_err(dev, "calloc xcopy data error\n");
		return tcmu_set_sense_data(sense, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE,
					   NULL);
	}

	/* Parse and check the parameter list */
	ret = xcopy_parse_parameter_list(dev, cmd, xcopy);
	if (ret != 0)
		goto err;

	xcopy->origdev = dev;
	pthread_cond_init(&xcopy->cond, NULL);
	pthread_mutex_init(&xcopy->lock, NULL);

	cmd->cmdstate = xcopy;

	ret = async_handle_cmd(dev, cmd, xcopy_work_fn);
	if (ret == TCMU_ASYNC_HANDLED)
		return ret;

err:
	free(xcopy);
	return ret;
}
/* async compare_and_write */

struct caw_state {
	size_t requested;
	void *read_buf;
	struct tcmulib_cmd *origcmd;
};

static struct tcmulib_cmd *
caw_init_readcmd(struct tcmulib_cmd *origcmd, size_t length)
{
	struct tcmulib_cmd *readcmd;
	struct caw_state *state;

	state = calloc(1, sizeof(*state));
	if (!state)
		goto out;
	readcmd = calloc(1, sizeof(*readcmd));
	if (!readcmd)
		goto free_state;
	readcmd->cdb = origcmd->cdb;

	if (alloc_iovec(readcmd, length))
		goto free_cmd;

	/* multi-op state maintainance */
	state->read_buf = readcmd->iovec->iov_base;
	state->requested = length;
	state->origcmd = origcmd;

	readcmd->cmdstate = state;
	return readcmd;

free_cmd:
	free(readcmd);
free_state:
	free(state);
out:
	return NULL;
}

static void caw_free_readcmd(struct tcmulib_cmd *readcmd)
{
	struct caw_state *state = readcmd->cmdstate;

	/* some handlers update iov_base */
	readcmd->iovec->iov_base = state->read_buf;
	free_iovec(readcmd);
	free(state);
	free(readcmd);
}

static void handle_caw_write_cbk(struct tcmu_device *dev,
				 struct tcmulib_cmd *cmd, int ret)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);

	pthread_mutex_unlock(&rdev->caw_lock);
	aio_command_finish(dev, cmd, ret);
}

static void handle_caw_read_cbk(struct tcmu_device *dev,
				struct tcmulib_cmd *readcmd, int ret)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	uint32_t cmp_offset;
	struct caw_state *state = readcmd->cmdstate;
	struct tcmulib_cmd *origcmd = state->origcmd;
	uint8_t *sense = origcmd->sense_buf;

	/* read failed - bail out */
	if (ret != SAM_STAT_GOOD) {
		memcpy(origcmd->sense_buf, readcmd->sense_buf,
		       sizeof(origcmd->sense_buf));
		goto finish_err;
	}

	cmp_offset = tcmu_compare_with_iovec(state->read_buf, origcmd->iovec,
					     state->requested);
	if (cmp_offset != -1) {
		/* verify failed - bail out */
		ret = tcmu_set_sense_data(sense, MISCOMPARE,
					  ASC_MISCOMPARE_DURING_VERIFY_OPERATION,
					  &cmp_offset);
		goto finish_err;
	}

	/* perform write */
	tcmu_seek_in_iovec(origcmd->iovec, state->requested);
	origcmd->done = handle_caw_write_cbk;

	ret = async_handle_cmd(dev, origcmd, write_work_fn);
	if (ret != TCMU_ASYNC_HANDLED)
		goto finish_err;

	caw_free_readcmd(readcmd);
	return;

finish_err:
	pthread_mutex_unlock(&rdev->caw_lock);
	aio_command_finish(dev, origcmd, ret);
	caw_free_readcmd(readcmd);
}

static int handle_caw(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret;
	struct tcmulib_cmd *readcmd;
	size_t half = (tcmu_iovec_length(cmd->iovec, cmd->iov_cnt)) / 2;
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);

	ret = check_lba_and_length(dev, cmd, cmd->cdb[13] * 2);
	if (ret)
		return ret;

	readcmd = caw_init_readcmd(cmd, half);
	if (!readcmd) {
		ret = SAM_STAT_TASK_SET_FULL;
		goto out;
	}

	readcmd->done = handle_caw_read_cbk;

	pthread_mutex_lock(&rdev->caw_lock);

	ret = async_handle_cmd(dev, readcmd, read_work_fn);
	if (ret == TCMU_ASYNC_HANDLED)
		return TCMU_ASYNC_HANDLED;

	pthread_mutex_unlock(&rdev->caw_lock);
	caw_free_readcmd(readcmd);
out:
	return ret;
}

/* async flush */
static int flush_work_fn(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);

	return rhandler->flush(dev, cmd);
}

static int handle_flush(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	cmd->done = handle_generic_cbk;
	return async_handle_cmd(dev, cmd, flush_work_fn);
}

static int handle_recv_copy_result(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct iovec *iovec = cmd->iovec;
	size_t iov_cnt = cmd->iov_cnt;
	uint8_t buf[128];
	uint16_t val16;
	uint32_t val32;

	memset(buf, 0, sizeof(buf));

	/*
	 * From spc4r31, section 6.18.4 OPERATING PARAMETERS service
	 * action
	 */

	/*
	 * SNLID = 1: the copy manager will support an EXTENDED COPY
	 * command parameter list in which the LIST ID USAGE field is
	 * set to 11b
	 */
	buf[4] = 0x01;

	/*
	 * MAXIMUM TARGET COUNT: the max number of target descriptors
	 * that the copy manager allows in a single EXTENDED COPY
	 * target descriptor list.
	 */
	val16 = htobe16(RCR_OP_MAX_TARGET_DESC_COUNT);
	memcpy(&buf[8], &val16, 2);

	/*
	 * MAXIMUM SEGMENT COUNT: the max number of segment descriptors
	 * that the copy manager allows in a single EXTENDED COPY
	 * segment descriptor list.
	 */
	val16 = htobe16(RCR_OP_MAX_SEGMENT_DESC_COUNT);
	memcpy(&buf[10], &val16, 2);

	/*
	 * MAXIMUM DESCRIPTOR LIST LENGTH: the max length, in bytes,
	 * of the target descriptor list and segment descriptor list.
	 */
	val32 = htobe32(RCR_OP_MAX_DESC_LIST_LEN);
	memcpy(&buf[12], &val32, 4);

	/*
	 * MAXIMUM SEGMENT LENGTH: the length, in bytes, of the largest
	 * amount of data that the copy manager supports writing via a
	 * single segment.
	 */
	val32 = htobe32(RCR_OP_MAX_SEGMENT_LEN);
	memcpy(&buf[16], &val32, 4);

	/*
	 * MAXIMUM CONCURRENT COPIES: the max number of EXTENDED COPY
	 * commands with the LIST ID USAGE field set to 00b or 10b that
	 * are supported for concurrent processing by the copy manager.
	 */
	val16 = htobe16(RCR_OP_TOTAL_CONCURR_COPIES);
	memcpy(&buf[34], &val16, 2);

	/*
	 * MAXIMUM CONCURRENT COPIES: the max number of EXTENDED COPY
	 * commands with the LIST ID USAGE field set to 00b or 10b that
	 * are supported for concurrent processing by the copy manager.
	 */
	buf[36] = RCR_OP_MAX_CONCURR_COPIES;

	/*
	 * DATA SEGMENT GRANULARITY: the length of the smallest data
	 * block that copy manager permits in a non-inline segment
	 * descriptor. In power of two.
	 */
	buf[37] = RCR_OP_DATA_SEG_GRAN_LOG2;

	/*
	 * INLINE DATA GRANULARITY: the length of the of the smallest
	 * block of inline data that the copy manager permits being
	 * written by a segment descriptor containing the 04h descriptor
	 * type code (see 6.3.7.7). In power of two.
	 */
	buf[38] = RCR_OP_INLINE_DATA_GRAN_LOG2;

	/*
	 * HELD DATA GRANULARITY: the length of the smallest block of
	 * held data that the copy manager shall transfer to the
	 * application client in response to a RECEIVE COPY RESULTS
	 * command with RECEIVE DATA service action (see 6.18.3).
	 * In power of two.
	 */
	buf[39] = RCR_OP_HELD_DATA_GRAN_LOG2;

	/*
	 * IMPLEMENTED DESCRIPTOR LIST LENGTH: the length, in bytes, of
	 * the list of implemented descriptor type codes.
	 */
	buf[43] = RCR_OP_IMPLE_DES_LIST_LENGTH;

	/*
	 * The list of implemented descriptor type codes: one byte for
	 * each segment or target DESCRIPTOR TYPE CODE value (see 6.3.5)
	 * supported by the copy manager,
	 */
	buf[44] = XCOPY_SEG_DESC_TYPE_CODE_B2B; /* block --> block */
	buf[45] = XCOPY_TARGET_DESC_TYPE_CODE_ID; /* Identification descriptor */

	/* AVAILABLE DATA (n-3)*/
	val32 = htobe32(42);
	memcpy(&buf[0], &val32, 4);

	tcmu_memcpy_into_iovec(iovec, iov_cnt, buf, sizeof(buf));

	return SAM_STAT_GOOD;
}

/* async write */
static int handle_write(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret;

	ret = check_lba_and_length(dev, cmd, tcmu_get_xfer_length(cmd->cdb));
	if (ret)
		return ret;

	cmd->done = handle_generic_cbk;
	return async_handle_cmd(dev, cmd, write_work_fn);
}

/* async read */
static int handle_read(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret;

	ret = check_lba_and_length(dev, cmd, tcmu_get_xfer_length(cmd->cdb));
	if (ret)
		return ret;

	cmd->done = handle_generic_cbk;
	return async_handle_cmd(dev, cmd, read_work_fn);
}

/* FORMAT UNIT */
struct format_unit_state {
	size_t length;
	off_t offset;
	void *write_buf;
	struct tcmulib_cmd *origcmd;
	uint32_t done_blocks;
};

static int format_unit_work_fn(struct tcmu_device *dev,
			       struct tcmulib_cmd *writecmd) {
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmulib_cmd *origcmd = writecmd->cmdstate;
	struct format_unit_state *state = origcmd->cmdstate;

	return rhandler->write(dev, writecmd, writecmd->iovec,
			       writecmd->iov_cnt, state->length, state->offset);
}

static void handle_format_unit_cbk(struct tcmu_device *dev,
				   struct tcmulib_cmd *writecmd, int ret) {
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	struct tcmulib_cmd *origcmd = writecmd->cmdstate;
	struct format_unit_state *state = origcmd->cmdstate;
	uint8_t *sense = origcmd->sense_buf;
	int rc;

	writecmd->iovec->iov_base = state->write_buf;
	state->offset += state->length;
	state->done_blocks += state->length / dev->block_size;
	if (state->done_blocks < dev->num_lbas)
		rdev->format_progress = (0x10000 * state->done_blocks) /
				       dev->num_lbas;

	/* Check for last commmand */
	if (state->done_blocks == dev->num_lbas) {
		tcmu_dev_dbg(dev,
			     "last format cmd, done_blocks:%lu num_lbas:%lu block_size:%lu\n",
			     state->done_blocks, dev->num_lbas, dev->block_size);
		goto free_iovec;
	}

	if (state->done_blocks < dev->num_lbas) {
		/* free iovec on every write, because seek in handlers consume
		 * the iovec, thus we can't re-use.
		 */
		free_iovec(writecmd);
		if ((dev->num_lbas - state->done_blocks) * dev->block_size < state->length)
		    state->length = (dev->num_lbas - state->done_blocks) * dev->block_size;
		if (alloc_iovec(writecmd, state->length)) {
			ret = tcmu_set_sense_data(sense, HARDWARE_ERROR,
						  ASC_INTERNAL_TARGET_FAILURE,
						  NULL);
			goto free_cmd;
		}

		/* copy incase handler changes it */
		state->write_buf = writecmd->iovec->iov_base;

		writecmd->done = handle_format_unit_cbk;

		tcmu_dev_dbg(dev,
			     "next format cmd, done_blocks:%lu num_lbas:%lu block_size:%lu\n",
			     state->done_blocks, dev->num_lbas, dev->block_size);

		rc = async_handle_cmd(dev, writecmd, format_unit_work_fn);
		if (rc != TCMU_ASYNC_HANDLED) {
			tcmu_dev_err(dev, " async handle cmd failure\n");
			ret = tcmu_set_sense_data(sense, MEDIUM_ERROR,
						  ASC_WRITE_ERROR,
						  NULL);
			goto free_iovec;
		}
	}

	return;

free_iovec:
	free_iovec(writecmd);
free_cmd:
	free(writecmd);
	free(state);
	pthread_mutex_lock(&rdev->format_lock);
	rdev->flags &= ~TCMUR_DEV_FORMATTING;
	pthread_mutex_unlock(&rdev->format_lock);
	aio_command_finish(dev, origcmd, ret);
}

static int handle_format_unit(struct tcmu_device *dev, struct tcmulib_cmd *cmd) {
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	struct tcmulib_cmd *writecmd;
	struct format_unit_state *state;
	size_t length = 1024 * 1024;
	uint8_t *sense = cmd->sense_buf;
	uint32_t block_size = tcmu_get_dev_block_size(dev);
	uint64_t num_lbas = tcmu_get_dev_num_lbas(dev);
	int ret;

	pthread_mutex_lock(&rdev->format_lock);
	if (rdev->flags & TCMUR_DEV_FORMATTING) {
		pthread_mutex_unlock(&rdev->format_lock);
		return tcmu_set_sense_data(sense, NOT_READY,
					  ASC_NOT_READY_FORMAT_IN_PROGRESS,
					  &rdev->format_progress);
	}
	rdev->format_progress = 0;
	rdev->flags |= TCMUR_DEV_FORMATTING;
	pthread_mutex_unlock(&rdev->format_lock);

	writecmd = calloc(1, sizeof(*writecmd));
	if (!writecmd)
		goto clear_format;
	writecmd->done = handle_format_unit_cbk;
	writecmd->cmdstate = cmd;

	state = calloc(1, sizeof(*state));
	if (!state)
		goto free_cmd;

	cmd->cmdstate = state;
	state->done_blocks = 0;
	state->length = length;

	/* Check length on first write to make sure its not less than 1MB */
	if ((num_lbas - state->done_blocks) * block_size < length)
		state->length = (num_lbas - state->done_blocks) * block_size;

	if (alloc_iovec(writecmd, state->length)) {
		goto free_state;
	}

	tcmu_dev_dbg(dev, "start emulate format, done_blocks:%lu num_lbas:%lu block_size:%lu\n",
		     state->done_blocks, num_lbas, block_size);

	/* copy incase handler changes it */
	state->write_buf = writecmd->iovec->iov_base;

	ret = async_handle_cmd(dev, writecmd, format_unit_work_fn);
	if (ret != TCMU_ASYNC_HANDLED)
		goto free_iov;

	return TCMU_ASYNC_HANDLED;

free_iov:
	free_iovec(writecmd);
free_state:
	free(state);
free_cmd:
	free(writecmd);
clear_format:
	pthread_mutex_lock(&rdev->format_lock);
	rdev->flags &= ~TCMUR_DEV_FORMATTING;
	pthread_mutex_unlock(&rdev->format_lock);
	return SAM_STAT_TASK_SET_FULL;
}

/* ALUA */
static int handle_stpg(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct list_head group_list;
	int ret;

	list_head_init(&group_list);

	ret = tcmu_get_tgt_port_grps(dev, &group_list);
	if (ret)
		return tcmu_set_sense_data(cmd->sense_buf, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE, NULL);

	ret = tcmu_emulate_set_tgt_port_grps(dev, &group_list, cmd);
	tcmu_release_tgt_port_grps(&group_list);
	return ret;
}

static int handle_rtpg(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct list_head group_list;
	int ret;

	list_head_init(&group_list);

	ret = tcmu_get_tgt_port_grps(dev, &group_list);
	if (ret)
		return tcmu_set_sense_data(cmd->sense_buf, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE, NULL);

	ret = tcmu_emulate_report_tgt_port_grps(dev, &group_list, cmd);
	tcmu_release_tgt_port_grps(&group_list);
	return ret;
}

/* command passthrough */
static int passthrough_work_fn(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);

	return rhandler->handle_cmd(dev, cmd);
}

static int handle_passthrough(struct tcmu_device *dev,
			      struct tcmulib_cmd *cmd)
{
	cmd->done = handle_generic_cbk;
	return async_handle_cmd(dev, cmd, passthrough_work_fn);
}

bool tcmur_handler_is_passthrough_only(struct tcmur_handler *rhandler)
{
	if (rhandler->write || rhandler->read || rhandler->flush)
		return false;

	return true;
}

int tcmur_cmd_passthrough_handler(struct tcmu_device *dev,
				  struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	int ret;

	if (!rhandler->handle_cmd)
		return TCMU_NOT_HANDLED;

	/*
	 * Support handlers that implement their own threading/AIO
	 * and only use runner's main event loop.
	 */
	if (!rhandler->nr_threads)
		return rhandler->handle_cmd(dev, cmd);
	/*
	 * Since we call ->handle_cmd via async_handle_cmd(), ->handle_cmd
	 * can finish in the callers context(asynchronous handler) or work
	 * queue context (synchronous handlers), thus we'd need to check if
	 * ->handle_cmd handled the passthough command here as well as in
	 * handle_passthrough_cbk().
	 */
	track_aio_request_start(rdev);
	ret = handle_passthrough(dev, cmd);
	if (ret != TCMU_ASYNC_HANDLED)
		track_aio_request_finish(rdev, NULL);

	return ret;
}

static int tcmur_cmd_handler(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret = TCMU_NOT_HANDLED;
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	uint8_t *cdb = cmd->cdb;

	track_aio_request_start(rdev);

	switch(cdb[0]) {
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		ret = handle_read(dev, cmd);
		break;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		ret = handle_write(dev, cmd);
		break;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		if (rhandler->flush)
			ret = handle_flush(dev, cmd);
		break;
	case EXTENDED_COPY:
		ret = handle_xcopy(dev, cmd);
		break;
	case RECEIVE_COPY_RESULTS:
		if ((cdb[1] & 0x1f) == RCR_SA_OPERATING_PARAMETERS)
			ret = handle_recv_copy_result(dev, cmd);
		break;
	case COMPARE_AND_WRITE:
		ret = handle_caw(dev, cmd);
		break;
	case WRITE_VERIFY:
	case WRITE_VERIFY_16:
		ret = handle_write_verify(dev, cmd);
		break;
	case WRITE_SAME_16:
		ret = handle_write_same_16(dev, cmd);
		break;
	case FORMAT_UNIT:
		ret = handle_format_unit(dev, cmd);
		break;
	case MAINTENANCE_IN:
		if ((cdb[1] & 0x1f) == MI_REPORT_TARGET_PGS)
			ret = handle_rtpg(dev, cmd);
		break;
	case MAINTENANCE_OUT:
		if (cdb[1] == MO_SET_TARGET_PGS)
			ret = handle_stpg(dev, cmd);
		break;
	default:
		ret = TCMU_NOT_HANDLED;
	}

	if (ret != TCMU_ASYNC_HANDLED)
		track_aio_request_finish(rdev, NULL);
	return ret;
}

static int handle_inquiry(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct list_head group_list;
	struct tgt_port *port;
	int ret;

	list_head_init(&group_list);

	ret = tcmu_get_tgt_port_grps(dev, &group_list);
	if (ret)
		return tcmu_set_sense_data(cmd->sense_buf, HARDWARE_ERROR,
					   ASC_INTERNAL_TARGET_FAILURE, NULL);

	/*
	 * Detect if the user did not setup ALUA or the kernel did not fully
	 * support it. ALUA tcmu support was added in 4.11. Before that and
	 * in the unsetup case, we will end up with at least the default ALUA
	 * group and a empty members (groups are not set to any LUNs) file. For
	 * these cases we just return tpgs=0.
	 */
	port = tcmu_get_enabled_port(&group_list);
	if (!port)
		tcmu_dev_dbg(dev, "no enabled ports found. Skipping ALUA support\n");

	ret = tcmu_emulate_inquiry(dev, port, cmd->cdb, cmd->iovec,
				   cmd->iov_cnt, cmd->sense_buf);
	tcmu_release_tgt_port_grps(&group_list);
	return ret;
}

static int handle_generic_cmd(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	struct iovec *iovec = cmd->iovec;
	size_t iov_cnt = cmd->iov_cnt;
	uint8_t *sense = cmd->sense_buf;
	uint32_t block_size = tcmu_get_dev_block_size(dev);
	uint64_t num_lbas = tcmu_get_dev_num_lbas(dev);

	switch (cdb[0]) {
	case INQUIRY:
		return handle_inquiry(dev, cmd);
	case TEST_UNIT_READY:
		return tcmu_emulate_test_unit_ready(cdb, iovec, iov_cnt, sense);
	case SERVICE_ACTION_IN_16:
		if (cdb[1] == READ_CAPACITY_16)
			return tcmu_emulate_read_capacity_16(num_lbas,
							     block_size,
							     cdb, iovec,
							     iov_cnt, sense);
		else
			return TCMU_NOT_HANDLED;
	case READ_CAPACITY:
		if ((cdb[1] & 0x01) || (cdb[8] & 0x01))
			/* Reserved bits for MM logical units */
			return tcmu_set_sense_data(sense, ILLEGAL_REQUEST,
						   ASC_INVALID_FIELD_IN_CDB,
						   NULL);
		else
			return tcmu_emulate_read_capacity_10(num_lbas,
							     block_size,
							     cdb, iovec,
							     iov_cnt, sense);
	case MODE_SENSE:
	case MODE_SENSE_10:
		return tcmu_emulate_mode_sense(cdb, iovec, iov_cnt, sense);
	case START_STOP:
		return tcmu_emulate_start_stop(dev, cdb, sense);
	case MODE_SELECT:
	case MODE_SELECT_10:
		return tcmu_emulate_mode_select(cdb, iovec, iov_cnt, sense);
	default:
		return TCMU_NOT_HANDLED;
	}
}

static bool command_is_generic(struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;

	switch(cdb[0]) {
	case INQUIRY:
	case TEST_UNIT_READY:
	case MODE_SENSE:
	case MODE_SENSE_10:
	case START_STOP:
	case MODE_SELECT:
	case MODE_SELECT_10:
	case READ_CAPACITY:
		return true;
	case SERVICE_ACTION_IN_16:
		if (cdb[1] == READ_CAPACITY_16)
			return true;
		/* fall through */
	default:
		return false;
	}
}

int tcmur_generic_handle_cmd(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	int ret;

	if (rdev->flags & TCMUR_DEV_FORMATTING && cmd->cdb[0] != INQUIRY)
		return tcmu_set_sense_data(cmd->sense_buf, NOT_READY,
					   ASC_NOT_READY_FORMAT_IN_PROGRESS,
					   &rdev->format_progress);

	/*
	 * The handler want to handle some commands by itself,
	 * try to passthrough it first
	 */
	if (rhandler->handle_cmd) {
		ret = rhandler->handle_cmd(dev, cmd);
		if (ret != TCMU_NOT_HANDLED)
			return ret;
	}

	/* Falls back to the runner's generic handle callout */
	if (command_is_generic(cmd))
		return handle_generic_cmd(dev, cmd);
	else
		return tcmur_cmd_handler(dev, cmd);
}
