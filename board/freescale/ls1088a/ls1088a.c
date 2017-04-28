/*
 * Copyright 2017 NXP
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#include <common.h>
#include <i2c.h>
#include <malloc.h>
#include <errno.h>
#include <netdev.h>
#include <fsl_ifc.h>
#include <fsl_ddr.h>
#include <fsl_sec.h>
#include <asm/io.h>
#include <fdt_support.h>
#include <libfdt.h>
#include <fsl-mc/fsl_mc.h>
#include <environment.h>
#include <asm/arch-fsl-layerscape/soc.h>
#include <asm/arch/ppa.h>

#include "../common/qixis.h"
#include "ls1088a_qixis.h"
#include <fsl_immap.h>

DECLARE_GLOBAL_DATA_PTR;

unsigned long long get_qixis_addr(void)
{
	unsigned long long addr;

	if (gd->flags & GD_FLG_RELOC)
		addr = QIXIS_BASE_PHYS;
	else
		addr = QIXIS_BASE_PHYS_EARLY;

	/*
	 * IFC address under 256MB is mapped to 0x30000000, any address above
	 * is mapped to 0x5_10000000 up to 4GB.
	 */
	addr = addr  > 0x10000000 ? addr + 0x500000000ULL : addr + 0x30000000;

	return addr;
}

int checkboard(void)
{
	char buf[64];
	u8 sw;
	static const char *const freq[] = {"100", "125", "156.25",
					    "100 separate SSCG"};
	int clock;

#ifdef CONFIG_TARGET_LS1088AQDS
	printf("Board: LS1088A-QDS, ");
#else
	printf("Board: LS1088A-RDB, ");
#endif
	sw = QIXIS_READ(arch);
	printf("Board Arch: V%d, ", sw >> 4);
#ifdef CONFIG_TARGET_LS1088AQDS
	printf("Board version: %c, boot from ", (sw & 0xf) + 'A' - 1);
#else
	printf("Board version: %c, boot from ", (sw & 0xf) + 'A');
#endif

	memset((u8 *)buf, 0x00, ARRAY_SIZE(buf));

	sw = QIXIS_READ(brdcfg[0]);
	sw = (sw & QIXIS_LBMAP_MASK) >> QIXIS_LBMAP_SHIFT;

#ifdef CONFIG_SD_BOOT
	puts("SD card\n");
#endif
	switch (sw) {
#ifdef CONFIG_TARGET_LS1088AQDS
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
		printf("vBank: %d\n", sw);
		break;
	case 8:
		puts("PromJet\n");
		break;
	case 15:
		puts("IFCCard\n");
		break;
	case 14:
#else
	case 0:
#endif
		puts("QSPI:");
		sw = QIXIS_READ(brdcfg[0]);
		sw = (sw & QIXIS_QMAP_MASK) >> QIXIS_QMAP_SHIFT;
		if (sw == 0 || sw == 4)
			puts("0\n");
		else if (sw == 1)
			puts("1\n");
		else
			puts("EMU\n");
		break;

	default:
		printf("invalid setting of SW%u\n", QIXIS_LBMAP_SWITCH);
		break;
	}

#ifdef CONFIG_TARGET_LS1088AQDS
	printf("FPGA: v%d (%s), build %d",
	       (int)QIXIS_READ(scver), qixis_read_tag(buf),
	       (int)qixis_read_minor());
	/* the timestamp string contains "\n" at the end */
	printf(" on %s", qixis_read_time(buf));
#else
	printf("CPLD: v%d.%d\n", QIXIS_READ(scver), QIXIS_READ(tagdata));
#endif

	/*
	 * Display the actual SERDES reference clocks as configured by the
	 * dip switches on the board.  Note that the SWx registers could
	 * technically be set to force the reference clocks to match the
	 * values that the SERDES expects (or vice versa).  For now, however,
	 * we just display both values and hope the user notices when they
	 * don't match.
	 */
	puts("SERDES1 Reference : ");
	sw = QIXIS_READ(brdcfg[2]);
	clock = (sw >> 6) & 3;
	printf("Clock1 = %sMHz ", freq[clock]);
	clock = (sw >> 4) & 3;
	printf("Clock2 = %sMHz", freq[clock]);

	puts("\nSERDES2 Reference : ");
	clock = (sw >> 2) & 3;
	printf("Clock1 = %sMHz ", freq[clock]);
	clock = (sw >> 0) & 3;
	printf("Clock2 = %sMHz\n", freq[clock]);

	return 0;
}

