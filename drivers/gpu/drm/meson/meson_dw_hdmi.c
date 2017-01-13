/*
 * Copyright (C) 2016 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/component.h>
#include <linux/of_graph.h>
#include <linux/gpio/consumer.h>
#include <linux/reset.h>
#include <linux/clk.h>

#include <drm/drmP.h>
#include <drm/drm_edid.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/bridge/dw_hdmi.h>

#include "meson_drv.h"
#include "meson_venc.h"
#include "meson_vclk.h"
#include "meson_dw_hdmi.h"
#include "meson_registers.h"

/*
 * HDMI Output is composed of :
 * - A Synopsys DesignWare HDMI Controller IP
 * - A TOP control block controlling the Clocks and PHY
 * - A custom HDMI PHY in order convert video to TMDS signal
 *  ___________________________________
 * |            HDMI TOP               |<= HPD
 * |___________________________________|
 * |                  |                |
 * |  Synopsys HDMI   |   HDMI PHY     |=> TMDS
 * |    Controller    |________________|
 * |___________________________________|<=> DDC
 *
 * The HDMI TOP block only supports HPD sensing.
 * The Synopsys HDMI Controller interrupt is routed
 * throught the TOP Block interrupt.
 * Communication to the TOP Block and the Synopsys
 * HDMI Controller is done a pair of addr+read/write
 * registers.
 * The HDMI PHY is configured by registers in the
 * HHI register block.
 *
 * Pixel data arrives in 4:4:4 format from the VENC
 * block and the VPU HDMI mux selects either the ENCI
 * encoder for the 576i or 480i formats or the ENCP
 * encoder for all the other formats including
 * interlaced HD formats.
 * The VENC uses a DVI encoder on top of the ENCI
 * or ENCP encoders to generate DVI timings for the
 * HDMI controller.
 *
 * GXBB, GXL and GXM embeds the Synopsys DesignWare
 * HDMI TX IP version 2.01a with HDCP and I2C & S/PDIF
 * audio source interfaces.
 */

/* TOP Block Communication Channel */
#define HDMITX_TOP_ADDR_REG	0x0
#define HDMITX_TOP_DATA_REG	0x4
#define HDMITX_TOP_CTRL_REG	0x8

/* Controller Communication Channel */
#define HDMITX_DWC_ADDR_REG	0x10
#define HDMITX_DWC_DATA_REG	0x14
#define HDMITX_DWC_CTRL_REG	0x18

/* HHI Registers */
#define HHI_MEM_PD_REG0		0x100 /* 0x40 */
#define HHI_HDMI_CLK_CNTL	0x1cc /* 0x73 */
#define HHI_HDMI_PHY_CNTL0	0x3a0 /* 0xe8 */
#define HHI_HDMI_PHY_CNTL1	0x3a4 /* 0xe9 */
#define HHI_HDMI_PHY_CNTL2	0x3a8 /* 0xea */
#define HHI_HDMI_PHY_CNTL3	0x3ac /* 0xeb */

static DEFINE_SPINLOCK(reg_lock);

enum meson_venc_source {
	MESON_VENC_SOURCE_NONE = 0,
	MESON_VENC_SOURCE_ENCI = 1,
	MESON_VENC_SOURCE_ENCP = 2,
};

struct meson_dw_hdmi {
	struct drm_encoder encoder;
	struct dw_hdmi_plat_data dw_plat_data;
	struct meson_drm *priv;
	struct device *dev;
	void __iomem *hdmitx;
	struct gpio_desc *hpd;
	struct reset_control *hdmitx_apb;
	struct reset_control *hdmitx_ctrl;
	struct reset_control *hdmitx_phy;	
	struct clk *hdmi_pclk;
	struct clk *venci_clk;
};
#define encoder_to_meson_dw_hdmi(x) \
	container_of(x, struct meson_dw_hdmi, encoder)

#define plat_data_to_meson_dw_hdmi(x) \
	container_of(x, struct meson_dw_hdmi, dw_plat_data)

static inline int dw_hdmi_is_compatible(struct meson_dw_hdmi *dw_hdmi,
					const char *compat)
{
	return of_device_is_compatible(dw_hdmi->dev->of_node, compat);
}

