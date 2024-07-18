// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for panels based on Sitronix ST7703 controller, souch as:
 *
 * - Rocktech jh057n00900 5.5" MIPI-DSI panel
 *
 * Copyright (C) Purism SPC 2019
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>

#include <video/videomode.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DRV_NAME "panel-sitronix-st7703"

/* Manufacturer specific Commands send via DSI */
#define ST7703_CMD_ALL_PIXEL_OFF 0x22
#define ST7703_CMD_ALL_PIXEL_ON	 0x23
#define ST7703_CMD_SETDISP	 0xB2
#define ST7703_CMD_SETRGBIF	 0xB3
#define ST7703_CMD_SETCYC	 0xB4
#define ST7703_CMD_SETBGP	 0xB5
#define ST7703_CMD_SETVCOM	 0xB6
#define ST7703_CMD_SETOTP	 0xB7
#define ST7703_CMD_SETPOWER_EXT	 0xB8
#define ST7703_CMD_SETEXTC	 0xB9
#define ST7703_CMD_SETMIPI	 0xBA
#define ST7703_CMD_SETVDC	 0xBC
#define ST7703_CMD_UNKNOWN_BF	 0xBF
#define ST7703_CMD_SETSCR	 0xC0
#define ST7703_CMD_SETPOWER	 0xC1
#define ST7703_CMD_UNKNOWN_C6	 0xC6
#define ST7703_CMD_SETIO	 0xC7
#define ST7703_CMD_SETCABC	 0xC8
#define ST7703_CMD_SETPANEL	 0xCC
#define ST7703_CMD_SETGAMMA	 0xE0
#define ST7703_CMD_SETEQ	 0xE3
#define ST7703_CMD_SETGIP1	 0xE9
#define ST7703_CMD_SETGIP2	 0xEA
#define ST7703_CMD_UNKNOWN_EF	 0xEF

struct st7703 {
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *vcc;
	struct regulator *iovcc;
	bool prepared;
	struct mipi_dsi_device  *dsi;

	struct dentry *debugfs;
	const struct st7703_panel_desc *desc;
};

struct st7703_panel_desc {
	const struct drm_display_mode *mode;
	unsigned int lanes;
	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	int (*init_sequence)(struct st7703 *ctx);
};

static const u32 bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
};

static inline struct st7703 *panel_to_st7703(struct drm_panel *panel)
{
	return container_of(panel, struct st7703, panel);
}

#define dsi_generic_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_generic_write(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static int jh057n_init_sequence(struct st7703 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;

	/*
	 * Init sequence was supplied by the panel vendor. Most of the commands
	 * resemble the ST7703 but the number of parameters often don't match
	 * so it's likely a clone.
	 */
	dsi_generic_write_seq(dsi, ST7703_CMD_SETEXTC,
			      0xF1, 0x12, 0x83);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETRGBIF,
			      0x10, 0x10, 0x05, 0x05, 0x03, 0xFF, 0x00, 0x00,
			      0x00, 0x00);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETSCR,
			      0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x08, 0x70,
			      0x00);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETVDC, 0x4E);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETPANEL, 0x0B);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETCYC, 0x80);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETDISP, 0xF0, 0x12, 0x30);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETEQ,
			      0x07, 0x07, 0x0B, 0x0B, 0x03, 0x0B, 0x00, 0x00,
			      0x00, 0x00, 0xFF, 0x00, 0xC0, 0x10);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETBGP, 0x08, 0x08);
	msleep(20);

	dsi_generic_write_seq(dsi, ST7703_CMD_SETVCOM, 0x3F, 0x3F);
	dsi_generic_write_seq(dsi, ST7703_CMD_UNKNOWN_BF, 0x02, 0x11, 0x00);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETGIP1,
			      0x82, 0x10, 0x06, 0x05, 0x9E, 0x0A, 0xA5, 0x12,
			      0x31, 0x23, 0x37, 0x83, 0x04, 0xBC, 0x27, 0x38,
			      0x0C, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0C, 0x00,
			      0x03, 0x00, 0x00, 0x00, 0x75, 0x75, 0x31, 0x88,
			      0x88, 0x88, 0x88, 0x88, 0x88, 0x13, 0x88, 0x64,
			      0x64, 0x20, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			      0x02, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETGIP2,
			      0x02, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			      0x00, 0x00, 0x00, 0x00, 0x02, 0x46, 0x02, 0x88,
			      0x88, 0x88, 0x88, 0x88, 0x88, 0x64, 0x88, 0x13,
			      0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
			      0x75, 0x88, 0x23, 0x14, 0x00, 0x00, 0x02, 0x00,
			      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x0A,
			      0xA5, 0x00, 0x00, 0x00, 0x00);
	dsi_generic_write_seq(dsi, ST7703_CMD_SETGAMMA,
			      0x00, 0x09, 0x0E, 0x29, 0x2D, 0x3C, 0x41, 0x37,
			      0x07, 0x0B, 0x0D, 0x10, 0x11, 0x0F, 0x10, 0x11,
			      0x18, 0x00, 0x09, 0x0E, 0x29, 0x2D, 0x3C, 0x41,
			      0x37, 0x07, 0x0B, 0x0D, 0x10, 0x11, 0x0F, 0x10,
			      0x11, 0x18);

	return 0;
}