bool if_board_diff_clk(void)
{
#ifdef CONFIG_TARGET_LS1088AQDS
	u8 diff_conf = QIXIS_READ(brdcfg[11]);
	return diff_conf & 0x40;
#else
	u8 diff_conf = QIXIS_READ(dutcfg[11]);
	return diff_conf & 0x80;
#endif
}

unsigned long get_board_sys_clk(void)
{
	u8 sysclk_conf = QIXIS_READ(brdcfg[1]);

	switch (sysclk_conf & 0x0f) {
	case QIXIS_SYSCLK_83:
		return 83333333;
	case QIXIS_SYSCLK_100:
		return 100000000;
	case QIXIS_SYSCLK_125:
		return 125000000;
	case QIXIS_SYSCLK_133:
		return 133333333;
	case QIXIS_SYSCLK_150:
		return 150000000;
	case QIXIS_SYSCLK_160:
		return 160000000;
	case QIXIS_SYSCLK_166:
		return 166666666;
	}

	return 66666666;
}

unsigned long get_board_ddr_clk(void)
{
	u8 ddrclk_conf = QIXIS_READ(brdcfg[1]);

	if (if_board_diff_clk())
		return get_board_sys_clk();
	switch ((ddrclk_conf & 0x30) >> 4) {
	case QIXIS_DDRCLK_100:
		return 100000000;
	case QIXIS_DDRCLK_125:
		return 125000000;
	case QIXIS_DDRCLK_133:
		return 133333333;
	}

	return 66666666;
}

int select_i2c_ch_pca9547(u8 ch)
{
	int ret;

	ret = i2c_write(I2C_MUX_PCA_ADDR_PRI, 0, 1, &ch, 1);
	if (ret) {
		puts("PCA: failed to select proper channel\n");
		return ret;
	}

	return 0;
}

void board_retimer_init(void)
{
	u8 reg;

	/* Retimer is connected to I2C1_CH5 */
	select_i2c_ch_pca9547(I2C_MUX_CH5);

	/* Access to Control/Shared register */
	reg = 0x0;
	i2c_write(I2C_RETIMER_ADDR, 0xff, 1, &reg, 1);

	/* Read device revision and ID */
	i2c_read(I2C_RETIMER_ADDR, 1, 1, &reg, 1);
	debug("Retimer version id = 0x%x\n", reg);

	/* Enable Broadcast. All writes target all channel register sets */
	reg = 0x0c;
	i2c_write(I2C_RETIMER_ADDR, 0xff, 1, &reg, 1);

	/* Reset Channel Registers */
	i2c_read(I2C_RETIMER_ADDR, 0, 1, &reg, 1);
	reg |= 0x4;
	i2c_write(I2C_RETIMER_ADDR, 0, 1, &reg, 1);

	/* Set data rate as 10.3125 Gbps */
	reg = 0x90;
	i2c_write(I2C_RETIMER_ADDR, 0x60, 1, &reg, 1);
	reg = 0xb3;
	i2c_write(I2C_RETIMER_ADDR, 0x61, 1, &reg, 1);
	reg = 0x90;
	i2c_write(I2C_RETIMER_ADDR, 0x62, 1, &reg, 1);
	reg = 0xb3;
	i2c_write(I2C_RETIMER_ADDR, 0x63, 1, &reg, 1);
	reg = 0xcd;
	i2c_write(I2C_RETIMER_ADDR, 0x64, 1, &reg, 1);

	/* Select VCO Divider to full rate (000) */
	i2c_read(I2C_RETIMER_ADDR, 0x2F, 1, &reg, 1);
	reg &= 0x0f;
	reg |= 0x70;
	i2c_write(I2C_RETIMER_ADDR, 0x2F, 1, &reg, 1);

#ifdef	CONFIG_TARGET_LS1088AQDS
	/* Retimer is connected to I2C1_CH5 */
	select_i2c_ch_pca9547(I2C_MUX_CH5);

	/* Access to Control/Shared register */
	reg = 0x0;
	i2c_write(I2C_RETIMER_ADDR2, 0xff, 1, &reg, 1);

	/* Read device revision and ID */
	i2c_read(I2C_RETIMER_ADDR2, 1, 1, &reg, 1);
	debug("Retimer version id = 0x%x\n", reg);

	/* Enable Broadcast. All writes target all channel register sets */
	reg = 0x0c;
	i2c_write(I2C_RETIMER_ADDR2, 0xff, 1, &reg, 1);

	/* Reset Channel Registers */
	i2c_read(I2C_RETIMER_ADDR2, 0, 1, &reg, 1);
	reg |= 0x4;
	i2c_write(I2C_RETIMER_ADDR2, 0, 1, &reg, 1);

	/* Set data rate as 10.3125 Gbps */
	reg = 0x90;
	i2c_write(I2C_RETIMER_ADDR2, 0x60, 1, &reg, 1);
	reg = 0xb3;
	i2c_write(I2C_RETIMER_ADDR2, 0x61, 1, &reg, 1);
	reg = 0x90;
	i2c_write(I2C_RETIMER_ADDR2, 0x62, 1, &reg, 1);
	reg = 0xb3;
	i2c_write(I2C_RETIMER_ADDR2, 0x63, 1, &reg, 1);
	reg = 0xcd;
	i2c_write(I2C_RETIMER_ADDR2, 0x64, 1, &reg, 1);

	/* Select VCO Divider to full rate (000) */
	i2c_read(I2C_RETIMER_ADDR2, 0x2F, 1, &reg, 1);
	reg &= 0x0f;
	reg |= 0x70;
	i2c_write(I2C_RETIMER_ADDR2, 0x2F, 1, &reg, 1);
#endif
	/*return the default channel*/
	select_i2c_ch_pca9547(I2C_MUX_CH_DEFAULT);
}
static int i2c_multiplexer_select_vid_channel(u8 channel)
{
	return select_i2c_ch_pca9547(channel);
}