static unsigned int dw_hdmi_top_read(struct meson_dw_hdmi *dw_hdmi,
				     unsigned int addr)
{
	unsigned long flags;
	unsigned int data;

	spin_lock_irqsave(&reg_lock, flags);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_TOP_ADDR_REG);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_TOP_ADDR_REG);
	data = readl(dw_hdmi->hdmitx + HDMITX_TOP_DATA_REG);
	data = readl(dw_hdmi->hdmitx + HDMITX_TOP_DATA_REG);
	spin_unlock_irqrestore(&reg_lock, flags);

	return data;
}

static inline void dw_hdmi_top_write(struct meson_dw_hdmi *dw_hdmi,
				     unsigned int addr, unsigned int data)
{
	unsigned long flags;

	spin_lock_irqsave(&reg_lock, flags);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_TOP_ADDR_REG);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_TOP_ADDR_REG);
	writel(data, dw_hdmi->hdmitx + HDMITX_TOP_DATA_REG);
	spin_unlock_irqrestore(&reg_lock, flags);
}

static inline void dw_hdmi_top_write_bits(struct meson_dw_hdmi *dw_hdmi,
					  unsigned int addr,
					  unsigned int mask,
					  unsigned int val)
{
	unsigned int data = dw_hdmi_top_read(dw_hdmi, addr);

	data &= ~mask;
	data |= val;

	dw_hdmi_top_write(dw_hdmi, addr, data);
}

static inline void dw_hdmi_dwc_write(struct meson_dw_hdmi *dw_hdmi,
				     unsigned int addr, unsigned int data)
{
	unsigned long flags;

	spin_lock_irqsave(&reg_lock, flags);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_DWC_ADDR_REG);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_DWC_ADDR_REG);
	writel(data, dw_hdmi->hdmitx + HDMITX_DWC_DATA_REG);
	spin_unlock_irqrestore(&reg_lock, flags);
}

static unsigned int dw_hdmi_dwc_read(struct meson_dw_hdmi *dw_hdmi,
				     unsigned int addr)
{
	unsigned long flags;
	unsigned int data;

	spin_lock_irqsave(&reg_lock, flags);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_DWC_ADDR_REG);
	writel(addr & 0xffff, dw_hdmi->hdmitx + HDMITX_DWC_ADDR_REG);
	data = readl(dw_hdmi->hdmitx + HDMITX_DWC_DATA_REG);
	data = readl(dw_hdmi->hdmitx + HDMITX_DWC_DATA_REG);
	spin_unlock_irqrestore(&reg_lock, flags);

	return data;
}

static inline void dw_hdmi_dwc_write_bits(struct meson_dw_hdmi *dw_hdmi,
					  unsigned int addr,
					  unsigned int mask,
					  unsigned int val)
{
	unsigned int data = dw_hdmi_dwc_read(dw_hdmi, addr);

	data &= ~mask;
	data |= val;

	dw_hdmi_dwc_write(dw_hdmi, addr, data);
}

/* Bridge */

static void meson_hdmi_phy_setup_mode(struct meson_dw_hdmi *dw_hdmi,
				      struct drm_display_mode *mode)
{
	struct meson_drm *priv = dw_hdmi->priv;
	unsigned int phy_mode = 0;
	int vic = drm_match_cea_mode(mode);

	/* Bandwidth modes */
	switch (vic) {
	case 16: /* 1080p60 */
	case 31: /* 1080p50 */
		phy_mode = 3;
	/* TODO advanced 4k2k modes */
	default:
		phy_mode = 4;
	}

	if (dw_hdmi_is_compatible(dw_hdmi, "amlogic,meson-gxl-dw-hdmi") ||
	    dw_hdmi_is_compatible(dw_hdmi, "amlogic,meson-gxm-dw-hdmi")) {
		switch (phy_mode) {
		case 1: /* 5.94Gbps, 3.7125Gbsp */
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x333d3282);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0x2136315b);
			break;
		case 2: /* 2.97Gbps */
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x33303382);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0x2036315b);
			break;
		case 3: /* 1.485Gbps */
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x33303362);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0x2016315b);
			break;
		default: /* 742.5Mbps, and below */
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x33604142);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0x0016315b);
			break;
		}
	} else if (dw_hdmi_is_compatible(dw_hdmi,
					 "amlogic,meson-gxbb-dw-hdmi")) {
		switch (phy_mode) {
		case 1: /* 5.94Gbps, 3.7125Gbsp */
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x33353245);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0x2100115b);
			break;
		case 2: /* 2.97Gbps */
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x33634283);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0xb000115b);
			break;
		case 3: /* 1.485Gbps, and below */
		default:
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0x33632122);
			regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL3, 0x2000115b);
			break;
		}
	}
}

