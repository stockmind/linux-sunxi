// SPDX-License-Identifier: GPL-2.0
/*
 * VirtualBox Guest Shared Folders support: Directory inode and file operations
 *
 * Copyright (C) 2006-2016 Oracle Corporation
 */

#include <linux/vbox_utils.h>
#include "vfsmod.h"

/**
 * Open a directory. Read the complete content into a buffer.
 *
 * @param inode     inode
 * @param file      file
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_dir_open(struct inode *inode, struct file *file)
{
	int err;
	struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
	struct sf_dir_info *sf_d;
	struct sf_inode_info *sf_i = GET_INODE_INFO(inode);
	struct shfl_createparms params = {};

	if (file->private_data)
		return 0;

	sf_d = sf_dir_info_alloc();
	if (!sf_d)
		return -ENOMEM;

	params.handle = SHFL_HANDLE_NIL;
	params.create_flags = 0
	    | SHFL_CF_DIRECTORY
	    | SHFL_CF_ACT_OPEN_IF_EXISTS
	    | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;

	err = vboxsf_create(sf_g->root, sf_i->path, &params);
	if (err == 0) {
		if (params.result == SHFL_FILE_EXISTS) {
			err = sf_dir_read_all(sf_g, sf_i, sf_d, params.handle);
			if (!err)
				file->private_data = sf_d;
		} else
			err = -ENOENT;

		vboxsf_close(sf_g->root, params.handle);
	}

	if (err)
		sf_dir_info_free(sf_d);

	return err;
}

/**
 * This is called when reference count of [file] goes to zero. Notify
 * the host that it can free whatever is associated with this directory
 * and deallocate our own internal buffers
 *
 * @param inode     inode
 * @param file      file
 * returns 0 on success, Linux error code otherwise
 */
static int sf_dir_release(struct inode *inode, struct file *file)
{
	if (file->private_data)
		sf_dir_info_free(file->private_data);

	return 0;
}

/**
 * Translate RTFMODE into DT_xxx (in conjunction to rtDirType())
 * @param mode     file mode
 * returns d_type
 */
static int sf_get_d_type(u32 mode)
{
	int d_type;

	switch (mode & SHFL_TYPE_MASK) {
	case SHFL_TYPE_FIFO:
		d_type = DT_FIFO;
		break;
	case SHFL_TYPE_DEV_CHAR:
		d_type = DT_CHR;
		break;
	case SHFL_TYPE_DIRECTORY:
		d_type = DT_DIR;
		break;
	case SHFL_TYPE_DEV_BLOCK:
		d_type = DT_BLK;
		break;
	case SHFL_TYPE_FILE:
		d_type = DT_REG;
		break;
	case SHFL_TYPE_SYMLINK:
		d_type = DT_LNK;
		break;
	case SHFL_TYPE_SOCKET:
		d_type = DT_SOCK;
		break;
	case SHFL_TYPE_WHITEOUT:
		d_type = DT_WHT;
		break;
	default:
		d_type = DT_UNKNOWN;
		break;
	}
	return d_type;
}

/**
 * Extract element ([dir]->f_pos) from the directory [dir] into [d_name].
 *
 * @returns 0 for success, 1 for end reached, Linux error code otherwise.
 */
static int sf_getdent(struct file *dir, char d_name[NAME_MAX], int *d_type)
{
	loff_t cur;
	struct sf_glob_info *sf_g;
	struct sf_dir_info *sf_d;
	struct sf_inode_info *sf_i;
	struct inode *inode;
	struct list_head *pos, *list;

	inode = GET_F_DENTRY(dir)->d_inode;
	sf_i = GET_INODE_INFO(inode);
	sf_g = GET_GLOB_INFO(inode->i_sb);
	sf_d = dir->private_data;

	if (sf_i->force_reread) {
		int err;
		struct shfl_createparms params = {};

		params.handle = SHFL_HANDLE_NIL;
		params.create_flags = 0
		    | SHFL_CF_DIRECTORY
		    | SHFL_CF_ACT_OPEN_IF_EXISTS
		    | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;

		err = vboxsf_create(sf_g->root, sf_i->path, &params);
		if (err)
			return err;

		if (params.result != SHFL_FILE_EXISTS) {
			sf_dir_info_free(sf_d);
			return -ENOENT;
		}

		sf_dir_info_empty(sf_d);
		err = sf_dir_read_all(sf_g, sf_i, sf_d, params.handle);
		vboxsf_close(sf_g->root, params.handle);
		if (err)
			return err;

		sf_i->force_reread = 0;
	}

	cur = 0;
	list = &sf_d->info_list;
	list_for_each(pos, list) {
		struct sf_dir_buf *b;
		struct shfl_dirinfo *info;
		loff_t i;

		b = list_entry(pos, struct sf_dir_buf, head);
		if (dir->f_pos >= cur + b->entries) {
			cur += b->entries;
			continue;
		}

		for (i = 0, info = b->buf; i < dir->f_pos - cur; ++i) {
			size_t size;

			size = offsetof(struct shfl_dirinfo, name.string) +
			       info->name.size;
			info = (struct shfl_dirinfo *)((uintptr_t) info + size);
		}

		*d_type = sf_get_d_type(info->info.attr.mode);

		return sf_nlscpy(sf_g, d_name, NAME_MAX,
				 info->name.string.utf8, info->name.length);
	}

	return 1;
}

