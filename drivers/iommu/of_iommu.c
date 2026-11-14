// SPDX-License-Identifier: GPL-2.0-only
/*
 * OF helpers for IOMMU
 *
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/export.h>
#include <linux/iommu.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_iommu.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/fsl/mc.h>

#include "iommu-priv.h"

static int of_iommu_xlate(struct device *dev,
			  struct of_phandle_args *iommu_spec)
{
	const struct iommu_ops *ops;
	int ret;

	if (!of_device_is_available(iommu_spec->np))
		return -ENODEV;

	ret = iommu_fwspec_init(dev, of_fwnode_handle(iommu_spec->np));
	if (ret)
		return ret;

	ops = iommu_ops_from_fwnode(&iommu_spec->np->fwnode);
	if (!ops->of_xlate || !try_module_get(ops->owner))
		return -ENODEV;

	ret = ops->of_xlate(dev, iommu_spec);
	module_put(ops->owner);
	return ret;
}

/*
 * A non-PCI device can be reached through several stream IDs for one input
 * ID (one iommu-map entry each, e.g. a video codec's non-pixel context bank).
 * of_map_id() returns only the first match, so walk the map and add every
 * matching single-ID entry. Returns the number of entries added, or an error.
 */
static int of_iommu_configure_dev_id_multi(struct device_node *master_np,
					   struct device *dev, u32 id)
{
	const __be32 *map;
	u32 map_mask = 0xffffffff;
	int map_len, offset = 0, found = 0, err;

	map = of_get_property(master_np, "iommu-map", &map_len);
	if (!map || map_len % sizeof(*map))
		return 0;
	map_len /= sizeof(*map);
	of_property_read_u32(master_np, "iommu-map-mask", &map_mask);
	id &= map_mask;

	while (map_len - offset >= 4) {
		struct of_phandle_args spec = {};
		u32 id_base = be32_to_cpup(map + offset);
		u32 cells = 1, id_len, i;

		spec.np = of_find_node_by_phandle(be32_to_cpup(map + offset + 1));
		if (!spec.np)
			return -ENODEV;
		of_property_read_u32(spec.np, "#iommu-cells", &cells);
		if (cells > MAX_PHANDLE_ARGS || map_len - offset < 3 + cells) {
			of_node_put(spec.np);
			return -EINVAL;
		}
		id_len = be32_to_cpup(map + offset + 2 + cells);
		if (id >= id_base && id - id_base < id_len) {
			for (i = 0; i < cells; i++)
				spec.args[i] = be32_to_cpup(map + offset + 2 + i);
			spec.args[0] += id - id_base;
			spec.args_count = cells;
			err = of_iommu_xlate(dev, &spec);
			if (err) {
				of_node_put(spec.np);
				return err;
			}
			found++;
		}
		of_node_put(spec.np);
		offset += 3 + cells;
	}

	return found;
}

static int of_iommu_configure_dev_id(struct device_node *master_np,
				     struct device *dev,
				     const u32 *id)
{
	struct of_phandle_args iommu_spec = {};
	int err;

	if (!dev_is_pci(dev)) {
		err = of_iommu_configure_dev_id_multi(master_np, dev, *id);
		if (err)
			return err < 0 ? err : 0;
	}

	err = of_map_iommu_id(master_np, *id, &iommu_spec);
	if (err)
		return err;

	err = of_iommu_xlate(dev, &iommu_spec);
	of_node_put(iommu_spec.np);
	return err;
}

static int of_iommu_configure_dev(struct device_node *master_np,
				  struct device *dev)
{
	struct of_phandle_args iommu_spec;
	int err = -ENODEV, idx = 0;

	while (!of_parse_phandle_with_args(master_np, "iommus",
					   "#iommu-cells",
					   idx, &iommu_spec)) {
		err = of_iommu_xlate(dev, &iommu_spec);
		of_node_put(iommu_spec.np);
		idx++;
		if (err)
			break;
	}

	return err;
}

struct of_pci_iommu_alias_info {
	struct device *dev;
	struct device_node *np;
};

static int of_pci_iommu_init(struct pci_dev *pdev, u16 alias, void *data)
{
	struct of_pci_iommu_alias_info *info = data;
	u32 input_id = alias;

	return of_iommu_configure_dev_id(info->np, info->dev, &input_id);
}

static int of_iommu_configure_device(struct device_node *master_np,
				     struct device *dev, const u32 *id)
{
	return (id) ? of_iommu_configure_dev_id(master_np, dev, id) :
		      of_iommu_configure_dev(master_np, dev);
}

static void of_pci_check_device_ats(struct device *dev, struct device_node *np)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

	if (fwspec && of_property_read_bool(np, "ats-supported"))
		fwspec->flags |= IOMMU_FWSPEC_PCI_RC_ATS;
}

/*
 * Returns:
 *  0 on success, an iommu was configured
 *  -ENODEV if the device does not have any IOMMU
 *  -EPROBEDEFER if probing should be tried again
 *  -errno fatal errors
 */