static const struct drm_display_mode jh057n00900_mode = {
	.hdisplay    = 720,
	.hsync_start = 720 + 90,
	.hsync_end   = 720 + 90 + 20,
	.htotal	     = 720 + 90 + 20 + 20,
	.vdisplay    = 1440,
	.vsync_start = 1440 + 20,
	.vsync_end   = 1440 + 20 + 4,
	.vtotal	     = 1440 + 20 + 4 + 12,
	.clock	     = 75276,
	.flags	     = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm    = 65,
	.height_mm   = 130,
};

struct st7703_panel_desc jh057n00900_panel_desc = {
	.mode = &jh057n00900_mode,
	.lanes = 4,
	.mode_flags = MIPI_DSI_MODE_VIDEO |
		MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.init_sequence = jh057n_init_sequence,
};

#define dsi_dcs_write_seq(dsi, cmd, seq...) do {			\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write(dsi, cmd, d, ARRAY_SIZE(d));	\
		msleep(20);						\
		if (ret < 0)						\
			return ret;					\
	} while (0)

// #define dsi_dcs_write_seq(dsi, seq...) w = 3;do {			\
// 		static const u8 d[] = { seq };				\
// 		int ret;						\
// 		ret = mipi_dsi_generic_write(dsi, d, ARRAY_SIZE(d));	\
// 		msleep(60);						\
// 		if (ret < 0)						\
// 		{								\
// 			printk("kd035: cannot gwrite cmd = %X\n", d[0]);	\
// 			if(w-- == 0)				\
// 			{							\
// 				printk("kd035: cannot gwrite total\n");			\
// 				return ret;				\
// 			}							\
// 			continue;					\
// 		}								\
// 		break;							\
// 	} while (1)

// #define dsi_dcs_write_seq(dsi, seq...) w = 3;do {			\
// 		static const u8 d[] = { seq };				\
// 		int ret;						\
// 		if(w-- == 0)				\
// 		{							\
// 			break;							\
// 		}							\
// 		ret = mipi_dsi_generic_write(dsi, d, ARRAY_SIZE(d));	\
// 		if (ret < 0)						\
// 			printk("ERROR %X %d\n", d[0], ret);	\
// 		else	\
// 			printk("SUCCESS %X\n", d[0]);	\
// 		msleep(60);						\
// 	} while (1)

static void readInfo(struct mipi_dsi_device *dsi)
{
	u8 data[4];
	int ret;
	// int i;
	// unsigned int addr;
	// u8 cmd = 0;
	// u8 regs[] = {0xb9, 0xba, 0xb8, 0xbf, 0xb3, 0xc0, 0xbc, 0xcc,
	// 		0xb4, 0xb2, 0xe3, 0xc1, 0xb5, 0xe9, 0xea, 0xe0};

	data[0] = data[1] = data[2] = data[3] = 0;

	printk("Reading registers!\n");

	ret = mipi_dsi_dcs_read(dsi, 0xDA, data, 1);
	printk("kd035: ID = %x %x %x. ret = %d\n", data[0], data[1], data[2], ret);
	// ret = mipi_dsi_dcs_read(dsi, 0xDB, data+1, 1);
	// printk("kd035: ID = %x %x %x. ret = %d\n", data[0], data[1], data[2], ret);
	// ret = mipi_dsi_dcs_read(dsi, 0xDC, data+2, 1);
	// printk("kd035: ID = %x %x %x. ret = %d\n", data[0], data[1], data[2], ret);

	// cmd = 4;
	// ret = mipi_dsi_generic_read(dsi, &cmd, 1, data, 1);
	// printk("kd035: ID = %x %x %x. ret = %d\n", data[0], data[1], data[2], ret);

	// cmd = 0xB2;
	// ret = mipi_dsi_generic_read(dsi, &cmd, 1, data, 1);
	// printk("kd035: 0xB2 = %x %x %x. ret = %d\n", data[0], data[1], data[2], ret);

	// for(addr = 0x0a; addr <= 0x0f; addr++)
	// {
	// 	ret = mipi_dsi_dcs_read(dsi, addr, data, 1);
	// 	printk("kd035: REG = %x. VAL = %x. ret = %d\n", addr, data[0], ret);
	// }

	// for(i = 0; i < sizeof(regs); i++)
	// {
	// 	ret = mipi_dsi_dcs_read(dsi, regs[i], data, 1);
	// 	printk("kd035: %x = %x, ret = %d\n", regs[i], data[0], ret);
	// }

	printk("End registers\n\n");
}