/**
 * This is called when vfs wants to populate internal buffers with
 * directory [dir]s contents. [opaque] is an argument to the
 * [filldir]. [filldir] magically modifies it's argument - [opaque]
 * and takes following additional arguments (which i in turn get from
 * the host via sf_getdent):
 *
 * name : name of the entry (i must also supply it's length huh?)
 * type : type of the entry (FILE | DIR | etc) (i ellect to use DT_UNKNOWN)
 * pos : position/index of the entry
 * ino : inode number of the entry (i fake those)
 *
 * [dir] contains:
 * f_pos : cursor into the directory listing
 * private_data : mean of communication with the host side
 *
 * Extract elements from the directory listing (incrementing f_pos
 * along the way) and feed them to [filldir] until:
 *
 * a. there are no more entries (i.e. sf_getdent set done to 1)
 * b. failure to compute fake inode number
 * c. filldir returns an error (see comment on that)
 */
static int sf_dir_iterate(struct file *dir, struct dir_context *ctx)
{
	for (;;) {
		int err;
		ino_t fake_ino;
		loff_t sanity;
		char d_name[NAME_MAX];
		int d_type = DT_UNKNOWN;

		err = sf_getdent(dir, d_name, &d_type);
		switch (err) {
		case 1:
			return 0;

		case 0:
			break;

		case -1:
		default:
			/* skip erroneous entry and proceed */
			dir->f_pos += 1;
			ctx->pos += 1;
			continue;
		}

		/* d_name now contains a valid entry name */
		sanity = ctx->pos + 0xbeef;
		fake_ino = sanity;
		/*
		 * On 32 bit systems pos is 64 signed, while ino is 32 bit
		 * unsigned so fake_ino may overflow, check for this.
		 */
		if (sanity - fake_ino) {
			vbg_err("vboxsf: can not compute ino\n");
			return -EINVAL;
		}
		if (!dir_emit(ctx, d_name, strlen(d_name), fake_ino, d_type))
			return 0;

		dir->f_pos += 1;
		ctx->pos += 1;
	}
}

const struct file_operations sf_dir_fops = {
	.open = sf_dir_open,
	.iterate = sf_dir_iterate,
	.release = sf_dir_release,
	.read = generic_read_dir,
	.llseek = generic_file_llseek,
};

/* iops */

/**
 * This is called when vfs failed to locate dentry in the cache. The
 * job of this function is to allocate inode and link it to dentry.
 * [dentry] contains the name to be looked in the [parent] directory.
 * Failure to locate the name is not a "hard" error, in this case NULL
 * inode is added to [dentry] and vfs should proceed trying to create
 * the entry via other means. NULL(or "positive" pointer) ought to be
 * returned in case of success and "negative" pointer on error
 */
static struct dentry *sf_lookup(struct inode *parent, struct dentry *dentry,
				unsigned int flags)
{
	struct shfl_fsobjinfo fsinfo;
	struct sf_inode_info *sf_i;
	struct sf_glob_info *sf_g;
	struct shfl_string *path;
	struct inode *inode;
	ino_t ino;
	int err;

	sf_g = GET_GLOB_INFO(parent->i_sb);
	sf_i = GET_INODE_INFO(parent);

	err = sf_path_from_dentry(__func__, sf_g, sf_i, dentry, &path);
	if (err)
		return ERR_PTR(err);

	err = sf_stat(__func__, sf_g, path, &fsinfo, 1);
	if (err) {
		kfree(path);
		if (err != -ENOENT)
			return ERR_PTR(err);
		/*
		 * -ENOENT: add NULL inode to dentry so it later can
		 * be created via call to create/mkdir/open
		 */
		inode = NULL;
	} else {
		ino = iunique(parent->i_sb, 1);
		inode = iget_locked(parent->i_sb, ino);
		if (!inode) {
			kfree(path);
			return ERR_PTR(-ENOMEM);
		}

		GET_INODE_INFO(inode)->path = path;
		sf_init_inode(sf_g, inode, &fsinfo);

		unlock_new_inode(inode);
	}

	dentry->d_time = jiffies;
	d_set_d_op(dentry, &sf_dentry_ops);
	d_add(dentry, inode);
	return NULL;
}

