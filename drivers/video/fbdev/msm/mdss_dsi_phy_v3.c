/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/iopoll.h>
#include "mdss_dsi_phy.h"

#define CMN_CLK_CFG0                         0x010
#define CMN_CLK_CFG1                         0x014
#define CMN_GLBL_CTRL                        0x018
#define CMN_RBUF_CTRL                        0x01C
#define CMN_VREG_CTRL                        0x020
#define CMN_CTRL_0                           0x024
#define CMN_CTRL_1                           0x028
#define CMN_CTRL_2                           0x02C
#define CMN_LANE_CFG0                        0x030
#define CMN_LANE_CFG1                        0x034
#define CMN_PLL_CNTRL                        0x038
#define CMN_DSI_LANE_CTRL0                   0x098
#define CMN_DSI_LANE_CTRL1                   0x09C
#define CMN_DSI_LANE_CTRL2                   0x0A0
#define CMN_DSI_LANE_CTRL3                   0x0A4
#define CMN_DSI_LANE_CTRL4                   0x0A8
#define CMN_TIMING_CTRL_0                    0x0AC
#define CMN_TIMING_CTRL_1                    0x0B0
#define CMN_TIMING_CTRL_2                    0x0B4
#define CMN_TIMING_CTRL_3                    0x0B8
#define CMN_TIMING_CTRL_4                    0x0BC
#define CMN_TIMING_CTRL_5                    0x0C0
#define CMN_TIMING_CTRL_6                    0x0C4
#define CMN_TIMING_CTRL_7                    0x0C8
#define CMN_TIMING_CTRL_8                    0x0CC
#define CMN_TIMING_CTRL_9                    0x0D0
#define CMN_TIMING_CTRL_10                   0x0D4
#define CMN_TIMING_CTRL_11                   0x0D8
#define CMN_PHY_STATUS                       0x0EC

#define LNX_CFG0(n)                         ((0x200 + (0x80 * (n))) + 0x00)
#define LNX_CFG1(n)                         ((0x200 + (0x80 * (n))) + 0x04)
#define LNX_CFG2(n)                         ((0x200 + (0x80 * (n))) + 0x08)
#define LNX_CFG3(n)                         ((0x200 + (0x80 * (n))) + 0x0C)
#define LNX_TEST_DATAPATH(n)                ((0x200 + (0x80 * (n))) + 0x10)
#define LNX_PIN_SWAP(n)                     ((0x200 + (0x80 * (n))) + 0x14)
#define LNX_HSTX_STR_CTRL(n)                ((0x200 + (0x80 * (n))) + 0x18)
#define LNX_OFFSET_TOP_CTRL(n)              ((0x200 + (0x80 * (n))) + 0x1C)
#define LNX_OFFSET_BOT_CTRL(n)              ((0x200 + (0x80 * (n))) + 0x20)
#define LNX_LPTX_STR_CTRL(n)                ((0x200 + (0x80 * (n))) + 0x24)
#define LNX_LPRX_CTRL(n)                    ((0x200 + (0x80 * (n))) + 0x28)
#define LNX_TX_DCTRL(n)                     ((0x200 + (0x80 * (n))) + 0x2C)

#define DSI_PHY_W32(b, off, val) MIPI_OUTP((b) + (off), (val))
#define DSI_PHY_R32(b, off) MIPI_INP((b) + (off))

static bool mdss_dsi_phy_v3_is_pll_on(struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 data;

	/* In split-dsi case, the PLL on the slave control is never used */
	if (mdss_dsi_is_ctrl_clk_slave(ctrl))
		return false;

	data = DSI_PHY_R32(ctrl->phy_io.base, CMN_CTRL_0);

	/* Make sure the register has been read prior to checking the status */
	mb();

	return (data & BIT(5));
}

static void mdss_dsi_phy_v3_set_pll_source(
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 pll_src, reg;

	if (mdss_dsi_is_ctrl_clk_slave(ctrl))
		pll_src = 0x01; /* external PLL */
	else
		pll_src = 0x00; /* internal PLL */

	/* set the PLL src and set global clock enable */
	reg = (pll_src << 2) | BIT(5);
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CLK_CFG1, reg);
}

static void mdss_dsi_phy_v3_lanes_disable(struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 data = DSI_PHY_R32(ctrl->phy_io.base, CMN_CTRL_0);

	/* disable all lanes irrespective of whether they are used or not */
	data &= ~0x1F;
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CTRL_0, data);
	DSI_PHY_W32(ctrl->phy_io.base, CMN_DSI_LANE_CTRL0, 0);
}

static void mdss_dsi_phy_v3_lanes_enable(struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 data = DSI_PHY_R32(ctrl->phy_io.base, CMN_CTRL_0);

	/* todo: only power up the lanes that are used */
	data |= 0x1F;
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CTRL_0, data);
	DSI_PHY_W32(ctrl->phy_io.base, CMN_DSI_LANE_CTRL0, 0x1F);
}