static inline void dw_hdmi_phy_reset(struct meson_dw_hdmi *dw_hdmi)
{
	struct meson_drm *priv = dw_hdmi->priv;

	/* Enable and software reset */
	regmap_update_bits(priv->hhi, HHI_HDMI_PHY_CNTL1, 0xf, 0xf);
	
	mdelay(2);

	/* Enable and unreset */
	regmap_update_bits(priv->hhi, HHI_HDMI_PHY_CNTL1, 0xf, 0xe);
	
	mdelay(2); 
}

static void dw_hdmi_set_vclk(struct meson_dw_hdmi *dw_hdmi,
			     struct drm_display_mode *mode)
{
	struct meson_drm *priv = dw_hdmi->priv;
	int vic = drm_match_cea_mode(mode);
	unsigned vclk_freq;
	unsigned venc_freq;
	unsigned hdmi_freq;

	vclk_freq = mode->clock;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		vclk_freq *= 2;

	venc_freq = vclk_freq;
	hdmi_freq = vclk_freq;

	if (meson_venc_hdmi_venc_repeat(vic))
		venc_freq *= 2;

	vclk_freq = max(venc_freq, hdmi_freq);

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		venc_freq /= 2;

	DRM_DEBUG_DRIVER("vclk:%d venc=%d hdmi=%d enci=%d\n",
		vclk_freq, venc_freq, hdmi_freq,
		priv->venc.hdmi_use_enci);
	
	meson_vclk_setup(priv, MESON_VCLK_TARGET_HDMI, vclk_freq,
			 venc_freq, hdmi_freq, priv->venc.hdmi_use_enci);
}

static int dw_hdmi_phy_init(struct dw_hdmi *hdmi, 
			    const struct dw_hdmi_plat_data *data,
			    struct drm_display_mode *mode)
{
	struct meson_dw_hdmi *dw_hdmi = plat_data_to_meson_dw_hdmi(data);
	struct meson_drm *priv = dw_hdmi->priv;
	unsigned int wr_clk =
		readl_relaxed(priv->io_base + _REG(VPU_HDMI_SETTING));

	DRM_DEBUG_DRIVER("%d:\"%s\"\n", mode->base.id, mode->name);

	/* Enable clocks */
	regmap_update_bits(priv->hhi, HHI_HDMI_CLK_CNTL, 0xffff, 0x100);

	/* Bring HDMITX MEM output of power down */
	regmap_update_bits(priv->hhi, HHI_MEM_PD_REG0, 0xff << 8, 0);

