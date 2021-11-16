#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>

#include "onic.h"

extern const char onic_drv_name[];
extern const char onic_drv_ver[];

static void onic_get_drvinfo(struct net_device *netdev,
			     struct ethtool_drvinfo *drvinfo)
{
	struct onic_priv *xpriv = netdev_priv(netdev);

	strlcpy(drvinfo->driver, onic_drv_name, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, onic_drv_ver,
		sizeof(drvinfo->version));
	strlcpy(drvinfo->bus_info, pci_name(xpriv->pcidev),
		sizeof(drvinfo->bus_info));
}

static const struct ethtool_ops onic_ethtool_ops = {
	.get_drvinfo = onic_get_drvinfo,
	.get_link = ethtool_op_get_link,
};

void onic_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &onic_ethtool_ops;
}