static int xbd599_init_sequence(struct st7703 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;

	/*
	 * Init sequence was supplied by the panel vendor.
	 */

	// /* Magic sequence to unlock user commands below. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETEXTC, 0xF1, 0x12, 0x83);

	// // Unknown command
	// dsi_dcs_write_seq(dsi, 0xB1, 0x00, 0x00, 0x00, 0xDA, 0x80);

	// /* Set display resolution. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETDISP,
	// 		  0x78, /* NL = 120 */
	// 		  0x13, /* RES_V_LSB = 0, BLK_CON = VSSD,
	// 			 * RESO_SEL = 640RGB
	// 			 */
	// 		  0xF0  /* WHITE_GND_EN = 1 (GND),
	// 			 * WHITE_FRAME_SEL = 7 frames,
	// 			 * ISC = 0 frames
	// 			 */);

	// /* RGB I/F porch timing */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETRGBIF,
	// 		  0x1A, /* VBP_RGB_GEN */
	// 		  0x1E, /* VFP_RGB_GEN */
	// 		  0x28, /* DE_BP_RGB_GEN */
	// 		  0x28, /* DE_FP_RGB_GEN */
	// 		  /* The rest is undocumented in ST7703 datasheet */
	// 		  0x03, 0xFF,
	// 		  0x00, 0x00,
	// 		  0x00, 0x00);

	// /* Zig-Zag Type C column inversion. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETCYC, 0x80);

	// /* Reference voltage. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETBGP,
	// 		  0x10, /* VREF_SEL = 5.1V */
	// 		  0x10  /* NVREF_SEL = 5.1V */);
	// msleep(20);  // unneeded?

	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETVCOM,
	// 		  0x48, /* VCOMDC_F = -0.95V */
	// 		  0x48  /* VCOMDC_B = -0.95V */);

	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETPOWER_EXT,
	// 		  0x2E, /* PCCS = 2, ECP_DC_DIV = 1/72 HSYNC */
	// 		  0x22, /* DT = 15ms XDK_ECP = x2 */
	// 		  0xF0, /* PFM_DC_DIV = /1 */
	// 		  0x13  /* ECP_SYNC_EN = 1, VGX_SYNC_EN = 1 */);

	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETMIPI,
	// 		  0x33, /* VC_main = 0, Lane_Number = 3 (4 lanes) */
	// 		  0x81, /* DSI_LDO_SEL = 1.7V, RTERM = 90 Ohm */
	// 		  0x05, /* IHSRX = x6 (Low High Speed drive ability) */
	// 		  0xF9, /* TX_CLK_SEL = fDSICLK/16 */
	// 		  0x0E, /* HFP_OSC (min. HFP number in DSI mode) */
	// 		  0x0E, /* HBP_OSC (min. HBP number in DSI mode) */
	// 		  /* The rest is undocumented in ST7703 datasheet */
	// 		  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	// 		  0x44, 0x25, 0x00, 0x90, 0x0A, 0x00, 0x00, 0x01,
	// 		  0x4F, 0x01, 0x00, 0x00, 0x37);

	// /* NVDDD_SEL = -1.8V, VDDD_SEL = out of range (possibly 2.0V?) */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETVDC, 0x4F);

	// /* Undocumented command. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_UNKNOWN_BF, 0x02, 0x11, 0x00);

	// /* Source driving settings. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETSCR,
	// 		  0x73, /* N_POPON */
	// 		  0x73, /* N_NOPON */
	// 		  0x50, /* I_POPON */
	// 		  0x50, /* I_NOPON */
	// 		  0x00, /* SCR[31,24] */
	// 		  0x00, /* SCR[23,16] */
	// 		  0x12, /* SCR[15,8] */
	// 		  0x70, /* SCR[7,0] */
	// 		  0x00  /* Undocumented */);

	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETPOWER,
	// 		  0x64, /* VBTHS, VBTLS: VGH = 17V, VBL = -11V */
	// 		  0xC1, /* FBOFF_VGH = 0, FBOFF_VGL = 0 */
	// 		  0x2C, /* VRP  */
	// 		  0x2C, /* VRN */
	// 		  0x77, /* reserved */
	// 		  0xE4, /* APS = 1 (small),
	// 			 * VGL_DET_EN = 1, VGH_DET_EN = 1,
	// 			 * VGL_TURBO = 1, VGH_TURBO = 1
	// 			 */
	// 		  0xCF, /* VGH1_L_DIV, VGL1_L_DIV (1.5MHz) */
	// 		  0xCF, /* VGH1_R_DIV, VGL1_R_DIV (1.5MHz) */
	// 		  0x7E, /* VGH2_L_DIV, VGL2_L_DIV (2.6MHz) */
	// 		  0x7E, /* VGH2_R_DIV, VGL2_R_DIV (2.6MHz) */
	// 		  0x3E, /* VGH3_L_DIV, VGL3_L_DIV (4.5MHz) */
	// 		  0x3E  /* VGH3_R_DIV, VGL3_R_DIV (4.5MHz) */);

	// /*
	//  * SS_PANEL = 1 (reverse scan), GS_PANEL = 0 (normal scan)
	//  * REV_PANEL = 1 (normally black panel), BGR_PANEL = 1 (BGR)
	//  */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETPANEL, 0x0B);

	// /* Zig-Zag Type C column inversion. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETCYC, 0x80);

	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETEQ,
	// 		  0x00, /* PNOEQ */
	// 		  0x00, /* NNOEQ */
	// 		  0x0B, /* PEQGND */
	// 		  0x0B, /* NEQGND */
	// 		  0x10, /* PEQVCI */
	// 		  0x10, /* NEQVCI */
	// 		  0x00, /* PEQVCI1 */
	// 		  0x00, /* NEQVCI1 */
	// 		  0x00, /* reserved */
	// 		  0x00, /* reserved */
	// 		  0xFF, /* reserved */
	// 		  0x00, /* reserved */
	// 		  0xC0, /* ESD_DET_DATA_WHITE = 1, ESD_WHITE_EN = 1 */
	// 		  0x10  /* SLPIN_OPTION=1 (no need vsync after sleep-in)
	// 			 * VEDIO_NO_CHECK_EN = 0
	// 			 * ESD_WHITE_GND_EN = 0
	// 			 * ESD_DET_TIME_SEL = 0 frames
	// 			 */);

	// /* Undocumented command. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_UNKNOWN_C6,
	// 		  0x01, 0x00, 0xFF, 0xFF, 0x00);

	// /* This command is to set forward GIP timing. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETGIP1,
	// 		  0x82, 0x10, 0x06, 0x05, 0xA2, 0x0A, 0xA5, 0x12,
	// 		  0x31, 0x23, 0x37, 0x83, 0x04, 0xBC, 0x27, 0x38,
	// 		  0x0C, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0C, 0x00,
	// 		  0x03, 0x00, 0x00, 0x00, 0x75, 0x75, 0x31, 0x88,
	// 		  0x88, 0x88, 0x88, 0x88, 0x88, 0x13, 0x88, 0x64,
	// 		  0x64, 0x20, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
	// 		  0x02, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	// 		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

	// /* This command is to set backward GIP timing. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETGIP2,
	// 		  0x02, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	// 		  0x00, 0x00, 0x00, 0x00, 0x02, 0x46, 0x02, 0x88,
	// 		  0x88, 0x88, 0x88, 0x88, 0x88, 0x64, 0x88, 0x13,
	// 		  0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
	// 		  0x75, 0x88, 0x23, 0x14, 0x00, 0x00, 0x02, 0x00,
	// 		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	// 		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0A,
	// 		  0xA5, 0x00, 0x00, 0x00, 0x00);

	// /* Adjust the gamma characteristics of the panel. */
	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETGAMMA,
	// 		  0x00, 0x09, 0x0D, 0x23, 0x27, 0x3C, 0x41, 0x35,
	// 		  0x07, 0x0D, 0x0E, 0x12, 0x13, 0x10, 0x12, 0x12,
	// 		  0x18, 0x00, 0x09, 0x0D, 0x23, 0x27, 0x3C, 0x41,
	// 		  0x35, 0x07, 0x0D, 0x0E, 0x12, 0x13, 0x10, 0x12,
	// 		  0x12, 0x18);

	return 0;
}