	/* Bring out of reset */
	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_SW_RESET,  0);

	/* Enable internal pixclk, tmds_clk, spdif_clk, i2s_clk, cecclk */
	dw_hdmi_top_write_bits(dw_hdmi, HDMITX_TOP_CLK_CNTL,
			       0x3, 0x3);
	dw_hdmi_top_write_bits(dw_hdmi, HDMITX_TOP_CLK_CNTL,
			       0x3 << 4, 0x3 << 4);
	/* Enable normal output to PHY */
	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_BIST_CNTL, BIT(12));

	/* clk40 */
	/* TOFIX clk40 for 4k2k */
	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_TMDS_CLK_PTTN_01, 0x001f001f);
	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_TMDS_CLK_PTTN_23, 0x001f001f);

	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_TMDS_CLK_PTTN_CNTL, 0x1);
	msleep(20);
	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_TMDS_CLK_PTTN_CNTL, 0x2);

	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_INTR_STAT_CLR, 0x1f);

	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_INTR_MASKN,
			  BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4));

	/* Setup PHY parameters */
	meson_hdmi_phy_setup_mode(dw_hdmi, mode);

	/* Setup PHY */
	regmap_update_bits(priv->hhi, HHI_HDMI_PHY_CNTL1, 
			   0xffff << 16, 0x0390 << 16);

	/* BIT_INVERT */
	if (dw_hdmi_is_compatible(dw_hdmi, "amlogic,meson-gxl-dw-hdmi") ||
	    dw_hdmi_is_compatible(dw_hdmi, "amlogic,meson-gxm-dw-hdmi"))
		regmap_update_bits(priv->hhi, HHI_HDMI_PHY_CNTL1,
				   BIT(17), 0);
	else
		regmap_update_bits(priv->hhi, HHI_HDMI_PHY_CNTL1,
				   BIT(17), BIT(17));

	/* Disable clock, fifo, fifo_wr */
	regmap_update_bits(priv->hhi, HHI_HDMI_PHY_CNTL1, 0xf, 0);
	
	msleep(100);
	
	dw_hdmi_phy_reset(dw_hdmi);
	dw_hdmi_phy_reset(dw_hdmi);
	dw_hdmi_phy_reset(dw_hdmi);

	if (priv->venc.hdmi_use_enci)
		writel_relaxed(0, priv->io_base + _REG(ENCI_VIDEO_EN));
	else
		writel_relaxed(0, priv->io_base + _REG(ENCP_VIDEO_EN));

	writel_bits_relaxed(0x3, 0,
			    priv->io_base + _REG(VPU_HDMI_SETTING));
	writel_bits_relaxed(0xf << 8, 0,
			    priv->io_base + _REG(VPU_HDMI_SETTING));
	
	if (priv->venc.hdmi_use_enci)
		writel_relaxed(1, priv->io_base + _REG(ENCI_VIDEO_EN));
	else
		writel_relaxed(1, priv->io_base + _REG(ENCP_VIDEO_EN));
	
	writel_bits_relaxed(0xf << 8, wr_clk & (0xf << 8),
			    priv->io_base + _REG(VPU_HDMI_SETTING));


	if (priv->venc.hdmi_use_enci)
		writel_bits_relaxed(0x3, MESON_VENC_SOURCE_ENCI,
				    priv->io_base + _REG(VPU_HDMI_SETTING));
	else
		writel_bits_relaxed(0x3, MESON_VENC_SOURCE_ENCP,
				    priv->io_base + _REG(VPU_HDMI_SETTING));

	return 0;
}

static void dw_hdmi_phy_disable(struct dw_hdmi *hdmi,
				const struct dw_hdmi_plat_data *data)
{
	struct meson_dw_hdmi *dw_hdmi = plat_data_to_meson_dw_hdmi(data);
	struct meson_drm *priv = dw_hdmi->priv;

	DRM_DEBUG_DRIVER("\n");

	regmap_write(priv->hhi, HHI_HDMI_PHY_CNTL0, 0);
}

/*
 * Workaround until we find a way to use the PHY HPD level irq
 * and read the pad value...
 */
static bool dw_hdmi_read_hpd(struct dw_hdmi *hdmi,
			     const struct dw_hdmi_plat_data *data)
{
	struct meson_dw_hdmi *dw_hdmi = plat_data_to_meson_dw_hdmi(data);
	bool is_connected = !!gpiod_get_value(dw_hdmi->hpd);

	dw_hdmi_setup_rx_sense(dw_hdmi->dev, is_connected, is_connected);

	return is_connected;
}

