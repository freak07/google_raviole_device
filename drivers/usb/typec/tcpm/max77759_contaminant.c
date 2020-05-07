// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Google LLC
 *
 * USB contaminant detection
 */

#include <linux/device.h>
#include <linux/irqreturn.h>
#include <linux/usb/tcpm.h>
#include <linux/usb/typec.h>

#include "max77759_helper.h"
#include "tcpci.h"
#include "tcpci_max77759.h"
#include "tcpci_max77759_vendor_reg.h"

#include <../../../power/supply/google/logbuffer.h>

enum contamiant_state {
	NOT_DETECTED,
	DETECTED,
	FLOATING_CABLE,
	SINK,
	DISABLED,
};

/* To be kept in sync with TCPC_VENDOR_ADC_CTRL1.ADCINSEL */
enum fladc_select {
	CC1_SCALE1 = 1,
	CC1_SCALE2,
	CC2_SCALE1,
	CC2_SCALE2,
	SBU1,
	SBU2,
};

/* Updated in MDR2 slide. */
#define FLADC_1uA_LSB_MV		100
/* High range CC */
#define FLADC_CC_HIGH_RANGE_LSB_MV	208
/* Low range CC */
#define FLADC_CC_LOW_RANGE_LSB_MV      126

/* 1uA current source */
#define FLADC_CC_SCALE1			1
/* 5 uA current source */
#define FLADC_CC_SCALE2			5

#define FLADC_1uA_CC_OFFSET_MV		300
#define FLADC_CC_HIGH_RANGE_OFFSET_MV	624
#define FLADC_CC_LOW_RANGE_OFFSET_MV	378

/* Actually translates to 18.7K */
#define ACCESSORY_THRESHOLD_CC_K	25
#define CONTAMINANT_THRESHOLD_SBU_K	1000
#define	CONTAMINANT_THRESHOLD_CC_K	1000

#define READ1_SLEEP_MS			10
#define READ2_SLEEP_MS			5

static bool contaminant_detect_maxq;

struct max77759_contaminant {
	struct max77759_plat *chip;
	enum contamiant_state state;
};

static int adc_to_mv(struct max77759_contaminant *contaminant,
		     enum fladc_select channel, bool ua_src, u8 fladc)
{
	/* SBU channels only have 1 scale with 1uA. */
	if ((ua_src && (channel == CC1_SCALE1 || channel == CC2_SCALE1 ||
			channel == SBU1 || channel == SBU2)))
		/* Mean of range */
		return FLADC_1uA_CC_OFFSET_MV + (fladc * FLADC_1uA_LSB_MV);
	else if (!ua_src && (channel == CC1_SCALE1 || channel == CC2_SCALE1))
		return FLADC_CC_HIGH_RANGE_OFFSET_MV + (fladc *
						FLADC_CC_HIGH_RANGE_LSB_MV);
	else if (!ua_src && (channel == CC1_SCALE2 || channel == CC2_SCALE2))
		return FLADC_CC_LOW_RANGE_OFFSET_MV + (fladc *
						FLADC_CC_LOW_RANGE_LSB_MV);
	else
		logbuffer_log(contaminant->chip->log,
			      "ADC ERROR: SCALE UNKNOWN");

	return fladc;
}

static inline bool status_check(u8 reg, u8 mask, u8 val)
{
	return (reg & mask) == val;
}

