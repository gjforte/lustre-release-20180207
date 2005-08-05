/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/lvfs/lvfs_mount.c
 *  Client/server mount routines
 *
 *  Copyright (c) 2005 Cluster File Systems, Inc.
 *   Author: Nathan Rutman <nathan@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org/
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <linux/obd.h>
#include <linux/lvfs.h>
#include <linux/lustre_disk.h>
#include <linux/lustre_fsfilt.h>
#include <linux/obd_class.h>
#include <lustre/lustre_user.h>

static int (*client_fill_super)(struct super_block *sb) = NULL;

static int dentry_readdir(struct obd_device *obd, struct dentry *dir, 
                          struct vfsmount *inmnt, struct list_head *dentry_list)
{
        /* see mds_cleanup_orphans */
        struct lvfs_run_ctxt saved;
        struct file *file;
        struct dentry *dentry;
        struct vfsmount *mnt;
        int err = 0;
        ENTRY;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        dentry = dget(dir);
        if (IS_ERR(dentry))
                GOTO(out_pop, err = PTR_ERR(dentry));
        mnt = mntget(inmnt);
        if (IS_ERR(mnt)) { 
                l_dput(dentry);
                GOTO(out_pop, err = PTR_ERR(mnt));
        }

        file = dentry_open(dentry, mnt, O_RDONLY);
        if (IS_ERR(file))
                /* dentry_open_it() drops the dentry, mnt refs */
                GOTO(out_pop, err = PTR_ERR(file));

        INIT_LIST_HEAD(dentry_list);
        err = l_readdir(file, dentry_list);
        filp_close(file, 0);
        /*  filp_close->fput() drops the dentry, mnt refs */

out_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        RETURN(err);
}

int parse_mount_data(struct lvfs_run_ctxt *mount_ctxt, 
                     struct lustre_disk_data *ldd)
{
        struct lvfs_run_ctxt saved;
        struct file *file;
        loff_t off = 0;
        unsigned long len;
        int err;

        push_ctxt(&saved, mount_ctxt, NULL);
        
        file = filp_open(MOUNT_DATA_FILE, O_RDONLY, 0644);
        if (IS_ERR(file)) {
                err = PTR_ERR(file);
                CERROR("cannot open %s: err = %d\n", MOUNT_DATA_FILE, err);
                goto out;
        }
 
        len = file->f_dentry->d_inode->i_size;
        CERROR("Have %s, size %lu\n", MOUNT_DATA_FILE, len);

        err = lustre_fread(file, ldd, len, &off);
        if (err) {
                CERROR("OBD filter: error reading %s: err %d\n", 
                       MOUNT_DATA_FILE, err);
                goto out_close;
        }
        if (ldd->ldd_magic != LDD_MAGIC) {
                CERROR("Bad magic in %s: %x!=%x\n", MOUNT_DATA_FILE, 
                       ldd->ldd_magic, LDD_MAGIC);
                err = -EINVAL;
        }

out_close:
        filp_close(file, 0);
out:
        pop_ctxt(&saved, mount_ctxt, NULL);
        return(err);
}

int parse_last_rcvd(struct obd_device *obd, char *uuid, int *first_mount)
{
        struct lvfs_run_ctxt saved;
        struct file *file;
        struct lr_server_data *lsd;
        loff_t off = 0;
        int err;
 
        OBD_ALLOC_WAIT(lsd, sizeof(*lsd));
        if (!lsd)
                return -ENOMEM;
 
        /* requires a mounted device */
        LASSERT(obd);
         /*setup llog ctxt*/
        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        /* open and test the last rcvd file */
        file = filp_open(LAST_RCVD, O_RDONLY, 0644);
        if (IS_ERR(file)) {
                err = PTR_ERR(file);
                CERROR("cannot open %s file: err = %d\n", LAST_RCVD, err);
                goto out;
        }
 
        CERROR("Have last_rcvd, size %lu\n",
               (unsigned long)file->f_dentry->d_inode->i_size);
        err = fsfilt_read_record(obd, file, lsd, sizeof(*lsd), &off);
        if (err) {
                CERROR("OBD filter: error reading %s: err %d\n", LAST_RCVD, err);
                goto out_close;
        }
 
        strcpy(uuid, lsd->lsd_uuid);
        *first_mount = (lsd->lsd_mount_count == 0);
        CERROR("UUID from %s: %s, init=%d\n", LAST_RCVD, uuid, *first_mount);
 
out_close:
        filp_close(file, 0);
out:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        OBD_FREE(lsd, sizeof(*lsd));
        return(err);
}