/* this function set the DDR bit in case of 0.9V VDD */
static void ddr_enable_0_9_vdd(void)
{
	struct ccsr_ddr *ddr1;
	ddr1  = (void *)(CONFIG_SYS_FSL_DDR_ADDR);
	u32 tmp;

	tmp = in_le32(&ddr1->ddr_cdr1);

	tmp |= DDR_CDR1_V0PT9_EN;
	out_le32(&ddr1->ddr_cdr1, tmp);
}

/* read the current value of the LTC Regulator Voltage */
static inline int read_voltage(void)
{
	int  ret, vcode = 0;

	/* select the PAGE 0 using PMBus commands PAGE for VDD*/
	ret = i2c_write(I2C_VOL_MONITOR_ADDR,
		PMBUS_CMD_PAGE, 1, PWM_CHANNEL0, 1);
	if (ret) {
			printf("VID: failed to select VDD Page 0\n");
			return ret;
	}
	/*read the output voltage using PMBus command READ_VOUT*/

	ret = i2c_read(I2C_VOL_MONITOR_ADDR,
		PMBUS_CMD_READ_VOUT, 1, (void *)&vcode, 2);
	if (ret) {
		printf("VID: failed to read the volatge\n");
		return ret;
	}
	return vcode;
}

/* read the current value(SVDD) of the LTM Regulator Voltage */
static inline int read_svdd_LTM4675(void)
{
	int  ret, vcode = 0;

	/* select the PAGE 0 using PMBus commands PAGE for VDD*/
	ret = i2c_write(I2C_SVDD_MONITOR_ADDR,
		PMBUS_CMD_PAGE, 1, PWM_CHANNEL0, 1);
	if (ret) {
			printf("VID: failed to select VDD Page 0\n");
			return ret;
	}

	/*read the output voltage using PMBus command READ_VOUT*/
	ret = i2c_read(I2C_SVDD_MONITOR_ADDR,
		PMBUS_CMD_READ_VOUT, 1, (void *)&vcode, 2);
	if (ret) {
		printf("VID: failed to read the volatge\n");
		return ret;
	}
	return vcode;
}


/* returns the Lower byte of the vdd code */
static u8 get_LSB(int vdd)
{
	u8 *lower = (u8 *)&vdd;
	return *(lower);
}