static int read_adc_mv(struct max77759_contaminant *contaminant,
		       enum fladc_select channel, int sleep_msec, bool raw,
		       bool ua_src)
{
	struct regmap *regmap = contaminant->chip->data.regmap;
	u8 fladc;
	struct logbuffer *log = contaminant->chip->log;
	int ret;

	/* Channel & scale select */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_ADC_CTRL1,
				    ADCINSEL_MASK, channel <<
				    ADC_CHANNEL_OFFSET);
	if (ret < 0)
		return ret;

	/* Enable ADC */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_ADC_CTRL1, ADCEN,
				    ADCEN);
	if (ret < 0)
		return ret;

	MAX77759_LOG_REGISTER(regmap, TCPC_VENDOR_ADC_CTRL1, log);

	usleep_range(sleep_msec * 1000, (sleep_msec + 1) * 1000);
	ret = max77759_read8(regmap, TCPC_VENDOR_FLADC_STATUS, &fladc);
	if (ret < 0)
		return ret;
	logbuffer_log(log, "Contaminant: ADC %u", fladc);

	/* Disable ADC */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_ADC_CTRL1, ADCEN,
				    0);
	if (ret < 0)
		return ret;
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_ADC_CTRL1,
				    ADCINSEL_MASK, 0);
	if (ret < 0)
		return ret;
	if (!raw)
		return adc_to_mv(contaminant, channel, ua_src, fladc);
	else
		return fladc;
}

static int read_resistance_kohm(struct max77759_contaminant *contaminant,
				enum fladc_select channel, int sleep_msec,
				bool raw)
{
	struct regmap *regmap = contaminant->chip->data.regmap;
	struct logbuffer *log = contaminant->chip->log;
	int mv;
	u8 switch_setting;
	int ret;

	if (channel == CC1_SCALE1 || channel == CC2_SCALE1 || channel ==
	    CC1_SCALE2 || channel == CC2_SCALE2) {
		/* Enable 1uA current source */
		max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
				      CCLPMODESEL_MASK,
				      ULTRA_LOW_POWER_MODE);
		/*
		 * CC resistive ladder is automatically disabled when
		 * 1uA source is ON and Flash ADC channel is not CC scale1.
		 * 1uA soruce is default on here.
		 *
		 * REMOVED IN MDR2.0 V2.0
		 */
		/* OVP disable */
		ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
					    CCOVPDIS, CCOVPDIS);
		if (ret < 0)
			return ret;
		MAX77759_LOG_REGISTER(regmap, TCPC_VENDOR_CC_CTRL2, log);

		mv = read_adc_mv(contaminant, channel, sleep_msec, raw, true);
		/* OVP enable */
		ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
					    CCOVPDIS, 0);

		if (ret < 0)
			return ret;
		/* returns KOhm as 1uA source is used. */
		return mv;
	}

	logbuffer_log(log, "Contaminant: SBU read");
	/*
	 * SBU measurement
	 * OVP disable
	 */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2, SBUOVPDIS,
				    SBUOVPDIS);
	if (ret < 0)
		return ret;

	/* Cache switch setting */
	ret = max77759_read8(regmap, TCPC_VENDOR_SBUSW_CTRL, &switch_setting);
	if (ret < 0)
		return ret;
	MAX77759_LOG_REGISTER(regmap, TCPC_VENDOR_SBUSW_CTRL, log);

	/* SBU switches auto configure when channel is selected. */
	/* Enable 1ua current source */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2, SBURPCTRL,
				    SBURPCTRL);
	if (ret < 0)
		return ret;
	MAX77759_LOG_REGISTER(regmap, TCPC_VENDOR_CC_CTRL2, log);

	mv = read_adc_mv(contaminant, channel, sleep_msec, raw, true);
	/* Disable current source */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2, SBURPCTRL,
				    0);
	if (ret < 0)
		return ret;
	/* Set switch to original setting */
	ret = max77759_write8(regmap, TCPC_VENDOR_SBUSW_CTRL,
			      switch_setting);
	if (ret < 0)
		return ret;

	/* OVP disable */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2, SBUOVPDIS
				    , 0);
	if (ret < 0)
		return ret;

	/*
	 * 1ua current source on sbu;
	 * return KOhm
	 */
	logbuffer_log(contaminant->chip->log, "Contaminant: SBU read %#x", mv);
	return mv;
}