static int do_lcfg(char *cfgname, ptl_nid_t nid, int cmd,
                   char *s1, char *s2, char *s3, char *s4)
{
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg    * lcfg = NULL;
        int err;

        lustre_cfg_bufs_reset(&bufs, cfgname);
        if (s1) 
                lustre_cfg_bufs_set_string(&bufs, 1, s1);
        if (s2) 
                lustre_cfg_bufs_set_string(&bufs, 2, s2);
        if (s3) 
                lustre_cfg_bufs_set_string(&bufs, 3, s3);
        if (s4) 
                lustre_cfg_bufs_set_string(&bufs, 4, s4);

        lcfg = lustre_cfg_new(cmd, &bufs);
        lcfg->lcfg_nid = nid;
        err = class_process_config(lcfg);
        lustre_cfg_free(lcfg);
        return(err);
}

int class_manual_cleanup(struct obd_device *obd, char *flags)
{
        int err, rc = 0;
        
        LASSERT(obd);
        err = do_lcfg(obd->obd_name, 0, LCFG_CLEANUP, flags, 0, 0, 0);
        if (err) {
                CERROR("cleanup failed (%d): %s\n", err, obd->obd_name);
                rc = err;
        }
        
        err = do_lcfg(obd->obd_name, 0, LCFG_DETACH, 0, 0, 0, 0);
        if (err) {
                CERROR("detach failed (%d): %s\n", err, obd->obd_name);
                if (!rc) 
                        rc = err;
        }

        return(rc);
}

struct lustre_sb_info *lustre_init_sbi(struct super_block *sb)
{
        struct lustre_sb_info *sbi = NULL;

        OBD_ALLOC(sbi, sizeof(*sbi));
        if (!sbi)
                RETURN(NULL);

        s2sbi_nocast(sb) = sbi;
        return(sbi);
}

void lustre_free_sbi(struct super_block *sb)
{
        struct lustre_sb_info *sbi = s2sbi(sb);
        ENTRY;

        if (sbi != NULL) {
                if (sbi->lsi_ldd != NULL) 
                        OBD_FREE(sbi->lsi_ldd, sizeof(*sbi->lsi_ldd));
                if (sbi->lsi_lmd != NULL) 
                        OBD_FREE(sbi->lsi_lmd, sizeof(*sbi->lsi_lmd));
                LASSERT(sbi->lsi_llsbi == NULL);
                OBD_FREE(sbi, sizeof(*sbi));
                s2sbi_nocast(sb) = NULL;
        }
        EXIT;
}
           
static int lustre_update_llog(struct obd_device *obd)
{
        int err = 0;

        // FIXME this should be called from lov_add_obd?
#if 0
        if (mgcobd->cfobd_logs_info.ost_number > 0) {
                struct obd_ioctl_data ioc_data = { 0 };
                CERROR("update new logs.\n");
                err = obd_iocontrol(OBD_IOC_UPDATE_LOG, obd->obd_self_export,
                                    sizeof ioc_data, &ioc_data, NULL);
                if (err)
                        CERROR("Failed to Update logs. \n");
        }
#endif
      return err;
}