/*returns the Upper byte of the vdd code*/
static u8 get_MSB(int vdd)
{
	u8 *lower = (u8 *)&vdd;
	++lower;
	return *(lower);
}


/* this function sets the 5-byte buffer which needs to be sent following the
 * PMBus command PAGE_PLUS_WRITE
 */
static void set_buffer_page_plus_write(u8 *buff, int vid)
{
	buff[0] = 0x04;

	buff[1] = PWM_CHANNEL0;

	buff[2] = PMBUS_CMD_VOUT_COMMAND;

	buff[3] = get_LSB(vid);

	buff[4] = get_MSB(vid);
}
/* this function sets the VDD and returns the value set */
static int set_voltage_to_LTC(int vid)
{
	int ret, vdd_last;
	u8 buff[5];

	/*number of bytes of the rest of the package*/
	set_buffer_page_plus_write((u8 *)&buff, vid);

	/*write the desired voltage code to the regulator*/
	ret = i2c_write(I2C_VOL_MONITOR_ADDR,
		PMBUS_CMD_PAGE_PLUS_WRITE, 1, (void *)&buff, 5);
	if (ret) {
		printf("VID: I2C failed to write to the volatge regulator\n");
		return -1;
	}

	/*wait for the volatge to get to the desired value*/

	do {
		vdd_last = read_voltage();
		if (vdd_last < 0) {
			printf("VID: Couldn't read sensor abort VID adjust\n");
			return -1;
		}
	} while (vdd_last != vid);

	return vdd_last;
}
#ifdef CONFIG_TARGET_LS1088AQDS
static int change_0_9_svddqds(int svdd)
{
	int ret, vdd_last;
	u8 buff[5];

	/*number of bytes of the rest of the package*/
	set_buffer_page_plus_write((u8 *)&buff, svdd);

	/*write the desired voltage code to the SVDD regulator*/
	ret = i2c_write(I2C_SVDD_MONITOR_ADDR,
		PMBUS_CMD_PAGE_PLUS_WRITE, 1, (void *)&buff, 5);
	if (ret) {
		printf("VID: I2C failed to write to the volatge regulator\n");
		return -1;
	}

	/*wait for the volatge to get to the desired value*/
	do {
		vdd_last = read_svdd_LTM4675();
		if (vdd_last < 0) {
			printf("VID: Couldn't read sensor abort VID adjust\n");
			return -1;
		}
	} while (vdd_last != svdd);

	return 1;
}
#else
static int change_0_9_svddrdb(int svdd)
{
	int ret;
	u8 brdcfg4;

	printf("SVDD changing of RDB\n");

	/*read the BRDCFG54 via CLPD*/
	ret = i2c_read(CONFIG_SYS_I2C_FPGA_ADDR,
		QIXIS_BRDCFG4_OFFSET, 1, (void *)&brdcfg4, 1);
	if (ret) {
		printf("VID: I2C failed to read the CPLD BRDCFG4\n");
		return -1;
	}

	brdcfg4 = brdcfg4 | 0x08;

	/* write to the BRDCFG4 */
	ret = i2c_write(CONFIG_SYS_I2C_FPGA_ADDR,
		QIXIS_BRDCFG4_OFFSET, 1, (void *)&brdcfg4, 1);
	if (ret) {
		debug("VID: I2C failed to set the SVDD CPLD BRDCFG4\n");
		return -1;
	}

	/*wait for the volatge to get to the desired value*/
	udelay(10000);

	return 1;
}
#endif

