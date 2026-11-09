/* SPDX-License-Identifier: GPL-2.0+ */
static int gb_copy_prop(void *target, int dest, const void *runtime,
			int src, const char *name, bool required)
{
	const void *value;
	int len;

	value = fdt_getprop(runtime, src, name, &len);
	if (!value)
		return required ? -EINVAL : 0;
	if (len < 0 || len > 4096)
		return -EINVAL;
	return fdt_setprop(target, dest, name, value, len);
}

/* Use the file DTB as the real base. Copy only firmware memory/boot fixups,
 * keeping its device descriptions and phandle namespace intact.
 */
static int gb_fdt_fixups(void *target, const void *runtime)
{
	char path[512];
	const char *name;
	int rmem, tmem, src, dst, rreserved, treserved, ret, len, count;
	const void *reg;
	u64 address, size;
	static const char *const props[] = { "reg", "size", "alloc-ranges", "no-map" };
	static const char *const chosen_props[] = { "smeminfo", "hwversion", "pureason" };

	if (fdt_address_cells(target, 0) != 2 || fdt_size_cells(target, 0) != 2 ||
	    fdt_address_cells(runtime, 0) != 2 || fdt_size_cells(runtime, 0) != 2)
		return -EINVAL;
	src = -1;
	count = 0;
	while ((src = fdt_node_offset_by_prop_value(runtime, src, "device_type", "memory", 7)) >= 0) {
		reg = fdt_getprop(runtime, src, "reg", &len);
		if (!reg || len < 16 || len % 16 || len > 24 * 16 || ++count > 24 ||
		    fdt_get_path(runtime, src, path, sizeof(path)))
			return -EINVAL;
		dst = fdt_path_offset(target, path);
		if (dst < 0 || fdt_setprop(target, dst, "reg", reg, len))
			return -EINVAL;
	}
	if (!count)
		return -EINVAL;
	/* Extra file memory nodes must also be represented by firmware. */
	dst = -1;
	while ((dst = fdt_node_offset_by_prop_value(target, dst, "device_type", "memory", 7)) >= 0) {
		if (fdt_get_path(target, dst, path, sizeof(path)) || fdt_path_offset(runtime, path) < 0)
			return -EINVAL;
	}
	rreserved = fdt_path_offset(runtime, "/reserved-memory");
	treserved = fdt_path_offset(target, "/reserved-memory");
	if (rreserved < 0 || treserved < 0 ||
	    fdt_address_cells(target, treserved) != 2 || fdt_size_cells(target, treserved) != 2 ||
	    fdt_address_cells(runtime, rreserved) != 2 || fdt_size_cells(runtime, rreserved) != 2)
		return -EINVAL;
	fdt_for_each_subnode(src, runtime, rreserved) {
		name = fdt_get_name(runtime, src, NULL);
		treserved = fdt_path_offset(target, "/reserved-memory");
		dst = fdt_subnode_offset(target, treserved, name);
		if (dst < 0)
			return -EINVAL;
		for (int i = 0; i < ARRAY_SIZE(props); i++) {
			ret = gb_copy_prop(target, dst, runtime, src, props[i], false);
			if (ret)
				return ret;
		}
	}
	rmem = fdt_num_mem_rsv(runtime);
	if (rmem < 0 || rmem > 128)
		return -EINVAL;
	for (int i = 0; i < rmem; i++) {
		ret = fdt_get_mem_rsv(runtime, i, &address, &size);
		if (ret || !size || address + size < address)
			return -EINVAL;
		ret = fdt_add_mem_rsv(target, address, size);
		if (ret)
			return ret;
	}
	rmem = fdt_path_offset(runtime, "/chosen");
	tmem = fdt_path_offset(target, "/chosen");
	if (rmem < 0 || tmem < 0)
		return -EINVAL;
	for (int i = 0; i < ARRAY_SIZE(chosen_props); i++) {
		ret = gb_copy_prop(target, tmem, runtime, rmem, chosen_props[i], false);
		if (ret)
			return ret;
	}
	puts("EXT4 FDT_BASE=file runtime_memory_fixups=1\n");
	return 0;
}