static void server_put_super(struct super_block *sb)
{
        struct list_head dentry_list;
        struct lustre_sb_info *sbi = s2sbi(sb);
        struct l_linux_dirent *dirent, *n;
        struct obd_device *obd;
        struct mgc_obd *mgcobd;
        char flags[2] = "";
        int err;
                                                                                       
        obd = sbi->lsi_mgc;
        CERROR("server put_super %s\n", obd->obd_name);
        mgcobd = &obd->u.mgc;
                                                                                       
        lustre_update_llog(obd);
                                                                                       
        //FIXME cleanup does the mntput; we have to make sure MGS is done with
        //it as well

        if (sbi->lsi_flags & LSI_UMOUNT_FORCE)
                strcat(flags, "A");
        if (sbi->lsi_flags & LSI_UMOUNT_FAILOVER)
                strcat(flags, "F");

        /* Clean up all the -conf obd's in the LOGS directory.
            FIXME this may not be complete / reasonable.
            Really, we should have a list of every obd we started,
            maybe an additional field to obd_device for group_uuid, then
            just use lustre_manual_cleanup.
            CRAY_MDS:
                 client  client-clean  mdsA  mdsA-clean  mdsA-conf
            CRAY_OST:
                 OSS-conf  OST_uml2-conf
            This does clean up oss, ost, mds, but not mdt. mdt is set up
            as part of mdsA-conf.
         */
        
        /* Find all the logs in the CONFIGS directory */
        err = dentry_readdir(obd, mgcobd->mgc_configs_dir,
                       mgcobd->mgc_vfsmnt, &dentry_list);
        if (err)
                CERROR("Can't read LOGS dir, %d\n", err);
                                                                                       
        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                char *logname;
                int len;

                list_del(&dirent->lld_list);
                
                logname = dirent->lld_name;
                /* Confobd start adds "-conf" */
                len = strlen(logname) - 5;
                if ((len < 1) || strcmp(logname, "-conf")) {
                        CDEBUG(D_CONFIG, "ignoring %s\n", logname);
                        OBD_FREE(dirent, sizeof(*dirent));
                        continue;
                }
                logname[len] = 0;
                                                                                       
                obd = class_name2obd(logname);
                if (!obd) {
                        CERROR("no obd %s\n", logname);
                        continue;
                }
                                                                                       
                CERROR("stopping %s\n", logname);
                err = class_manual_cleanup(obd, flags);
                if (err) {
                        CERROR("failed to cleanup %s: %d\n", logname, err);
                }
                OBD_FREE(dirent, sizeof(*dirent));
        }
                                                                                       
        /* FIXME so until we decide the above, completly evil hack 
        the MDT, soon to be known as the MDS, will be started at insmod and 
        removed at rmmod, so take out of here. */
        obd = class_name2obd("MDT");
        if (obd)
                class_manual_cleanup(obd, flags);

        class_del_profile(sbi->lsi_ldd->ldd_svname);
        lustre_common_put_super(sb);
}

static void server_umount_begin(struct super_block *sb)
{
        struct lustre_sb_info *sbi = s2sbi(sb);
                                                                                       
        CERROR("Umount -f\n");
        // FIXME decide FORCE or FAILOVER based on mount option -o umount=failover
        sbi->lsi_flags |= LSI_UMOUNT_FORCE;
}

static struct super_operations server_ops =
{
        //.statfs         = NULL,
        .put_super      = server_put_super,
        .umount_begin   = server_umount_begin, /* umount -f */
};

#define log2(n) ffz(~(n))
#define LUSTRE_SUPER_MAGIC 0x0BD00BD1

static int server_fill_super_common(struct super_block *sb)
{
        struct inode *root = 0;
        //struct ll_sb_info *sbi = ll_s2sbi(sb);
        ENTRY;
                                                                                 
        CERROR("Server sb, dev=%d\n", (int)sb->s_dev);
                                                                                 
        sb->s_blocksize = 4096;
        sb->s_blocksize_bits = log2(sb->s_blocksize);
        sb->s_magic = LUSTRE_SUPER_MAGIC;
        sb->s_maxbytes = 0; //PAGE_CACHE_MAXBYTES;
        sb->s_flags |= MS_RDONLY;
        sb->s_op = &server_ops;
 
        root = new_inode(sb);
        if (!root) {
                CERROR("Can't make root inode\n");
                RETURN(-EIO);
        }
                                                                                 
        /* returns -EIO for every operation */
        /* make_bad_inode(root); -- badness - can't umount */
        /* apparently we need to be a directory for the mount to finish */
        root->i_mode = S_IFDIR;
                                                                                 
        sb->s_root = d_alloc_root(root);
        if (!sb->s_root) {
                CERROR("Can't make root dentry\n");
                iput(root);
                RETURN(-EIO);
        }
                                                                                 
        RETURN(0);
}
                           

