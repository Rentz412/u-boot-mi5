/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __GEMINI_H
#define __GEMINI_H
struct blk_desc;
int gemini_ufs_open(struct blk_desc **desc);
int gemini_ufs_close(bool restore);
int gemini_ufs_error(void);
int gemini_ufs_quiesce(void);
#endif