static const struct drm_display_mode xbd599_mode = {
	.hdisplay    = 720,
	.hsync_start = 720 + 40,
	.hsync_end   = 720 + 40 + 40,
	.htotal	     = 720 + 40 + 40 + 40,
	.vdisplay    = 1440,
	.vsync_start = 1440 + 18,
	.vsync_end   = 1440 + 18 + 10,
	.vtotal	     = 1440 + 18 + 10 + 17,
	.clock	     = 69000,
	.flags	     = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm    = 68,
	.height_mm   = 136,
};

static const struct st7703_panel_desc xbd599_desc = {
	.mode = &xbd599_mode,
	.lanes = 4,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.init_sequence = xbd599_init_sequence,
};

static int p0500063b_init_sequence(struct st7703 *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	u8 b, val;
	int ret, w;
	// readInfo(dsi);

	/*
	 * Init sequence was supplied by the panel vendor.
	 */

	// msleep(60);
	dev_err(&ctx->dsi->dev, "INIT 1\n");
	/* Magic sequence to unlock user commands below. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETEXTC, 0xF1, 0x12, 0x83);
	dev_err(&ctx->dsi->dev, "INIT 2\n");

	// msleep(60);
	// Unknown command
	dsi_dcs_write_seq(dsi, 0xB1, 0x00, 0x00, 0x00, 0xDA, 0x80);
	dev_err(&ctx->dsi->dev, "INIT 3\n");

	// msleep(60);
	/* Set display resolution. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETDISP,
			  0x78, /* NL = 120 */
			  0x13, /* RES_V_LSB = 0, BLK_CON = VSSD,
				 * RESO_SEL = 640RGB
				 */
			  0xF0  /* WHITE_GND_EN = 1 (GND),
				 * WHITE_FRAME_SEL = 7 frames,
				 * ISC = 0 frames
				 */);

	// msleep(60);
	/* RGB I/F porch timing */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETRGBIF,
			  0x1A, /* VBP_RGB_GEN */
			  0x1E, /* VFP_RGB_GEN */
			  0x28, /* DE_BP_RGB_GEN */
			  0x28, /* DE_FP_RGB_GEN */
			  /* The rest is undocumented in ST7703 datasheet */
			  0x03, 0xFF,
			  0x00, 0x00,
			  0x00, 0x00);
	dev_err(&ctx->dsi->dev, "INIT 4\n");

	// msleep(60);
	/* Zig-Zag Type C column inversion. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETCYC, 0x80);

	dev_err(&ctx->dsi->dev, "INIT 5\n");
	/* Reference voltage. */
	// msleep(60);
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETBGP,
			  0x10, /* VREF_SEL = 5.1V */
			  0x10  /* NVREF_SEL = 5.1V */);
	// msleep(20);  // unneeded?
	dev_err(&ctx->dsi->dev, "INIT 6\n");

	// msleep(60);
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETVCOM,
			  0x48, /* VCOMDC_F = -0.95V */
			  0x48  /* VCOMDC_B = -0.95V */);
	dev_err(&ctx->dsi->dev, "INIT 7\n");

	// msleep(60);
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETPOWER_EXT,
			  0x2E, /* PCCS = 2, ECP_DC_DIV = 1/72 HSYNC */
			  0x22, /* DT = 15ms XDK_ECP = x2 */
			  0xF0, /* PFM_DC_DIV = /1 */
			  0x13  /* ECP_SYNC_EN = 1, VGX_SYNC_EN = 1 */);
	dev_err(&ctx->dsi->dev, "INIT 7.1\n");

	// msleep(60);
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETMIPI,
			  0x33, /* VC_main = 0, Lane_Number = 3 (4 lanes) */
			  0x81, /* DSI_LDO_SEL = 1.7V, RTERM = 90 Ohm */
			  0x05, /* IHSRX = x6 (Low High Speed drive ability) */
			  0xF9, /* TX_CLK_SEL = fDSICLK/16 */
			  0x0E, /* HFP_OSC (min. HFP number in DSI mode) */
			  0x0E, /* HBP_OSC (min. HBP number in DSI mode) */
			  /* The rest is undocumented in ST7703 datasheet */
			  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			  0x44, 0x25, 0x00, 0x90, 0x0A, 0x00, 0x00, 0x01,
			  0x4F, 0x01, 0x00, 0x00, 0x37);


	// dsi_dcs_write_seq(dsi, ST7703_CMD_SETMIPI,
	// 		  0x33, /* VC_main = 0, Lane_Number = 3 (4 lanes) */
	// 		  0x81, /* DSI_LDO_SEL = 1.7V, RTERM = 90 Ohm */
	// 		  0x05, /* IHSRX = x6 (Low High Speed drive ability) */
	// 		  0xF9, /* TX_CLK_SEL = fDSICLK/16 */
	// 		  0x0E, /* HFP_OSC (min. HFP number in DSI mode) */
	// 		  0x0E /* HBP_OSC (min. HBP number in DSI mode) */);
	// msleep(60);

	dev_err(&ctx->dsi->dev, "INIT 8\n");
	/* NVDDD_SEL = -1.8V, VDDD_SEL = out of range (possibly 2.0V?) */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETVDC, 0x4F);
	dev_err(&ctx->dsi->dev, "INIT 9\n");

	// msleep(60);
	/* Undocumented command. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_UNKNOWN_BF, 0x02, 0x11, 0x00);

	// msleep(60);
	dev_err(&ctx->dsi->dev, "INIT 10\n");
	/* Source driving settings. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETSCR,
			  0x73, /* N_POPON */
			  0x73, /* N_NOPON */
			  0x50, /* I_POPON */
			  0x50, /* I_NOPON */
			  0x00, /* SCR[31,24] */
			  0x00, /* SCR[23,16] */
			  0x12, /* SCR[15,8] */
			  0x70, /* SCR[7,0] */
			  0x00  /* Undocumented */);

	// msleep(60);
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETPOWER,
			  0x64, /* VBTHS, VBTLS: VGH = 16V, VBL = -11V */
			  0xC1, /* FBOFF_VGH = 1, FBOFF_VGL = 1 */
			  0x2C, /* VRP  */
			  0x2C, /* VRN */
			  0x77, /* reserved */
			  0xE4, /* APS = 4 (large),
				 * VGL_DET_EN = 1, VGH_DET_EN = 1,
				 * VGL_TURBO = 1, VGH_TURBO = 0
				 */
			  0xCF, /* VGH1_L_DIV (2.6MHz), VGL1_L_DIV (1.5MHz) */
			  0xCF, /* VGH1_R_DIV (2.6MHz), VGL1_R_DIV (1.5MHz) */
			  0x7E, /* VGH2_L_DIV (4.5MHz), VGL2_L_DIV (1.8MHz) */
			  0x7E, /* VGH2_R_DIV (4.5MHz), VGL2_R_DIV (1.8MHz) */
			  0x3E, /* VGH3_L_DIV (9.0MHz), VGL3_L_DIV (1.8MHz) */
			  0x3E  /* VGH3_R_DIV (9.0MHz), VGL3_R_DIV (1.8MHz) */);

	dev_err(&ctx->dsi->dev, "INIT 11\n");
	// msleep(60);
	/* Undocumented command. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_UNKNOWN_C6,
			  0x82, 0x00, 0xBF, 0xFF, 0x00, 0xFF);
	// msleep(60);

	// Set IO
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETIO,
			  0xB8, /* Enable CABC PWM signal, enable inverse polarity CABC, VOUT pin frame sync=1, HOUT pin frame sync=1 */
			  0x00, /* VSync delay time=0, HSync delay time=0 */
			  /* The rest is undocumented in ST7703 datasheet */
			  0x0A, 0x00, 0x00, 0x00);
	// msleep(60);

	dev_err(&ctx->dsi->dev, "INIT 12\n");
	// Content adaptive brightness control
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETCABC,
			  0x10, /* pwm div=FOSC/2 */
			  0x40, /* PWM period=FPWM/? */
			  /* The rest is undocumented in ST7703 datasheet */
			  0x1E, 0x02);
	// msleep(60);

	/*
	 * SS_PANEL = 1 (reverse scan), GS_PANEL = 0 (normal scan)
	 * REV_PANEL = 1 (normally black panel), BGR_PANEL = 1 (BGR)
	 */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETPANEL, 0x0B);

	dev_err(&ctx->dsi->dev, "INIT 13\n");
	// msleep(60);
	/* Adjust the gamma characteristics of the panel. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETGAMMA,
			0x00, 0x0B, 0x10, 0x24, 0x29, 0x38,
			0x44, 0x39, 0x0A, 0x0D, 0x0D, 0x12, 0x14, 0x13,
			0x15, 0x10, 0x15, 0x00, 0x0B, 0x10, 0x24, 0x29,
			0x38, 0x44, 0x39, 0x0A, 0x0D, 0x0D, 0x12, 0x14,
			0x13, 0x15, 0x10, 0x15);

	// msleep(60);
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETEQ,
			0x07, /* PNOEQ */
			0x07, /* NNOEQ */
			0x0B, /* PEQGND */
			0x0B, /* NEQGND */
			0x0B, /* PEQVCI */
			0x0B, /* NEQVCI */
			0x00, /* PEQVCI1 */
			0x00, /* NEQVCI1 */
			0x00, /* reserved */
			0x00, /* reserved */
			0xFF, /* reserved */
			0x00, /* reserved */
			0xC0, /* ESD_DET_DATA_WHITE = 1, ESD_WHITE_EN = 1 */
			0x10  /* SLPIN_OPTION = 1 (no need vsync after sleep-in)
			       * VEDIO_NO_CHECK_EN = 0
			       * ESD_WHITE_GND_EN = 0
			       * ESD_DET_TIME_SEL = 0 frames
			       */);

	// msleep(60);
	dev_err(&ctx->dsi->dev, "INIT 14\n");
	/* This command is to set forward GIP timing. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETGIP1,
			0xC8, 0x10, 0x11, 0x03, 0xC3, 0x80,
			0x81, 0x12, 0x31, 0x23, 0xAF, 0x8E, 0xAD, 0x6D,
			0x8F, 0x10, 0x03, 0x00, 0x19, 0x00, 0x00, 0x00,
			0x03, 0x00, 0x19, 0x00, 0x00, 0x00, 0x9F, 0x84,
			0x6A, 0xB6, 0x48, 0x20, 0x64, 0x20, 0x20, 0x88,
			0x88, 0x9F, 0x85, 0x7A, 0xB7, 0x58, 0x31, 0x75,
			0x31, 0x31, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x80, 0x81, 0x5F, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00);
	// msleep(60);

	/* This command is to set backward GIP timing. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_SETGIP2,
			0x96, 0x1C, 0x01, 0x01, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0xF3,
			0x1A, 0xB1, 0x38, 0x57, 0x13, 0x57, 0x57, 0x88,
			0x88, 0x98, 0xF2, 0x0A, 0xB0, 0x28, 0x46, 0x02,
			0x46, 0x46, 0x88, 0x88, 0x23, 0x10, 0x00, 0x00,
			0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D,
			0x80, 0x00, 0xF0, 0x00, 0x03, 0xCF, 0x12, 0x30,
			0x70, 0x80, 0x81, 0x40, 0x80, 0x81, 0x00, 0x00,
			0x00, 0x00);

	// msleep(60);
	dev_err(&ctx->dsi->dev, "INIT 15\n");
	/* Undocumented command. */
	dsi_dcs_write_seq(dsi, ST7703_CMD_UNKNOWN_EF, 0xFF, 0xFF, 0x01);
	// msleep(60);

	// dev_err(&ctx->dsi->dev, "PX ON\n");
	// dsi_dcs_write_seq(ctx->dsi, 0x23); // all pixels on
	// msleep(60);

	// ret = mipi_dsi_dcs_read(ctx->dsi, 0x0A, &val, 1);
	// if (ret < 0) {
	// 	dev_err(&ctx->dsi->dev, "Read register 0x0A failed: %d\n", ret);
	// 	// return -ENODEV;
	// } else {
	// 	dev_info(&ctx->dsi->dev, "Read register 0x0A: 0x%02x\n", val);
	// }

	// dsi_dcs_write_seq(ctx->dsi, 0x23, 0xFF); // all pixels on
	dev_err(&ctx->dsi->dev, "INIT 16\n");

	// readInfo(dsi);

	return 0;
}