/* Get the log "profile" from a remote MGMT and process it.
  FIXME  If remote doesn't exist, try local
  This func should work for both clients and servers */
int lustre_get_process_log(struct super_block *sb, char * profile,
                       struct config_llog_instance *cfg)
{
#if 0
FIXME
        char  *peer = "MDS_PEER_UUID";
        struct obd_device *obd;
        struct lustre_handle mdc_conn = {0, };
        struct obd_export *exp;
        char  *mdcname = "mdc_dev";
        char  *mdsname;
        class_uuid_t uuid;
        struct obd_uuid mdc_uuid;
        struct llog_ctxt *ctxt;
        int allow_recov = (lmd->lmd_flags & LMD_FLG_RECOVER) > 0;
        int err, rc = 0;
        ENTRY;

        LASSERT(lmd->lmd_mgmtnid.primary != PTL_NID_ANY);

        //lustre_generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &mdc_uuid);
        CDEBUG(D_HA, "generated uuid: %s\n", mdc_uuid.uuid);

        err = do_lcfg(mdcname, lmd->lmd_mgmtnid.primary, LCFG_ADD_UUID, 
                      peer, 0, 0, 0)
        if (err < 0)
                GOTO(out, err);

        /* FIXME setup MGC, not MDC */
        err = do_lcfg(mdcname, 0, LCFG_ATTACH, 
                      LUSTRE_MDC_NAME, mdc_uuid.uuid, 0, 0)
        if (err < 0)
                GOTO(out_del_uuid, err);

        /* FIXME get the mds name from the mgmt node */
        sprintf(mdsname, "%s-mds0001", lmd->lmd_dev);
        CERROR("MDS device: %s @ %s\n", mdsname, libcfs_nid2str(lcfg->lcfg_nid));
        err = do_lcfg(mdcname, 0, LCFG_SETUP, 
                      mdsname, peer, 0, 0)
        if (err < 0)
                GOTO(out_detach, err);

        obd = class_name2obd(mdcname);
        if (obd == NULL)
                GOTO(out_cleanup, err = -EINVAL);

        /* Disable initial recovery on this import */
        err = obd_set_info(obd->obd_self_export,
                           strlen("initial_recov"), "initial_recov",
                           sizeof(allow_recov), &allow_recov);
        if (err)
                GOTO(out_cleanup, err);

        err = obd_connect(&mdc_conn, obd, &mdc_uuid, NULL /* ocd */);
        if (err) {
                CERROR("cannot connect to %s: rc = %d\n", mdsname, err);
                GOTO(out_cleanup, err);
        }

        exp = class_conn2export(&mdc_conn);

        ctxt = llog_get_context(exp->exp_obd, LLOG_CONFIG_REPL_CTXT);
#if 1
        rc = class_config_parse_llog(ctxt, profile, cfg);
#else
        /*
         * For debugging, it's useful to just dump the log
         */
        rc = class_config_dump_llog(ctxt, profile, cfg);
#endif
        switch (rc) {
        case 0:
                break;
        case -EINVAL:
                LCONSOLE_ERROR("%s: The configuration '%s' could not be read "
                               "from the MDS.  Make sure this client and the "
                               "MDS are running compatible versions of "
                               "Lustre.\n",
                               obd->obd_name, profile);
                /* fall through */
        default:
                CERROR("class_config_parse_llog failed: rc = %d\n", rc);
                break;
        }

