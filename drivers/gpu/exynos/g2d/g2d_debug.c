/*
 * linux/drivers/gpu/exynos/g2d/g2d_debug.c
 *
 * Copyright (C) 2017 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>

#include "g2d.h"
#include "g2d_task.h"
#include "g2d_uapi.h"
#include "g2d_debug.h"

static unsigned int g2d_debug;

#define G2D_MAX_STAMP_SIZE 1024

static struct g2d_stamp {
	ktime_t time;
	struct g2d_task *task;
	u32 state;
	u32 job_id;
	u32 val;
	u8 cpu;
} g2d_stamp_list[G2D_MAX_STAMP_SIZE];

static atomic_t p_stamp;

static int g2d_stamp_show(struct seq_file *s, void *unused)
{
	int ptr = atomic_read(&p_stamp);
	struct g2d_stamp *stamp;
	int i;

	if (ptr < 0)
		return 0;

	/* in chronological order */
	ptr = (ptr + 1) & (G2D_MAX_STAMP_SIZE - 1);
	i = ptr;

	while (1) {
		stamp = &g2d_stamp_list[i];

		seq_printf(s, "[%d] %u:%u@%u (0x%x) %06llu\n", i++,
			stamp->cpu, stamp->job_id, stamp->val, stamp->state,
			ktime_to_us(stamp->time));

		i &= (G2D_MAX_STAMP_SIZE - 1);

		if (i == ptr)
			break;
	}

	return 0;
}

static int g2d_debug_logs_open(struct inode *inode, struct file *file)
{
	return single_open(file, g2d_stamp_show, inode->i_private);
}

static const struct file_operations g2d_debug_logs_fops = {
	.open = g2d_debug_logs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void g2d_init_debug(struct g2d_device *g2d_dev)
{
	atomic_set(&p_stamp, -1);

	g2d_dev->debug_root = debugfs_create_dir("g2d", NULL);
	if (!g2d_dev->debug_root) {
		dev_err(g2d_dev->dev, "debugfs : failed to create root directory\n");
		return;
	}

	g2d_dev->debug = debugfs_create_u32("debug",
					0644, g2d_dev->debug_root, &g2d_debug);
	if (!g2d_dev->debug) {
		dev_err(g2d_dev->dev, "debugfs : failed to create debug file\n");
		return;
	}

	g2d_dev->debug_logs = debugfs_create_file("logs",
					0444, g2d_dev->debug_root, g2d_dev, &g2d_debug_logs_fops);
	if (!g2d_dev->debug_logs) {
		dev_err(g2d_dev->dev, "debugfs : failed to create debug logs file\n");
		return;
	}
}

void g2d_destroy_debug(struct g2d_device *g2d_dev)
{
	debugfs_remove_recursive(g2d_dev->debug_root);
}

static struct regs_info g2d_reg_info[] = {
		/* Start, Size, Name */
		{ 0x0,		0x20,	"General" },
		{ 0x34,		0x10,	"Secure Layer" },
		{ 0xF0,		0x10,	"AFBC debugging" },
		{ 0x80,		0x70,	"Job manager" },
		{ 0x8000,	0x100,	"HW flow control" },
		{ 0x2000,	0x120,	"Layer CSC Coefficient" },
		{ 0x120,	0xE0,	"Destination" },
		{ 0x200,	0x100,	"Layer0" },
		{ 0x300,	0x100,	"Layer1" },
		{ 0x400,	0x100,	"Layer2" },
		{ 0x500,	0x100,	"Layer3" },
		{ 0x600,	0x100,	"Layer4" },
		{ 0x700,	0x100,	"Layer5" },
		{ 0x800,	0x100,	"Layer6" },
		{ 0x900,	0x100,	"Layer7" },
		{ 0xA00,	0x100,	"Layer8" },
		{ 0xB00,	0x100,	"Layer9" },
		{ 0xC00,	0x100,	"Layer10" },
		{ 0xD00,	0x100,	"Layer11" },
		{ 0xE00,	0x100,	"Layer12" },
		{ 0xF00,	0x100,	"Layer13" },
		{ 0x1000,	0x100,	"Layer14" },
		{ 0x1100,	0x100,	"Layer15" },
};

static void g2d_dump_task(struct g2d_task *task)
{
	struct g2d_device *g2d_dev = task->g2d_dev;
	unsigned int i, num_array;
	struct g2d_reg *regs;

	num_array = ARRAY_SIZE(g2d_reg_info) - G2D_MAX_IMAGES + task->num_source;

	for (i = 0; i < num_array; i++) {
		pr_info("[%s: %04X .. %04X]\n",
			g2d_reg_info[i].name, g2d_reg_info[i].start,
			g2d_reg_info[i].start + g2d_reg_info[i].size);
		print_hex_dump(KERN_INFO, "", DUMP_PREFIX_ADDRESS, 32, 4,
			g2d_dev->reg + g2d_reg_info[i].start,
			g2d_reg_info[i].size, false);
	}

	regs = page_address(task->cmd_page);

	for (i = 0; i < task->cmd_count; i++)
		pr_info("G2D: CMD[%03d] %#06x, %#010x\n",
			i, regs[i].offset, regs[i].value);
}

void g2d_stamp_task(struct g2d_task *task, u32 val)
{
	int ptr = atomic_inc_return(&p_stamp) & (G2D_MAX_STAMP_SIZE - 1);
	struct g2d_stamp *stamp = &g2d_stamp_list[ptr];

	if (task) {
		stamp->state = task->state;
		stamp->job_id = task->job_id;
		stamp->task = task;
	} else {
		stamp->task = NULL;
	}

	stamp->time = ktime_get();
	stamp->val = val;
	stamp->cpu = smp_processor_id();

	/* when error status, dump the task */
	if ((stamp->val == G2D_STAMP_STATE_TIMEOUT_HW) ||
		(stamp->val == G2D_STAMP_STATE_ERR_INT) ||
		(stamp->val == G2D_STAMP_STATE_MMUFAULT))
		g2d_dump_task(task);

	if (stamp->val == G2D_STAMP_STATE_DONE) {
		if (g2d_debug == 1) {
			pr_info("Job #%x took %06llu to H/W process\n",
			task->job_id, ktime_us_delta(task->ktime_end, task->ktime_begin));
		} else if (g2d_debug == 2) {
			g2d_dump_task(task);
		}
	}
}