static const struct drm_display_mode p0500063b_mode = {
	// .clock	     = 61776,

	// .flags	     = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,

	// .hdisplay    = 720,
	// .hsync_start = 720 + 10,
	// .hsync_end   = 720 + 10 + 20,
	// .htotal	     = 720 + 10 + 20 + 30,

	// .vdisplay    = 1280,
	// .vsync_start = 1280 + 10,
	// .vsync_end   = 1280 + 10 + 10,
	// .vtotal	     = 1280 + 10 + 10 + 20,

	// .width_mm    = 62,
	// .height_mm   = 110,

	.type        = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,


	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.clock = 48308,
	// pixel clock = (hactive + hfront_porch + hsync_len + hback_porch) x (vactive + vfront_porch + vsync_len + vback_porch) x frame rate
	.hdisplay = 640,
	.hsync_start = 640 + 84,
	.hsync_end = 640 + 84 + 2,
	.htotal = 640 + 84 + 2 + 84,
	.vdisplay = 960,
	.vsync_start = 960 + 16,
	.vsync_end = 960 + 16 + 2,
	.vtotal = 960 + 16 + 2 + 16,
	.width_mm = 75,
	.height_mm = 50
};

static const struct st7703_panel_desc p0500063b_desc = {
	.mode = &p0500063b_mode,
	.lanes = 4,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.init_sequence = p0500063b_init_sequence,
};

