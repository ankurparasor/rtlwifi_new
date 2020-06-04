// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "coex.h"
#include "fw.h"
#include "tx.h"
#include "rx.h"
#include "phy.h"
#include "rtw8821c.h"
#include "rtw8821c_table.h"
#include "mac.h"
#include "reg.h"
#include "debug.h"
#include "bf.h"

static void rtw8821ce_efuse_parsing(struct rtw_efuse *efuse,
				    struct rtw8821c_efuse *map)
{
	ether_addr_copy(efuse->addr, map->e.mac_addr);
}

static int rtw8821c_read_efuse(struct rtw_dev *rtwdev, u8 *log_map)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw8821c_efuse *map;
	int i;

	map = (struct rtw8821c_efuse *)log_map;

	efuse->rfe_option = map->rfe_option;
	efuse->rf_board_option = map->rf_board_option;
	efuse->crystal_cap = map->xtal_k;
	efuse->pa_type_2g = map->pa_type;
	efuse->pa_type_5g = map->pa_type;
	efuse->lna_type_2g = map->lna_type_2g[0];
	efuse->lna_type_5g = map->lna_type_5g[0];
	efuse->channel_plan = map->channel_plan;
	efuse->country_code[0] = map->country_code[0];
	efuse->country_code[1] = map->country_code[1];
	efuse->bt_setting = map->rf_bt_setting;
	efuse->regd = map->rf_board_option & 0x7;
	efuse->thermal_meter[0] = map->thermal_meter;
	efuse->thermal_meter_k = map->thermal_meter;
	efuse->tx_bb_swing_setting_2g = map->tx_bb_swing_setting_2g;
	efuse->tx_bb_swing_setting_5g = map->tx_bb_swing_setting_5g;

	for (i = 0; i < 4; i++)
		efuse->txpwr_idx_table[i] = map->txpwr_idx_table[i];

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		rtw8821ce_efuse_parsing(efuse, map);
		break;
	default:
		/* unsupported now */
		return -ENOTSUPP;
	}

	return 0;
}

static const u32 rtw8821c_txscale_tbl[] = {
	0x081, 0x088, 0x090, 0x099, 0x0a2, 0x0ac, 0x0b6, 0x0c0, 0x0cc, 0x0d8,
	0x0e5, 0x0f2, 0x101, 0x110, 0x120, 0x131, 0x143, 0x156, 0x16a, 0x180,
	0x197, 0x1af, 0x1c8, 0x1e3, 0x200, 0x21e, 0x23e, 0x261, 0x285, 0x2ab,
	0x2d3, 0x2fe, 0x32b, 0x35c, 0x38e, 0x3c4, 0x3fe
};

static const u8 rtw8821c_get_swing_index(struct rtw_dev *rtwdev)
{
	u8 i = 0;
	u32 swing, table_value;

	swing = rtw_read32_mask(rtwdev, REG_TXSCALE_A, 0xffe00000);
	for (i = 0; i < ARRAY_SIZE(rtw8821c_txscale_tbl); i++) {
		table_value = rtw8821c_txscale_tbl[i];
		if (swing == table_value)
			break;
	}

	return i;
}

static void rtw8821c_pwrtrack_init(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 swing_idx = rtw8821c_get_swing_index(rtwdev);

	if (swing_idx >= ARRAY_SIZE(rtw8821c_txscale_tbl))
		dm_info->default_ofdm_index = 24;
	else
		dm_info->default_ofdm_index = swing_idx;

	ewma_thermal_init(&dm_info->avg_thermal[RF_PATH_A]);
	dm_info->delta_power_index[RF_PATH_A] = 0;
	dm_info->delta_power_index_last[RF_PATH_A] = 0;
	dm_info->pwr_trk_triggered = false;
	dm_info->pwr_trk_init_trigger = true;
	dm_info->thermal_meter_k = rtwdev->efuse.thermal_meter_k;
}

static void rtw8821c_phy_bf_init(struct rtw_dev *rtwdev)
{
	rtw_bf_phy_init(rtwdev);
	/* Grouping bitmap parameters */
	rtw_write32(rtwdev, 0x1C94, 0xAFFFAFFF);
}

