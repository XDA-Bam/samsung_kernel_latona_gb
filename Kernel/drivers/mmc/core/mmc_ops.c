/*
 *  linux/drivers/mmc/core/mmc_ops.h
 *
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/scatterlist.h>

#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>

#include "core.h"
#include "mmc_ops.h"

static int _mmc_select_card(struct mmc_host *host, struct mmc_card *card)
{
	int err;
	struct mmc_command cmd;

	BUG_ON(!host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SELECT_CARD;

	if (card) {
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;
	} else {
		cmd.arg = 0;
		cmd.flags = MMC_RSP_NONE | MMC_CMD_AC;
	}

	err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	return 0;
}

int mmc_select_card(struct mmc_card *card)
{
	BUG_ON(!card);

	return _mmc_select_card(card->host, card);
}

int mmc_deselect_cards(struct mmc_host *host)
{
	return _mmc_select_card(host, NULL);
}

int mmc_card_sleepawake(struct mmc_host *host, int sleep)
{
	struct mmc_command cmd;
	struct mmc_card *card = host->card;
	int err;

	if (sleep)
		mmc_deselect_cards(host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SLEEP_AWAKE;
	cmd.arg = card->rca << 16;
	if (sleep)
		cmd.arg |= 1 << 15;

	cmd.flags = MMC_RSP_R1B | MMC_CMD_AC;
	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (err)
		return err;

	/*
	 * If the host does not wait while the card signals busy, then we will
	 * will have to wait the sleep/awake timeout.  Note, we cannot use the
	 * SEND_STATUS command to poll the status because that command (and most
	 * others) is invalid while the card sleeps.
	 */
	if (!(host->caps & MMC_CAP_WAIT_WHILE_BUSY))
		mmc_delay(DIV_ROUND_UP(card->ext_csd.sa_timeout, 10000));

	if (!sleep)
		err = mmc_select_card(card);

	return err;
}

int mmc_go_idle(struct mmc_host *host)
{
	int err;
	struct mmc_command cmd;

	/*
	 * Non-SPI hosts need to prevent chipselect going active during
	 * GO_IDLE; that would put chips into SPI mode.  Remind them of
	 * that in case of hardware that won't pull up DAT3/nCS otherwise.
	 *
	 * SPI hosts ignore ios.chip_select; it's managed according to
	 * rules that must accomodate non-MMC slaves which this layer
	 * won't even know about.
	 */
	if (!mmc_host_is_spi(host)) {
		mmc_set_chip_select(host, MMC_CS_HIGH);
		mmc_delay(1);
	}

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_GO_IDLE_STATE;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_NONE | MMC_CMD_BC;

	err = mmc_wait_for_cmd(host, &cmd, 0);

	mmc_delay(1);

	if (!mmc_host_is_spi(host)) {
		mmc_set_chip_select(host, MMC_CS_DONTCARE);
		mmc_delay(1);
	}

	host->use_spi_crc = 0;

	return err;
}

