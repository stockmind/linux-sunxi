// SPDX-License-Identifier: GPL-2.0
/*
 * VirtualBox Guest Shared Folders: Operations for symbolic links.
 *
 * Copyright (C) 2010-2016 Oracle Corporation
 */

#include "vfsmod.h"

static const char *sf_get_link(struct dentry *dentry, struct inode *inode,
			       struct delayed_call *done)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
	struct sf_inode_info *sf_i = GET_INODE_INFO(inode);
	char *path;
	int err;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	path = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!path)
		return ERR_PTR(-ENOMEM);

	err = vboxsf_readlink(sf_g->root, sf_i->path, PATH_MAX, path);
	if (err) {
		kfree(path);
		return ERR_PTR(err);
	}

	set_delayed_call(done, kfree_link, path);
	return path;
}

const struct inode_operations sf_lnk_iops = {
	.get_link = sf_get_link
};
