/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef GEMINI_PANEL_H
#define GEMINI_PANEL_H

#include <stddef.h>

enum gemini_panel_kind {
	GEMINI_PANEL_UNKNOWN,
	GEMINI_PANEL_JDI,
	GEMINI_PANEL_LGD,
};

/* len includes the terminating NUL from the firmware FDT property. */
int gemini_panel_from_bootargs(const char *args, size_t len);
const char *gemini_panel_name(int panel);

#endif
