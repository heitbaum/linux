// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cadence Display Port Interface (DP) driver
 *
 * Copyright (C) 2023-2024 NXP Semiconductor, Inc.
 *
 */
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>

#include "cdns-mhdp8501-core.h"

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf);
static struct device_attribute firmware_version = __ATTR_RO(firmware_version);

ssize_t firmware_version_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct cdns_mhdp8501_device *mhdp = dev_get_drvdata(dev);

	u32 version = readl(mhdp->base.regs + VER_L) | readl(mhdp->base.regs + VER_H) << 8;
	u32 lib_version = readl(mhdp->base.regs + VER_LIB_L_ADDR) |
			  readl(mhdp->base.regs + VER_LIB_H_ADDR) << 8;

	return sprintf(buf, "FW version %d, Lib version %d\n", version, lib_version);
}

static void cdns_mhdp8501_create_device_files(struct cdns_mhdp8501_device *mhdp)
{
	if (device_create_file(mhdp->dev, &firmware_version)) {
		DRM_ERROR("Unable to create firmware_version sysfs\n");
		device_remove_file(mhdp->dev, &firmware_version);
	}
}

static void cdns_mhdp8501_remove_device_files(struct cdns_mhdp8501_device *mhdp)
{
	device_remove_file(mhdp->dev, &firmware_version);
}

static int cdns_mhdp8501_read_hpd(struct cdns_mhdp8501_device *mhdp)
{
	u8 status;
	int ret;

	ret = cdns_mhdp_mailbox_send_recv(&mhdp->base, MB_MODULE_ID_GENERAL,
					  GENERAL_GET_HPD_STATE,
					  0, NULL, sizeof(status), &status);
	if (ret) {
		dev_err(mhdp->dev, "read hpd failed: %d\n", ret);
		return ret;
	}

	return status;
}

enum drm_connector_status cdns_mhdp8501_detect(struct drm_bridge *bridge, struct drm_connector *connector)
{
	struct cdns_mhdp8501_device *mhdp = bridge->driver_private;

	u8 hpd = 0xf;

	hpd = cdns_mhdp8501_read_hpd(mhdp);
	if (hpd == 1)
		return connector_status_connected;
	else if (hpd == 0)
		return connector_status_disconnected;

	return connector_status_unknown;
}

enum drm_mode_status
cdns_mhdp8501_mode_valid(struct drm_bridge *bridge,
			 const struct drm_display_info *info,
			 const struct drm_display_mode *mode)
{
	/* We don't support double-clocked */
	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		return MODE_BAD;

	/* MAX support pixel clock rate 594MHz */
	if (mode->clock > 594000)
		return MODE_CLOCK_HIGH;

	if (mode->hdisplay > 3840)
		return MODE_BAD_HVALUE;

	if (mode->vdisplay > 2160)
		return MODE_BAD_VVALUE;

	return MODE_OK;
}

static void hotplug_work_func(struct work_struct *work)
{
	struct cdns_mhdp8501_device *mhdp = container_of(work,
						     struct cdns_mhdp8501_device,
						     hotplug_work.work);
	enum drm_connector_status status = cdns_mhdp8501_detect(&mhdp->bridge, mhdp->curr_conn);

	drm_bridge_hpd_notify(&mhdp->bridge, status);

	/*
	 * iMX8MQ has two HPD interrupts: one for plugout and one for plugin.
	 * These interrupts cannot be masked and cleaned, so we must enable one
	 * and disable the other to avoid continuous interrupt generation.
	 */
	if (status == connector_status_connected) {
		/* Cable connected  */
		dev_dbg(mhdp->dev, "HDMI/DP Cable Plug In\n");
		enable_irq(mhdp->irq[IRQ_OUT]);

		/* Reset HDMI/DP link with sink */
		if (mhdp->connector_type == DRM_MODE_CONNECTOR_HDMIA)
			cdns_hdmi_handle_hotplug(mhdp);
		else
			cdns_dp_check_link_state(mhdp);

	} else if (status == connector_status_disconnected) {
		/* Cable Disconnected  */
		dev_dbg(mhdp->dev, "HDMI/DP Cable Plug Out\n");
		enable_irq(mhdp->irq[IRQ_IN]);
	}
}

static irqreturn_t cdns_mhdp8501_irq_thread(int irq, void *data)
{
	struct cdns_mhdp8501_device *mhdp = data;

	disable_irq_nosync(irq);

	mod_delayed_work(system_percpu_wq, &mhdp->hotplug_work,
			 msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));

	return IRQ_HANDLED;
}