static void read_comparators(struct max77759_contaminant *contaminant,
			     u8 *vendor_cc_status2_cc1,
			     u8 *vendor_cc_status2_cc2)
{
	struct regmap *regmap = contaminant->chip->data.regmap;
	struct logbuffer *log = contaminant->chip->log;
	int ret;

	logbuffer_log(log, "Contaminant: enable comparators");

	/* Enable 80uA source */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
				    CCRPCTRL_MASK, UA_80_SRC);
	if (ret < 0)
		return;

	/* Enable comparators */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL1, CCCOMPEN
				    , CCCOMPEN);
	if (ret < 0)
		return;
	MAX77759_LOG_REGISTER(regmap, TCPC_VENDOR_CC_CTRL1, log);

	/* Disable low power mode */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
				    CCLPMODESEL_MASK,
				    LOW_POWER_MODE_DISABLE);
	if (ret < 0)
		return;
	MAX77759_LOG_REGISTER(regmap, TCPC_VENDOR_CC_CTRL2, log);

	/* Sleep to allow comparators settle */
	usleep_range(5000, 6000);
	ret = max77759_update_bits8(regmap, TCPC_TCPC_CTRL,
				    TCPC_TCPC_CTRL_ORIENTATION,
				    PLUG_ORNT_CC1);
	if (ret < 0)
		return;
	MAX77759_LOG_REGISTER(regmap, TCPC_TCPC_CTRL, log);

	usleep_range(5000, 6000);
	ret = max77759_read8(regmap, VENDOR_CC_STATUS2,
			     vendor_cc_status2_cc1);
	if (ret < 0)
		return;
	logbuffer_log(log, "Contaminant: VENDOR_CC_STATUS2: %u"
		      , *vendor_cc_status2_cc1);

	ret = max77759_update_bits8(regmap, TCPC_TCPC_CTRL,
				    TCPC_TCPC_CTRL_ORIENTATION,
				    PLUG_ORNT_CC2);
	if (ret < 0)
		return;
	MAX77759_LOG_REGISTER(regmap, TCPC_TCPC_CTRL, log);

	usleep_range(5000, 6000);
	ret = max77759_read8(regmap, VENDOR_CC_STATUS2,
			     vendor_cc_status2_cc2);
	if (ret < 0)
		return;
	logbuffer_log(contaminant->chip->log, "Contaminant: VENDOR_CC_STATUS2: %u"
		      , *vendor_cc_status2_cc2);
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL1, CCCOMPEN,
				    0);
	if (ret < 0)
		return;
	max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
			      CCRPCTRL_MASK, 0);
}

static int detect_contaminant(struct max77759_contaminant *contaminant)
{
	int cc1_k, cc2_k, sbu1_k, sbu2_k;
	u8 vendor_cc_status2_cc1 = 0xff, vendor_cc_status2_cc2 = 0xff;
	struct max77759_plat *chip = contaminant->chip;

	read_comparators(contaminant, &vendor_cc_status2_cc1,
			 &vendor_cc_status2_cc2);

	logbuffer_log(chip->log, "Contaminant: vcc2_cc1:%u vcc2_cc2:%u",
		      vendor_cc_status2_cc1, vendor_cc_status2_cc2);

	/*
	 * Early return for sink. Either of CC sees Rd.
	 * But, both shouldn't.
	 */
	if ((!(CC1_VUFP_RD0P5 & vendor_cc_status2_cc1) ||
	     !(CC2_VUFP_RD0P5 & vendor_cc_status2_cc2)) &&
	    !(CC1_VUFP_RD0P5 & vendor_cc_status2_cc1 &&
	      CC2_VUFP_RD0P5 & vendor_cc_status2_cc2)) {
		logbuffer_log(chip->log, "Contaminant: AP SINK detected");
		return SINK;
	}

	/* CCLPMODESEL_AUTO_LOW_POWER in use. */
	cc1_k = read_resistance_kohm(contaminant, CC1_SCALE1, READ1_SLEEP_MS,
				     false);
	cc2_k = read_resistance_kohm(contaminant, CC2_SCALE1, READ2_SLEEP_MS,
				     false);
	logbuffer_log(chip->log, "Contaminant: cc1_k:%u cc2_k:%u", cc1_k, cc2_k)
		;

	if (cc1_k < CONTAMINANT_THRESHOLD_CC_K ||
	    cc2_k < CONTAMINANT_THRESHOLD_CC_K) {
		sbu1_k = read_resistance_kohm(contaminant, SBU1, READ1_SLEEP_MS,
					      false);
		sbu2_k = read_resistance_kohm(contaminant, SBU2, READ2_SLEEP_MS,
					      false);
		logbuffer_log(chip->log, "Contaminant: sbu1_k:%u sbu2_k:%u",
			      sbu1_k, sbu2_k);
		if (sbu1_k < CONTAMINANT_THRESHOLD_SBU_K || sbu2_k <
		    CONTAMINANT_THRESHOLD_SBU_K) {
			logbuffer_log(chip->log,
				      "Contaminant: AP contaminant detected");
			return DETECTED;
		}
		logbuffer_log(chip->log,
			      "Contaminant: AP floating cable detected");
		return FLOATING_CABLE;
	}

	logbuffer_log(chip->log, "Contaminant: AP contaminant NOT detected");
	return NOT_DETECTED;
}