        err = obd_disconnect(exp);

out_cleanup:
        err = do_lcfg(mdcname, 0, LCFG_CLEANUP, 
                      0, 0, 0, 0)
        if (err < 0)
                GOTO(out, err);

out_detach:
        err = do_lcfg(mdcname, 0, LCFG_DETACH, 
                      0, 0, 0, 0)
        if (err < 0)
                GOTO(out, err);

out_del_uuid:
        err = do_lcfg(mdcname, 0, LCFG_DEL_UUID, 
                      peer, 0, 0, 0)
out:
        if (rc == 0)
                rc = err;

        RETURN(rc);
#endif
}

/* Process all local logs.
FIXME clients and servers should use the same fn. No need to have MDS 
do client and confobd do servers. MGC should do both. */
int lustre_process_logs(struct super_block *sb,
                        struct config_llog_instance *cfg, int allow_recov)
{
        char lr_uuid[40];
        struct obd_ioctl_data ioc_data = { 0 };
        struct list_head dentry_list;
        struct l_linux_dirent *dirent, *n;
        struct obd_device *obd;
        struct mgc_obd *mgcobd;
        struct lustre_sb_info *sbi = s2sbi(sb);
        int is_first_mount = 0;
        int err;
                                                                                       
        obd = sbi->lsi_mgc;
        LASSERT(obd);
        mgcobd = &obd->u.mgc;
                                                                                       
        err = parse_last_rcvd(obd, lr_uuid, &is_first_mount);
        if (err) {
                CERROR("Can't read %s\n", LAST_RCVD);
                return(err);
        }
                                                                                       
        if ((strncmp(lr_uuid, "OST", 3) == 0) && is_first_mount) {
                /* Always register with MGS.  If this is the first mount
                   for an OST, we might have to change our name */
                err = 1;//ost_register(lmd, lr_uuid);
                if (err) {
                        CERROR("OST register Failed\n");
                        return(err);
                }
        } else if (strncmp(lr_uuid, "MDS", 3) == 0) {
   #if 0 
                //FIXME stripe count is set in the mds llog
                uint32_t stripe_size;
                err = get_stripe_size(obd, &stripe_size);
                if (err) {
                        CERROR("Can't read %s\n", STRIPE_FILE);
                        return(err);
                }
                mgcobd_start_accept(obd, lmd, lr_uuid, stripe_size);
   #endif                                                                       
        }
                                                                                       
        /* Find all the logs in the CONFIGS directory */
        err = dentry_readdir(obd, mgcobd->mgc_configs_dir,
                       mgcobd->mgc_vfsmnt, &dentry_list);
        if (err) {
                CERROR("Can't read LOGS dir\n");
                return(err);
        }
                                                                                       
        /* Start up all the -conf logs in the CONFIGS directory */
        list_for_each_entry_safe(dirent, n, &dentry_list, lld_list) {
                char *logname;
                int len;

                list_del(&dirent->lld_list);
                logname = dirent->lld_name;
                                                                                       
                /* Confobd start adds "-conf" */
                len = strlen(logname) - 5;
                if ((len < 1) || (strcmp(logname + len, "-conf") != 0)) {
                        CDEBUG(D_CONFIG, "ignoring %s\n", logname);
                        OBD_FREE(dirent, sizeof(*dirent));
                        continue;
                }
                logname[len] = 0;
                                                                                       
                CERROR("starting log %s\n", logname);
                ioc_data.ioc_inllen1 = len + 1;
                ioc_data.ioc_inlbuf1 = logname;
                err = obd_iocontrol(OBD_IOC_START, obd->obd_self_export,
                                    sizeof ioc_data, &ioc_data, NULL);
                if (err) {
                        CERROR("failed to start log %s: %d\n", logname, err);
                }
                OBD_FREE(dirent, sizeof(*dirent));
        }
                                                                                       
        return(err);
}
                                                 