static irqreturn_t dw_hdmi_top_irq(int irq, void *dev_id)
{
	struct meson_dw_hdmi *dw_hdmi = dev_id;
	u32 stat;

	stat = dw_hdmi_top_read(dw_hdmi, HDMITX_TOP_INTR_STAT);

	DRM_DEBUG_DRIVER("stat %x\n", stat);

	/* HPD Events */
	if (stat & (HDMITX_TOP_INTR_HDP_RISE | HDMITX_TOP_INTR_HDP_FALL)) {
		bool hpd_connected = false;

		if (stat & HDMITX_TOP_INTR_HDP_RISE)
			hpd_connected = true;

		dw_hdmi_setup_rx_sense(dw_hdmi->dev, hpd_connected,
				       hpd_connected);
	}

	dw_hdmi_top_write(dw_hdmi, HDMITX_TOP_INTR_STAT_CLR, stat);

	/* HDMI Controller Interrupt */
	if (stat & 1)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static enum drm_mode_status dw_hdmi_mode_valid(struct drm_connector *connector,
					       struct drm_display_mode *mode)
{
	unsigned vclk_freq;
	unsigned venc_freq;
	unsigned hdmi_freq;
	int vic = drm_match_cea_mode(mode);

	DRM_DEBUG_DRIVER("Modeline %d:\"%s\" %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x\n",
		mode->base.id, mode->name, mode->vrefresh, mode->clock,
		mode->hdisplay, mode->hsync_start,
		mode->hsync_end, mode->htotal,
		mode->vdisplay, mode->vsync_start,
		mode->vsync_end, mode->vtotal, mode->type, mode->flags);

	/* For now, only accept VIC modes */
	if (!vic)
		return MODE_BAD;

	/* For now, filter by supported VIC modes */
	if (!meson_venc_hdmi_supported_vic(vic))
		return MODE_BAD;

	vclk_freq = mode->clock;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		vclk_freq *= 2;

	venc_freq = vclk_freq;
	hdmi_freq = vclk_freq;

	if (meson_venc_hdmi_venc_repeat(vic))
		venc_freq *= 2;

	vclk_freq = max(venc_freq, hdmi_freq);

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		venc_freq /= 2;

	dev_dbg(connector->dev->dev, "%s: vclk:%d venc=%d hdmi=%d\n", __func__,
		vclk_freq, venc_freq, hdmi_freq);

	switch (vclk_freq) {
		case 54000:
		case 74250:
		case 148500:
		case 297000:
		case 594000:
			return MODE_OK;
	}

	return MODE_CLOCK_RANGE;
}

/* Encoder */

static void meson_venc_hdmi_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs meson_venc_hdmi_encoder_funcs = {
	.destroy        = meson_venc_hdmi_encoder_destroy,
};

static int meson_venc_hdmi_encoder_atomic_check(struct drm_encoder *encoder,
					struct drm_crtc_state *crtc_state,
					struct drm_connector_state *conn_state)
{
	return 0;
}

static void meson_venc_hdmi_encoder_disable(struct drm_encoder *encoder)
{
	struct meson_dw_hdmi *dw_hdmi = encoder_to_meson_dw_hdmi(encoder);
	struct meson_drm *priv = dw_hdmi->priv;

	DRM_DEBUG_DRIVER("\n");

	writel_bits_relaxed(0x3, 0,
			    priv->io_base + _REG(VPU_HDMI_SETTING));

	writel_relaxed(0, priv->io_base + _REG(ENCI_VIDEO_EN));
	writel_relaxed(0, priv->io_base + _REG(ENCP_VIDEO_EN));
}

static void meson_venc_hdmi_encoder_enable(struct drm_encoder *encoder)
{
	struct meson_dw_hdmi *dw_hdmi = encoder_to_meson_dw_hdmi(encoder);
	struct meson_drm *priv = dw_hdmi->priv;

	DRM_DEBUG_DRIVER("%s\n", priv->venc.hdmi_use_enci ? "VENCI" : "VENCP");

	if (priv->venc.hdmi_use_enci)
		writel_relaxed(1, priv->io_base + _REG(ENCI_VIDEO_EN));
	else
		writel_relaxed(1, priv->io_base + _REG(ENCP_VIDEO_EN));
}

static void meson_venc_hdmi_encoder_mode_set(struct drm_encoder *encoder,
				   struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct meson_dw_hdmi *dw_hdmi = encoder_to_meson_dw_hdmi(encoder);
	struct meson_drm *priv = dw_hdmi->priv;
	int vic = drm_match_cea_mode(mode);

	DRM_DEBUG_DRIVER("%d:\"%s\" vic %d\n",
			 mode->base.id, mode->name, vic);

	/* Should have been filtered */
	if (!vic)
		return;

	/* VENC + VENC-DVI Mode setup */
	meson_venc_hdmi_mode_set(priv, vic, mode);

	/* VCLK Set clock */
	dw_hdmi_set_vclk(dw_hdmi, mode);

	/* Setup YUV444 to HDMI-TX, no 10bit diphering */
	writel_relaxed(0, priv->io_base + _REG(VPU_HDMI_FMT_CTRL));
}

static const struct drm_encoder_helper_funcs
				meson_venc_hdmi_encoder_helper_funcs = {
	.atomic_check	= meson_venc_hdmi_encoder_atomic_check,
	.disable	= meson_venc_hdmi_encoder_disable,
	.enable		= meson_venc_hdmi_encoder_enable,
	.mode_set	= meson_venc_hdmi_encoder_mode_set,
};

/* DW HDMI Regmap */

static int meson_dw_hdmi_reg_read(void *context, unsigned int reg,
				  unsigned int *result)
{
	*result = dw_hdmi_dwc_read(context, reg);

	return 0;

}

static int meson_dw_hdmi_reg_write(void *context, unsigned int reg,
				   unsigned int val)
{
	dw_hdmi_dwc_write(context, reg, val);

	return 0;
}

static const struct regmap_config meson_dw_hdmi_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
 
	.reg_read = meson_dw_hdmi_reg_read,
	.reg_write = meson_dw_hdmi_reg_write,
 
	.max_register = 0x10000,
};