/* this function disables the SERDES, changes the SVDD Voltage and enables it*/
int switch_svdd(u32 svdd)
{
	struct ccsr_gur *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	struct ccsr_serdes *serdes1_base, *serdes2_base;
	u32 cfg_rcwsrds1 = gur_in32(&gur->rcwsr[FSL_CHASSIS3_SRDS1_REGSR - 1]);
	u32 cfg_rcwsrds2 = gur_in32(&gur->rcwsr[FSL_CHASSIS3_SRDS2_REGSR - 1]);
	u32 cfg_tmp, reg = 0;
	int ret = 1;
	int i;

	/* Only support switch SVDD to 900mV */
	if (svdd != 0x0E66)
		return -1;

	serdes1_base = (void *)CONFIG_SYS_FSL_LSCH3_SERDES_ADDR;
	serdes2_base =  (void *)(CONFIG_SYS_FSL_LSCH3_SERDES_ADDR + 0x10000);

	/* Put the all enabled lanes in reset */

#ifdef CONFIG_SYS_FSL_SRDS_1
	cfg_tmp = cfg_rcwsrds1 & FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg_tmp >>= 16;

	for (i = 0; i < 4 && cfg_tmp & (0xf << (3 - i)); i++) {
		reg = in_le32(&serdes1_base->lane[i].gcr0);
		reg &= 0xFF9FFFFF;
		out_le32(&serdes1_base->lane[i].gcr0, reg);
		reg = in_le32(&serdes1_base->lane[i].gcr0);
	}
#endif

#ifdef CONFIG_SYS_FSL_SRDS_2
	cfg_tmp = cfg_rcwsrds2 & FSL_CHASSIS3_SRDS2_PRTCL_MASK;

	for (i = 0; i < 4 && cfg_tmp & (0xf << (3 - i)); i++) {
		reg = in_le32(&serdes2_base->lane[i].gcr0);
		reg &= 0xFF9FFFFF;
		out_le32(&serdes2_base->lane[i].gcr0, reg);
		reg = in_le32(&serdes2_base->lane[i].gcr0);
	}
#endif

	/* Put the all enabled PLL in reset */
#ifdef CONFIG_SYS_FSL_SRDS_1
	cfg_tmp = cfg_rcwsrds1 & 0x3;
	for (i = 0; i < 2 && !(cfg_tmp & (0x1 << (1 - i))); i++) {
		reg = in_le32(&serdes1_base->bank[i].rstctl);
		reg &= 0xFFFFFFBF;
		reg |= 0x10000000;
		out_le32(&serdes1_base->bank[i].rstctl, reg);
	}
	udelay(1);

	reg = in_le32(&serdes1_base->bank[i].rstctl);
	reg &= 0xFFFFFF1F;
	out_le32(&serdes1_base->bank[i].rstctl, reg);
#endif

#ifdef CONFIG_SYS_FSL_SRDS_2

	cfg_tmp = cfg_rcwsrds1 & 0xC;
	cfg_tmp >>= 2;
	for (i = 0; i < 2 && !(cfg_tmp & (0x1 << (1 - i))); i++) {
		reg = in_le32(&serdes2_base->bank[i].rstctl);
		reg &= 0xFFFFFFBF;
		reg |= 0x10000000;
		out_le32(&serdes2_base->bank[i].rstctl, reg);
	}
	udelay(1);

	reg = in_le32(&serdes2_base->bank[i].rstctl);
	reg &= 0xFFFFFF1F;
	out_le32(&serdes2_base->bank[i].rstctl, reg);

#endif

	/* Put the Rx/Tx calibration into reset */
#ifdef CONFIG_SYS_FSL_SRDS_1
	reg = in_le32(&serdes1_base->srdstcalcr);
	reg &= 0xF7FFFFFF;
	out_le32(&serdes1_base->srdstcalcr, reg);
	reg = in_le32(&serdes1_base->srdsrcalcr);
	reg &= 0xF7FFFFFF;
	out_le32(&serdes1_base->srdsrcalcr, reg);
#endif

#ifdef CONFIG_SYS_FSL_SRDS_2
	reg = in_le32(&serdes2_base->srdstcalcr);
	reg &= 0xF7FFFFFF;
	out_le32(&serdes2_base->srdstcalcr, reg);
	reg = in_le32(&serdes2_base->srdsrcalcr);
	reg &= 0xF7FFFFFF;
	out_le32(&serdes2_base->srdsrcalcr, reg);
#endif

#ifdef CONFIG_TARGET_LS1088AQDS
	ret = change_0_9_svddqds(svdd);
	if (ret < 0) {
		printf("could not change SVDD\n");
		ret = -1;
	}
#else
	ret = change_0_9_svddrdb(svdd);
	if (ret < 0) {
		printf("could not change SVDD\n");
		ret = -1;
	}
#endif

	/* For each PLL that’s not disabled via RCW enable the SERDES */
#ifdef CONFIG_SYS_FSL_SRDS_1
	cfg_tmp = cfg_rcwsrds1 & 0x3;
	for (i = 0; i < 2 && !(cfg_tmp & (0x1 << (1 - i))); i++) {
		reg = in_le32(&serdes1_base->bank[i].rstctl);
		reg |= 0x00000020;
		out_le32(&serdes1_base->bank[i].rstctl, reg);
		udelay(1);

		reg = in_le32(&serdes1_base->bank[i].rstctl);
		reg |= 0x00000080;
		out_le32(&serdes1_base->bank[i].rstctl, reg);
		udelay(1);
		/* Take the Rx/Tx calibration out of reset */
		if (!(cfg_tmp == 0x3 && i == 1)) {
			udelay(1);
			reg = in_le32(&serdes1_base->srdstcalcr);
			reg |= 0x08000000;
			out_le32(&serdes1_base->srdstcalcr, reg);
			reg = in_le32(&serdes1_base->srdsrcalcr);
			reg |= 0x08000000;
			out_le32(&serdes1_base->srdsrcalcr, reg);
		}
	udelay(1);
	}
#endif
#ifdef CONFIG_SYS_FSL_SRDS_2
	cfg_tmp = cfg_rcwsrds1 & 0xC;
	cfg_tmp >>= 2;
	for (i = 0; i < 2 && !(cfg_tmp & (0x1 << (1 - i))); i++) {
		reg = in_le32(&serdes2_base->bank[i].rstctl);
		reg |= 0x00000020;
		out_le32(&serdes2_base->bank[i].rstctl, reg);
		udelay(1);

		reg = in_le32(&serdes2_base->bank[i].rstctl);
		reg |= 0x00000080;
		out_le32(&serdes2_base->bank[i].rstctl, reg);
		udelay(1);

		/* Take the Rx/Tx calibration out of reset */
		if (!(cfg_tmp == 0x3 && i == 1)) {
			udelay(1);
			reg = in_le32(&serdes2_base->srdstcalcr);
			reg |= 0x08000000;
			out_le32(&serdes2_base->srdstcalcr, reg);
			reg = in_le32(&serdes2_base->srdsrcalcr);
			reg |= 0x08000000;
			out_le32(&serdes2_base->srdsrcalcr, reg);
		}

		udelay(1);
	}
#endif

	/* Wait for at lesat 625us to ensure the PLLs being reset are locked */
	udelay(800);

#ifdef CONFIG_SYS_FSL_SRDS_1
	cfg_tmp = cfg_rcwsrds1 & 0x3;
	for (i = 0; i < 2 && !(cfg_tmp & (0x1 << (1 - i))); i++) {
		/* if the PLL is not locked, set RST_ERR */
		reg = in_le32(&serdes1_base->bank[i].pllcr0);
		if (!((reg >> 23) & 0x1)) {
			reg = in_le32(&serdes1_base->bank[i].rstctl);
			reg |= 0x20000000;
			out_le32(&serdes1_base->bank[i].rstctl, reg);
		} else {
			udelay(1);
			reg = in_le32(&serdes1_base->bank[i].rstctl);
			reg &= 0xFFFFFFEF;
			reg |= 0x00000040;
			out_le32(&serdes1_base->bank[i].rstctl, reg);
			udelay(1);
		}
	}
#endif

#ifdef CONFIG_SYS_FSL_SRDS_2
	cfg_tmp = cfg_rcwsrds1 & 0xC;
	cfg_tmp >>= 2;

	for (i = 0; i < 2 && !(cfg_tmp & (0x1 << (1 - i))); i++) {
		reg = in_le32(&serdes2_base->bank[i].pllcr0);
		if (!((reg >> 23) & 0x1)) {
			reg = in_le32(&serdes2_base->bank[i].rstctl);
			reg |= 0x20000000;
			out_le32(&serdes2_base->bank[i].rstctl, reg);
		} else {
			udelay(1);
			reg = in_le32(&serdes2_base->bank[i].rstctl);
			reg &= 0xFFFFFFEF;
			reg |= 0x00000040;
			out_le32(&serdes2_base->bank[i].rstctl, reg);
			udelay(1);
		}
	}
#endif
	/* Take the all enabled lanes out of reset */
#ifdef CONFIG_SYS_FSL_SRDS_1
	cfg_tmp = cfg_rcwsrds1 & FSL_CHASSIS3_SRDS1_PRTCL_MASK;
	cfg_tmp >>= 16;

	for (i = 0; i < 4 && cfg_tmp & (0xf << (3 - i)); i++) {
		reg = in_le32(&serdes1_base->lane[i].gcr0);
		reg |= 0x00600000;
		out_le32(&serdes1_base->lane[i].gcr0, reg);
	}
#endif
#ifdef CONFIG_SYS_FSL_SRDS_2
	cfg_tmp = cfg_rcwsrds2 & FSL_CHASSIS3_SRDS2_PRTCL_MASK;

	for (i = 0; i < 4 && cfg_tmp & (0xf << (3 - i)); i++) {
		reg = in_le32(&serdes2_base->lane[i].gcr0);
		reg |= 0x00600000;
		out_le32(&serdes2_base->lane[i].gcr0, reg);
	}
#endif

	/* For each PLL being reset, and achieved PLL lock set RST_DONE */
#ifdef CONFIG_SYS_FSL_SRDS_1
	cfg_tmp = cfg_rcwsrds1 & 0x3;
	for (i = 0; i < 2; i++) {
		reg = in_le32(&serdes1_base->bank[i].pllcr0);
		if (!(cfg_tmp & (0x1 << (1 - i))) && ((reg >> 23) & 0x1)) {
			reg = in_le32(&serdes1_base->bank[i].rstctl);
			reg |= 0x40000000;
			out_le32(&serdes1_base->bank[i].rstctl, reg);
		}
	}
#endif
#ifdef CONFIG_SYS_FSL_SRDS_2
	cfg_tmp = cfg_rcwsrds1 & 0xC;
	cfg_tmp >>= 2;

	for (i = 0; i < 2; i++) {
		reg = in_le32(&serdes2_base->bank[i].pllcr0);
		if (!(cfg_tmp & (0x1 << (1 - i))) && ((reg >> 23) & 0x1)) {
			reg = in_le32(&serdes2_base->bank[i].rstctl);
			reg |= 0x40000000;
			out_le32(&serdes2_base->bank[i].rstctl, reg);
		}
	}
#endif

	return 1;
}