static void rtw8821c_phy_set_param(struct rtw_dev *rtwdev)
{
	u8 crystal_cap, val;

	/* power on BB/RF domain */
	val = rtw_read8(rtwdev, REG_SYS_FUNC_EN);
	val |= BIT_FEN_PCIEA;
	rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);

	/* toggle BB reset */
	val |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
	rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);
	val &= ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);
	val |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
	rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);

	rtw_write8(rtwdev, REG_RF_CTRL,
		   BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	usleep_range(10, 11);
	rtw_write8(rtwdev, REG_WLRF1 + 3,
		   BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	usleep_range(10, 11);

	/* pre init before header files config */
	rtw_write32_clr(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

	rtw_phy_load_tables(rtwdev);

	crystal_cap = rtwdev->efuse.crystal_cap & 0x3F;
	rtw_write32_mask(rtwdev, REG_AFE_XTAL_CTRL, 0x7e000000, crystal_cap);
	rtw_write32_mask(rtwdev, REG_AFE_PLL_CTRL, 0x7e, crystal_cap);
	rtw_write32_mask(rtwdev, REG_CCK0_FAREPORT, BIT(18) | BIT(22), 0);

	/* post init after header files config */
	rtw_write32_set(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);
	rtwdev->chip->ch_param[0] = rtw_read32_mask(rtwdev, REG_TXSF2, MASKDWORD);
	rtwdev->chip->ch_param[1] = rtw_read32_mask(rtwdev, REG_TXSF6, MASKDWORD);
	rtwdev->chip->ch_param[2] = rtw_read32_mask(rtwdev, REG_TXFILTER, MASKDWORD);

	rtw_phy_init(rtwdev);
	rtwdev->dm_info.cck_pd_default = rtw_read8(rtwdev, REG_CSRATIO) & 0x1f;

	rtw8821c_pwrtrack_init(rtwdev);

	rtw8821c_phy_bf_init(rtwdev);
}

static int rtw8821c_mac_init(struct rtw_dev *rtwdev)
{
	u32 value32;
	u16 pre_txcnt;

	/* protocol configuration */
	rtw_write8(rtwdev, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
	rtw_write8_set(rtwdev, REG_TX_HANG_CTRL, BIT_EN_EOF_V1);
	pre_txcnt = WLAN_PRE_TXCNT_TIME_TH | BIT_EN_PRECNT;
	rtw_write8(rtwdev, REG_PRECNT_CTRL, (u8)(pre_txcnt & 0xFF));
	rtw_write8(rtwdev, REG_PRECNT_CTRL + 1, (u8)(pre_txcnt >> 8));
	value32 = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
		  (WLAN_MAX_AGG_PKT_LIMIT << 16) |
		  (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24);
	rtw_write32(rtwdev, REG_PROT_MODE_CTRL, value32);
	rtw_write16(rtwdev, REG_BAR_MODE_CTRL + 2,
		    WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
	rtw_write8_set(rtwdev, REG_INIRTS_RATE_SEL, BIT(5));

	/* EDCA configuration */
	rtw_write8_clr(rtwdev, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0);
	rtw_write16(rtwdev, REG_TXPAUSE, 0);
	rtw_write8(rtwdev, REG_SLOT, WLAN_SLOT_TIME);
	rtw_write8(rtwdev, REG_PIFS, WLAN_PIFS_TIME);
	rtw_write32(rtwdev, REG_SIFS, WLAN_SIFS_CFG);
	rtw_write16(rtwdev, REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
	rtw_write16(rtwdev, REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
	rtw_write32(rtwdev, REG_RD_NAV_NXT, WLAN_NAV_CFG);
	rtw_write16(rtwdev, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);

	/* Set beacon cotnrol - enable TSF and other related functions */
	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);

	/* Set send beacon related registers */
	rtw_write32(rtwdev, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
	rtw_write8(rtwdev, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
	rtw_write8(rtwdev, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
	rtw_write8_clr(rtwdev, REG_TX_PTCL_CTRL + 1, BIT_SIFS_BK_EN >> 8);

	/* WMAC configuration */
	rtw_write32(rtwdev, REG_RXFLTMAP0, WLAN_RX_FILTER0);
	rtw_write16(rtwdev, REG_RXFLTMAP2, WLAN_RX_FILTER2);
	rtw_write32(rtwdev, REG_RCR, WLAN_RCR_CFG);
	rtw_write8(rtwdev, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
	rtw_write8(rtwdev, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
	rtw_write8(rtwdev, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
	rtw_write8(rtwdev, REG_ACKTO_CCK, 0x40);
	rtw_write8_set(rtwdev, REG_WMAC_TRXPTCL_CTL_H, BIT(1));
	rtw_write8_set(rtwdev, REG_SND_PTCL_CTRL, BIT(6));
	rtw_write32(rtwdev, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
	rtw_write8(rtwdev, REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);

	return 0;
}

static void rtw8821c_cfg_ldo25(struct rtw_dev *rtwdev, bool enable)
{
	u8 ldo_pwr;

	ldo_pwr = rtw_read8(rtwdev, REG_LDO_EFUSE_CTRL + 3);
	ldo_pwr = enable ? ldo_pwr | BIT(7) : ldo_pwr & ~BIT(7);
	rtw_write8(rtwdev, REG_LDO_EFUSE_CTRL + 3, ldo_pwr);
}

static void rtw8821c_set_channel_rf(struct rtw_dev *rtwdev, u8 channel, u8 bw)
{
	u32 rf_reg18;

	rf_reg18 = rtw_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK);

	rf_reg18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
		      RF18_BW_MASK);

	rf_reg18 |= (channel <= 14 ? RF18_BAND_2G : RF18_BAND_5G);
	rf_reg18 |= (channel & RF18_CHANNEL_MASK);

	if (channel >= 100 && channel <= 140)
		rf_reg18 |= RF18_RFSI_GE;
	else if (channel > 140)
		rf_reg18 |= RF18_RFSI_GT;

	switch (bw) {
	case RTW_CHANNEL_WIDTH_5:
	case RTW_CHANNEL_WIDTH_10:
	case RTW_CHANNEL_WIDTH_20:
	default:
		rf_reg18 |= RF18_BW_20M;
		break;
	case RTW_CHANNEL_WIDTH_40:
		rf_reg18 |= RF18_BW_40M;
		break;
	case RTW_CHANNEL_WIDTH_80:
		rf_reg18 |= RF18_BW_80M;
		break;
	}

	if (channel <= 14) {
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(6), 0x1);
		rtw_write_rf(rtwdev, RF_PATH_A, 0x64, 0xf, 0xf);
	} else {
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(6), 0x0);
	}

	rtw_write_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK, rf_reg18);

	rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 0);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 1);
}

static void rtw8821c_set_channel_rxdfir(struct rtw_dev *rtwdev, u8 bw)
{
	if (bw == RTW_CHANNEL_WIDTH_40) {
		/* RX DFIR for BW40 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
		rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0x0);
	} else if (bw == RTW_CHANNEL_WIDTH_80) {
		/* RX DFIR for BW80 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
		rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0x1);
	} else {
		/* RX DFIR for BW20, BW10 and BW5 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 0x1);
		rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0x0);
	}
}

static void rtw8821c_set_channel_bb(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				    u8 primary_ch_idx)
{
	u32 val32;

	if (channel <= 14) {
		rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x0);
		rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x0);
		rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 15);

		rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x0);
		rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x96a);
		if (channel == 14) {
			rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, 0x0000b81c);
			rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, 0x0000);
			rtw_write32_mask(rtwdev, REG_TXFILTER, MASKDWORD, 0x00003667);
		} else {
			rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD,
					 rtwdev->chip->ch_param[0]);
			rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD,
					 rtwdev->chip->ch_param[1] & MASKLWORD);
			rtw_write32_mask(rtwdev, REG_TXFILTER, MASKDWORD,
					 rtwdev->chip->ch_param[2]);
		}
	} else if (channel > 35) {
		rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x1);
		rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x1);
		rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x0);
		rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 15);

		if (channel >= 36 && channel <= 64)
			rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x1);
		else if (channel >= 100 && channel <= 144)
			rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x2);
		else if (channel >= 149)
			rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x3);

		if (channel >= 36 && channel <= 48)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x494);
		else if (channel >= 52 && channel <= 64)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x453);
		else if (channel >= 100 && channel <= 116)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x452);
		else if (channel >= 118 && channel <= 177)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x412);
	}

	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
	default:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xffcffc00;
		val32 |= 0x10010000;
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_40:
		if (primary_ch_idx == 1)
			rtw_write32_set(rtwdev, REG_RXSB, BIT(4));
		else
			rtw_write32_clr(rtwdev, REG_RXSB, BIT(4));

		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xff3ff300;
		val32 |= 0x20020000 | ((primary_ch_idx & 0xf) << 2) |
			 RTW_CHANNEL_WIDTH_40;
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_80:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xfcffcf00;
		val32 |= 0x40040000 | ((primary_ch_idx & 0xf) << 2) |
			 RTW_CHANNEL_WIDTH_80;
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_5:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xefcefc00;
		val32 |= 0x200240;
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x0);
		rtw_write32_mask(rtwdev, REG_ADC40, BIT(31), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_10:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xefcefc00;
		val32 |= 0x300380;
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x0);
		rtw_write32_mask(rtwdev, REG_ADC40, BIT(31), 0x1);
		break;
	}
}

static u32 rtw8821c_get_bb_swing(struct rtw_dev *rtwdev, u8 channel)
{
	struct rtw_efuse efuse = rtwdev->efuse;
	u8 tx_bb_swing;
	u32 swing2setting[4] = {0x200, 0x16a, 0x101, 0x0b6};

	tx_bb_swing = channel <= 14 ? efuse.tx_bb_swing_setting_2g :
				      efuse.tx_bb_swing_setting_5g;
	if (tx_bb_swing > 9)
		tx_bb_swing = 0;

	return swing2setting[(tx_bb_swing / 3)];
}

static void rtw8821c_set_channel_bb_swing(struct rtw_dev *rtwdev, u8 channel,
					  u8 bw, u8 primary_ch_idx)
{
	rtw_write32_mask(rtwdev, REG_TXSCALE_A, GENMASK(31, 21),
			 rtw8821c_get_bb_swing(rtwdev, channel));
	rtw8821c_pwrtrack_init(rtwdev);
}

static void rtw8821c_set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				 u8 primary_chan_idx)
{
	rtw8821c_set_channel_bb(rtwdev, channel, bw, primary_chan_idx);
	rtw8821c_set_channel_bb_swing(rtwdev, channel, bw, primary_chan_idx);
	rtw_set_channel_mac(rtwdev, channel, bw, primary_chan_idx);
	rtw8821c_set_channel_rf(rtwdev, channel, bw);
	rtw8821c_set_channel_rxdfir(rtwdev, bw);
}

static void query_phy_status_page0(struct rtw_dev *rtwdev, u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	s8 min_rx_power = -120;
	u8 pwdb = GET_PHY_STAT_P0_PWDB(phy_status);

	pkt_stat->rx_power[RF_PATH_A] = pwdb - 100;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 1);
	pkt_stat->bw = RTW_CHANNEL_WIDTH_20;
	pkt_stat->signal_power = max(pkt_stat->rx_power[RF_PATH_A],
				     min_rx_power);
}

static void query_phy_status_page1(struct rtw_dev *rtwdev, u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 rxsc, bw;
	s8 min_rx_power = -120;

	if (pkt_stat->rate > DESC_RATE11M && pkt_stat->rate < DESC_RATEMCS0)
		rxsc = GET_PHY_STAT_P1_L_RXSC(phy_status);
	else
		rxsc = GET_PHY_STAT_P1_HT_RXSC(phy_status);

	if (rxsc >= 1 && rxsc <= 8)
		bw = RTW_CHANNEL_WIDTH_20;
	else if (rxsc >= 9 && rxsc <= 12)
		bw = RTW_CHANNEL_WIDTH_40;
	else if (rxsc >= 13)
		bw = RTW_CHANNEL_WIDTH_80;
	else
		bw = GET_PHY_STAT_P1_RF_MODE(phy_status);

	pkt_stat->rx_power[RF_PATH_A] = GET_PHY_STAT_P1_PWDB_A(phy_status) - 110;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 1);
	pkt_stat->bw = bw;
	pkt_stat->signal_power = max(pkt_stat->rx_power[RF_PATH_A],
				     min_rx_power);
}

static void query_phy_status(struct rtw_dev *rtwdev, u8 *phy_status,
			     struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 page;

	page = *phy_status & 0xf;

	switch (page) {
	case 0:
		query_phy_status_page0(rtwdev, phy_status, pkt_stat);
		break;
	case 1:
		query_phy_status_page1(rtwdev, phy_status, pkt_stat);
		break;
	default:
		rtw_warn(rtwdev, "unused phy status page (%d)\n", page);
		return;
	}
}

static void rtw8821c_query_rx_desc(struct rtw_dev *rtwdev, u8 *rx_desc,
				   struct rtw_rx_pkt_stat *pkt_stat,
				   struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_hdr *hdr;
	u32 desc_sz = rtwdev->chip->rx_pkt_desc_sz;
	u8 *phy_status = NULL;

	memset(pkt_stat, 0, sizeof(*pkt_stat));

	pkt_stat->phy_status = GET_RX_DESC_PHYST(rx_desc);
	pkt_stat->icv_err = GET_RX_DESC_ICV_ERR(rx_desc);
	pkt_stat->crc_err = GET_RX_DESC_CRC32(rx_desc);
	pkt_stat->decrypted = !GET_RX_DESC_SWDEC(rx_desc);
	pkt_stat->is_c2h = GET_RX_DESC_C2H(rx_desc);
	pkt_stat->pkt_len = GET_RX_DESC_PKT_LEN(rx_desc);
	pkt_stat->drv_info_sz = GET_RX_DESC_DRV_INFO_SIZE(rx_desc);
	pkt_stat->shift = GET_RX_DESC_SHIFT(rx_desc);
	pkt_stat->rate = GET_RX_DESC_RX_RATE(rx_desc);
	pkt_stat->cam_id = GET_RX_DESC_MACID(rx_desc);
	pkt_stat->ppdu_cnt = GET_RX_DESC_PPDU_CNT(rx_desc);
	pkt_stat->tsf_low = GET_RX_DESC_TSFL(rx_desc);

	/* drv_info_sz is in unit of 8-bytes */
	pkt_stat->drv_info_sz *= 8;

	/* c2h cmd pkt's rx/phy status is not interested */
	if (pkt_stat->is_c2h)
		return;

	hdr = (struct ieee80211_hdr *)(rx_desc + desc_sz + pkt_stat->shift +
				       pkt_stat->drv_info_sz);
	if (pkt_stat->phy_status) {
		phy_status = rx_desc + desc_sz + pkt_stat->shift;
		query_phy_status(rtwdev, phy_status, pkt_stat);
	}

	rtw_rx_fill_rx_status(rtwdev, pkt_stat, hdr, rx_status, phy_status);
}

static void
rtw8821c_set_tx_power_index_by_rate(struct rtw_dev *rtwdev, u8 path, u8 rs)
{
	struct rtw_hal *hal = &rtwdev->hal;
	static const u32 offset_txagc[2] = {0x1d00, 0x1d80};
	static u32 phy_pwr_idx;
	u8 rate, rate_idx, pwr_index, shift;
	int j;

	for (j = 0; j < rtw_rate_size[rs]; j++) {
		rate = rtw_rate_section[rs][j];
		pwr_index = hal->tx_pwr_tbl[path][rate];
		shift = rate & 0x3;
		phy_pwr_idx |= ((u32)pwr_index << (shift * 8));
		if (shift == 0x3 || rate == DESC_RATEVHT1SS_MCS9) {
			rate_idx = rate & 0xfc;
			rtw_write32(rtwdev, offset_txagc[path] + rate_idx,
				    phy_pwr_idx);
			phy_pwr_idx = 0;
		}
	}
}

static void rtw8821c_set_tx_power_index(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	int rs, path;

	for (path = 0; path < hal->rf_path_num; path++) {
		for (rs = 0; rs < RTW_RATE_SECTION_MAX; rs++) {
			if (rs == RTW_RATE_SECTION_HT_2S ||
			    rs == RTW_RATE_SECTION_VHT_2S)
				continue;
			rtw8821c_set_tx_power_index_by_rate(rtwdev, path, rs);
		}
	}
}

static void rtw8821c_false_alarm_statistics(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 cck_enable;
	u32 cck_fa_cnt;
	u32 ofdm_fa_cnt;
	u32 crc32_cnt;
	u32 cca32_cnt;

	cck_enable = rtw_read32(rtwdev, REG_RXPSEL) & BIT(28);
	cck_fa_cnt = rtw_read16(rtwdev, REG_FA_CCK);
	ofdm_fa_cnt = rtw_read16(rtwdev, REG_FA_OFDM);

	dm_info->cck_fa_cnt = cck_fa_cnt;
	dm_info->ofdm_fa_cnt = ofdm_fa_cnt;
	if (cck_enable)
		dm_info->total_fa_cnt += cck_fa_cnt;
	dm_info->total_fa_cnt = ofdm_fa_cnt;

	crc32_cnt = rtw_read32(rtwdev, REG_CRC_CCK);
	dm_info->cck_ok_cnt = FIELD_GET(GENMASK(15, 0), crc32_cnt);
	dm_info->cck_err_cnt = FIELD_GET(GENMASK(31, 16), crc32_cnt);

	crc32_cnt = rtw_read32(rtwdev, REG_CRC_OFDM);
	dm_info->ofdm_ok_cnt = FIELD_GET(GENMASK(15, 0), crc32_cnt);
	dm_info->ofdm_err_cnt = FIELD_GET(GENMASK(31, 16), crc32_cnt);

	crc32_cnt = rtw_read32(rtwdev, REG_CRC_HT);
	dm_info->ht_ok_cnt = FIELD_GET(GENMASK(15, 0), crc32_cnt);
	dm_info->ht_err_cnt = FIELD_GET(GENMASK(31, 16), crc32_cnt);

	crc32_cnt = rtw_read32(rtwdev, REG_CRC_VHT);
	dm_info->vht_ok_cnt = FIELD_GET(GENMASK(15, 0), crc32_cnt);
	dm_info->vht_err_cnt = FIELD_GET(GENMASK(31, 16), crc32_cnt);

	cca32_cnt = rtw_read32(rtwdev, REG_CCA_OFDM);
	dm_info->ofdm_cca_cnt = FIELD_GET(GENMASK(31, 16), cca32_cnt);
	dm_info->total_cca_cnt = dm_info->ofdm_cca_cnt;
	if (cck_enable) {
		cca32_cnt = rtw_read32(rtwdev, REG_CCA_CCK);
		dm_info->cck_cca_cnt = FIELD_GET(GENMASK(15, 0), cca32_cnt);
		dm_info->total_cca_cnt += dm_info->cck_cca_cnt;
	}

	rtw_write32_set(rtwdev, REG_FAS, BIT(17));
	rtw_write32_clr(rtwdev, REG_FAS, BIT(17));
	rtw_write32_clr(rtwdev, REG_RXDESC, BIT(15));
	rtw_write32_set(rtwdev, REG_RXDESC, BIT(15));
	rtw_write32_set(rtwdev, REG_CNTRST, BIT(0));
	rtw_write32_clr(rtwdev, REG_CNTRST, BIT(0));
}

static void rtw8821c_do_iqk(struct rtw_dev *rtwdev)
{
	static int do_iqk_cnt;
	struct rtw_iqk_para para = {.clear = 0, .segment_iqk = 0};
	u32 rf_reg, iqk_fail_mask;
	int counter;
	bool reload;

	if (rtw_is_assoc(rtwdev))
		para.segment_iqk = 1;

	rtw_fw_do_iqk(rtwdev, &para);

	for (counter = 0; counter < 300; counter++) {
		rf_reg = rtw_read_rf(rtwdev, RF_PATH_A, RF_DTXLOK, RFREG_MASK);
		if (rf_reg == 0xabcde)
			break;
		msleep(20);
	}
	rtw_write_rf(rtwdev, RF_PATH_A, RF_DTXLOK, RFREG_MASK, 0x0);

	reload = !!rtw_read32_mask(rtwdev, REG_IQKFAILMSK, BIT(16));
	iqk_fail_mask = rtw_read32_mask(rtwdev, REG_IQKFAILMSK, GENMASK(7, 0));
	rtw_dbg(rtwdev, RTW_DBG_PHY,
		"iqk counter=%d reload=%d do_iqk_cnt=%d n_iqk_fail(mask)=0x%02x\n",
		counter, reload, ++do_iqk_cnt, iqk_fail_mask);
}

static void rtw8821c_phy_calibration(struct rtw_dev *rtwdev)
{
	rtw8821c_do_iqk(rtwdev);
}

static void
rtw8821c_txagc_swing_offset(struct rtw_dev *rtwdev, u8 pwr_idx_offset,
			    s8 pwr_idx_offset_lower,
			    s8 *txagc_idx, u8 *swing_idx)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	s8 delta_pwr_idx = dm_info->delta_power_index[RF_PATH_A];
	u8 swing_upper_bound = dm_info->default_ofdm_index + 10;
	u8 swing_lower_bound = 0;
	u8 max_pwr_idx_offset = 0xf;
	s8 agc_index = 0;
	u8 swing_index = dm_info->default_ofdm_index;

	pwr_idx_offset = min_t(u8, pwr_idx_offset, max_pwr_idx_offset);
	pwr_idx_offset_lower = max_t(s8, pwr_idx_offset_lower, -15);

	if (delta_pwr_idx >= 0) {
		if (delta_pwr_idx <= pwr_idx_offset) {
			agc_index = delta_pwr_idx;
			swing_index = dm_info->default_ofdm_index;
		} else if (delta_pwr_idx > pwr_idx_offset) {
			agc_index = pwr_idx_offset;
			swing_index = dm_info->default_ofdm_index +
					delta_pwr_idx - pwr_idx_offset;
			swing_index = min_t(u8, swing_index, swing_upper_bound);
		}
	} else if (delta_pwr_idx < 0) {
		if (delta_pwr_idx >= pwr_idx_offset_lower) {
			agc_index = delta_pwr_idx;
			swing_index = dm_info->default_ofdm_index;
		} else if (delta_pwr_idx < pwr_idx_offset_lower) {
			if (dm_info->default_ofdm_index >
				(pwr_idx_offset_lower - delta_pwr_idx))
				swing_index = dm_info->default_ofdm_index +
					delta_pwr_idx - pwr_idx_offset_lower;
			else
				swing_index = swing_lower_bound;

			agc_index = pwr_idx_offset_lower;
		}
	}

	if (swing_index >= ARRAY_SIZE(rtw8821c_txscale_tbl)) {
		rtw_warn(rtwdev, "swing index overflow\n");
		swing_index = ARRAY_SIZE(rtw8821c_txscale_tbl) - 1;
	}

	*txagc_idx = agc_index;
	*swing_idx = swing_index;
}

static void rtw8821c_pwrtrack_set_pwr(struct rtw_dev *rtwdev, u8 pwr_idx_offset,
				      s8 pwr_idx_offset_lower)
{
	s8 txagc_idx;
	u8 swing_idx;

	rtw8821c_txagc_swing_offset(rtwdev, pwr_idx_offset, pwr_idx_offset_lower,
				    &txagc_idx, &swing_idx);
	rtw_write32_mask(rtwdev, REG_TXAGCIDX, GENMASK(6, 1), txagc_idx);
	rtw_write32_mask(rtwdev, REG_TXSCALE_A, GENMASK(31, 21),
			 rtw8821c_txscale_tbl[swing_idx]);
}

static void rtw8821c_pwrtrack_set(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 pwr_idx_offset, tx_pwr_idx;
	s8 pwr_idx_offset_lower;
	u8 channel = rtwdev->hal.current_channel;
	u8 band_width = rtwdev->hal.current_band_width;
	u8 regd = rtwdev->regd.txpwr_regd;
	u8 tx_rate = dm_info->tx_rate;
	u8 max_pwr_idx = rtwdev->chip->max_power_index;

	tx_pwr_idx = rtw_phy_get_tx_power_index(rtwdev, RF_PATH_A, tx_rate,
						band_width, channel, regd);

	tx_pwr_idx = min_t(u8, tx_pwr_idx, max_pwr_idx);

	pwr_idx_offset = max_pwr_idx - tx_pwr_idx;
	pwr_idx_offset_lower = 0 - tx_pwr_idx;

	rtw8821c_pwrtrack_set_pwr(rtwdev, pwr_idx_offset, pwr_idx_offset_lower);
}

static void rtw8821c_phy_pwrtrack(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	struct rtw_swing_table swing_table;
	u8 thermal_value, delta;

	rtw_phy_config_swing_table(rtwdev, &swing_table);

	if (rtwdev->efuse.thermal_meter[0] == 0xff)
		return;

	thermal_value = rtw_read_rf(rtwdev, RF_PATH_A, RF_T_METER, 0xfc00);

	rtw_phy_pwrtrack_avg(rtwdev, thermal_value, RF_PATH_A);

	if (dm_info->pwr_trk_init_trigger)
		dm_info->pwr_trk_init_trigger = false;
	else if (!rtw_phy_pwrtrack_thermal_changed(rtwdev, thermal_value,
						   RF_PATH_A))
		goto iqk;

	delta = rtw_phy_pwrtrack_get_delta(rtwdev, RF_PATH_A);

	delta = min_t(u8, delta, RTW_PWR_TRK_TBL_SZ - 1);

	dm_info->delta_power_index[RF_PATH_A] =
		rtw_phy_pwrtrack_get_pwridx(rtwdev, &swing_table, RF_PATH_A,
					    RF_PATH_A, delta);
	if (dm_info->delta_power_index[RF_PATH_A] ==
			dm_info->delta_power_index_last[RF_PATH_A])
		goto iqk;
	else
		dm_info->delta_power_index_last[RF_PATH_A] =
			dm_info->delta_power_index[RF_PATH_A];
	rtw8821c_pwrtrack_set(rtwdev);

iqk:
	if (rtw_phy_pwrtrack_need_iqk(rtwdev))
		rtw8821c_do_iqk(rtwdev);
}

static void rtw8821c_pwr_track(struct rtw_dev *rtwdev)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;

	if (efuse->power_track_type != 0)
		return;

	if (!dm_info->pwr_trk_triggered) {
		rtw_write_rf(rtwdev, RF_PATH_A, RF_T_METER,
			     GENMASK(17, 16), 0x03);
		dm_info->pwr_trk_triggered = true;
		return;
	}

	rtw8821c_phy_pwrtrack(rtwdev);
	dm_info->pwr_trk_triggered = false;
}

static void rtw8821c_bf_config_bfee_su(struct rtw_dev *rtwdev,
				       struct rtw_vif *vif,
				       struct rtw_bfee *bfee, bool enable)
{
	if (enable)
		rtw_bf_enable_bfee_su(rtwdev, vif, bfee);
	else
		rtw_bf_remove_bfee_su(rtwdev, bfee);
}

static void rtw8821c_bf_config_bfee_mu(struct rtw_dev *rtwdev,
				       struct rtw_vif *vif,
				       struct rtw_bfee *bfee, bool enable)
{
	if (enable)
		rtw_bf_enable_bfee_mu(rtwdev, vif, bfee);
	else
		rtw_bf_remove_bfee_mu(rtwdev, bfee);
}

static void rtw8821c_bf_config_bfee(struct rtw_dev *rtwdev, struct rtw_vif *vif,
				    struct rtw_bfee *bfee, bool enable)
{
	if (bfee->role == RTW_BFEE_SU)
		rtw8821c_bf_config_bfee_su(rtwdev, vif, bfee, enable);
	else if (bfee->role == RTW_BFEE_MU)
		rtw8821c_bf_config_bfee_mu(rtwdev, vif, bfee, enable);
	else
		rtw_warn(rtwdev, "wrong bfee role\n");
}

static void rtw8821c_phy_cck_pd_set(struct rtw_dev *rtwdev, u8 new_lvl)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 pd[CCK_PD_LV_MAX] = {3, 7, 13, 13, 13};

	if (dm_info->min_rssi > 60) {
		new_lvl = 4;
		pd[4] = 0x1d;
		goto set_cck_pd;
	}

	if (dm_info->cck_pd_lv[RTW_CHANNEL_WIDTH_20][RF_PATH_A] == new_lvl)
		return;

	dm_info->cck_fa_avg = CCK_FA_AVG_RESET;

set_cck_pd:
	dm_info->cck_pd_lv[RTW_CHANNEL_WIDTH_20][RF_PATH_A] = new_lvl;
	rtw_write32_mask(rtwdev, REG_PWRTH, 0x3f0000, pd[new_lvl]);
	rtw_write32_mask(rtwdev, REG_PWRTH2, 0x1f0000,
			 dm_info->cck_pd_default + new_lvl * 2);
}

static struct rtw_pwr_seq_cmd trans_carddis_to_cardemu_8821c[] = {
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x004A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4) | BIT(7), 0},
	{0x0300,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x0301,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_act_8821c[] = {
	{0x0020,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0001,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_DELAY, 1, RTW_PWR_DELAY_MS},
	{0x0000,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(4) | BIT(3) | BIT(2)), 0},
	{0x0075,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x0075,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(4) | BIT(3)), 0},
	{0x10C3,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(0), 0},
	{0x0020,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), BIT(3)},
	{0x0074,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0x0022,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0062,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(7) | BIT(6) | BIT(5)),
	 (BIT(7) | BIT(6) | BIT(5))},
	{0x0061,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(7) | BIT(6) | BIT(5)), 0},
	{0x007C,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_act_to_cardemu_8821c[] = {
	{0x0093,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x001F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x0049,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0002,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x10C3,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{0x0020,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x0000,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_carddis_8821c[] = {
	{0x0007,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x20},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), BIT(2)},
	{0x004A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), 0},
	{0x004F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0046,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(6), BIT(6)},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), 0},
	{0x0046,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), BIT(7)},
	{0x0062,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), BIT(4)},
	{0x0081,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7) | BIT(6), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4), BIT(3)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{0x0090,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0044,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x0040,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x90},
	{0x0041,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x00},
	{0x0042,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x04},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static const struct rtw_pwr_seq_cmd *card_enable_flow_8821c[] = {
	trans_carddis_to_cardemu_8821c,
	trans_cardemu_to_act_8821c,
	NULL
};