static int st7703_enable(struct drm_panel *panel)
{
	struct st7703 *ctx = panel_to_st7703(panel);
	dev_err(&ctx->dsi->dev, "ENABLE BEGIN\n");
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret, w;

	dsi = ctx->dsi;
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = ctx->desc->init_sequence(ctx);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev,
			"Panel init sequence failed: %d\n", ret);
		return ret;
	}

	msleep(20);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}

	/* Panel is operational 120 msec after reset */
	msleep(120);

	readInfo(dsi);

	// mipi_dsi_dcs_set_display_on(dsi);
	// msleep(20);

	// mipi_dsi_dcs_set_display_on(dsi);
	// msleep(20);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	// if (ret)
	// 	return ret;
	readInfo(dsi);

	// msleep(500);
	// dev_err(&ctx->dsi->dev, "PXL ON\n");
	// dsi_dcs_write_seq(ctx->dsi, 0x23); // all pixels on
	// msleep(500);
	// msleep(120);
	dev_dbg(&ctx->dsi->dev, "Panel init sequence done\n");
	dev_err(&ctx->dsi->dev, "ENABLE END\n");

	return 0;
}

static int st7703_disable(struct drm_panel *panel)
{
	struct st7703 *ctx = panel_to_st7703(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	dsi = ctx->dsi;
	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "Failed to turn off the display: %d\n",
			ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "Failed to enter sleep mode: %d\n",
			ret);

	return 0;
}