static int enable_dry_detection(struct max77759_contaminant *contaminant)
{
	struct regmap *regmap = contaminant->chip->data.regmap;
	struct max77759_plat *chip = contaminant->chip;
	u8 temp;
	int ret;

	/* tunable: 1ua / Ultra low power mode enabled. */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL1,
				    CCCONNDRY, CCCONNDRY);
	if (ret < 0)
		return ret;
	ret = max77759_read8(regmap, TCPC_VENDOR_CC_CTRL1, &temp);
	if (ret < 0)
		return ret;
	logbuffer_log(chip->log, "Contaminant: TCPC_VENDOR_CC_CTRL1 %u", temp);

	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
				    CCLPMODESEL_MASK, ULTRA_LOW_POWER_MODE);
	if (ret < 0)
		return ret;
	ret = max77759_read8(regmap, TCPC_VENDOR_CC_CTRL2, &temp);
	if (ret < 0)
		return ret;
	logbuffer_log(chip->log, "Contaminant: TCPC_VENDOR_CC_CTRL2 %u", temp);

	/* Enable Look4Connection before sending the command */
	ret = max77759_update_bits8(regmap, TCPC_TCPC_CTRL,
				    TCPC_TCPC_CTRL_EN_LK4CONN_ALRT,
				    TCPC_TCPC_CTRL_EN_LK4CONN_ALRT);
	if (ret < 0)
		return ret;

	ret = max77759_write8(regmap, TCPC_COMMAND, TCPC_CMD_LOOK4CONNECTION);
	if (ret < 0)
		return ret;
	logbuffer_log(chip->log, "Contaminant: Dry detecion enabled");
	return 0;
}

static int maxq_detect_contaminant(struct max77759_contaminant *contaminant,
				   u8 cc_status)
{
	int cc1_raw = 0, cc2_raw = 0, sbu1_raw, sbu2_raw;
	u8 vendor_cc_status2_cc1, vendor_cc_status2_cc2, cc1_vufp_rd0p5;
	u8 cc2_vufp_rd0p5, maxq_detect_type;
	int ret;
	struct max77759_plat *chip = contaminant->chip;

	logbuffer_log(chip->log, "Contaminant: Query Maxq");
	if (contaminant->state == NOT_DETECTED) {
		cc1_raw = read_resistance_kohm(contaminant, CC1_SCALE1,
					       READ1_SLEEP_MS, true);
		cc2_raw = read_resistance_kohm(contaminant, CC2_SCALE1,
					       READ2_SLEEP_MS, true);
	}

	sbu1_raw = read_resistance_kohm(contaminant, SBU1,
					READ1_SLEEP_MS, true);
	sbu2_raw = read_resistance_kohm(contaminant, SBU2,
					READ2_SLEEP_MS, true);
	read_comparators(contaminant, &vendor_cc_status2_cc1,
			 &vendor_cc_status2_cc2);
	logbuffer_log(chip->log, "Contaminant: Query Maxq vcc2_1:%u vcc2_2:%u",
		      vendor_cc_status2_cc1, vendor_cc_status2_cc2);

	cc1_vufp_rd0p5 = vendor_cc_status2_cc1 & CC1_VUFP_RD0P5 ? 1 : 0;
	cc2_vufp_rd0p5 = vendor_cc_status2_cc2 & CC2_VUFP_RD0P5 ? 1 : 0;
	maxq_detect_type = contaminant->state == NOT_DETECTED ?
		MAXQ_DETECT_TYPE_CC_AND_SBU : MAXQ_DETECT_TYPE_SBU_ONLY;

	ret = maxq_query_contaminant(cc1_raw, cc2_raw, sbu1_raw, sbu2_raw,
				     cc1_vufp_rd0p5, cc2_vufp_rd0p5,
				     maxq_detect_type, 0);

	/* Upon errors, falling back to NOT_DETECTED state. */
	if (ret < 0) {
		logbuffer_log(chip->log, "Contaminant: Maxq errors");
		return NOT_DETECTED;
	}

	return ret;
}