static int adjust_vdd(ulong vdd_override)
{
	int re_enable = disable_interrupts();
	struct ccsr_gur *gur = (void *)(CONFIG_SYS_FSL_GUTS_ADDR);
	u32 fusesr;
	u8 vid;
	int vdd_target, vdd_last;
	int ret;
	static const uint16_t vdd[32] = {
		0,		/* unused */
		0x0FCC,		/* 0.9875V */
		0x0F99,		/* 0.9750V */
		9625,
		0x0F33,
		9375,
		9250,
		9125,
		0x0E66,		/* 0.9000V */
		8875,
		8750,
		8625,
		8500,
		8375,
		8250,
		8125,
		0x1000,		/* 1.0000V */
		0x1033,		/* 1.0125V */
		0x1066,		/* 1.0250V */
		10375,
		10500,
		10625,
		10750,
		10875,
		11000,
		0,      /* reserved */
	};

	/*select the channel on which LTC3882 voltage regulator is present*/
	ret = i2c_multiplexer_select_vid_channel(I2C_MUX_CH_VOL_MONITOR);
	if (ret) {
		debug("VID: I2C failed to switch channel\n");
		ret = -1;
		goto exit;
	}

	/* get the voltage ID from fuse status register */
	fusesr = in_le32(&gur->dcfg_fusesr);
	debug("FUSESR register read %x\n", fusesr);
	if (fusesr == 0) {
		ret = -1;
		goto exit;
	}
	/*calculate the VID */
	vid = (fusesr >> FSL_CHASSIS3_DCFG_FUSESR_ALTVID_SHIFT) &
		FSL_CHASSIS3_DCFG_FUSESR_ALTVID_MASK;
	if ((vid == 0) || (vid == FSL_CHASSIS3_DCFG_FUSESR_ALTVID_MASK)) {
		vid = (fusesr >> FSL_CHASSIS3_DCFG_FUSESR_VID_SHIFT) &
			FSL_CHASSIS3_DCFG_FUSESR_VID_MASK;
	}


	debug("VID = %d\n", vid);
	vdd_target = vdd[vid];
	debug("vdd_target read %d\n", vdd_target);
/*
 * Read voltage monitor to check real voltage.
 */
	vdd_last = read_voltage();
	if (vdd_last < 0) {
		printf("VID: Couldn't read sensor abort VID adjustment\n");
		ret = -1;
		goto exit;
	}

	vdd_last = set_voltage_to_LTC(vdd_target);
	if (vdd_last > 0) {
		printf("VID: Core voltage after change is %x\n", vdd_last);
	} else {
		ret = -1;
		goto exit;
	}

	if (vdd_last == 0x0E66) {
		ddr_enable_0_9_vdd();
		ret = switch_svdd(0x0E66);
		if (ret < 0) {
			ret = -1;
			goto exit;
		}
	}

exit:
	if (re_enable)
		enable_interrupts();
	i2c_multiplexer_select_vid_channel(I2C_MUX_CH_DEFAULT);
	return ret;
}

