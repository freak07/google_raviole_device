// SPDX-License-Identifier: GPL-2.0+
/*
 * drivers/regulator/s2mpg10-regulator.c
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 */

#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <../drivers/pinctrl/samsung/pinctrl-samsung.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/mfd/samsung/s2mpg10.h>
#include <linux/mfd/samsung/s2mpg10-register.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/regulator/pmic_class.h>

static struct regulator_desc regulators[S2MPG10_REGULATOR_MAX];

static unsigned int s2mpg10_of_map_mode(unsigned int val)
{
	switch (val) {
	case SEC_OPMODE_SUSPEND: /* ON in Standby Mode */
		return 0x1;
	case SEC_OPMODE_MIF: /* ON in PWREN_MIF mode */
		return 0x2;
	case SEC_OPMODE_ON: /* ON in Normal Mode */
		return 0x3;
	default:
		return 0x3;
	}
}

static int s2m_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);
	unsigned int val;
	int id = rdev_get_id(rdev);
	int enable_mask = rdev->desc->enable_mask;
	int enable_shift = 0;

	while (1) {
		if (enable_mask & 0x1) {
			break;
		} else {
			enable_shift++;
			enable_mask = enable_mask >> 1;
		}

		if (enable_shift > 7) {
			pr_err("%s [%d]: error caculating enable_shift!\n",
			       __func__, id);
		}
	};

	val = (mode << enable_shift) & rdev->desc->enable_mask;

	s2mpg10->opmode[id] = val;
	return 0;
}

static int s2m_enable(struct regulator_dev *rdev)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);

	return s2mpg10_update_reg(s2mpg10->i2c, rdev->desc->enable_reg,
				  s2mpg10->opmode[rdev_get_id(rdev)],
				  rdev->desc->enable_mask);
}

static int s2m_disable(struct regulator_dev *rdev)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);
	unsigned int val;

	if (rdev->desc->enable_is_inverted)
		val = rdev->desc->enable_mask;
	else
		val = 0;

	return s2mpg10_update_reg(s2mpg10->i2c, rdev->desc->enable_reg, val,
				  rdev->desc->enable_mask);
}

static int s2m_is_enabled(struct regulator_dev *rdev)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);
	int ret;
	u8 val;

	ret = s2mpg10_read_reg(s2mpg10->i2c, rdev->desc->enable_reg, &val);
	if (ret)
		return ret;

	if (rdev->desc->enable_is_inverted)
		return (val & rdev->desc->enable_mask) == 0;
	else
		return (val & rdev->desc->enable_mask) != 0;
}

static int get_ramp_delay(int ramp_delay)
{
	unsigned char cnt = 0;

	ramp_delay /= 6;

	while (true) {
		ramp_delay = ramp_delay >> 1;
		if (ramp_delay == 0)
			break;
		cnt++;
	}
	return cnt;
}

/* function for ramp_up delay only */
static int s2m_set_ramp_delay(struct regulator_dev *rdev, int ramp_delay)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);
	int ramp_shift, reg_id = rdev_get_id(rdev);
	int ramp_mask = 0x03;
	int ramp_addr;
	unsigned int ramp_value = 0;

	if (reg_id >= S2MPG10_LDO1 && reg_id <= S2MPG10_LDO31) {
		pr_info("%s: LDOs don't need ramp delay, id : %d\n", __func__,
			reg_id);
		return 0;
	}
	ramp_value = get_ramp_delay(ramp_delay / 1000);
	if (ramp_value > 4) {
		pr_warn("%s: ramp_delay: %d not supported\n", rdev->desc->name,
			ramp_delay);
	}

	switch (reg_id) {
	case S2MPG10_BUCK4:
	case S2MPG10_BUCK8:
		ramp_shift = 6;
		break;
	case S2MPG10_BUCK3:
	case S2MPG10_BUCK7:
		ramp_shift = 4;
		break;
	case S2MPG10_BUCK2:
	case S2MPG10_BUCK6:
	case S2MPG10_BUCK10:
		ramp_shift = 2;
		break;
	case S2MPG10_BUCK1:
	case S2MPG10_BUCK5:
	case S2MPG10_BUCK9:
		ramp_shift = 0;
		break;
	default:
		return -EINVAL;
	}

	switch (reg_id) {
	case S2MPG10_BUCK1:
	case S2MPG10_BUCK2:
	case S2MPG10_BUCK3:
	case S2MPG10_BUCK4:
		ramp_addr = S2MPG10_PM_DVS_RAMP1;
		break;
	case S2MPG10_BUCK5:
	case S2MPG10_BUCK6:
	case S2MPG10_BUCK7:
	case S2MPG10_BUCK8:
		ramp_addr = S2MPG10_PM_DVS_RAMP2;
		break;
	case S2MPG10_BUCK9:
	case S2MPG10_BUCK10:
		ramp_addr = S2MPG10_PM_DVS_RAMP3;
		break;
	default:
		return -EINVAL;
	}

	return s2mpg10_update_reg(s2mpg10->i2c, ramp_addr,
				  ramp_value << ramp_shift,
				  ramp_mask << ramp_shift);
}

static int s2m_get_voltage_sel(struct regulator_dev *rdev)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);
	int ret;
	u8 val;

	ret = s2mpg10_read_reg(s2mpg10->i2c, rdev->desc->vsel_reg, &val);
	if (ret)
		return ret;

	val &= rdev->desc->vsel_mask;

	return val;
}