int mmc_send_op_cond(struct mmc_host *host, u32 ocr, u32 *rocr)
{
	struct mmc_command cmd;
	int i, err = 0;

	BUG_ON(!host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SEND_OP_COND;
	cmd.arg = mmc_host_is_spi(host) ? 0 : ocr;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R3 | MMC_CMD_BCR;

	for (i = 100; i; i--) {
		err = mmc_wait_for_cmd(host, &cmd, 0);
		if (err)
			break;

		/* if we're just probing, do a single pass */
		if (ocr == 0)
			break;

		/* otherwise wait until reset completes */
		if (mmc_host_is_spi(host)) {
			if (!(cmd.resp[0] & R1_SPI_IDLE))
				break;
		} else {
			if (cmd.resp[0] & MMC_CARD_BUSY)
				break;
		}

		err = -ETIMEDOUT;

		mmc_delay(10);
	}

	if (rocr && !mmc_host_is_spi(host))
		*rocr = cmd.resp[0];

	return err;
}

int mmc_all_send_cid(struct mmc_host *host, u32 *cid)
{
	int err;
	struct mmc_command cmd;

	BUG_ON(!host);
	BUG_ON(!cid);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_ALL_SEND_CID;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_R2 | MMC_CMD_BCR;

	err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	memcpy(cid, cmd.resp, sizeof(u32) * 4);

	return 0;
}

int mmc_set_relative_addr(struct mmc_card *card)
{
	int err;
	struct mmc_command cmd;

	BUG_ON(!card);
	BUG_ON(!card->host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SET_RELATIVE_ADDR;
	cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	return 0;
}

static int
mmc_send_cxd_native(struct mmc_host *host, u32 arg, u32 *cxd, int opcode)
{
	int err;
	struct mmc_command cmd;

	BUG_ON(!host);
	BUG_ON(!cxd);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = opcode;
	cmd.arg = arg;
	cmd.flags = MMC_RSP_R2 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	memcpy(cxd, cmd.resp, sizeof(u32) * 4);

	return 0;
}

static int
mmc_send_cxd_data(struct mmc_card *card, struct mmc_host *host,
		u32 opcode, void *buf, unsigned len)
{
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;
	void *data_buf;

	/* dma onto stack is unsafe/nonportable, but callers to this
	 * routine normally provide temporary on-stack buffers ...
	 */
	data_buf = kmalloc(len, GFP_KERNEL);
	if (data_buf == NULL)
		return -ENOMEM;

	memset(&mrq, 0, sizeof(struct mmc_request));
	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&data, 0, sizeof(struct mmc_data));

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = opcode;
	cmd.arg = 0;

	/* NOTE HACK:  the MMC_RSP_SPI_R1 is always correct here, but we
	 * rely on callers to never use this with "native" calls for reading
	 * CSD or CID.  Native versions of those commands use the R2 type,
	 * not R1 plus a data block.
	 */
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = len;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	sg_init_one(&sg, data_buf, len);

	if (opcode == MMC_SEND_CSD || opcode == MMC_SEND_CID) {
		/*
		 * The spec states that CSR and CID accesses have a timeout
		 * of 64 clock cycles.
		 */
		data.timeout_ns = 0;
		data.timeout_clks = 64;
	} else
		mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(host, &mrq);

	memcpy(buf, data_buf, len);
	kfree(data_buf);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}

int mmc_send_csd(struct mmc_card *card, u32 *csd)
{
	int ret, i;

	if (!mmc_host_is_spi(card->host))
		return mmc_send_cxd_native(card->host, card->rca << 16,
				csd, MMC_SEND_CSD);

	ret = mmc_send_cxd_data(card, card->host, MMC_SEND_CSD, csd, 16);
	if (ret)
		return ret;

	for (i = 0;i < 4;i++)
		csd[i] = be32_to_cpu(csd[i]);

	return 0;
}

int mmc_send_cid(struct mmc_host *host, u32 *cid)
{
	int ret, i;

	if (!mmc_host_is_spi(host)) {
		if (!host->card)
			return -EINVAL;
		return mmc_send_cxd_native(host, host->card->rca << 16,
				cid, MMC_SEND_CID);
	}

	ret = mmc_send_cxd_data(NULL, host, MMC_SEND_CID, cid, 16);
	if (ret)
		return ret;

	for (i = 0;i < 4;i++)
		cid[i] = be32_to_cpu(cid[i]);

	return 0;
}

int mmc_send_ext_csd(struct mmc_card *card, u8 *ext_csd)
{
	return mmc_send_cxd_data(card, card->host, MMC_SEND_EXT_CSD,
			ext_csd, 512);
}

#ifdef CONFIG_MMC_DISCARD_MOVINAND
static int mmc_send_trimsize_cmd(struct mmc_card *card, u32 arg)
{
	int err;
       unsigned int timeout = 0;
	struct mmc_command cmd;
	u32 status;

	BUG_ON(!card);
	BUG_ON(!card->host);

	memset(&cmd, 0, sizeof(struct mmc_command));
	
    cmd.opcode = 62;
    cmd.arg = arg;
    cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

    // Check to see if the moviNAND release DAT[0] or not
    while (timeout < 0xF0000) {
		mmc_send_status(card, &status);
		if(((status >> 9) & 0xF) == 0x4) break;
			timeout++;
	}

    if(timeout >= 0xF0000) {
        printk("mmc_get_trim_size() : Time-out waiting for releasing DAT0\r\n");
        return 0;
    }

	return 0;
}

static int mmc_send_trimsize_erase_cmd(struct mmc_card *card)
{
	int err;
    unsigned int timeout = 0;
	struct mmc_command cmd;
	u32 status;

	BUG_ON(!card);
	BUG_ON(!card->host);

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = MMC_ERASE_GROUP_START;
	cmd.arg = 0x4000A018;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		printk(KERN_ERR "mmc_erase: group start error %d, "
		       "status %#x\n", err, cmd.resp[0]);
		err = -EINVAL;
		goto out;
	}

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = MMC_ERASE_GROUP_END;
	cmd.arg = 0x00006400;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		printk(KERN_ERR "mmc_erase: group end error %d, status %#x\n",
		       err, cmd.resp[0]);
		err = -EINVAL;
		goto out;
	}

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = MMC_ERASE;
	cmd.arg = 0x0;
	cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	cmd.erase_timeout = 1000;
	//mmc_set_erase_timeout(card, &cmd, 0x0, 1000);
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err) {
		printk(KERN_ERR "mmc_erase: erase error %d, status %#x\n",
		       err, cmd.resp[0]);
		err = -EIO;
		goto out;
	}

	if (mmc_host_is_spi(card->host))
		goto out;

	do {
		memset(&cmd, 0, sizeof(struct mmc_command));
		cmd.opcode = MMC_SEND_STATUS;
		cmd.arg = card->rca << 16;
		cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;
		/* Do not retry else we can't see errors */
		err = mmc_wait_for_cmd(card->host, &cmd, 0);
		if (err || (cmd.resp[0] & 0xFDF92000)) {
			printk(KERN_ERR "error %d requesting status %#x\n",
				err, cmd.resp[0]);
			err = -EIO;
			goto out;
		}
	} while (!(cmd.resp[0] & R1_READY_FOR_DATA) ||
		 R1_CURRENT_STATE(cmd.resp[0]) == 7);