int board_init(void)
{
	init_final_memctl_regs();
#if defined(CONFIG_TARGET_LS1088ARDB) && defined(CONFIG_FSL_MC_ENET)
	u32 __iomem *irq_ccsr = (u32 __iomem *)ISC_BASE;
#endif

	select_i2c_ch_pca9547(I2C_MUX_CH_DEFAULT);
	board_retimer_init();

#ifdef CONFIG_ENV_IS_NOWHERE
	gd->env_addr = (ulong)&default_environment[0];
#endif

#if defined(CONFIG_TARGET_LS1088ARDB) && defined(CONFIG_FSL_MC_ENET)
	/* invert AQR105 IRQ pins polarity */
	out_le32(irq_ccsr + IRQCR_OFFSET / 4, AQR105_IRQ_MASK);
#endif
	if (adjust_vdd(0) < 0)
		printf("core voltage not adjusted\n");

#ifdef CONFIG_FSL_CAAM
	sec_init();
#endif
#ifdef CONFIG_FSL_LS_PPA
       ppa_init();
#endif
	return 0;
}

int board_early_init_f(void)
{
	fsl_lsch3_early_init_f();
	return 0;
}

void detail_board_ddr_info(void)
{
	puts("\nDDR    ");
	print_size(gd->bd->bi_dram[0].size + gd->bd->bi_dram[1].size, "");
	print_ddr_info(0);
}