static int s2m_set_voltage_sel(struct regulator_dev *rdev,
			       unsigned int sel)
{
	struct s2mpg10_pmic *s2mpg10 = rdev_get_drvdata(rdev);
	int ret;

	ret = s2mpg10_update_reg(s2mpg10->i2c, rdev->desc->vsel_reg, sel,
				 rdev->desc->vsel_mask);
	if (ret < 0)
		goto out;

	if (rdev->desc->apply_bit)
		ret = s2mpg10_update_reg(s2mpg10->i2c, rdev->desc->apply_reg,
					 rdev->desc->apply_bit,
					 rdev->desc->apply_bit);
	return ret;
out:
	pr_warn("%s: failed to set regulator voltage\n", rdev->desc->name);
	ret = -EINVAL;
	return ret;
}

static int s2m_set_voltage_time_sel(struct regulator_dev *rdev,
				    unsigned int old_selector,
				    unsigned int new_selector)
{
	unsigned int ramp_delay = 0;
	int old_volt, new_volt;

	if (rdev->constraints->ramp_delay)
		ramp_delay = rdev->constraints->ramp_delay;
	else if (rdev->desc->ramp_delay)
		ramp_delay = rdev->desc->ramp_delay;

	if (ramp_delay == 0) {
		pr_warn("%s: ramp_delay not set\n", rdev->desc->name);
		return -EINVAL;
	}

	/* sanity check */
	if (!rdev->desc->ops->list_voltage)
		return -EINVAL;

	old_volt = rdev->desc->ops->list_voltage(rdev, old_selector);
	new_volt = rdev->desc->ops->list_voltage(rdev, new_selector);

	if (old_selector < new_selector)
		return DIV_ROUND_UP(new_volt - old_volt, ramp_delay);
	else
		return DIV_ROUND_UP(old_volt - new_volt, ramp_delay);

	return 0;
}

static struct regulator_ops s2mpg10_regulator_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.is_enabled = s2m_is_enabled,
	.enable = s2m_enable,
	.disable = s2m_disable,
	.get_voltage_sel = s2m_get_voltage_sel,
	.set_voltage_sel = s2m_set_voltage_sel,
	.set_voltage_time_sel = s2m_set_voltage_time_sel,
	.set_mode = s2m_set_mode,
	.set_ramp_delay = s2m_set_ramp_delay,
};

#define _BUCK(macro) S2MPG10_BUCK##macro
#define _LDO(macro) S2MPG10_LDO##macro
#define _REG(ctrl) S2MPG10_PM##ctrl
#define _TIME(macro) S2MPG10_ENABLE_TIME##macro
#define _MIN(group) S2MPG10_REG_MIN##group
#define _STEP(group) S2MPG10_REG_STEP##group
#define _N_VOLTAGES(num) S2MPG10_REG_N_VOLTAGES_##num
#define _MASK(num) S2MPG10_REG_ENABLE_MASK##num

#define REG_DESC(_name, _id, g, v, n, e, em, t)                           \
	{                                                                 \
		.name = _name, .id = _id, .ops = &s2mpg10_regulator_ops,  \
		.type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,          \
		.min_uV = _MIN(g), .uV_step = _STEP(g), .n_voltages = n,  \
		.vsel_reg = v, .vsel_mask = n - 1, .enable_reg = e,       \
		.enable_mask = em, .enable_time = t,                      \
		.of_map_mode = s2mpg10_of_map_mode                        \
	}