out:
	return err;
}


static int mmc_read_trimsize_data(struct mmc_card *card, u32 *trimsize)
{
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;
	u8 *buf;
	unsigned int len = 512;

	buf = kmalloc(len, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	memset(&mrq, 0, sizeof(struct mmc_request));
	memset(&cmd, 0, sizeof(struct mmc_command));
	memset(&data, 0, sizeof(struct mmc_data));

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = MMC_READ_SINGLE_BLOCK;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = len;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	//data.timeout_ns = card->csd.tacc_ns * 10;
	//data.timeout_clks = card->csd.tacc_clks * 10;

	mrq.stop = NULL;
	mmc_set_data_timeout(&data, card);

	sg_init_one(&sg, buf, len);

	mmc_wait_for_req(card->host, &mrq);


	
	*trimsize = (buf[84] << 0 |
			buf[84 + 1] << 8 |
			buf[84 + 2] << 16 |
			buf[84 + 3] << 24);
	*trimsize /= 512; /* in sectors */
	kfree(buf);

	if (cmd.error)
		return cmd.error;
	if (data.error)
		return data.error;

	return 0;
}

int mmc_send_trimsize(struct mmc_card *card, u32 *trimsize)
{
	int ret;
	u32 status;

	/*init trimsize to zero*/
	*trimsize = 0;

#if 0
    /*set trimsize forcely*/

    if(card->ext_csd.rev == 5 )
    	{
    	if(card->ext_csd.sectors <= 32 * 1024 * 1024 * 2) {
			*trimsize = 256;
    	} else {
    		*trimsize = 512;
    		}
    	}

	return 0;
#else

	ret = mmc_send_trimsize_cmd(card, 0xEFAC62EC);

	if (ret)
		return ret;
	ret = mmc_send_trimsize_cmd(card, 0x10210000);
	if (ret)
		return ret;
	
	/*do not return even if error is happend to escape vendor mode*/
    mmc_send_trimsize_erase_cmd(card);	

		
	ret = mmc_send_trimsize_cmd(card,  0xEFAC62EC);
	if (ret)
		return ret;
	ret = mmc_send_trimsize_cmd(card,  0xDECCEE);
	if (ret)
		return ret;
	
	
	ret = mmc_send_trimsize_cmd(card, 0xEFAC62EC);

	if (ret)
		return ret;
	ret = mmc_send_trimsize_cmd(card, 0xCCEE);
	if (ret)
		return ret;

	/*do not return even if error is happend to escape vendor mode*/
	mmc_read_trimsize_data(card, trimsize);
	printk("[MMC] %s trim size is %d\n",__func__,*trimsize);
	
	ret = mmc_send_trimsize_cmd(card, 0xEFAC62EC);
	if (ret)
		return ret;
	ret = mmc_send_trimsize_cmd(card, 0xDECCEE);
	if (ret)
		return ret;
#endif

	return 0;
}
#endif /* CONFIG_MMC_DISCARD_MOVINAND */

int mmc_spi_read_ocr(struct mmc_host *host, int highcap, u32 *ocrp)
{
	struct mmc_command cmd;
	int err;

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SPI_READ_OCR;
	cmd.arg = highcap ? (1 << 30) : 0;
	cmd.flags = MMC_RSP_SPI_R3;

	err = mmc_wait_for_cmd(host, &cmd, 0);

	*ocrp = cmd.resp[1];
	return err;
}

int mmc_spi_set_crc(struct mmc_host *host, int use_crc)
{
	struct mmc_command cmd;
	int err;

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SPI_CRC_ON_OFF;
	cmd.flags = MMC_RSP_SPI_R1;
	cmd.arg = use_crc;

	err = mmc_wait_for_cmd(host, &cmd, 0);
	if (!err)
		host->use_spi_crc = use_crc;
	return err;
}

int mmc_switch(struct mmc_card *card, u8 set, u8 index, u8 value)
{
	int err;
	struct mmc_command cmd;
	u32 status;

	BUG_ON(!card);
	BUG_ON(!card->host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SWITCH;
	cmd.arg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
		  (index << 16) |
		  (value << 8) |
		  set;
	cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	/*WA : add delay for 24nm iNAND which have FW defect.*/
	mdelay(2);

	/* Must check status to be sure of no errors */
	do {
		err = mmc_send_status(card, &status);
		if (err)
			return err;
		if (card->host->caps & MMC_CAP_WAIT_WHILE_BUSY)
			break;
		if (mmc_host_is_spi(card->host))
			break;
	} while (R1_CURRENT_STATE(status) == 7);

	if (mmc_host_is_spi(card->host)) {
		if (status & R1_SPI_ILLEGAL_COMMAND)
			return -EBADMSG;
	} else {
		if (status & 0xFDFFA000)
			printk(KERN_WARNING "%s: unexpected status %#x after "
			       "switch", mmc_hostname(card->host), status);
		if (status & R1_SWITCH_ERROR)
			return -EBADMSG;
	}

	return 0;
}

int mmc_send_status(struct mmc_card *card, u32 *status)
{
	int err;
	struct mmc_command cmd;

	BUG_ON(!card);
	BUG_ON(!card->host);

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_SEND_STATUS;
	if (!mmc_host_is_spi(card->host))
		cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, MMC_CMD_RETRIES);
	if (err)
		return err;

	/* NOTE: callers are required to understand the difference
	 * between "native" and SPI format status words!
	 */
	if (status)
		*status = cmd.resp[0];

	return 0;
}

	/* Lock/Unlock CMD */
int mmc_send_lock_cmd(struct mmc_host *host, int lock)
{
	struct mmc_request mrq = {0};
	struct mmc_command cmd = {0};
	struct mmc_data data = {0};
	struct scatterlist sg;
	u8 *data_buf;
	u32 status;
	int err = 0;

	/* Set data 1 */
	data_buf = kmalloc(512, GFP_KERNEL);
	if (!data_buf)
		return -ENOMEM;

	memset((void *)data_buf, 0, sizeof(data_buf));

	if (lock) {
		data_buf[0] = 0x05;		// lock
	} else {
		data_buf[0] = 0x02;		// unlock
	}
	data_buf[1] = 0x04;			// set passwd
	data_buf[2] = 0x31;
	data_buf[3] = 0x32;
	data_buf[4] = 0x33;
	data_buf[5] = 0x34;

	/* Set command CMD42 */
	mrq.cmd = &cmd;
	mrq.data = &data;
	cmd.opcode = MMC_LOCK_UNLOCK;
	cmd.arg = 0;

	cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_ADTC;

	/* Set data 2 */
	data.blksz = 512;
	data.blocks = 1;
	data.flags = MMC_DATA_WRITE;

/*	
	printk(KERN_DEBUG "[TEST] CMD42's buf data = 0x%x%x%x%x%x%x(%s).\n",
			data_buf[0],
			data_buf[1],
			data_buf[2],
			data_buf[3],
			data_buf[4],
			data_buf[5],
			lock ? "lock" : "unlock");
*/
	/* Send CMD */
	data.sg = &sg;
	data.sg_len = 1;

	mmc_set_data_timeout(&data, host->card);
	
	sg_init_one(&sg, data_buf, 512);
	mmc_wait_for_req(host, &mrq);
	kfree(data_buf);

	if (cmd.error) {
		printk(KERN_ERR "%s: CMD%d: Command Error, return %d.\n",
				mmc_hostname(host),
				cmd.opcode, cmd.error);
		return cmd.error;
	}
	if (data.error) {
		printk(KERN_ERR "%s: CMD%d: Data Error, return %d.\n",
				mmc_hostname(host),
				cmd.opcode, data.error);
		return data.error;
	}

	/* check status */
	do {
		err = mmc_send_status(host->card, &status);
		if (err)
			return err;
	} while (R1_CURRENT_STATE(status) == 7);

	return err;
}