#if defined(CONFIG_ARCH_MISC_INIT)
int arch_misc_init(void)
{
	return 0;
}
#endif

#ifdef CONFIG_FSL_MC_ENET
void fdt_fixup_board_enet(void *fdt)
{
	int offset;

	offset = fdt_path_offset(fdt, "/fsl-mc");

	if (offset < 0)
		offset = fdt_path_offset(fdt, "/fsl,dprc@0");

	if (offset < 0) {
		printf("%s: ERROR: fsl-mc node not found in device tree (error %d)\n",
		       __func__, offset);
		return;
	}

	if (get_mc_boot_status() == 0)
		fdt_status_okay(fdt, offset);
	else
		fdt_status_fail(fdt, offset);
}
#endif

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, bd_t *bd)
{
	int err, i;
	u64 base[CONFIG_NR_DRAM_BANKS];
	u64 size[CONFIG_NR_DRAM_BANKS];

	ft_cpu_setup(blob, bd);

	/* fixup DT for the two GPP DDR banks */
	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		base[i] = gd->bd->bi_dram[i].start;
		size[i] = gd->bd->bi_dram[i].size;
	}

#ifdef CONFIG_RESV_RAM
	/* reduce size if reserved memory is within this bank */
	if (gd->arch.resv_ram >= base[0] &&
	    gd->arch.resv_ram < base[0] + size[0])
		size[0] = gd->arch.resv_ram - base[0];
	else if (gd->arch.resv_ram >= base[1] &&
		 gd->arch.resv_ram < base[1] + size[1])
		size[1] = gd->arch.resv_ram - base[1];
#endif

	fdt_fixup_memory_banks(blob, base, size, CONFIG_NR_DRAM_BANKS);

	fdt_fsl_mc_fixup_iommu_map_entry(blob);

#ifdef CONFIG_FSL_MC_ENET
	fdt_fixup_board_enet(blob);
	err = fsl_mc_ldpaa_exit(bd);
	if (err)
		return err;
#endif

	return 0;
}
#endif