/**
 * This should allocate memory for sf_inode_info, compute a unique inode
 * number, get an inode from vfs, initialize inode info, instantiate
 * dentry.
 *
 * @param parent        inode entry of the directory
 * @param dentry        directory cache entry
 * @param path          path name
 * @param info          file information
 * @param handle        handle
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_instantiate(struct inode *parent, struct dentry *dentry,
			  struct shfl_string *path, struct shfl_fsobjinfo *info,
			  SHFLHANDLE handle)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	struct sf_inode_info *sf_i;
	struct inode *inode;
	ino_t ino;

	ino = iunique(parent->i_sb, 1);
	inode = iget_locked(parent->i_sb, ino);
	if (!inode)
		return -ENOMEM;

	sf_i = GET_INODE_INFO(inode);
	sf_i->path = path;
	sf_i->force_restat = 1;
	sf_i->handle = handle;
	sf_init_inode(sf_g, inode, info);

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	return 0;
}

/**
 * Create a new regular file / directory.
 *
 * @param parent        inode of the directory
 * @param dentry        directory cache entry
 * @param mode          file mode
 * @param fDirectory    true if directory, false otherwise
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_create_aux(struct inode *parent, struct dentry *dentry,
			 umode_t mode, int fDirectory)
{
	int err;
	struct shfl_createparms params = {};
	struct shfl_string *path;
	struct sf_inode_info *sf_i = GET_INODE_INFO(parent);
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);

	err = sf_path_from_dentry(__func__, sf_g, sf_i, dentry, &path);
	if (err)
		return err;

	params.handle = SHFL_HANDLE_NIL;
	params.create_flags = 0
	    | SHFL_CF_ACT_CREATE_IF_NEW
	    | SHFL_CF_ACT_FAIL_IF_EXISTS
	    | SHFL_CF_ACCESS_READWRITE | (fDirectory ? SHFL_CF_DIRECTORY : 0);
	params.info.attr.mode = 0
	    | (fDirectory ? SHFL_TYPE_DIRECTORY : SHFL_TYPE_FILE)
	    | (mode & 0777);
	params.info.attr.additional = SHFLFSOBJATTRADD_NOTHING;

	err = vboxsf_create(sf_g->root, path, &params);
	if (err)
		goto free_path;

	if (params.result != SHFL_FILE_CREATED) {
		err = -EPERM;
		goto free_path;
	}

	err = sf_instantiate(parent, dentry, path, &params.info,
			     fDirectory ? SHFL_HANDLE_NIL : params.handle);
	if (err)
		goto close_handle;

	/*
	 * Don't close this handle right now. We assume that the same file is
	 * opened with sf_reg_open() and later closed with sf_reg_close(). Save
	 * the handle in between. Does not apply to directories. True?
	 */
	if (fDirectory)
		vboxsf_close(sf_g->root, params.handle);

	sf_i->force_restat = 1;
	return 0;

close_handle:
	vboxsf_close(sf_g->root, params.handle);

free_path:
	kfree(path);
	return err;
}

/**
 * Create a new regular file.
 *
 * @param parent        inode of the directory
 * @param dentry        directory cache entry
 * @param mode          file mode
 * @param excl          Possible O_EXCL...
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_create(struct inode *parent, struct dentry *dentry, umode_t mode,
		     bool excl)
{
	return sf_create_aux(parent, dentry, mode, 0);
}

/**
 * Create a new directory.
 *
 * @param parent        inode of the directory
 * @param dentry        directory cache entry
 * @param mode          file mode
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_mkdir(struct inode *parent, struct dentry *dentry, umode_t mode)
{
	return sf_create_aux(parent, dentry, mode, 1);
}

/**
 * Remove a regular file / directory.
 *
 * @param parent        inode of the directory
 * @param dentry        directory cache entry
 * @param fDirectory    true if directory, false otherwise
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_unlink_aux(struct inode *parent, struct dentry *dentry,
			 int fDirectory)
{
	int err;
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	struct sf_inode_info *sf_i = GET_INODE_INFO(parent);
	struct shfl_string *path;
	uint32_t flags;

	err = sf_path_from_dentry(__func__, sf_g, sf_i, dentry, &path);
	if (err)
		return err;

	flags = fDirectory ? SHFL_REMOVE_DIR : SHFL_REMOVE_FILE;
	if (dentry
	    && dentry->d_inode
	    && ((dentry->d_inode->i_mode & S_IFLNK) == S_IFLNK))
		flags |= SHFL_REMOVE_SYMLINK;

	err = vboxsf_remove(sf_g->root, path, flags);
	if (err)
		goto free_path;

	/* directory access/change time changed */
	sf_i->force_restat = 1;
	/* directory content changed */
	sf_i->force_reread = 1;