static bool meson_hdmi_connector_is_available(struct device *dev)
{
	struct device_node *ep, *remote;

	/* HDMI Connector is on the second port, first endpoint */
	ep = of_graph_get_endpoint_by_regs(dev->of_node, 1, 0);
	if (!ep)
		return false;

	/* If the endpoint node exists, consider it enabled */
	remote = of_graph_get_remote_port(ep);
	if (remote) {
		of_node_put(ep);
		return true;
	}

	of_node_put(ep);
	of_node_put(remote);

	return false;
}

static int meson_dw_hdmi_bind(struct device *dev, struct device *master,
				void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct meson_dw_hdmi *meson_dw_hdmi;
	struct drm_device *drm = data;
	struct meson_drm *priv = drm->dev_private;
	struct dw_hdmi_plat_data *dw_plat_data;
	struct drm_encoder *encoder;
	struct resource *res;
	int irq;
	int ret;

	DRM_DEBUG_DRIVER("\n");

	if (!meson_hdmi_connector_is_available(dev)) {
		dev_info(drm->dev, "HDMI Output connector not available\n");
		return -ENODEV;
	}

	meson_dw_hdmi = devm_kzalloc(dev, sizeof(*meson_dw_hdmi),
				     GFP_KERNEL);
	if (!meson_dw_hdmi)
		return -ENOMEM;

	meson_dw_hdmi->priv = priv;
	meson_dw_hdmi->dev = dev;
	dw_plat_data = &meson_dw_hdmi->dw_plat_data;
	encoder = &meson_dw_hdmi->encoder;

	meson_dw_hdmi->hdmitx_apb = devm_reset_control_get_exclusive(dev,
						"hdmitx_apb");
	if (IS_ERR(meson_dw_hdmi->hdmitx_apb)) {
		dev_err(dev, "Failed to get hdmitx_apb reset\n");
		return PTR_ERR(meson_dw_hdmi->hdmitx_apb);
	}

	meson_dw_hdmi->hdmitx_ctrl = devm_reset_control_get_exclusive(dev,
						"hdmitx");
	if (IS_ERR(meson_dw_hdmi->hdmitx_ctrl)) {
		dev_err(dev, "Failed to get hdmitx reset\n");
		return PTR_ERR(meson_dw_hdmi->hdmitx_ctrl);
	}

	meson_dw_hdmi->hdmitx_phy = devm_reset_control_get_exclusive(dev,
						"hdmitx_phy");
	if (IS_ERR(meson_dw_hdmi->hdmitx_phy)) {
		dev_err(dev, "Failed to get hdmitx_phy reset\n");
		return PTR_ERR(meson_dw_hdmi->hdmitx_phy);
	}

	meson_dw_hdmi->hpd = devm_gpiod_get(dev, "hpd", GPIOD_IN);
	if (IS_ERR(meson_dw_hdmi->hpd))
		return PTR_ERR(meson_dw_hdmi->hpd);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	meson_dw_hdmi->hdmitx = devm_ioremap_resource(dev, res);
	if (IS_ERR(meson_dw_hdmi->hdmitx))
		return PTR_ERR(meson_dw_hdmi->hdmitx);

	meson_dw_hdmi->hdmi_pclk = devm_clk_get(dev, "isfr");
	if (IS_ERR(meson_dw_hdmi->hdmi_pclk)) {
		dev_err(dev, "Unable to get HDMI pclk\n");
		return PTR_ERR(meson_dw_hdmi->hdmi_pclk);
	}
	clk_prepare_enable(meson_dw_hdmi->hdmi_pclk);

	meson_dw_hdmi->venci_clk = devm_clk_get(dev, "venci");
	if (IS_ERR(meson_dw_hdmi->venci_clk)) {
		dev_err(dev, "Unable to get venci clk\n");
		return PTR_ERR(meson_dw_hdmi->venci_clk);
	}
	clk_prepare_enable(meson_dw_hdmi->venci_clk);

	dw_plat_data->regm = devm_regmap_init(dev, NULL, meson_dw_hdmi,
					      &meson_dw_hdmi_regmap_config);
	if (IS_ERR(dw_plat_data->regm))
		return PTR_ERR(dw_plat_data->regm);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "Failed to get hdmi top irq\n");
		return irq;
	}

	ret = devm_request_irq(dev, irq, dw_hdmi_top_irq, IRQF_SHARED,
			       "dw_hdmi_top_irq", meson_dw_hdmi);
	if (ret) {
		dev_err(dev, "Failed to request hdmi top irq\n");
		return ret;
	}

	/* Encoder */

	drm_encoder_helper_add(encoder, &meson_venc_hdmi_encoder_helper_funcs);

	ret = drm_encoder_init(drm, encoder, &meson_venc_hdmi_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, "meson_hdmi");
	if (ret) {
		dev_err(priv->dev, "Failed to init HDMI encoder\n");
		return ret;
	}

	encoder->possible_crtcs = BIT(0);

	DRM_DEBUG_DRIVER("encoder initialized\n");

	/* Enable clocks */
	regmap_update_bits(priv->hhi, HHI_HDMI_CLK_CNTL, 0xffff, 0x100);

	/* Bring HDMITX MEM output of power down */
	regmap_update_bits(priv->hhi, HHI_MEM_PD_REG0, 0xff << 8, 0);

	/* Reset HDMITX APB & TX & PHY */
	reset_control_reset(meson_dw_hdmi->hdmitx_apb);
	reset_control_reset(meson_dw_hdmi->hdmitx_ctrl);
	reset_control_reset(meson_dw_hdmi->hdmitx_phy);

	/* Enable APB3 fail on error */
	writel_bits_relaxed(BIT(15), BIT(15),
			    meson_dw_hdmi->hdmitx + HDMITX_TOP_CTRL_REG);
	writel_bits_relaxed(BIT(15), BIT(15),
			    meson_dw_hdmi->hdmitx + HDMITX_DWC_CTRL_REG);

	/* Bring out of reset */
	dw_hdmi_top_write(meson_dw_hdmi, HDMITX_TOP_SW_RESET,  0);

	msleep(1);
	
	dw_hdmi_top_write(meson_dw_hdmi, HDMITX_TOP_CLK_CNTL, 0xff);

	/* Bridge / Connector */

	dw_plat_data->mode_valid = dw_hdmi_mode_valid;
	dw_plat_data->hdmi_phy_init = dw_hdmi_phy_init;
	dw_plat_data->hdmi_phy_disable = dw_hdmi_phy_disable;
	dw_plat_data->hdmi_read_hpd = dw_hdmi_read_hpd;
	dw_plat_data->input_fmt = DW_HDMI_INPUT_FMT_YCBCR444;

	ret = dw_hdmi_bind(pdev, encoder, &meson_dw_hdmi->dw_plat_data);
	if (ret)
		return ret;

	DRM_DEBUG_DRIVER("HDMI controller initialized\n");

	return 0;
}

static void meson_dw_hdmi_unbind(struct device *dev, struct device *master,
				   void *data)
{
	dw_hdmi_unbind(dev);
}

static const struct component_ops meson_dw_hdmi_ops = {
	.bind	= meson_dw_hdmi_bind,
	.unbind	= meson_dw_hdmi_unbind,
};

static int meson_dw_hdmi_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &meson_dw_hdmi_ops);
}

static int meson_dw_hdmi_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &meson_dw_hdmi_ops);

	return 0;
}

static const struct of_device_id meson_dw_hdmi_of_table[] = {
	{ .compatible = "amlogic,meson-gxbb-dw-hdmi" },
	{ .compatible = "amlogic,meson-gxl-dw-hdmi" },
	{ .compatible = "amlogic,meson-gxm-dw-hdmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, meson_dw_hdmi_of_table);

static struct platform_driver meson_dw_hdmi_platform_driver = {
	.probe		= meson_dw_hdmi_probe,
	.remove		= meson_dw_hdmi_remove,
	.driver		= {
		.name		= "meson-dw-hdmi",
		.of_match_table	= meson_dw_hdmi_of_table,
	},
};
module_platform_driver(meson_dw_hdmi_platform_driver);
