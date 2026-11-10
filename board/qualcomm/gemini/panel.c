// SPDX-License-Identifier: GPL-2.0+
/* Decode the active panel reported by stock Mi 5 aboot, not the input DTB. */
#include <linux/errno.h>
#include <linux/string.h>

#include "panel.h"

const char *gemini_panel_name(int panel)
{
	if (panel == GEMINI_PANEL_JDI)
		return "jdi";
	if (panel == GEMINI_PANEL_LGD)
		return "lgd";
	return NULL;
}

int gemini_panel_from_bootargs(const char *args, size_t len)
{
	static const char key[] = "mdss_mdp.panel=";
	static const char prefix[] = "1:dsi:0:";
	static const char jdi[] = "qcom,mdss_dsi_jdi_fhd_r63452_cmd";
	static const char lgd[] = "qcom,mdss_dsi_lgd_fhd_td4322_cmd";
	const char *word, *value = NULL, *end;
	size_t size, value_size = 0;

	if (!args || !len || len > 2048 || strnlen(args, len) != len - 1)
		return -EINVAL;
	while (*args) {
		while (*args == ' ' || *args == '\t')
			args++;
		word = args;
		while (*args && *args != ' ' && *args != '\t')
			args++;
		size = args - word;
		if (size < sizeof(key) - 1 || memcmp(word, key, sizeof(key) - 1))
			continue;
		if (value)
			return -EEXIST;
		value = word + sizeof(key) - 1;
		value_size = size - (sizeof(key) - 1);
	}
	if (!value)
		return -ENOENT;
	if (value_size <= sizeof(prefix) - 1 ||
	    memcmp(value, prefix, sizeof(prefix) - 1))
		return -EINVAL;
	for (size_t i = 0; i < value_size; i++)
		if ((unsigned char)value[i] < 0x21 ||
		    (unsigned char)value[i] > 0x7e)
			return -EINVAL;
	value += sizeof(prefix) - 1;
	value_size -= sizeof(prefix) - 1;
	end = memchr(value, ':', value_size);
	size = end ? (size_t)(end - value) : value_size;
	if (size == sizeof(jdi) - 1 && !memcmp(value, jdi, size))
		return GEMINI_PANEL_JDI;
	if (size == sizeof(lgd) - 1 && !memcmp(value, lgd, size))
		return GEMINI_PANEL_LGD;
	return -ENODEV;
}
