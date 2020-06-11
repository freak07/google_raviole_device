/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * include/linux/mfd/samsung/s2mpg10-meter.h
 *
 * Copyright (C) 2015 Samsung Electronics
 *
 * header file including meter information of s2mpg10
 */

#ifndef __LINUX_MFD_S2MPG10_METER_H
#define __LINUX_MFD_S2MPG10_METER_H

#include "s2mpg10-register.h"

struct s2mpg10_meter {
	struct i2c_client *i2c;

	/* mutex for s2mpg10 meter */
	struct mutex meter_lock;
	u8 meter_en;
	u8 ext_meter_en;
	u8 chg_mux_sel[S2MPG1X_METER_CHANNEL_MAX];
	u32 lpf_data[S2MPG1X_METER_CHANNEL_MAX]; /* 21-bit data */
#if IS_ENABLED(CONFIG_DRV_SAMSUNG_PMIC)
	struct device *dev;
#endif
};

/* Public s2mpg10 Meter functions */
int s2mpg10_meter_load_measurement(struct s2mpg10_meter *s2mpg10,
				   s2mpg1x_meter_mode mode, u64 *data,
				   u32 *count, unsigned long *jiffies_capture);
int s2mpg10_meter_set_muxsel(struct s2mpg10_meter *s2mpg10, int channel,
			     s2mpg10_meter_muxsel m);
int s2mpg10_set_int_samp_rate(struct s2mpg10_meter *s2mpg10,
			      s2mpg1x_int_samp_rate hz);
int s2mpg10_set_ext_samp_rate(struct s2mpg10_meter *s2mpg10,
			      s2mpg1x_ext_samp_rate hz);

int s2mpg10_meter_onoff(struct s2mpg10_meter *s2mpg10, bool onoff);
int s2mpg10_ext_meter_onoff(struct s2mpg10_meter *s2mpg10, bool onoff);
u32 s2mpg10_muxsel_to_power_resolution(s2mpg10_meter_muxsel m);
int s2mpg10_meter_ext_channel_onoff(struct s2mpg10_meter *s2mpg10, u8 channels);
int s2mpg10_meter_set_async_blocking(struct s2mpg10_meter *s2mpg10,
				     unsigned long *jiffies_capture);

#endif /* __LINUX_MFD_S2MPG10_METER_H */