#define DATA_LANES_COUNT	4
static int cdns_mhdp8501_dt_parse(struct cdns_mhdp8501_device *mhdp,
				  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *remote, *endpoint;
	u32 data_lanes[DATA_LANES_COUNT];
	u32 lane_value;
	int ret, i;

	remote = of_graph_get_remote_node(np, 1, 0);
	if (!remote) {
		dev_err(dev, "fail to get remote node\n");
		of_node_put(remote);
		return -EINVAL;
	}

	/* get connector type */
	if (of_device_is_compatible(remote, "hdmi-connector")) {
		mhdp->connector_type = DRM_MODE_CONNECTOR_HDMIA;

	} else if (of_device_is_compatible(remote, "dp-connector")) {
		mhdp->connector_type = DRM_MODE_CONNECTOR_DisplayPort;

	} else {
		dev_err(dev, "Unknown connector type\n");
		of_node_put(remote);
		return -EINVAL;
	}

	of_node_put(remote);

	endpoint = of_graph_get_endpoint_by_regs(np, 1, -1);

	/* Get the data lanes ordering */
	ret = of_property_count_u32_elems(endpoint, "data-lanes");
	if (ret < 0)
		return  -EINVAL;
	if (ret != DATA_LANES_COUNT) {
		dev_err(dev, "expected 4 data lanes\n");
		return  -EINVAL;
	}

	ret = of_property_read_u32_array(endpoint, "data-lanes",
					 data_lanes, DATA_LANES_COUNT);
	if (ret)
		return  -EINVAL;

	mhdp->lane_mapping  = 0;
	for (i = 0; i < DATA_LANES_COUNT; i++) {
		lane_value = (data_lanes[i] >= 0 && data_lanes[i] <= 3) ? data_lanes[i] : 0;
		mhdp->lane_mapping |= lane_value << (i * 2);
	}

	return true;
}

static int cdns_mhdp8501_get_connector_type(struct device *dev)
{
	struct device_node *remote;
	int type;

	remote = of_graph_get_remote_node(dev->of_node, 1, 0);
	if (!remote) {
		dev_err(dev, "fail to get remote node\n");
		return -EINVAL;
	}

	if (of_device_is_compatible(remote, "hdmi-connector")) {
		type = DRM_MODE_CONNECTOR_HDMIA;
	} else if (of_device_is_compatible(remote, "dp-connector")) {
		type = DRM_MODE_CONNECTOR_DisplayPort;
	} else {
		dev_err(dev, "Unknown connector type\n");
		type = -EINVAL;
	}

	of_node_put(remote);

	return type;
}

static int cdns_mhdp8501_add_bridge(struct cdns_mhdp8501_device *mhdp)
{
	mhdp->bridge.type = mhdp->connector_type;
	mhdp->bridge.driver_private = mhdp;
	mhdp->bridge.of_node = mhdp->dev->of_node;
	mhdp->bridge.vendor = "NXP";
	mhdp->bridge.product = "i.MX8";
	mhdp->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID |
			   DRM_BRIDGE_OP_HPD;

	/* bridge.funcs was set by devm_drm_bridge_alloc() in probe */
	if (mhdp->connector_type == DRM_MODE_CONNECTOR_HDMIA) {
		mhdp->bridge.ops |= DRM_BRIDGE_OP_HDMI | DRM_BRIDGE_OP_HDMI_SPD_INFOFRAME;
		mhdp->bridge.ddc = cdns_hdmi_i2c_adapter(mhdp);
	}

	drm_bridge_add(&mhdp->bridge);

	return 0;
}

