/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
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
 *
 */

#define EXPORT_SYMTAB

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>

#define DEBUG_SUBSYSTEM S_RPC

#include <linux/obd_support.h>
#include <linux/obd_class.h>
#include <linux/lustre_net.h>

int ptlrpc_enqueue(struct ptlrpc_client *peer, struct ptlrpc_request *req)
{
        struct ptlrpc_request *srv_req;
        
        if (!peer->cli_obd) { 
                EXIT;
                return -1;
        }

        OBD_ALLOC(srv_req, sizeof(*srv_req));
        if (!srv_req) { 
                EXIT;
                return -ENOMEM;
        }

        CDEBUG(0, "peer obd minor %d, incoming req %p, srv_req %p\n",
               peer->cli_obd->obd_minor, req, srv_req);

        memset(srv_req, 0, sizeof(*req)); 

        /* move the request buffer */
        srv_req->rq_reqbuf = req->rq_reqbuf;
        srv_req->rq_reqlen = req->rq_reqlen;
        srv_req->rq_obd = peer->cli_obd;

        /* remember where it came from */
        srv_req->rq_reply_handle = req;

        spin_lock(&peer->cli_lock);
        list_add(&srv_req->rq_list, &peer->cli_obd->obd_req_list); 
        spin_unlock(&peer->cli_lock);
        wake_up(&peer->cli_obd->obd_req_waitq);
        return 0;
}

int ptlrpc_connect_client(int dev, char *uuid, int req_portal, int rep_portal, 
                          req_pack_t req_pack, rep_unpack_t rep_unpack,
                          struct ptlrpc_client *cl)
{
        int err; 

        memset(cl, 0, sizeof(*cl));
        spin_lock_init(&cl->cli_lock);
        cl->cli_xid = 1;
        cl->cli_obd = NULL; 
        cl->cli_request_portal = req_portal;
        cl->cli_reply_portal = rep_portal;
        cl->cli_rep_unpack = rep_unpack;
        cl->cli_req_pack = req_pack;
        sema_init(&cl->cli_rpc_sem, 32);

        /* non networked client */
        if (dev >= 0 && dev < MAX_OBD_DEVICES) {
                struct obd_device *obd = &obd_dev[dev];
                
                if ((!obd->obd_flags & OBD_ATTACHED) ||
                    (!obd->obd_flags & OBD_SET_UP)) { 
                        CERROR("target device %d not att or setup\n", dev);
                        return -EINVAL;
                }
                if (strcmp(obd->obd_type->typ_name, "ost") && 
                    strcmp(obd->obd_type->typ_name, "mds")) { 
                        return -EINVAL;
                }

                cl->cli_obd = &obd_dev[dev];
                return 0;
        }

        /* networked */
        err = kportal_uuid_to_peer(uuid, &cl->cli_server);
        if (err != 0)
                CERROR("cannot find peer %s!\n", uuid);

        return err;
}

struct ptlrpc_bulk_desc *ptlrpc_prep_bulk(struct lustre_peer *peer)
{
        struct ptlrpc_bulk_desc *bulk;

        OBD_ALLOC(bulk, sizeof(*bulk));
        if (bulk != NULL) {
                memset(bulk, 0, sizeof(*bulk));
                memcpy(&bulk->b_peer, peer, sizeof(*peer));
                init_waitqueue_head(&bulk->b_waitq);
        }

        return bulk;
}

struct ptlrpc_request *ptlrpc_prep_req(struct ptlrpc_client *cl, 
                                       int opcode, int namelen, char *name,
                                       int tgtlen, char *tgt)
{
        struct ptlrpc_request *request;
        int rc;
        ENTRY; 

        OBD_ALLOC(request, sizeof(*request));
        if (!request) { 
                CERROR("request allocation out of memory\n");
                return NULL;
        }

        memset(request, 0, sizeof(*request));
        //spin_lock_init(&request->rq_lock);

        spin_lock(&cl->cli_lock);
        request->rq_xid = cl->cli_xid++;
        spin_unlock(&cl->cli_lock);

        rc = cl->cli_req_pack(name, namelen, tgt, tgtlen,
                          &request->rq_reqhdr, &request->rq_req,
                          &request->rq_reqlen, &request->rq_reqbuf);
        if (rc) { 
                CERROR("cannot pack request %d\n", rc); 
                return NULL;
        }
        request->rq_reqhdr->opc = opcode;
        request->rq_reqhdr->xid = request->rq_xid;

        EXIT;
        return request;
}