/*
 * Don't want to be in workqueue as this is time critical for the state machine
 * to forward progress.
 */
bool process_contaminant_alert(struct max77759_contaminant *contaminant)
{
	u8 cc_status;
	struct regmap *regmap = contaminant->chip->data.regmap;
	enum contamiant_state state;
	struct max77759_plat *chip = contaminant->chip;

	/*
	 * Contaminant alert should only be processed when ALERT.CC_STAT is set.
	 * Caller i.e. the top level interrupt handler can check this to
	 * prevent redundant reads.
	 */
	max77759_read8(regmap, TCPC_CC_STATUS, &cc_status);
	logbuffer_log(chip->log, "Contaminant: CC_STATUS: %#x", cc_status);

	/* Exit if still LookingForConnection. */
	if (cc_status & TCPC_CC_STATUS_TOGGLING) {
		logbuffer_log(chip->log, "Contaminant: Looking for connection");
		return false;
	}

	if (contaminant->state == NOT_DETECTED || contaminant->state == SINK) {
		/* ConnectResult = 0b -> Rp */
		if (status_check(cc_status, TCPC_CC_STATUS_TERM,
				 TCPC_CC_STATUS_TERM_RP) &&
		    (status_check(cc_status, TCPC_CC_STATUS_CC1_MASK,
				  TCPC_CC_STATE_WTRSEL) ||
		     status_check(cc_status, TCPC_CC_STATUS_CC2_MASK,
				  TCPC_CC_STATE_WTRSEL))) {
			logbuffer_log(chip->log, "Contaminant: Check if wet");
			state = contaminant_detect_maxq ?
				maxq_detect_contaminant(contaminant, cc_status)
				: detect_contaminant(contaminant);
			contaminant->state = state;

			if (state == DETECTED || state == FLOATING_CABLE) {
				enable_dry_detection(contaminant);
				return true;
			}

			/* Sink or Not detected */
			enable_contaminant_detection(contaminant->chip);
			return true;
		}
	} else if (contaminant->state == DETECTED || contaminant->state ==
		   FLOATING_CABLE) {
		/* Contaminant heuristic in AP. */
		if (status_check(cc_status, TCPC_CC_STATUS_TERM,
				 TCPC_CC_STATUS_TERM_RP) &&
		    status_check(cc_status, TCPC_CC_STATUS_CC1_MASK,
				 TCPC_CC_STATE_SRC_OPEN) &&
		    status_check(cc_status, TCPC_CC_STATUS_CC2_MASK,
				 TCPC_CC_STATE_SRC_OPEN)) {
			logbuffer_log(chip->log, "Contaminant: Check if dry");
			state = contaminant_detect_maxq ?
				maxq_detect_contaminant(contaminant, cc_status)
				: detect_contaminant(contaminant);
			contaminant->state = NOT_DETECTED;

			if (state == DETECTED || state == FLOATING_CABLE) {
				enable_dry_detection(contaminant);
				return true;
			}

			/*
			 * Re-enable contaminant detection, hence toggling as
			 * well.
			 */
			enable_contaminant_detection(contaminant->chip);
			return true;
		}
		/*
		 * Re-enable dry detection, could be a spurious
		 * interrupt.
		 */
		enable_dry_detection(contaminant);
		/* TCPM does not manage ports in dry detection phase. */
		return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(process_contaminant_alert);

void disable_contaminant_detection(struct max77759_plat *chip)
{
	struct regmap *regmap = chip->data.regmap;
	int ret;

	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
				    CCLPMODESEL_MASK, 0);
	if (ret < 0)
		return;
	ret = max77759_update_bits8(regmap, TCPC_TCPC_CTRL,
				    TCPC_TCPC_CTRL_EN_LK4CONN_ALRT,
				    TCPC_TCPC_CTRL_EN_LK4CONN_ALRT);
	if (ret < 0)
		return;
	ret = max77759_write8(regmap, TCPC_COMMAND,
			      TCPC_CMD_LOOK4CONNECTION);
	if (ret < 0)
		return;
	logbuffer_log(chip->log, "Contaminant: Contaminant detection disabled");
}
EXPORT_SYMBOL_GPL(disable_contaminant_detection);

int enable_contaminant_detection(struct max77759_plat *chip)
{
	struct regmap *regmap = chip->data.regmap;
	u8 vcc2;
	int ret;

	/* tunable: 1ms water detection debounce */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL3,
				    CCWTRDEB_MASK, CCWTRDEB_1MS <<
				    CCWTRDEB_SHIFT);
	if (ret < 0)
		return ret;

	/* tunable: 1000mV/1000K thershold for water detection */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL3,
				    CCWTRSEL_MASK, CCWTRSEL_1V <<
				    CCWTRSEL_SHIFT);
	if (ret < 0)
		return ret;
	/* Contaminant detection mode: contaminant detection */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL1,
				    CCCONNDRY, 0);
	if (ret < 0)
		return ret;
	ret = max77759_read8(regmap, TCPC_VENDOR_CC_CTRL2, &vcc2);
	if (ret < 0)
		return ret;

	/* tunable: Periodic contaminant detection */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_CC_CTRL2,
				    CCLPMODESEL_MASK,
				    AUTO_ULTRA_LOWER_MODE);
	if (ret < 0)
		return ret;

	ret = max77759_read8(regmap, TCPC_VENDOR_CC_CTRL2, &vcc2);
	if (ret < 0)
		return ret;

	/* Mask flash adc interrupt */
	ret = max77759_update_bits8(regmap, TCPC_VENDOR_ALERT_MASK2,
				    MSK_FLASH_ADCINT, 0);
	if (ret < 0)
		return ret;
	/* Enable Look4Connection before sending the command */
	ret = max77759_update_bits8(regmap, TCPC_TCPC_CTRL,
				    TCPC_TCPC_CTRL_EN_LK4CONN_ALRT,
				    TCPC_TCPC_CTRL_EN_LK4CONN_ALRT);
	if (ret < 0)
		return ret;
	ret = max77759_write8(regmap, TCPC_COMMAND,
			      TCPC_CMD_LOOK4CONNECTION);
	if (ret < 0)
		return ret;
	logbuffer_log(chip->log, "Contaminant: Contaminant detection enabled");

	return 0;
}
EXPORT_SYMBOL_GPL(enable_contaminant_detection);

struct max77759_contaminant *max77759_contaminant_init(struct max77759_plat
							 *plat, bool enable)
{
	struct max77759_contaminant *contaminant;
	struct device *dev = plat->dev;

	contaminant = devm_kzalloc(dev, sizeof(*contaminant), GFP_KERNEL);
	if (!contaminant)
		return ERR_PTR(-ENOMEM);

	contaminant->chip = plat;

	/*
	 * Do not enable in *.ATTACHED state as it would cause an unncessary
	 * disconnect.
	 */
	if (enable)
		enable_contaminant_detection(plat);

	return contaminant;
}
EXPORT_SYMBOL_GPL(max77759_contaminant_init);