int mgc_fs_setup(struct super_block *sb, struct vfsmount *mnt)
{
        struct lvfs_run_ctxt saved;
        struct lustre_sb_info *sbi = s2sbi(sb);
        struct obd_device *obd = sbi->lsi_mgc;
        struct mgc_obd *mgcobd = &obd->u.mgc;
        struct dentry *dentry;
        int err;

        LASSERT(obd);

        obd->obd_fsops = fsfilt_get_ops(MT_STR(sbi->lsi_ldd))
        if (IS_ERR(obd->obd_fsops)) {
               CERROR("No fstype %s rc=%ld\n", MT_STR(sbi->lsi_ldd), 
                      PTR_ERR(obd->obd_fsops));
               return(PTR_ERR(obd->obd_fsops));
        }

        mgcobd->mgc_vfsmnt = mnt;
        mgcobd->mgc_sb = mnt->mnt_root->d_inode->i_sb; // is this different than sb? */
        fsfilt_setup(obd, mgcobd->mgc_sb);

        OBD_SET_CTXT_MAGIC(&obd->obd_lvfs_ctxt);
        obd->obd_lvfs_ctxt.pwdmnt = mnt;
        obd->obd_lvfs_ctxt.pwd = mnt->mnt_root;
        obd->obd_lvfs_ctxt.fs = get_ds();
        //obd->obd_lvfs_ctxt.cb_ops = mds_lvfs_ops;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        dentry = lookup_one_len(MOUNT_CONFIGS_DIR, current->fs->pwd,
                                strlen(MOUNT_CONFIGS_DIR));
        if (IS_ERR(dentry)) {
                err = PTR_ERR(dentry);
                CERROR("cannot lookup %s directory: rc = %d\n", 
                       MOUNT_CONFIGS_DIR, err);
                goto err_ops;
        }
        mgcobd->mgc_configs_dir = dentry;
     #if 0   
        err = llog_setup(obd, LLOG_CONFIG_ORIG_CTXT, obd, 0,
                            NULL, &llog_lvfs_ops);
        if (err)
                goto err_dput;
     #endif
         
err_pop:
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        return(err);
err_dput:
        dput(mgcobd->mgc_configs_dir);
err_ops:        
        fsfilt_put_ops(obd->obd_fsops);
        obd->obd_fsops = NULL;
        goto err_pop;
}

int mgc_fs_cleanup(struct obdclass *obd)
{
        llog_cleanup(llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT));
        
        if (mgcobd->mgc_configs_dir != NULL) {
                struct lvfs_run_ctxt saved;
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                l_dput(mgcobd->mgc_configs_dir);
                mgcobd->mgc_configs_dir = NULL; 
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        }

        if (mgcobd->mgc_vfsmnt)
                // FIXME mntput should not be done by real server, only us */
                mntput(mgcobd->mgc_vfsmnt);
        mgc->mgc_sb = NULL;
        
        if (obd->obd_fsops) 
                fsfilt_put_ops(obd->obd_fsops);
        return(0);
}


/* Kernel mount using mount options in MOUNT_DATA_FILE */
static struct vfsmount *lustre_kern_mount(struct super_block *sb)
{
        struct lvfs_run_ctxt mount_ctxt;
        struct lustre_sb_info *sbi = s2sbi(sb);
        struct lustre_disk_data *ldd;
        struct lustre_mount_data *lmd = sbi->lsi_lmd;
        struct obd_device *obd;
        struct vfsmount *mnt;
        char *options = NULL;
        unsigned long page;
        int err;

        OBD_ALLOC(ldd, sizeof(*ldd));
        if (!ldd)
                return(-ENOMEM);

        /* Pre-mount ext3 with no options to read the MOUNT_DATA_FILE */
        CERROR("Pre-mount ext3 %s\n", lmd->lmd_dev);
        mnt = do_kern_mount("ext3", 0, lmd->lmd_dev, 0);
        if (IS_ERR(mnt)) {
                err = PTR_ERR(mnt);
                CERROR("premount failed: err = %d\n", err);
                goto out_free;
        }