static void mdss_dsi_phy_v3_config_timings(struct mdss_dsi_ctrl_pdata *ctrl)
{
	u32 i;
	struct mdss_dsi_phy_ctrl *pd =
		&(((ctrl->panel_data).panel_info.mipi).dsi_phy_db);

	for (i = 0; i < 12; i++) {
		DSI_PHY_W32(ctrl->phy_io.base, CMN_TIMING_CTRL_0 + (i * 0x04),
			pd->timing[i]);
	}
}

static void mdss_dsi_phy_v3_config_lane_settings(
	struct mdss_dsi_ctrl_pdata *ctrl)
{
	int i;
	u32 tx_dctrl[] = {0x98, 0x99, 0x98, 0x9a, 0x98};
	struct mdss_dsi_phy_ctrl *pd =
		&(((ctrl->panel_data).panel_info.mipi).dsi_phy_db);

	/* Strength settings */
	for (i = 0; i < 5; i++) {
		DSI_PHY_W32(ctrl->phy_io.base, LNX_LPTX_STR_CTRL(i),
			    pd->strength[(i * 2)]);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_LPRX_CTRL(i),
			    pd->strength[(i * 2) + 1]);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_HSTX_STR_CTRL(i), 0x88);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_PIN_SWAP(i), 0x0);
	}

	/* Other settings */
	for (i = 0; i < 5; i++) {
		DSI_PHY_W32(ctrl->phy_io.base, LNX_CFG0(i), pd->lanecfg[i * 4]);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_CFG1(i),
			    pd->lanecfg[(i * 4) + 1]);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_CFG2(i),
			    pd->lanecfg[(i * 4) + 2]);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_CFG3(i),
			    pd->lanecfg[(i * 4) + 3]);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_OFFSET_TOP_CTRL(i), 0x0);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_OFFSET_BOT_CTRL(i), 0x0);
		DSI_PHY_W32(ctrl->phy_io.base, LNX_TX_DCTRL(i), tx_dctrl[i]);
	}
}

int mdss_dsi_phy_v3_regulator_enable(struct mdss_dsi_ctrl_pdata *ctrl)
{
	/* Nothing to be done for cobalt */
	return 0;
}

int mdss_dsi_phy_v3_regulator_disable(struct mdss_dsi_ctrl_pdata *ctrl)
{
	/* Nothing to be done for cobalt */
	return 0;
}

void mdss_dsi_phy_v3_toggle_resync_fifo(struct mdss_dsi_ctrl_pdata *ctrl)
{
	DSI_PHY_W32(ctrl->phy_io.base, CMN_RBUF_CTRL, 0x00);
	DSI_PHY_W32(ctrl->phy_io.base, CMN_RBUF_CTRL, 0x01);

	/* make sure resync fifo is reset */
	wmb();
}

int mdss_dsi_phy_v3_shutdown(struct mdss_dsi_ctrl_pdata *ctrl)
{
	/* ensure that the PLL is already off */
	if (mdss_dsi_phy_v3_is_pll_on(ctrl))
		pr_warn("Disabling phy with PLL still enabled\n");

	mdss_dsi_phy_v3_lanes_disable(ctrl);

	/* Turn off all PHY blocks */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CTRL_0, 0x00);

	/* make sure phy is turned off */
	wmb();

	return 0;
}

int mdss_dsi_phy_v3_init(struct mdss_dsi_ctrl_pdata *ctrl,
			       enum phy_mode phy_mode)
{
	int rc = 0;
	u32 status;
	u32 const delay_us = 5;
	u32 const timeout_us = 1000;

	if (phy_mode != DSI_PHY_MODE_DPHY) {
		pr_err("PHY mode(%d) is not supported\n", phy_mode);
		return -ENOTSUPP;
	}

	if (mdss_dsi_phy_v3_is_pll_on(ctrl))
		pr_warn("PLL already on prior to configuring phy");

	/* wait for REFGEN READY */
	rc = readl_poll_timeout_atomic(ctrl->phy_io.base + CMN_PHY_STATUS,
		status, (status & BIT(0)), delay_us, timeout_us);
	if (rc) {
		pr_err("Ref gen not ready. Aborting\n");
		return rc;
	}

	/* de-assert digital power down */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CTRL_0, BIT(6));

	/* turn off resync FIFO */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_RBUF_CTRL, 0x00);

	/* Select MS1 byte-clk */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_GLBL_CTRL, 0x10);

	/* Enable LDO */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_VREG_CTRL, 0x59);

	mdss_dsi_phy_v3_config_timings(ctrl);

	/* Remove power down from all blocks */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CTRL_0, 0x7f);

	mdss_dsi_phy_v3_lanes_enable(ctrl);

	/* Select hybrid mode and use local-wordclk */
	DSI_PHY_W32(ctrl->phy_io.base, CMN_CTRL_2, 0x08);

	mdss_dsi_phy_v3_set_pll_source(ctrl);

	mdss_dsi_phy_v3_config_lane_settings(ctrl);

	/* wait for all writes to be flushed */
	wmb();

	return rc;
}