static const struct rtw_pwr_seq_cmd *card_disable_flow_8821c[] = {
	trans_act_to_cardemu_8821c,
	trans_cardemu_to_carddis_8821c,
	NULL
};

static const struct rtw_intf_phy_para usb2_param_8821c[] = {
	{0xFFFF, 0x00,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static const struct rtw_intf_phy_para usb3_param_8821c[] = {
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static const struct rtw_intf_phy_para pcie_gen1_param_8821c[] = {
	{0x0009, 0x6380,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static const struct rtw_intf_phy_para pcie_gen2_param_8821c[] = {
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static const struct rtw_intf_phy_para_table phy_para_table_8821c = {
	.usb2_para	= usb2_param_8821c,
	.usb3_para	= usb3_param_8821c,
	.gen1_para	= pcie_gen1_param_8821c,
	.gen2_para	= pcie_gen2_param_8821c,
	.n_usb2_para	= ARRAY_SIZE(usb2_param_8821c),
	.n_usb3_para	= ARRAY_SIZE(usb2_param_8821c),
	.n_gen1_para	= ARRAY_SIZE(pcie_gen1_param_8821c),
	.n_gen2_para	= ARRAY_SIZE(pcie_gen2_param_8821c),
};

static const struct rtw_rfe_def rtw8821c_rfe_defs[] = {
	[0] = RTW_DEF_RFE(8821c, 0, 0),
};

static struct rtw_hw_reg rtw8821c_dig[] = {
	[0] = { .addr = 0xc50, .mask = 0x7f },
};

static const struct rtw_ltecoex_addr rtw8821c_ltecoex_addr = {
	.ctrl = LTECOEX_ACCESS_CTRL,
	.wdata = LTECOEX_WRITE_DATA,
	.rdata = LTECOEX_READ_DATA,
};

static struct rtw_page_table page_table_8821c[] = {
	/* not sure what [0] stands for */
	{16, 16, 16, 14, 1},
	{16, 16, 16, 14, 1},
	{16, 16, 0, 0, 1},
	{16, 16, 16, 0, 1},
	{16, 16, 16, 14, 1},
};

static struct rtw_rqpn rqpn_table_8821c[] = {
	/* not sure what [0] stands for */
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_HIGH,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
};

static struct rtw_prioq_addrs prioq_addrs_8821c = {
	.prio[RTW_DMA_MAPPING_EXTRA] = {
		.rsvd = REG_FIFOPAGE_INFO_4, .avail = REG_FIFOPAGE_INFO_4 + 2,
	},
	.prio[RTW_DMA_MAPPING_LOW] = {
		.rsvd = REG_FIFOPAGE_INFO_2, .avail = REG_FIFOPAGE_INFO_2 + 2,
	},
	.prio[RTW_DMA_MAPPING_NORMAL] = {
		.rsvd = REG_FIFOPAGE_INFO_3, .avail = REG_FIFOPAGE_INFO_3 + 2,
	},
	.prio[RTW_DMA_MAPPING_HIGH] = {
		.rsvd = REG_FIFOPAGE_INFO_1, .avail = REG_FIFOPAGE_INFO_1 + 2,
	},
	.wsize = true,
};

static struct rtw_chip_ops rtw8821c_ops = {
	.phy_set_param		= rtw8821c_phy_set_param,
	.read_efuse		= rtw8821c_read_efuse,
	.query_rx_desc		= rtw8821c_query_rx_desc,
	.set_channel		= rtw8821c_set_channel,
	.mac_init		= rtw8821c_mac_init,
	.read_rf		= rtw_phy_read_rf,
	.write_rf		= rtw_phy_write_rf_reg_sipi,
	.set_antenna		= NULL,
	.set_tx_power_index	= rtw8821c_set_tx_power_index,
	.cfg_ldo25		= rtw8821c_cfg_ldo25,
	.false_alarm_statistics	= rtw8821c_false_alarm_statistics,
	.phy_calibration	= rtw8821c_phy_calibration,
	.cck_pd_set		= rtw8821c_phy_cck_pd_set,
	.pwr_track		= rtw8821c_pwr_track,
	.config_bfee		= rtw8821c_bf_config_bfee,
	.set_gid_table		= rtw_bf_set_gid_table,
	.cfg_csi_rate		= rtw_bf_cfg_csi_rate,
};

static const u8 rtw8821c_pwrtrk_5gb_n[][RTW_PWR_TRK_TBL_SZ] = {
	{0, 1, 1, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 8, 8, 9, 9, 9, 10, 10,
	 11, 11, 12, 12, 12, 12, 12},
	{0, 1, 1, 1, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 8, 8, 9, 9, 10, 10, 11,
	 11, 12, 12, 12, 12, 12, 12, 12},
	{0, 1, 2, 2, 3, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11,
	 11, 12, 12, 12, 12, 12, 12},
};

static const u8 rtw8821c_pwrtrk_5gb_p[][RTW_PWR_TRK_TBL_SZ] = {
	{0, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 11,
	 12, 12, 12, 12, 12, 12, 12},
	{0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 11,
	 12, 12, 12, 12, 12, 12, 12, 12},
	{0, 1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 10, 10, 11,
	 11, 12, 12, 12, 12, 12, 12, 12},
};

static const u8 rtw8821c_pwrtrk_5ga_n[][RTW_PWR_TRK_TBL_SZ] = {
	{0, 1, 1, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 8, 8, 9, 9, 9, 10, 10,
	 11, 11, 12, 12, 12, 12, 12},
	{0, 1, 1, 1, 2, 3, 3, 4, 4, 5, 5, 5, 6, 6, 7, 8, 8, 9, 9, 10, 10, 11,
	 11, 12, 12, 12, 12, 12, 12, 12},
	{0, 1, 2, 2, 3, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11,
	 11, 12, 12, 12, 12, 12, 12},
};

static const u8 rtw8821c_pwrtrk_5ga_p[][RTW_PWR_TRK_TBL_SZ] = {
	{0, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 11,
	 12, 12, 12, 12, 12, 12, 12},
	{0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 11,
	 12, 12, 12, 12, 12, 12, 12, 12},
	{0, 1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 10, 10, 11,
	 11, 12, 12, 12, 12, 12, 12, 12},
};

static const u8 rtw8821c_pwrtrk_2gb_n[] = {
	0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4,
	4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 9
};

static const u8 rtw8821c_pwrtrk_2gb_p[] = {
	0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5,
	5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9, 9, 9
};

static const u8 rtw8821c_pwrtrk_2ga_n[] = {
	0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4,
	4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 9
};

static const u8 rtw8821c_pwrtrk_2ga_p[] = {
	0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5,
	5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9, 9, 9
};

static const u8 rtw8821c_pwrtrk_2g_cck_b_n[] = {
	0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
	4, 5, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9
};

static const u8 rtw8821c_pwrtrk_2g_cck_b_p[] = {
	0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5,
	5, 6, 6, 7, 7, 7, 8, 8, 9, 9, 9, 9, 9, 9
};

static const u8 rtw8821c_pwrtrk_2g_cck_a_n[] = {
	0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
	4, 5, 5, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9
};

static const u8 rtw8821c_pwrtrk_2g_cck_a_p[] = {
	0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5,
	5, 6, 6, 7, 7, 7, 8, 8, 9, 9, 9, 9, 9, 9
};

const struct rtw_pwr_track_tbl rtw8821c_rtw_pwr_track_tbl = {
	.pwrtrk_5gb_n[0] = rtw8821c_pwrtrk_5gb_n[0],
	.pwrtrk_5gb_n[1] = rtw8821c_pwrtrk_5gb_n[1],
	.pwrtrk_5gb_n[2] = rtw8821c_pwrtrk_5gb_n[2],
	.pwrtrk_5gb_p[0] = rtw8821c_pwrtrk_5gb_p[0],
	.pwrtrk_5gb_p[1] = rtw8821c_pwrtrk_5gb_p[1],
	.pwrtrk_5gb_p[2] = rtw8821c_pwrtrk_5gb_p[2],
	.pwrtrk_5ga_n[0] = rtw8821c_pwrtrk_5ga_n[0],
	.pwrtrk_5ga_n[1] = rtw8821c_pwrtrk_5ga_n[1],
	.pwrtrk_5ga_n[2] = rtw8821c_pwrtrk_5ga_n[2],
	.pwrtrk_5ga_p[0] = rtw8821c_pwrtrk_5ga_p[0],
	.pwrtrk_5ga_p[1] = rtw8821c_pwrtrk_5ga_p[1],
	.pwrtrk_5ga_p[2] = rtw8821c_pwrtrk_5ga_p[2],
	.pwrtrk_2gb_n = rtw8821c_pwrtrk_2gb_n,
	.pwrtrk_2gb_p = rtw8821c_pwrtrk_2gb_p,
	.pwrtrk_2ga_n = rtw8821c_pwrtrk_2ga_n,
	.pwrtrk_2ga_p = rtw8821c_pwrtrk_2ga_p,
	.pwrtrk_2g_cckb_n = rtw8821c_pwrtrk_2g_cck_b_n,
	.pwrtrk_2g_cckb_p = rtw8821c_pwrtrk_2g_cck_b_p,
	.pwrtrk_2g_ccka_n = rtw8821c_pwrtrk_2g_cck_a_n,
	.pwrtrk_2g_ccka_p = rtw8821c_pwrtrk_2g_cck_a_p,
};

struct rtw_chip_info rtw8821c_hw_spec = {
	.ops = &rtw8821c_ops,
	.id = RTW_CHIP_TYPE_8821C,
	.fw_name = "rtw88/rtw8821c_fw.bin",
	.wlan_cpu = RTW_WCPU_11AC,
	.tx_pkt_desc_sz = 48,
	.tx_buf_desc_sz = 16,
	.rx_pkt_desc_sz = 24,
	.rx_buf_desc_sz = 8,
	.phy_efuse_size = 512,
	.log_efuse_size = 512,
	.ptct_efuse_size = 96,
	.txff_size = 65536,
	.rxff_size = 16384,
	.txgi_factor = 1,
	.is_pwr_by_rate_dec = true,
	.max_power_index = 0x3f,
	.csi_buf_pg_num = 0,
	.band = RTW_BAND_2G | RTW_BAND_5G,
	.page_size = 128,
	.dig_min = 0x1c,
	.ht_supported = true,
	.vht_supported = true,
	.lps_deep_mode_supported = BIT(LPS_DEEP_MODE_LCLK),
	.sys_func_en = 0xD8,
	.pwr_on_seq = card_enable_flow_8821c,
	.pwr_off_seq = card_disable_flow_8821c,
	.page_table = page_table_8821c,
	.rqpn_table = rqpn_table_8821c,
	.prioq_addrs = &prioq_addrs_8821c,
	.intf_table = &phy_para_table_8821c,
	.dig = rtw8821c_dig,
	.rf_base_addr = {0x2800, 0x2c00},
	.rf_sipi_addr = {0xc90, 0xe90},
	.ltecoex_addr = &rtw8821c_ltecoex_addr,
	.mac_tbl = &rtw8821c_mac_tbl,
	.agc_tbl = &rtw8821c_agc_tbl,
	.bb_tbl = &rtw8821c_bb_tbl,
	.rf_tbl = {&rtw8821c_rf_a_tbl},
	.rfe_defs = rtw8821c_rfe_defs,
	.rfe_defs_size = ARRAY_SIZE(rtw8821c_rfe_defs),
	.rx_ldpc = false,
	.pwr_track_tbl = &rtw8821c_rtw_pwr_track_tbl,
	.iqk_threshold = 8,
	.bfer_su_max_num = 2,
	.bfer_mu_max_num = 1,
};
EXPORT_SYMBOL(rtw8821c_hw_spec);

MODULE_FIRMWARE("rtw88/rtw8821c_fw.bin");

MODULE_AUTHOR("Realtek Corporation");
MODULE_DESCRIPTION("Realtek 802.11ac wireless 8821c driver");
MODULE_LICENSE("Dual BSD/GPL");