        OBD_SET_CTXT_MAGIC(&mount_ctxt);
        mount_ctxt.pwdmnt = mnt;
        mount_ctxt.pwd = mnt->mnt_root;
        mount_ctxt.fs = get_ds();
        //mount_ctxt.cb_ops = mds_lvfs_ops;

        err = parse_mount_data(&mount_ctxt, ldd); 
        //unlock_kernel();
        mntput(mnt);
        //lock_kernel();

        if (err) {
                CERROR("premount parse options failed: err = %d\n", err);
                goto out_free;
        }

        /* Done with our pre-mount, now do the real mount. */

        /* Glom up mount options */
        page = __get_free_page(GFP_KERNEL);
        if (!page) {
                err = -ENOMEM;
                goto out_free;
        }
        options = (char *)page;
        memset(options, 0, PAGE_SIZE);
        strcpy(options, ldd->ldd_mount_opts);
        if (strlen(lmd->lmd_opts)) {
                if (strlen(options)) 
                        strcat(options, ",");
                strcat(options, ldd->ldd_mount_opts);
        }

        CERROR("kern_mount: %s %s %s\n", MT_STR(ldd), lmd->lmd_dev, options);

        mnt = do_kern_mount(MT_STR(ldd), 0, lmd->lmd_dev, (void *)options);
        free_page(page);
        if (IS_ERR(mnt)) {
                err = PTR_ERR(mnt);
                CERROR("do_kern_mount failed: err = %d\n", err);
                goto out_free;
        }
        
        sbi->lsi_ldd = ldd;   /* freed at sbi cleanup */
        CDEBUG(D_SUPER, "%s: mnt = %p\n", lmd->lmd_dev, mnt);
        return(mnt);

out_free:
        OBD_FREE(ldd, sizeof(*ldd));
        sbi->lsi_ldd = NULL;    
        return(ERR_PTR(err));
}

static int server_fill_super(struct super_block *sb)
{
        struct config_llog_instance cfg;
        struct lustre_sb_info *sbi = s2sbi(sb);
        struct vfsmount *mnt;
        int err;

        cfg.cfg_instance = sbi->lsi_mgc->obd_name;
        snprintf(cfg.cfg_uuid.uuid, sizeof(cfg.cfg_uuid.uuid), 
                 sbi->lsi_mgc->obd_name);

        /* mount to read server info */
        mnt = lustre_kern_mount(sb);
        if (IS_ERR(mnt)) {
                CERROR("Unable to mount device %s: %d\n", 
                      sbi->lsi_lmd->lmd_dev, err);
                GOTO(out_free, err = PTR_ERR(mnt));
        }
        CERROR("Found service %s for fs %s on device %s\n",
               sbi->lsi_ldd->ldd_svname, sbi->lsi_ldd->ldd_fsname, 
               sbi->lsi_lmd->lmd_dev);
        
        err = mgc_fs_setup(sb, mnt);
        if (err) 
                goto out_free;
        
        /* Set up all obd devices for service */
        err = lustre_process_logs(sb, &cfg, 0);
        if (err < 0) {
                CERROR("Unable to process log: %d\n", err);
                GOTO(out_free, err);
        }
        
        /* Finally, put something at the mount point. */
        CERROR("Mounting server\n");
        err = server_fill_super_common(sb);
       
        /* FIXME overmount client here,
           or can we just start a client log and client_fill_super on this sb? 
           have to fix up the s_ops after! */
out_free:
        //FIXME mntput
        //FIXME manual_cleanup (server_put_super)
        if (sbi->lsi_ldd)
                class_del_profile(sbi->lsi_ldd->ldd_svname);
        return (err);
}