free_path:
	kfree(path);
	return err;
}

/**
 * Remove a regular file.
 *
 * @param parent        inode of the directory
 * @param dentry        directory cache entry
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_unlink(struct inode *parent, struct dentry *dentry)
{
	return sf_unlink_aux(parent, dentry, 0);
}

/**
 * Remove a directory.
 *
 * @param parent        inode of the directory
 * @param dentry        directory cache entry
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_rmdir(struct inode *parent, struct dentry *dentry)
{
	return sf_unlink_aux(parent, dentry, 1);
}

/**
 * Rename a regular file / directory.
 *
 * @param old_parent    inode of the old parent directory
 * @param old_dentry    old directory cache entry
 * @param new_parent    inode of the new parent directory
 * @param new_dentry    new directory cache entry
 * @param flags         flags
 * @returns 0 on success, Linux error code otherwise
 */
static int sf_rename(struct inode *old_parent, struct dentry *old_dentry,
		     struct inode *new_parent, struct dentry *new_dentry,
		     unsigned int flags)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(old_parent->i_sb);
	u32 shfl_flags = SHFL_RENAME_FILE | SHFL_RENAME_REPLACE_IF_EXISTS;
	struct sf_inode_info *sf_old_i = GET_INODE_INFO(old_parent);
	struct sf_inode_info *sf_new_i = GET_INODE_INFO(new_parent);
	/*
	 * As we save the relative path inside the inode structure,
	 * we need to change this if the rename is successful.
	 */
	struct sf_inode_info *sf_file_i = GET_INODE_INFO(old_dentry->d_inode);
	struct shfl_string *path;
	int err;

	if (flags)
		return -EINVAL;

	if (sf_g != GET_GLOB_INFO(new_parent->i_sb))
		return -EINVAL;

	err = sf_path_from_dentry(__func__, sf_g, sf_new_i, new_dentry, &path);
	if (err)
		return err;

	if (old_dentry->d_inode->i_mode & S_IFDIR)
		shfl_flags = 0;

	err = vboxsf_rename(sf_g->root, sf_file_i->path, path, shfl_flags);
	if (err == 0) {
		/* Set the new relative path in the inode. */
		kfree(sf_file_i->path);
		sf_file_i->path = path;
		sf_new_i->force_restat = 1;
		sf_old_i->force_restat = 1;
	} else {
		kfree(path);
	}

	return err;
}

static int sf_symlink(struct inode *parent, struct dentry *dentry,
		      const char *symname)
{
	int err;
	struct sf_inode_info *sf_i;
	struct sf_glob_info *sf_g;
	struct shfl_string *path, *ssymname;
	struct shfl_fsobjinfo info;
	int symname_len = strlen(symname) + 1;

	sf_g = GET_GLOB_INFO(parent->i_sb);
	sf_i = GET_INODE_INFO(parent);

	err = sf_path_from_dentry(__func__, sf_g, sf_i, dentry, &path);
	if (err)
		return err;

	ssymname = kmalloc(SHFLSTRING_HEADER_SIZE + symname_len, GFP_KERNEL);
	if (!ssymname) {
		err = -ENOMEM;
		goto free_path;
	}
	ssymname->length = symname_len - 1;
	ssymname->size = symname_len;
	memcpy(ssymname->string.utf8, symname, symname_len);

	err = vboxsf_symlink(sf_g->root, path, ssymname, &info);
	kfree(ssymname);

	if (err)
		goto free_path;

	err = sf_instantiate(parent, dentry, path, &info, SHFL_HANDLE_NIL);
	if (err)
		goto free_path;

	sf_i->force_restat = 1;
	return 0;

free_path:
	kfree(path);
	return err;
}

const struct inode_operations sf_dir_iops = {
	.lookup = sf_lookup,
	.create = sf_create,
	.mkdir = sf_mkdir,
	.rmdir = sf_rmdir,
	.unlink = sf_unlink,
	.rename = sf_rename,
	.getattr = sf_getattr,
	.setattr = sf_setattr,
	.symlink = sf_symlink
};