static int st7703_unprepare(struct drm_panel *panel)
{
	struct st7703 *ctx = panel_to_st7703(panel);

	if (!ctx->prepared)
		return 0;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	if (!IS_ERR_OR_NULL(ctx->iovcc))
		regulator_disable(ctx->iovcc);
	if (!ctx->prepared && IS_ERR_OR_NULL(ctx->vcc))
		regulator_disable(ctx->vcc);
	ctx->prepared = 0;

	msleep(40);
	return 0;
}

static int st7703_prepare(struct drm_panel *panel)
{
	struct st7703 *ctx = panel_to_st7703(panel);
	int ret = 0;

	if (ctx->prepared)
		return 0;

	dev_dbg(&ctx->dsi->dev, "Resetting the panel\n");
	if (!ctx->prepared && !IS_ERR_OR_NULL(ctx->vcc)) {
		ret = regulator_enable(ctx->vcc);
		if (ret < 0) {
			dev_err(&ctx->dsi->dev,
				"Failed to enable vcc supply: %d\n", ret);
			return ret;
		}
	}
	ctx->prepared = 1;

	if (!IS_ERR_OR_NULL(ctx->iovcc)) {
		ret = regulator_enable(ctx->iovcc);
		if (ret < 0) {
			dev_err(&ctx->dsi->dev,
				"Failed to enable iovcc supply: %d\n", ret);
			goto disable_vcc;
		}
	}

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);

	return 0;