static struct regulator_desc regulators[S2MPG10_REGULATOR_MAX] = {
	/* name, id, voltage_group, vsel_reg, n_voltages, */
	/* enable_reg, enable_mask, ramp_delay */
	REG_DESC("LDO1M", _LDO(1), 2, _REG(_L1M_CTRL), _N_VOLTAGES(128),
		 _REG(_L1M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO2M", _LDO(2), 4, _REG(_L2M_CTRL), _N_VOLTAGES(64),
		 _REG(_L2M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO3M", _LDO(3), 3, _REG(_L3M_CTRL), _N_VOLTAGES(64),
		 _REG(_L3M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO4M", _LDO(4), 4, _REG(_L4M_CTRL), _N_VOLTAGES(64),
		 _REG(_L4M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO5M", _LDO(5), 3, _REG(_L5M_CTRL), _N_VOLTAGES(64),
		 _REG(_L5M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO6M", _LDO(6), 3, _REG(_L6M_CTRL), _N_VOLTAGES(64),
		 _REG(_L6M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO7M", _LDO(7), 2, _REG(_L7M_CTRL), _N_VOLTAGES(128),
		 _REG(_LDO_CTRL1), _MASK(_4_3), _TIME(_LDO)),
	REG_DESC("LDO8M", _LDO(8), 3, _REG(_L8M_CTRL), _N_VOLTAGES(64),
		 _REG(_L8M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO9M", _LDO(9), 4, _REG(_L9M_CTRL), _N_VOLTAGES(64),
		 _REG(_L9M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO10M", _LDO(10), 5, _REG(_L10M_CTRL), _N_VOLTAGES(64),
		 _REG(_L10M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO11M", _LDO(11), 2, _REG(_L11M_CTRL1), _N_VOLTAGES(128),
		 _REG(_LDO_CTRL2), _MASK(_1_0), _TIME(_LDO)),
	REG_DESC("LDO12M", _LDO(12), 2, _REG(_L12M_CTRL1), _N_VOLTAGES(128),
		 _REG(_LDO_CTRL2), _MASK(_3_2), _TIME(_LDO)),
	REG_DESC("LDO13M", _LDO(13), 2, _REG(_L13M_CTRL1), _N_VOLTAGES(128),
		 _REG(_LDO_CTRL2), _MASK(_5_4), _TIME(_LDO)),
	REG_DESC("LDO14M", _LDO(14), 4, _REG(_L14M_CTRL), _N_VOLTAGES(64),
		 _REG(_L14M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO15M", _LDO(15), 2, _REG(_L15M_CTRL1), _N_VOLTAGES(128),
		 _REG(_LDO_CTRL2), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO16M", _LDO(16), 3, _REG(_L16M_CTRL), _N_VOLTAGES(64),
		 _REG(_L16M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO17M", _LDO(17), 3, _REG(_L17M_CTRL), _N_VOLTAGES(64),
		 _REG(_L17M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO18M", _LDO(18), 4, _REG(_L18M_CTRL), _N_VOLTAGES(64),
		 _REG(_L18M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO19M", _LDO(19), 4, _REG(_L19M_CTRL), _N_VOLTAGES(64),
		 _REG(_L19M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO20M", _LDO(20), 4, _REG(_L20M_CTRL), _N_VOLTAGES(64),
		 _REG(_L20M_CTRL), _MASK(_7_6), _TIME(_LDO)),
	REG_DESC("LDO21M", _LDO(21), 5, _REG(_L21M_CTRL), _N_VOLTAGES(64),
		 _REG(_L21M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO22M", _LDO(22), 5, _REG(_L22M_CTRL), _N_VOLTAGES(64),
		 _REG(_L22M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO23M", _LDO(23), 4, _REG(_L23M_CTRL), _N_VOLTAGES(64),
		 _REG(_L23M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO24M", _LDO(24), 3, _REG(_L24M_CTRL), _N_VOLTAGES(64),
		 _REG(_L24M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO25M", _LDO(25), 4, _REG(_L25M_CTRL), _N_VOLTAGES(64),
		 _REG(_L25M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO26M", _LDO(26), 5, _REG(_L26M_CTRL), _N_VOLTAGES(64),
		 _REG(_L26M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO27M", _LDO(27), 5, _REG(_L27M_CTRL), _N_VOLTAGES(64),
		 _REG(_L27M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO28M", _LDO(28), 3, _REG(_L28M_CTRL), _N_VOLTAGES(64),
		 _REG(_L28M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO29M", _LDO(29), 4, _REG(_L29M_CTRL), _N_VOLTAGES(64),
		 _REG(_L29M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO30M", _LDO(30), 4, _REG(_L30M_CTRL), _N_VOLTAGES(64),
		 _REG(_L30M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("LDO31M", _LDO(31), 4, _REG(_L31M_CTRL), _N_VOLTAGES(64),
		 _REG(_L31M_CTRL), _MASK(_7), _TIME(_LDO)),
	REG_DESC("BUCK1M", _BUCK(1), 1, _REG(_B1M_OUT1), _N_VOLTAGES(256),
		 _REG(_B1M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK2M", _BUCK(2), 1, _REG(_B2M_OUT1), _N_VOLTAGES(256),
		 _REG(_B2M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK3M", _BUCK(3), 1, _REG(_B3M_OUT1), _N_VOLTAGES(256),
		 _REG(_B3M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK4M", _BUCK(4), 1, _REG(_B4M_OUT1), _N_VOLTAGES(256),
		 _REG(_B4M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK5M", _BUCK(5), 1, _REG(_B5M_OUT1), _N_VOLTAGES(256),
		 _REG(_B5M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK6M", _BUCK(6), 1, _REG(_B6M_OUT1), _N_VOLTAGES(256),
		 _REG(_B6M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK7M", _BUCK(7), 1, _REG(_B7M_OUT1), _N_VOLTAGES(256),
		 _REG(_B7M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK8M", _BUCK(8), 1, _REG(_B8M_OUT1), _N_VOLTAGES(256),
		 _REG(_B8M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK9M", _BUCK(9), 1, _REG(_B9M_OUT1), _N_VOLTAGES(256),
		 _REG(_B9M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
	REG_DESC("BUCK10M", _BUCK(10), 1, _REG(_B10M_OUT1), _N_VOLTAGES(256),
		 _REG(_B10M_CTRL), _MASK(_7_6), _TIME(_BUCK)),
};

#if IS_ENABLED(CONFIG_OF)
static int s2mpg10_pmic_dt_parse_pdata(struct s2mpg10_dev *iodev,
				       struct s2mpg10_platform_data *pdata)
{
	struct device_node *pmic_np, *regulators_np, *reg_np;
	struct s2mpg10_regulator_data *rdata;
	unsigned int i;
	int ret;
	u32 val;

	pmic_np = iodev->dev->of_node;
	if (!pmic_np) {
		dev_err(iodev->dev, "could not find pmic sub-node\n");
		return -ENODEV;
	}

	regulators_np = of_find_node_by_name(pmic_np, "regulators");
	if (!regulators_np) {
		dev_err(iodev->dev, "could not find regulators sub-node\n");
		return -EINVAL;
	}

	/* count the number of regulators to be supported in pmic */
	pdata->num_regulators = 0;
	for_each_child_of_node(regulators_np, reg_np) {
		pdata->num_regulators++;
	}

	rdata = devm_kzalloc(iodev->dev, sizeof(*rdata) * pdata->num_regulators,
			     GFP_KERNEL);
	if (!rdata)
		return -ENOMEM;

	pdata->regulators = rdata;
	for_each_child_of_node(regulators_np, reg_np) {
		for (i = 0; i < ARRAY_SIZE(regulators); i++)
			if (!of_node_cmp(reg_np->name, regulators[i].name))
				break;

		if (i == ARRAY_SIZE(regulators)) {
			dev_warn(iodev->dev,
				 "don't know how to configure regulator %s\n",
				 reg_np->name);
			continue;
		}

		rdata->id = i;
		rdata->initdata = of_get_regulator_init_data(iodev->dev, reg_np,
							     &regulators[i]);
		rdata->reg_node = reg_np;
		rdata++;
	}

	if (of_gpio_count(pmic_np) < 1) {
		dev_err(iodev->dev, "could not find pmic gpios\n");
		return -EINVAL;
	}

	/* parse SMPL_WARN information */
	pdata->smpl_warn_pin = of_get_gpio(pmic_np, 0);
	if (pdata->smpl_warn_pin < 0)
		dev_err(iodev->dev, "smpl_warn_pin < 0: %d\n",
			pdata->smpl_warn_pin);

	ret = of_property_read_u32(pmic_np, "smpl_warn_lvl", &val);
	pdata->smpl_warn_lvl = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "smpl_warn_hys", &val);
	pdata->smpl_warn_hys = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "smpl_warn_lbdt", &val);
	pdata->smpl_warn_lbdt = ret ? 0 : val;

	/* parse OCP_WARN information */
	pdata->b2_ocp_warn_pin = of_get_gpio(pmic_np, 2);
	if (pdata->b2_ocp_warn_pin < 0)
		dev_err(iodev->dev, "b2_ocp_warn_pin < 0: %d\n",
			pdata->b2_ocp_warn_pin);

	ret = of_property_read_u32(pmic_np, "b2_ocp_warn_en", &val);
	pdata->b2_ocp_warn_en = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b2_ocp_warn_cnt", &val);
	pdata->b2_ocp_warn_cnt = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b2_ocp_warn_dvs_mask", &val);
	pdata->b2_ocp_warn_dvs_mask = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b2_ocp_warn_lvl", &val);
	pdata->b2_ocp_warn_lvl = ret ? 0 : val;

	pdata->b3_ocp_warn_pin = of_get_gpio(pmic_np, 1);
	if (pdata->b3_ocp_warn_pin < 0)
		dev_err(iodev->dev, "b3_ocp_warn_pin < 0: %d\n",
			pdata->b3_ocp_warn_pin);

	ret = of_property_read_u32(pmic_np, "b3_ocp_warn_en", &val);
	pdata->b3_ocp_warn_en = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b3_ocp_warn_cnt", &val);
	pdata->b3_ocp_warn_cnt = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b3_ocp_warn_dvs_mask", &val);
	pdata->b3_ocp_warn_dvs_mask = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b3_ocp_warn_lvl", &val);
	pdata->b3_ocp_warn_lvl = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b10_ocp_warn_en", &val);
	pdata->b10_ocp_warn_en = ret ? 0 : val;

	pdata->b10_ocp_warn_pin = of_get_gpio(pmic_np, 5);
	if (pdata->b10_ocp_warn_pin < 0)
		dev_err(iodev->dev, "b10_ocp_warn_pin < 0: %d\n",
			pdata->b10_ocp_warn_pin);

	ret = of_property_read_u32(pmic_np, "b10_ocp_warn_cnt", &val);
	pdata->b10_ocp_warn_cnt = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b10_ocp_warn_dvs_mask", &val);
	pdata->b10_ocp_warn_dvs_mask = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b10_ocp_warn_lvl", &val);
	pdata->b10_ocp_warn_lvl = ret ? 0 : val;

	/* parse SOFT_OCP_WARN information */
	pdata->b2_soft_ocp_warn_pin = of_get_gpio(pmic_np, 4);
	if (pdata->b2_soft_ocp_warn_pin < 0)
		dev_err(iodev->dev, "b2_soft_ocp_warn_pin < 0: %d\n",
			pdata->b2_soft_ocp_warn_pin);

	ret = of_property_read_u32(pmic_np, "b2_soft_ocp_warn_en", &val);
	pdata->b2_soft_ocp_warn_en = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b2_soft_ocp_warn_cnt", &val);
	pdata->b2_soft_ocp_warn_cnt = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b2_soft_ocp_warn_dvs_mask", &val);
	pdata->b2_soft_ocp_warn_dvs_mask = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b2_soft_ocp_warn_lvl", &val);
	pdata->b2_soft_ocp_warn_lvl = ret ? 0 : val;

	pdata->b3_soft_ocp_warn_pin = of_get_gpio(pmic_np, 3);
	if (pdata->b3_soft_ocp_warn_pin < 0)
		dev_err(iodev->dev, "b3_soft_ocp_warn_pin < 0: %d\n",
			pdata->b3_soft_ocp_warn_pin);

	ret = of_property_read_u32(pmic_np, "b3_soft_ocp_warn_en", &val);
	pdata->b3_soft_ocp_warn_en = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b3_soft_ocp_warn_cnt", &val);
	pdata->b3_soft_ocp_warn_cnt = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b3_soft_ocp_warn_dvs_mask", &val);
	pdata->b3_soft_ocp_warn_dvs_mask = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b3_soft_ocp_warn_lvl", &val);
	pdata->b3_soft_ocp_warn_lvl = ret ? 0 : val;

	pdata->b10_soft_ocp_warn_pin = of_get_gpio(pmic_np, 6);
	if (pdata->b10_soft_ocp_warn_pin < 0)
		dev_err(iodev->dev, "b10_soft_ocp_warn_pin < 0: %d\n",
			pdata->b10_soft_ocp_warn_pin);

	ret = of_property_read_u32(pmic_np, "b10_soft_ocp_warn_en", &val);
	pdata->b10_soft_ocp_warn_en = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b10_soft_ocp_warn_cnt", &val);
	pdata->b10_soft_ocp_warn_cnt = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b10_soft_ocp_warn_dvs_mask", &val);
	pdata->b10_soft_ocp_warn_dvs_mask = ret ? 0 : val;

	ret = of_property_read_u32(pmic_np, "b10_soft_ocp_warn_lvl", &val);
	pdata->b10_soft_ocp_warn_lvl = ret ? 0 : val;

	return 0;
}
#else
static int s2mpg10_pmic_dt_parse_pdata(struct s2mpg10_dev *iodev,
				       struct s2mpg10_platform_data *pdata)
{
	return 0;
}
#endif /* CONFIG_OF */

#if IS_ENABLED(CONFIG_DRV_SAMSUNG_PMIC)
static ssize_t s2mpg10_pmic_read_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t size)
{
	struct s2mpg10_pmic *s2mpg10 = dev_get_drvdata(dev);
	int ret;
	u8 val, reg_addr;

	if (!buf) {
		pr_info("%s: empty buffer\n", __func__);
		return -1;
	}

	ret = kstrtou8(buf, 0, &reg_addr);
	if (ret < 0)
		pr_info("%s: fail to transform i2c address\n", __func__);

	ret = s2mpg10_read_reg(s2mpg10->i2c, reg_addr, &val);
	if (ret < 0)
		pr_info("%s: fail to read i2c address\n", __func__);

	pr_info("%s: reg(0x%02x) data(0x%02x)\n", __func__, reg_addr, val);
	s2mpg10->read_addr = reg_addr;
	s2mpg10->read_val = val;

	return size;
}

static ssize_t s2mpg10_pmic_read_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct s2mpg10_pmic *s2mpg10 = dev_get_drvdata(dev);

	return sprintf(buf, "0x%02x: 0x%02x\n", s2mpg10->read_addr,
		       s2mpg10->read_val);
}

static ssize_t s2mpg10_pmic_write_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct s2mpg10_pmic *s2mpg10 = dev_get_drvdata(dev);
	int ret;
	u8 reg, data;

	if (!buf) {
		pr_info("%s: empty buffer\n", __func__);
		return size;
	}

	ret = sscanf(buf, "%x %x", &reg, &data);
	if (ret != 2) {
		pr_info("%s: input error\n", __func__);
		return size;
	}

	pr_info("%s: reg(0x%02x) data(0x%02x)\n", __func__, reg, data);

	ret = s2mpg10_write_reg(s2mpg10->i2c, reg, data);
	if (ret < 0)
		pr_info("%s: fail to write i2c addr/data\n", __func__);

	return size;
}

static ssize_t s2mpg10_pmic_write_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "echo (register addr.) (data) > s2mpg10_write\n");
}

static DEVICE_ATTR_RW(s2mpg10_pmic_write);
static DEVICE_ATTR_RW(s2mpg10_pmic_read);

int create_s2mpg10_pmic_sysfs(struct s2mpg10_pmic *s2mpg10)
{
	struct device *s2mpg10_pmic = s2mpg10->dev;
	int err = -ENODEV;

	pr_info("%s: master pmic sysfs start\n", __func__);
	s2mpg10->read_addr = 0;
	s2mpg10->read_val = 0;

	s2mpg10_pmic = pmic_device_create(s2mpg10, "s2mpg10-pmic");

	err = device_create_file(s2mpg10_pmic, &dev_attr_s2mpg10_pmic_write);
	if (err) {
		pr_err("s2mpg10_sysfs: failed to create device file, %s\n",
		       dev_attr_s2mpg10_pmic_write.attr.name);
	}

	err = device_create_file(s2mpg10_pmic, &dev_attr_s2mpg10_pmic_read);
	if (err) {
		pr_err("s2mpg10_sysfs: failed to create device file, %s\n",
		       dev_attr_s2mpg10_pmic_read.attr.name);
	}

	return 0;
}
#endif

static irqreturn_t s2mpg10_cpu1_ocp_warn_irq_handler(int irq, void *data)
{
	pr_info_ratelimited("OCP : CPU1 IRQ : %d triggered\n", irq);
	return IRQ_HANDLED;
}

static irqreturn_t s2mpg10_soft_cpu1_ocp_warn_irq_handler(int irq, void *data)
{
	pr_info_ratelimited("OCP : SOFT CPU1 IRQ : %d triggered\n", irq);
	return IRQ_HANDLED;
}

static irqreturn_t s2mpg10_cpu2_ocp_warn_irq_handler(int irq, void *data)
{
	pr_info_ratelimited("OCP : CPU2 IRQ : %d triggered\n", irq);
	return IRQ_HANDLED;
}

static irqreturn_t s2mpg10_soft_cpu2_ocp_warn_irq_handler(int irq, void *data)
{
	pr_info_ratelimited("OCP : SOFT CPU2 IRQ : %d triggered\n", irq);
	return IRQ_HANDLED;
}

static irqreturn_t s2mpg10_tpu_ocp_warn_irq_handler(int irq, void *data)
{
	pr_info_ratelimited("OCP : TPU IRQ : %d triggered\n", irq);
	return IRQ_HANDLED;
}

static irqreturn_t s2mpg10_soft_tpu_ocp_warn_irq_handler(int irq, void *data)
{
	pr_info_ratelimited("OCP : SOFT TPU IRQ : %d triggered\n", irq);
	return IRQ_HANDLED;
}

static irqreturn_t s2mpg10_buck_ocp_irq(int irq, void *data)
{
	struct s2mpg10_pmic *s2mpg10 = data;
	int i;

	mutex_lock(&s2mpg10->lock);

	for (i = 0; i < 12; i++) {
		if (s2mpg10->buck_ocp_irq[i] == irq) {
			pr_info_ratelimited("%s : BUCK[%d] OCP IRQ, %d\n",
					    __func__, i + 1, irq);
			break;
		}
	}

	mutex_unlock(&s2mpg10->lock);
	return IRQ_HANDLED;
}

int s2mpg10_smpl_warn(struct s2mpg10_pmic *s2mpg10,
		      struct s2mpg10_platform_data *pdata)
{
	u8 val;
	int ret;

	val = (pdata->smpl_warn_lbdt << S2MPG10_SMPL_WARN_LBDT_SHIFT) |
	      (pdata->smpl_warn_hys << S2MPG10_SMPL_WARN_HYS_SHIFT) |
	      (pdata->smpl_warn_lvl << S2MPG10_SMPL_WARN_LVL_SHIFT);

	pr_info("SMPL_WARN_CTRL : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_SMPL_WARN_CTRL, val);
	if (ret)
		pr_err("i2c write error setting smpl_warn\n");

	return ret;
}

void s2mpg10_ocp_warn(struct s2mpg10_pmic *s2mpg10,
		      struct s2mpg10_platform_data *pdata)
{
	u8 val;
	int ret;

	val = (pdata->b2_ocp_warn_en << S2MPG10_OCP_WARN_EN_SHIFT) |
	      (pdata->b2_ocp_warn_cnt << S2MPG10_OCP_WARN_CNT_SHIFT) |
	      (pdata->b2_ocp_warn_dvs_mask << S2MPG10_OCP_WARN_DVS_MASK_SHIFT) |
	      (pdata->b2_ocp_warn_lvl << S2MPG10_OCP_WARN_LVL_SHIFT);

	pr_info("B2M_OCP_WARN : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_B2M_OCP_WARN, val);
	if (ret)
		pr_err("i2c write error setting b2m_ocp_warn\n");

	val = (pdata->b3_ocp_warn_en << S2MPG10_OCP_WARN_EN_SHIFT) |
	      (pdata->b3_ocp_warn_cnt << S2MPG10_OCP_WARN_CNT_SHIFT) |
	      (pdata->b3_ocp_warn_dvs_mask << S2MPG10_OCP_WARN_DVS_MASK_SHIFT) |
	      (pdata->b3_ocp_warn_lvl << S2MPG10_OCP_WARN_LVL_SHIFT);

	pr_info("B3M_OCP_WARN : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_B3M_OCP_WARN, val);
	if (ret)
		pr_err("i2c write error setting b3m_ocp_warn\n");

	val = (pdata->b10_ocp_warn_en << S2MPG10_OCP_WARN_EN_SHIFT) |
	      (pdata->b10_ocp_warn_cnt << S2MPG10_OCP_WARN_CNT_SHIFT) |
	      (pdata->b10_ocp_warn_dvs_mask
	       << S2MPG10_OCP_WARN_DVS_MASK_SHIFT) |
	      (pdata->b10_ocp_warn_lvl << S2MPG10_OCP_WARN_LVL_SHIFT);

	pr_info("B10M_OCP_WARN : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_B10M_OCP_WARN, val);
	if (ret)
		pr_err("i2c write error setting b10m_ocp_warn\n");

	val = (pdata->b2_soft_ocp_warn_en << S2MPG10_OCP_WARN_EN_SHIFT) |
	      (pdata->b2_soft_ocp_warn_cnt << S2MPG10_OCP_WARN_CNT_SHIFT) |
	      (pdata->b2_soft_ocp_warn_dvs_mask
	       << S2MPG10_OCP_WARN_DVS_MASK_SHIFT) |
	      (pdata->b2_soft_ocp_warn_lvl << S2MPG10_OCP_WARN_LVL_SHIFT);

	pr_info("B2M_SOFT_OCP_WARN : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_B2M_SOFT_OCP_WARN,
				val);
	if (ret)
		pr_err("i2c write error setting b2m_soft_ocp_warn\n");

	val = (pdata->b3_soft_ocp_warn_en << S2MPG10_OCP_WARN_EN_SHIFT) |
	      (pdata->b3_soft_ocp_warn_cnt << S2MPG10_OCP_WARN_CNT_SHIFT) |
	      (pdata->b3_soft_ocp_warn_dvs_mask
	       << S2MPG10_OCP_WARN_DVS_MASK_SHIFT) |
	      (pdata->b3_soft_ocp_warn_lvl << S2MPG10_OCP_WARN_LVL_SHIFT);

	pr_info("B3M_SOFT_OCP_WARN : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_B3M_SOFT_OCP_WARN,
				val);
	if (ret)
		pr_err("i2c write error setting b3m_soft_ocp_warn\n");

	val = (pdata->b10_ocp_warn_en << S2MPG10_OCP_WARN_EN_SHIFT) |
	      (pdata->b10_soft_ocp_warn_cnt << S2MPG10_OCP_WARN_CNT_SHIFT) |
	      (pdata->b10_soft_ocp_warn_dvs_mask
	       << S2MPG10_OCP_WARN_DVS_MASK_SHIFT) |
	      (pdata->b10_soft_ocp_warn_lvl << S2MPG10_OCP_WARN_LVL_SHIFT);

	pr_info("B10M_SOFT_OCP_WARN : 0x%x\n", val);
	ret = s2mpg10_write_reg(s2mpg10->i2c, S2MPG10_PM_B10M_SOFT_OCP_WARN,
				val);
	if (ret)
		pr_err("i2c write error setting b10m_soft_ocp_warn\n");
}

void s2mpg10_oi_function(struct s2mpg10_pmic *s2mpg10)
{
	/* add OI configuration code if necessary */

	/* OI function enable */

	/* OI power down disable */

	/* OI detection time window : 500us, OI comp. output count : 50 times */
}

static int s2mpg10_pmic_probe(struct platform_device *pdev)
{
	struct s2mpg10_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct s2mpg10_platform_data *pdata = iodev->pdata;
	struct regulator_config config = {};
	struct s2mpg10_pmic *s2mpg10;
	int irq_base;
	int i, ret;

	if (iodev->dev->of_node) {
		ret = s2mpg10_pmic_dt_parse_pdata(iodev, pdata);
		if (ret)
			return ret;
	}

	if (!pdata) {
		dev_err(pdev->dev.parent, "Platform data not supplied\n");
		return -ENODEV;
	}

	s2mpg10 = devm_kzalloc(&pdev->dev, sizeof(struct s2mpg10_pmic),
			       GFP_KERNEL);
	if (!s2mpg10)
		return -ENOMEM;

	irq_base = pdata->irq_base;
	if (!irq_base) {
		dev_err(&pdev->dev, "Failed to get irq base %d\n", irq_base);
		return -ENODEV;
	}

	s2mpg10->rdev = devm_kzalloc(&pdev->dev,
				     sizeof(struct regulator_dev *) *
					     S2MPG10_REGULATOR_MAX,
				     GFP_KERNEL);
	s2mpg10->opmode =
		devm_kzalloc(&pdev->dev,
			     sizeof(unsigned int) * S2MPG10_REGULATOR_MAX,
			     GFP_KERNEL);
	s2mpg10->buck_ocp_irq = devm_kzalloc(&pdev->dev,
					     sizeof(int) * S2MPG10_BUCK_MAX,
					     GFP_KERNEL);

	s2mpg10->iodev = iodev;
	s2mpg10->i2c = iodev->pmic;

	mutex_init(&s2mpg10->lock);
	platform_set_drvdata(pdev, s2mpg10);

	for (i = 0; i < pdata->num_regulators; i++) {
		int id = pdata->regulators[i].id;

		config.dev = &pdev->dev;
		config.init_data = pdata->regulators[i].initdata;
		config.driver_data = s2mpg10;
		config.of_node = pdata->regulators[i].reg_node;
		s2mpg10->opmode[id] = regulators[id].enable_mask;

		s2mpg10->rdev[i] = regulator_register(&regulators[id], &config);
		if (IS_ERR(s2mpg10->rdev[i])) {
			ret = PTR_ERR(s2mpg10->rdev[i]);
			dev_err(&pdev->dev, "regulator init failed for %d\n",
				i);
			s2mpg10->rdev[i] = NULL;
			goto err;
		}
	}

	s2mpg10->num_regulators = pdata->num_regulators;

	/* request IRQ */
	for (i = 0; i < S2MPG10_BUCK_MAX; i++) {
		s2mpg10->buck_ocp_irq[i] =
			irq_base + S2MPG10_IRQ_OCP_B1M_INT4 + i;

		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->buck_ocp_irq[i], NULL,
						s2mpg10_buck_ocp_irq, 0,
						"BUCK_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request BUCK[%d] OCP IRQ: %d: %d\n",
				i + 1, s2mpg10->buck_ocp_irq[i], ret);
		}
	}

	if (s2mpg10->cpu1_ocp_warn_irq >= 0) {
		s2mpg10->cpu1_ocp_warn_irq =
				gpio_to_irq(pdata->b3_ocp_warn_pin);
		irq_set_status_flags(s2mpg10->cpu1_ocp_warn_irq,
				     IRQ_DISABLE_UNLAZY);
		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->cpu1_ocp_warn_irq,
						NULL,
						s2mpg10_cpu1_ocp_warn_irq_handler,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"CPU1_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request CPU1 OCP IRQ: %d: %d\n",
				s2mpg10->cpu1_ocp_warn_irq, ret);
		}
	}

	if (s2mpg10->soft_cpu1_ocp_warn_irq >= 0) {
		s2mpg10->soft_cpu1_ocp_warn_irq =
				gpio_to_irq(pdata->b3_soft_ocp_warn_pin);
		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->soft_cpu1_ocp_warn_irq,
						NULL,
						s2mpg10_soft_cpu1_ocp_warn_irq_handler,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"SOFT_CPU1_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request SOFT CPU1 OCP IRQ: %d: %d\n",
				s2mpg10->soft_cpu1_ocp_warn_irq, ret);
		}
	}

	if (s2mpg10->cpu2_ocp_warn_irq >= 0) {
		s2mpg10->cpu2_ocp_warn_irq =
				gpio_to_irq(pdata->b2_ocp_warn_pin);
		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->cpu2_ocp_warn_irq,
						NULL,
						s2mpg10_cpu2_ocp_warn_irq_handler,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"CPU2_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request CPU2 OCP IRQ: %d: %d\n",
				s2mpg10->cpu2_ocp_warn_irq, ret);
		}
	}

	if (s2mpg10->soft_cpu2_ocp_warn_irq >= 0) {
		s2mpg10->soft_cpu2_ocp_warn_irq =
				gpio_to_irq(pdata->b2_soft_ocp_warn_pin);
		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->soft_cpu2_ocp_warn_irq,
						NULL,
						s2mpg10_soft_cpu2_ocp_warn_irq_handler,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"SOFT_CPU2_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request SOFT CPU2 OCP IRQ: %d: %d\n",
				s2mpg10->soft_cpu2_ocp_warn_irq, ret);
		}
	}

	if (s2mpg10->tpu_ocp_warn_irq >= 0) {
		s2mpg10->tpu_ocp_warn_irq =
				gpio_to_irq(pdata->b10_ocp_warn_pin);
		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->tpu_ocp_warn_irq,
						NULL,
						s2mpg10_tpu_ocp_warn_irq_handler,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"TPU_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request TPU OCP IRQ: %d: %d\n",
				s2mpg10->tpu_ocp_warn_irq, ret);
		}
	}

	if (s2mpg10->soft_tpu_ocp_warn_irq >= 0) {
		s2mpg10->soft_tpu_ocp_warn_irq =
				gpio_to_irq(pdata->b10_soft_ocp_warn_pin);
		ret = devm_request_threaded_irq(&pdev->dev,
						s2mpg10->soft_tpu_ocp_warn_irq,
						NULL,
						s2mpg10_soft_tpu_ocp_warn_irq_handler,
						IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
						"SOFT_TPU_OCP_IRQ", s2mpg10);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to request SOFT TPU OCP IRQ: %d: %d\n",
				s2mpg10->soft_tpu_ocp_warn_irq, ret);
		}
	}
	s2mpg10_smpl_warn(s2mpg10, pdata);
	s2mpg10_ocp_warn(s2mpg10, pdata);
	s2mpg10_oi_function(s2mpg10);

#if IS_ENABLED(CONFIG_DRV_SAMSUNG_PMIC)
	/* create sysfs */
	ret = create_s2mpg10_pmic_sysfs(s2mpg10);
	if (ret < 0)
		return ret;
#endif

	/* DCTRLSEL config for LDO15M */
	s2mpg10_update_reg(s2mpg10->i2c, S2MPG10_PM_DCTRLSEL7,
			   DCTRLSEL_AP_ACTIVE_N << 4, GENMASK(7, 4));

	return 0;
err:
	for (i = 0; i < S2MPG10_REGULATOR_MAX; i++)
		regulator_unregister(s2mpg10->rdev[i]);

	return ret;
}

static int s2mpg10_pmic_remove(struct platform_device *pdev)
{
	struct s2mpg10_pmic *s2mpg10 = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < S2MPG10_REGULATOR_MAX; i++)
		regulator_unregister(s2mpg10->rdev[i]);

#if IS_ENABLED(CONFIG_DRV_SAMSUNG_PMIC)
	pmic_device_destroy(s2mpg10->dev->devt);
#endif
	return 0;
}

static void s2mpg10_pmic_shutdown(struct platform_device *pdev)
{
}

#if IS_ENABLED(CONFIG_PM)
static int s2mpg10_pmic_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s2mpg10_pmic *s2mpg10 = platform_get_drvdata(pdev);
	int ret;

	ret = s2mpg10_update_reg(s2mpg10->i2c, S2MPG10_PM_DCTRLSEL7,
				 DCTRLSEL_PWREN_MIF << 4, GENMASK(7, 4));

	return ret;
}

static int s2mpg10_pmic_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct s2mpg10_pmic *s2mpg10 = platform_get_drvdata(pdev);
	int ret;

	ret = s2mpg10_update_reg(s2mpg10->i2c, S2MPG10_PM_DCTRLSEL7,
				 DCTRLSEL_AP_ACTIVE_N << 4, GENMASK(7, 4));

	return ret;
}
#else
#define s2mpg10_pmic_suspend NULL
#define s2mpg10_pmic_resume NULL
#endif /* CONFIG_PM */

const struct dev_pm_ops s2mpg10_pmic_pm = {
	.suspend = s2mpg10_pmic_suspend,
	.resume = s2mpg10_pmic_resume,
};

static const struct platform_device_id s2mpg10_pmic_id[] = {
	{ "s2mpg10-regulator", 0 },
	{},
};

MODULE_DEVICE_TABLE(platform, s2mpg10_pmic_id);

static struct platform_driver s2mpg10_pmic_driver = {
	.driver = {
		   .name = "s2mpg10-regulator",
		   .owner = THIS_MODULE,
#if IS_ENABLED(CONFIG_PM)
		   .pm = &s2mpg10_pmic_pm,
#endif
		   .suppress_bind_attrs = true,
		    },
	.probe = s2mpg10_pmic_probe,
	.remove = s2mpg10_pmic_remove,
	.shutdown = s2mpg10_pmic_shutdown,
	.id_table = s2mpg10_pmic_id,
};

static int __init s2mpg10_pmic_init(void)
{
	return platform_driver_register(&s2mpg10_pmic_driver);
}

subsys_initcall(s2mpg10_pmic_init);

static void __exit s2mpg10_pmic_exit(void)
{
	platform_driver_unregister(&s2mpg10_pmic_driver);
}

module_exit(s2mpg10_pmic_exit);

/* Module information */
MODULE_AUTHOR("Sangbeom Kim <sbkim73@samsung.com>");
MODULE_DESCRIPTION("SAMSUNG S2MPG10 Regulator Driver");
MODULE_LICENSE("GPL");