void ptlrpc_free_req(struct ptlrpc_request *request)
{
        if (request == NULL)
                return;

        if (request->rq_repbuf != NULL)
                OBD_FREE(request->rq_repbuf, request->rq_replen);
        OBD_FREE(request, sizeof(*request));
}

static int ptlrpc_check_reply(struct ptlrpc_request *req)
{
        int rc = 0;

        if (req->rq_repbuf != NULL) {
                req->rq_flags = PTL_RPC_REPLY;
                GOTO(out, rc = 1);
        }

        if (sigismember(&(current->pending.signal), SIGKILL) ||
            sigismember(&(current->pending.signal), SIGTERM) ||
            sigismember(&(current->pending.signal), SIGINT)) { 
                req->rq_flags = PTL_RPC_INTR;
                GOTO(out, rc = 1);
        }

 out:
        return rc;
}

int ptlrpc_check_status(struct ptlrpc_request *req, int err)
{
        ENTRY;

        if (err != 0) {
                CERROR("err is %d\n", err);
                EXIT;
                return err;
        }

        if (req == NULL) {
                CERROR("req == NULL\n");
                EXIT;
                return -ENOMEM;
        }

        if (req->rq_rephdr == NULL) {
                CERROR("req->rq_rephdr == NULL\n");
                EXIT;
                return -ENOMEM;
        }

        if (req->rq_rephdr->status != 0) {
                CERROR("req->rq_rephdr->status is %d\n",
                       req->rq_rephdr->status);
                EXIT;
                /* XXX: translate this error from net to host */
                return req->rq_rephdr->status;
        }

        EXIT;
        return 0;
}

/* Abort this request and cleanup any resources associated with it. */
int ptlrpc_abort(struct ptlrpc_request *request)
{
        /* First remove the ME for the reply; in theory, this means
         * that we can tear down the buffer safely. */
        //spin_lock(&request->rq_lock);
        PtlMEUnlink(request->rq_reply_me_h);
        OBD_FREE(request->rq_repbuf, request->rq_replen);
        request->rq_repbuf = NULL;
        request->rq_replen = 0;
        //spin_unlock(&request->rq_lock);

        return 0;
}

int ptlrpc_queue_wait(struct ptlrpc_client *cl, struct ptlrpc_request *req)
{
        int rc = 0;
        ENTRY;

        init_waitqueue_head(&req->rq_wait_for_rep);

        if (cl->cli_obd) {
                /* Local delivery */
                down(&cl->cli_rpc_sem);
                rc = ptlrpc_enqueue(cl, req); 
        } else {
                /* Remote delivery via portals. */
                req->rq_req_portal = cl->cli_request_portal;
                req->rq_reply_portal = cl->cli_reply_portal;
                rc = ptl_send_rpc(req, cl);
        }
        if (rc) { 
                CERROR("error %d, opcode %d\n", rc, req->rq_reqhdr->opc);
                up(&cl->cli_rpc_sem);
                RETURN(-rc);
        }

        CDEBUG(D_OTHER, "-- sleeping\n");
        wait_event_interruptible(req->rq_wait_for_rep, ptlrpc_check_reply(req));
        CDEBUG(D_OTHER, "-- done\n");
        up(&cl->cli_rpc_sem);
        if (req->rq_flags == PTL_RPC_INTR) { 
                /* Clean up the dangling reply buffers */
                ptlrpc_abort(req);
                GOTO(out, rc = -EINTR);
        }

        if (req->rq_flags != PTL_RPC_REPLY) { 
                CERROR("Unknown reason for wakeup\n");
                /* XXX Phil - I end up here when I kill obdctl */
                ptlrpc_abort(req); 
                GOTO(out, rc = -EINTR);
        }

        rc = cl->cli_rep_unpack(req->rq_repbuf, req->rq_replen,
                                &req->rq_rephdr, &req->rq_rep);
        if (rc) {
                CERROR("unpack_rep failed: %d\n", rc);
                GOTO(out, rc);
        }
        CDEBUG(D_NET, "got rep %d\n", req->rq_rephdr->xid);

        if ( req->rq_rephdr->status == 0 )
                CDEBUG(D_NET, "--> buf %p len %d status %d\n", req->rq_repbuf,
                       req->rq_replen, req->rq_rephdr->status);

        EXIT;
 out:
        return rc;
}
