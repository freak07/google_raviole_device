// SPDX-License-Identifier: GPL-2.0-only
/*
 * Platform device driver for the Google GSA core.
 *
 * Copyright (C) 2020 Google LLC
 */
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/gsa/gsa_tpu.h>
#include "gsa_mbox.h"
#include "gsa_priv.h"

struct gsa_dev_state {
	struct device *dev;
	struct gsa_mbox *mb;
};

/*
 *  Internal command interface
 */
int gsa_send_cmd(struct device *dev, u32 cmd, u32 *req, u32 req_argc,
		 u32 *rsp, u32 rsp_argc)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gsa_dev_state *s = platform_get_drvdata(pdev);

	return gsa_send_mbox_cmd(s->mb, cmd, req, req_argc, rsp, rsp_argc);
};
EXPORT_SYMBOL_GPL(gsa_send_cmd);

int gsa_send_simple_cmd(struct device *dev, u32 cmd)
{
	return gsa_send_cmd(dev, cmd, NULL, 0, NULL, 0);
}
EXPORT_SYMBOL_GPL(gsa_send_simple_cmd);

int gsa_send_one_arg_cmd(struct device *dev, u32 cmd, u32 arg)
{
	return gsa_send_cmd(dev, cmd, &arg, 1, NULL, 0);
}
EXPORT_SYMBOL_GPL(gsa_send_one_arg_cmd);

static int gsa_send_load_img_cmd(struct device *dev, uint32_t cmd,
				 dma_addr_t hdr_da, phys_addr_t body_pa)
{
	u32 req[4];
	struct platform_device *pdev = to_platform_device(dev);
	struct gsa_dev_state *s = platform_get_drvdata(pdev);

	req[IMG_LOADER_HEADER_ADDR_LO_IDX] = (u32)hdr_da;
	req[IMG_LOADER_HEADER_ADDR_HI_IDX] = (u32)(hdr_da >> 32);
	req[IMG_LOADER_BODY_ADDR_LO_IDX] = (u32)body_pa;
	req[IMG_LOADER_BODY_ADDR_HI_IDX] = (u32)(body_pa >> 32);

	return gsa_send_mbox_cmd(s->mb, cmd, req, 4, NULL, 0);
}

/*
 *  External TPU interface
 */
int gsa_load_tpu_fw_image(struct device *gsa,
			  dma_addr_t img_meta,
			  phys_addr_t img_body)
{
	return gsa_send_load_img_cmd(gsa, GSA_MB_CMD_LOAD_TPU_FW_IMG,
				     img_meta, img_body);
}
EXPORT_SYMBOL_GPL(gsa_load_tpu_fw_image);

int gsa_unload_tpu_fw_image(struct device *gsa)
{
	return gsa_send_simple_cmd(gsa, GSA_MB_CMD_UNLOAD_TPU_FW_IMG);
}
EXPORT_SYMBOL_GPL(gsa_unload_tpu_fw_image);

int gsa_send_tpu_cmd(struct device *dev, enum gsa_tpu_cmd arg)
{
	int ret;
	u32 tpu_state;
	u32 cmd_arg = arg;

	ret = gsa_send_cmd(dev, GSA_MB_CMD_TPU_CMD,
			   &cmd_arg, 1, &tpu_state, 1);
	if (ret < 0)
		return ret;

	/* check arg count */
	if (ret < 1)
		return -EINVAL;

	return tpu_state;
}
EXPORT_SYMBOL_GPL(gsa_send_tpu_cmd);

/********************************************************************/

static int gsa_probe(struct platform_device *pdev)
{
	int err;
	struct gsa_dev_state *s;
	struct device *dev = &pdev->dev;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = dev;
	platform_set_drvdata(pdev, s);

	/*
	 * Set DMA mask and coherent to 32-bit as ATM we cannot use sysmmu.
	 * The main problem is that GSA can sleep independently from Main AP
	 * and if GSA is in sleep sysmmu block is powered off.
	 */
	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err) {
		dev_err(dev, "failed (%d) to setup dma mask\n", err);
		return err;
	}

	/* initialize mailbox */
	s->mb = gsa_mbox_init(pdev);
	if (IS_ERR(s->mb))
		return -ENOMEM;

	/* add children */
	err = devm_of_platform_populate(dev);
	if (err < 0) {
		dev_err(dev, "populate children failed (%d)\n", err);
		return err;
	}

	dev_info(dev, "Initialized\n");

	return 0;
}

static int gsa_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id gsa_of_match[] = {
	{ .compatible = "google,gs101-gsa-v1", },
	{},
};
MODULE_DEVICE_TABLE(of, gsa_of_match);

static struct platform_driver gsa_driver = {
	.probe = gsa_probe,
	.remove = gsa_remove,
	.driver	= {
		.name = "gsa",
		.of_match_table = gsa_of_match,
	},
};

static int __init gsa_driver_init(void)
{
	return platform_driver_register(&gsa_driver);
}

static void __exit gsa_driver_exit(void)
{
	platform_driver_unregister(&gsa_driver);
}

MODULE_DESCRIPTION("Google GSA core platform driver");
MODULE_LICENSE("GPL v2");
module_init(gsa_driver_init);
module_exit(gsa_driver_exit);