int of_iommu_configure(struct device *dev, struct device_node *master_np,
		       const u32 *id)
{
	bool dev_iommu_present;
	int err;

	if (!master_np)
		return -ENODEV;

	/* Serialise to make dev->iommu stable under our potential fwspec */
	mutex_lock(&iommu_probe_device_lock);
	if (dev_iommu_fwspec_get(dev)) {
		mutex_unlock(&iommu_probe_device_lock);
		return 0;
	}
	dev_iommu_present = dev->iommu;

	/*
	 * We don't currently walk up the tree looking for a parent IOMMU.
	 * See the `Notes:' section of
	 * Documentation/devicetree/bindings/iommu/iommu.txt
	 */
	if (dev_is_pci(dev)) {
		struct of_pci_iommu_alias_info info = {
			.dev = dev,
			.np = master_np,
		};

		pci_request_acs();
		err = pci_for_each_dma_alias(to_pci_dev(dev),
					     of_pci_iommu_init, &info);
		of_pci_check_device_ats(dev, master_np);
	} else {
		err = of_iommu_configure_device(master_np, dev, id);
	}

	if (err && dev_iommu_present)
		iommu_fwspec_free(dev);
	else if (err && dev->iommu)
		dev_iommu_free(dev);
	mutex_unlock(&iommu_probe_device_lock);

	/*
	 * If we're not on the iommu_probe_device() path (as indicated by the
	 * initial dev->iommu) then try to simulate it. This should no longer
	 * happen unless of_dma_configure() is being misused outside bus code.
	 */
	if (!err && dev->bus && !dev_iommu_present)
		err = iommu_probe_device(dev);

	if (err && err != -EPROBE_DEFER)
		dev_dbg(dev, "Adding to IOMMU failed: %d\n", err);

	return err;
}

static enum iommu_resv_type __maybe_unused
iommu_resv_region_get_type(struct device *dev,
			   struct resource *phys,
			   phys_addr_t start, size_t length)
{
	phys_addr_t end = start + length - 1;

	/*
	 * IOMMU regions without an associated physical region cannot be
	 * mapped and are simply reservations.
	 */
	if (phys->start >= phys->end)
		return IOMMU_RESV_RESERVED;

	/* may be IOMMU_RESV_DIRECT_RELAXABLE for certain cases */
	if (start == phys->start && end == phys->end)
		return IOMMU_RESV_DIRECT;

	dev_warn(dev, "treating non-direct mapping [%pr] -> [%pap-%pap] as reservation\n", phys,
		 &start, &end);
	return IOMMU_RESV_RESERVED;
}

/**
 * of_iommu_get_resv_regions - reserved region driver helper for device tree
 * @dev: device for which to get reserved regions
 * @list: reserved region list
 *
 * IOMMU drivers can use this to implement their .get_resv_regions() callback
 * for memory regions attached to a device tree node. See the reserved-memory
 * device tree bindings on how to use these:
 *
 *   Documentation/devicetree/bindings/reserved-memory/reserved-memory.txt
 */
void of_iommu_get_resv_regions(struct device *dev, struct list_head *list)
{
#if IS_ENABLED(CONFIG_OF_ADDRESS)
	struct of_phandle_iterator it;
	int err;

	of_for_each_phandle(&it, err, dev->of_node, "memory-region", NULL, 0) {
		const __be32 *maps, *end;
		struct resource phys;
		int size;

		memset(&phys, 0, sizeof(phys));

		/*
		 * The "reg" property is optional and can be omitted by reserved-memory regions
		 * that represent reservations in the IOVA space, which are regions that should
		 * not be mapped.
		 */
		if (of_property_present(it.node, "reg")) {
			err = of_address_to_resource(it.node, 0, &phys);
			if (err < 0) {
				dev_err(dev, "failed to parse memory region %pOF: %d\n",
					it.node, err);
				continue;
			}
		}

		maps = of_get_property(it.node, "iommu-addresses", &size);
		if (!maps)
			continue;

		end = maps + size / sizeof(__be32);

		while (maps < end) {
			struct device_node *np;
			u32 phandle;

			phandle = be32_to_cpup(maps++);
			np = of_find_node_by_phandle(phandle);

			if (np == dev->of_node) {
				int prot = IOMMU_READ | IOMMU_WRITE;
				struct iommu_resv_region *region;
				enum iommu_resv_type type;
				phys_addr_t iova;
				size_t length;

				if (of_dma_is_coherent(dev->of_node))
					prot |= IOMMU_CACHE;

				maps = of_translate_dma_region(np, maps, &iova, &length);
				if (length == 0) {
					dev_warn(dev, "Cannot reserve IOVA region of 0 size\n");
					continue;
				}
				type = iommu_resv_region_get_type(dev, &phys, iova, length);

				region = iommu_alloc_resv_region(iova, length, prot, type,
								 GFP_KERNEL);
				if (region)
					list_add_tail(&region->list, list);
			}
		}
	}
#endif
}
EXPORT_SYMBOL(of_iommu_get_resv_regions);