/* Set up a mgcobd to process startup logs */
static int lustre_start_mgc(struct super_block *sb)
{
        struct config_llog_instance cfg;
        struct lustre_sb_info *sbi = s2sbi(sb);
        struct obd_device *obd;
        char *mcname;
        int mcname_sz = sizeof(sb) * 2 + 5;
        int err = 0;

        /* Generate a string unique to this super, let's try
           the address of the super itself.*/
        OBD_ALLOC(mcname, mcname_sz);
        sprintf(mcname, "MGC_%p", sb);
        cfg.cfg_instance = mcname;
        snprintf(cfg.cfg_uuid.uuid, sizeof(cfg.cfg_uuid.uuid), mcname);
 
        err = do_lcfg(mcname, 0, LCFG_ATTACH, "mgc", cfg.cfg_uuid.uuid, 0, 0);
        if (err)
                goto out_free;
                                                                                       
        err = do_lcfg(mcname, 0, LCFG_SETUP, 0, 0, 0, 0);
        if (err) {
                CERROR("mgcobd setup error %d\n", err);
                do_lcfg(mcname, 0, LCFG_DETACH, 0, 0, 0, 0);
                goto out_free;
        }
        
        obd = class_name2obd(mcname);
        if (!obd) {
                CERROR("Can't find mgcobd %s\n", mcname);
                err = -ENOTCONN;
                goto out_free;
        }
        sbi->lsi_mgc = obd;
        
out_free:
        OBD_FREE(mcname, mcname_sz);
        return err;
}

static void lustre_stop_mgc(struct super_block *sb)
{
        struct lustre_sb_info *sbi = s2sbi(sb);
        struct obd_device *obd;

        obd = sbi->lsi_mgc;

        /* FIXME this should be called from mgc_cleanup from manual_cleanup */
        if (mgcobd->mgc_sb)
                /* if we're a server, eg. something's mounted */
                mgc_fs_cleanup(obd);

        class_manual_cleanup(obd, NULL);
}

/* Common umount */
void lustre_common_put_super(struct super_block *sb)
{
        CERROR("common put super %p\n", sb);

        lustre_stop_mgc(sb);
        lustre_free_sbi(sb);
}      

/* Common mount */
int lustre_fill_super(struct super_block *sb, void *data, int silent)
{
        struct lustre_mount_data * lmd = data;
        struct lustre_sb_info *sbi;
        int err;
        ENTRY;
 
        CDEBUG(D_VFSTRACE, "VFS Op: sb %p\n", sb);
        if (lmd_bad_magic(lmd))
                RETURN(-EINVAL);
 
        sbi = lustre_init_sbi(sb);
        if (!sbi)
                RETURN(-ENOMEM);

        /* save mount data */
        OBD_ALLOC(sbi->lsi_lmd, sizeof(*sbi->lsi_lmd));
        if (sbi->lsi_lmd == NULL) {
                lustre_free_sbi(sb);
                RETURN(-ENOMEM);
        }
        memcpy(sbi->lsi_lmd, lmd, sizeof(*lmd));
        
        lustre_start_mgc(sb);

        if (lmd_is_client(lmd)) {
                if (!client_fill_super) {
                        CERROR("Nothing registered for client_fill_super!\n"
                               "Is llite module loaded?\n");
                        err = -ENOSYS;
                } else {
                        CERROR("Mounting client\n");
                        /* Connect and start */
                        /* (should always be ll_fill_super) */
                        err = (*client_fill_super)(sb);
                }
        } else {
                CERROR("Mounting server\n");
                err = server_fill_super(sb);
        }
                                                                                
        if (err){
                CERROR("Unable to mount %s\n", lmd->lmd_dev);
                lustre_stop_mgc(sb);
                lustre_free_sbi(sb);
        }
        RETURN(err);
} 
                                                                                

/* We can't call ll_fill_super by name because it lives in a module that
   must be loaded after this one. */
void lustre_register_client_fill_super(int (*cfs)(struct super_block *sb))
{
        client_fill_super = cfs;
}

EXPORT_SYMBOL(lustre_fill_super);
EXPORT_SYMBOL(lustre_register_client_fill_super);
EXPORT_SYMBOL(lustre_common_put_super);
EXPORT_SYMBOL(lustre_get_process_log);
EXPORT_SYMBOL(class_manual_cleanup);