disable_vcc:
	regulator_disable(ctx->vcc);
	ctx->prepared = 0;

	return ret;
}

static int st7703_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct st7703 *ctx = panel_to_st7703(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "Failed to add mode %ux%u@%u\n",
			ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = mode->type;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_display_info_set_bus_formats(&connector->display_info,
			bus_formats, ARRAY_SIZE(bus_formats));
	return 1;
}

static const struct drm_panel_funcs st7703_drm_funcs = {
	.disable   = st7703_disable,
	.unprepare = st7703_unprepare,
	.prepare   = st7703_prepare,
	.enable	   = st7703_enable,
	.get_modes = st7703_get_modes,
};

static int allpixelson_set(void *data, u64 val)
{
	struct st7703 *ctx = data;
	struct mipi_dsi_device *dsi = ctx->dsi;

	dev_dbg(&ctx->dsi->dev, "Setting all pixels on\n");
	dsi_generic_write_seq(dsi, ST7703_CMD_ALL_PIXEL_ON);
	msleep(val * 1000);
	/* Reset the panel to get video back */
	drm_panel_disable(&ctx->panel);
	drm_panel_unprepare(&ctx->panel);
	drm_panel_prepare(&ctx->panel);
	drm_panel_enable(&ctx->panel);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(allpixelson_fops, NULL,
			allpixelson_set, "%llu\n");

static void st7703_debugfs_init(struct st7703 *ctx)
{
	ctx->debugfs = debugfs_create_dir(DRV_NAME, NULL);

	debugfs_create_file("allpixelson", 0600, ctx->debugfs, ctx,
			    &allpixelson_fops);
}

static void st7703_debugfs_remove(struct st7703 *ctx)
{
	debugfs_remove_recursive(ctx->debugfs);
	ctx->debugfs = NULL;
}

static int st7703_probe(struct mipi_dsi_device *dsi)
{
	dev_err(&dsi->dev, "PROBE BEGIN\n");
	struct st7703 *ctx;
	int ret;
	u8 model[4];

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(&dsi->dev, "cannot get reset gpio\n");
		return PTR_ERR(ctx->reset_gpio);
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dsi = dsi;
	ctx->desc = of_device_get_match_data(&dsi->dev);


	ctx->vcc = devm_regulator_get_optional(&dsi->dev, "vcc");
	if (IS_ERR(ctx->vcc)) {
		ret = PTR_ERR(ctx->vcc);
		if (ret != -EPROBE_DEFER)
			dev_err(&dsi->dev,
				"Failed to request vcc regulator: %d\n", ret);
	}
	ctx->iovcc = devm_regulator_get_optional(&dsi->dev, "iovcc");
	if (IS_ERR(ctx->iovcc)) {
		ret = PTR_ERR(ctx->iovcc);
		if (ret != -EPROBE_DEFER)
			dev_err(&dsi->dev,
				"Failed to request iovcc regulator: %d\n", ret);
	}

	ctx->panel.prepare_upstream_first = true;
	drm_panel_init(&ctx->panel, &dsi->dev, &st7703_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	drm_panel_add(&ctx->panel);
	dsi->mode_flags = ctx->desc->mode_flags;
	dsi->format = ctx->desc->format;
	dsi->lanes = ctx->desc->lanes;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev,
			"mipi_dsi_attach failed (%d). Is host ready?\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	dev_info(&dsi->dev, "%ux%u@%u %ubpp dsi %udl - ready\n",
		 ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
		 drm_mode_vrefresh(ctx->desc->mode),
		 mipi_dsi_pixel_format_to_bpp(dsi->format), dsi->lanes);

	st7703_debugfs_init(ctx);
	dev_err(&dsi->dev, "PROBE END\n");
	return 0;
}

static void st7703_shutdown(struct mipi_dsi_device *dsi)
{
	struct st7703 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = drm_panel_unprepare(&ctx->panel);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to unprepare panel: %d\n", ret);

	ret = drm_panel_disable(&ctx->panel);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to disable panel: %d\n", ret);
}

static void st7703_remove(struct mipi_dsi_device *dsi)
{
	struct st7703 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	st7703_shutdown(dsi);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);

	st7703_debugfs_remove(ctx);
}

static const struct of_device_id st7703_of_match[] = {
	{ .compatible = "rocktech,jh057n00900", .data = &jh057n00900_panel_desc },
	{ .compatible = "xingbangda,xbd599", .data = &xbd599_desc },
	{ .compatible = "dlc,dlc350v11", .data = &p0500063b_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, st7703_of_match);

static struct mipi_dsi_driver st7703_driver = {
	.probe	= st7703_probe,
	.remove = st7703_remove,
	.shutdown = st7703_shutdown,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = st7703_of_match,
	},
};
module_mipi_dsi_driver(st7703_driver);

MODULE_AUTHOR("Guido Günther <agx@sigxcpu.org>");
MODULE_DESCRIPTION("DRM driver for Sitronix ST7703 based MIPI DSI panels");
MODULE_LICENSE("GPL v2");