static int cdns_mhdp8501_probe(struct platform_device *pdev)
{
	const struct drm_bridge_funcs *funcs;
	struct device *dev = &pdev->dev;
	struct cdns_mhdp8501_device *mhdp;
	struct resource *res;
	enum phy_mode phy_mode;
	int connector_type;
	u32 reg;
	int ret;

	/*
	 * drm_bridge_add() requires a refcounted bridge, so the container has
	 * to come from devm_drm_bridge_alloc(). That needs the funcs up front,
	 * so resolve the connector type before allocating.
	 */
	connector_type = cdns_mhdp8501_get_connector_type(dev);
	if (connector_type < 0)
		return connector_type;

	if (connector_type == DRM_MODE_CONNECTOR_HDMIA)
		funcs = &cdns_hdmi_bridge_funcs;
	else
		funcs = &cdns_dp_bridge_funcs;

	mhdp = devm_drm_bridge_alloc(dev, struct cdns_mhdp8501_device, bridge,
				     funcs);
	if (IS_ERR(mhdp))
		return PTR_ERR(mhdp);

	mhdp->dev = dev;
	mhdp->connector_type = connector_type;

	INIT_DELAYED_WORK(&mhdp->hotplug_work, hotplug_work_func);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	mhdp->regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(mhdp->regs))
		return PTR_ERR(mhdp->regs);

	cdns_mhdp8501_create_device_files(mhdp);

	ret = cdns_mhdp8501_dt_parse(mhdp, pdev);
	if (ret < 0)
		return -EINVAL;

	mhdp->phy = devm_of_phy_get_by_index(dev, pdev->dev.of_node, 0);
	if (IS_ERR(mhdp->phy))
		return dev_err_probe(dev, PTR_ERR(mhdp->phy), "no PHY configured\n");

	mhdp->irq[IRQ_IN] = platform_get_irq_byname(pdev, "plug_in");
	if (mhdp->irq[IRQ_IN] < 0)
		return dev_err_probe(dev, mhdp->irq[IRQ_IN], "No plug_in irq number\n");

	mhdp->irq[IRQ_OUT] = platform_get_irq_byname(pdev, "plug_out");
	if (mhdp->irq[IRQ_OUT] < 0)
		return dev_err_probe(dev, mhdp->irq[IRQ_OUT], "No plug_out irq number\n");

	irq_set_status_flags(mhdp->irq[IRQ_IN], IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(dev, mhdp->irq[IRQ_IN],
					NULL, cdns_mhdp8501_irq_thread,
					IRQF_ONESHOT, dev_name(dev), mhdp);
	if (ret < 0) {
		dev_err(dev, "can't claim irq %d\n", mhdp->irq[IRQ_IN]);
		return -EINVAL;
	}

	irq_set_status_flags(mhdp->irq[IRQ_OUT], IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(dev, mhdp->irq[IRQ_OUT],
					NULL, cdns_mhdp8501_irq_thread,
					IRQF_ONESHOT, dev_name(dev), mhdp);
	if (ret < 0) {
		dev_err(dev, "can't claim irq %d\n", mhdp->irq[IRQ_OUT]);
		return -EINVAL;
	}

	/* cdns_mhdp8501_dt_parse() ensures connector_type is valid */
	if (mhdp->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		phy_mode = PHY_MODE_DP;
	else if (mhdp->connector_type == DRM_MODE_CONNECTOR_HDMIA)
		phy_mode = PHY_MODE_HDMI;

	dev_set_drvdata(dev, mhdp);

	/* init base struct for access mhdp mailbox */
	mhdp->base.dev = mhdp->dev;
	mhdp->base.regs = mhdp->regs;

	if (mhdp->connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
		drm_dp_aux_init(&mhdp->dp.aux);
		mhdp->dp.aux.name = "mhdp8501_dp_aux";
		mhdp->dp.aux.dev = dev;
		mhdp->dp.aux.transfer = cdns_dp_aux_transfer;
	}

	/* Enable APB clock */
	mhdp->apb_clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(mhdp->apb_clk))
		return dev_err_probe(dev, PTR_ERR(mhdp->apb_clk),
				     "couldn't get apb clk\n");
	/*
	 * Wait for the KEEP_ALIVE "message" on the first 8 bits.
	 * Updated each sched "tick" (~2ms)
	 */
	ret = readl_poll_timeout(mhdp->regs + KEEP_ALIVE, reg,
				 reg & CDNS_KEEP_ALIVE_MASK, 500,
				 CDNS_KEEP_ALIVE_TIMEOUT);
	if (ret) {
		dev_err(dev, "device didn't give any life sign: reg %d\n", reg);
		return ret;
	}

	ret = phy_init(mhdp->phy);
	if (ret) {
		dev_err(dev, "Failed to initialize PHY: %d\n", ret);
		return ret;
	}

	ret = phy_set_mode(mhdp->phy, phy_mode);
	if (ret) {
		dev_err(dev, "Failed to configure PHY: %d\n", ret);
		return ret;
	}

	/* Enable cable hotplug detect */
	if (cdns_mhdp8501_read_hpd(mhdp))
		enable_irq(mhdp->irq[IRQ_OUT]);
	else
		enable_irq(mhdp->irq[IRQ_IN]);

	return cdns_mhdp8501_add_bridge(mhdp);
}

static void cdns_mhdp8501_remove(struct platform_device *pdev)
{
	struct cdns_mhdp8501_device *mhdp = platform_get_drvdata(pdev);

	cdns_mhdp8501_remove_device_files(mhdp);

	if (mhdp->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		cdns_dp_aux_destroy(mhdp);

	drm_bridge_remove(&mhdp->bridge);
}

static const struct of_device_id cdns_mhdp8501_dt_ids[] = {
	{ .compatible = "fsl,imx8mq-mhdp8501",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, cdns_mhdp8501_dt_ids);

static struct platform_driver cdns_mhdp8501_driver = {
	.probe = cdns_mhdp8501_probe,
	.remove = cdns_mhdp8501_remove,
	.driver = {
		.name = "cdns-mhdp8501",
		.of_match_table = cdns_mhdp8501_dt_ids,
	},
};

module_platform_driver(cdns_mhdp8501_driver);

MODULE_AUTHOR("Sandor Yu <sandor.yu@nxp.com>");
MODULE_DESCRIPTION("Cadence MHDP8501 bridge driver");
MODULE_LICENSE("GPL");
