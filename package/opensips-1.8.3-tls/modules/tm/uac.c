/*
 * $Id: uac.c 9446 2012-11-22 17:39:57Z vladut-paiu $
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 *  2003-01-23  t_uac_dlg now uses get_out_socket (jiri)
 *  2003-01-27  fifo:t_uac_dlg completed (jiri)
 *  2003-01-29  scratchpad removed (jiri)
 *  2003-02-13  t_uac, t _uac_dlg, gethfblock, uri2proxy changed to use 
 *               proto & rb->dst (andrei)
 *  2003-02-27  FIFO/UAC now dumps reply -- good for CTD (jiri)
 *  2003-02-28  scratchpad compatibility abandoned (jiri)
 *  2003-03-01  kr set through a function now (jiri)
 *  2003-03-19  replaced all mallocs/frees w/ pkg_malloc/pkg_free (andrei)
 *  2003-04-02  port_no_str does not contain a leading ':' anymore (andrei)
 *  2003-07-08  appropriate log messages in check_params(...), 
 *               call calculate_hooks if next_hop==NULL in t_uac (dcm) 
 *  2003-10-24  updated to the new socket_info lists (andrei)
 *  2003-12-03  completion filed removed from transaction and uac callbacks
 *              merged in transaction callbacks as LOCAL_COMPLETED (bogdan)
 *  2004-02-11  FIFO/CANCEL + alignments (hash=f(callid,cseq)) (uli+jiri)
 *  2004-02-13  t->is_invite, t->local, t->noisy_ctimer replaced (bogdan)
 *  2004-08-23  avp support in t_uac (bogdan)
 *
 *
 * simple UAC for things such as SUBSCRIBE or SMS gateway;
 * no authentication and other UAC features -- just send
 * a message, retransmit and await a reply; forking is not
 * supported during client generation, in all other places
 * it is -- adding it should be simple
 */

#include <string.h>
#include "../../mem/shm_mem.h"
#include "../../dprint.h"
#include "../../md5.h"
#include "../../socket_info.h"
#include "../../receive.h"
#include "../../route.h"
#include "../../action.h"
#include "../../dset.h"
#include "../../data_lump.h"

#include "ut.h"
#include "h_table.h"
#include "t_hooks.h"
#include "t_funcs.h"
#include "t_msgbuilder.h"
#include "callid.h"
#include "uac.h"


#define FROM_TAG_LEN (MD5_LEN + 1 /* - */ + CRC16_LEN) /* length of FROM tags */

static char from_tag[FROM_TAG_LEN + 1];

 /* Enable/disable passing of provisional replies to FIFO applications */
int pass_provisional_replies = 0;

/* T holder for the last local transaction */
struct cell** last_localT;

/*
 * Initialize UAC
 */
int uac_init(void) 
{
	str src[3];
	struct socket_info *si;

	if (RAND_MAX < TM_TABLE_ENTRIES) {
		LM_WARN("uac does not spread across the whole hash table\n");
	}
	/* on tcp/tls bind_address is 0 so try to get the first address we listen
	 * on no matter the protocol */
	si=bind_address?bind_address:get_first_socket();
	if (si==0){
		LM_CRIT("null socket list\n");
		return -1;
	}

	/* calculate the initial From tag */
	src[0].s = "Long live SER server";
	src[0].len = strlen(src[0].s);
	src[1].s = si->address_str.s;
	src[1].len = strlen(src[1].s);
	src[2].s = si->port_no_str.s;
	src[2].len = strlen(src[2].s);

	MD5StringArray(from_tag, src, 3);
	from_tag[MD5_LEN] = '-';
	return 1;
}


/*
 * Generate a From tag
 */
void generate_fromtag(str* tag, str* callid)
{
	     /* calculate from tag from callid */
	crcitt_string_array(&from_tag[MD5_LEN + 1], callid, 1);
	tag->s = from_tag; 
	tag->len = FROM_TAG_LEN;
}


/*
 * Check value of parameters
 */
static inline int check_params(str* method, str* to, str* from, dlg_t** dialog)
{
	if (!method || !to || !from || !dialog) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	if (!method->s || !method->len) {
		LM_ERR("invalid request method\n");
		return -2;
	}

	if (!to->s || !to->len) {
		LM_ERR("invalid To URI\n");
		return -4;
	}

	if (!from->s || !from->len) {
		LM_ERR("invalid From URI\n");
		return -5;
	}
	return 0;
}

static inline unsigned int dlg2hash( dlg_t* dlg )
{
	str cseq_nr;
	unsigned int hashid;

	cseq_nr.s=int2str(dlg->loc_seq.value, &cseq_nr.len);
	hashid = tm_hash(dlg->id.call_id, cseq_nr);
	LM_DBG("%d\n", hashid);
	return hashid;
}


static inline struct sip_msg* buf_to_sip_msg(char *buf, unsigned int len,
															dlg_t *dialog)
{
	static struct sip_msg req;

	memset( &req, 0, sizeof(req) );
	req.id = get_next_msg_no();
	req.buf = buf;
	req.len = len;
	if (parse_msg(buf, len, &req)!=0) {
		LM_CRIT("BUG - buffer parsing failed!");
		return NULL;
	}
	/* populate some special fields in sip_msg */
	req.set_global_address=default_global_address;
	req.set_global_port=default_global_port;
	req.force_send_socket = dialog->send_sock; 
	if (set_dst_uri(&req, dialog->hooks.next_hop)) {
		LM_ERR("failed to set dst_uri");
		free_sip_msg(&req);
		return NULL;
	}
	req.rcv.proto = dialog->send_sock->proto;
	req.rcv.src_ip = req.rcv.dst_ip = dialog->send_sock->address;
	req.rcv.src_port = req.rcv.dst_port = dialog->send_sock->port_no;
	req.rcv.bind_address = dialog->send_sock;

	return &req;
}


/*
 * Send a request using data from the dialog structure
 */
int t_uac(str* method, str* headers, str* body, dlg_t* dialog,
				transaction_cb cb, void* cbp,release_tmcb_param release_func)
{
	union sockaddr_union to_su, new_to_su;
	struct cell *new_cell;
	struct retr_buf *request;
	static struct sip_msg *req;
	struct usr_avp **backup;
	char *buf, *buf1;
	int buf_len, buf_len1;
	int ret, flags, sflag_bk;
	int backup_route_type;
	unsigned int hi;
	struct socket_info *send_sock, *new_send_sock;
	str h_to, h_from, h_cseq, h_callid;

	ret=-1;
	
	/*** added by dcm 
	 * - needed by external ua to send a request within a dlg
	 */
	if(!dialog->hooks.next_hop && w_calculate_hooks(dialog)<0)
		goto error2;

	if(dialog->obp.s)
		dialog->hooks.next_hop = &dialog->obp;

	LM_DBG("next_hop=<%.*s>\n",dialog->hooks.next_hop->len,
			dialog->hooks.next_hop->s);

	/* calculate the socket corresponding to next hop */
	send_sock = uri2sock(0, dialog->hooks.next_hop, &to_su,
			PROTO_NONE);
	if (send_sock==0) {
		ret=ser_error;
		LM_ERR("no socket found\n");
		goto error2;
	}
	/* if a send socket defined verify if the same protocol */
	if(dialog->send_sock) {
		if(send_sock->proto != dialog->send_sock->proto)
		{
			dialog->send_sock = send_sock;
		}
	}
	else
	{
		dialog->send_sock = send_sock;
	}

	new_cell = build_cell(0);
	if (!new_cell) {
		ret=E_OUT_OF_MEM;
		LM_ERR("short of cell shmem\n");
		goto error2;
	}

	/* pass the transaction flags from dialog to transaction */
	new_cell->flags |= dialog->T_flags;

	/* add the callback the transaction for LOCAL_COMPLETED event */
	flags = TMCB_LOCAL_COMPLETED;
	/* Add also TMCB_LOCAL_RESPONSE_OUT if provisional replies are desired */
	if (pass_provisional_replies || pass_provisional(new_cell))
		flags |= TMCB_LOCAL_RESPONSE_OUT;
	if(cb && insert_tmcb(&(new_cell->tmcb_hl),flags,cb,cbp,release_func)!=1){
		ret=E_OUT_OF_MEM;
		LM_ERR("short of tmcb shmem\n");
		goto error2;
	}

	if (method->len==INVITE_LEN && memcmp(method->s, INVITE, INVITE_LEN)==0)
		new_cell->flags |= T_IS_INVITE_FLAG;
	new_cell->flags |= T_IS_LOCAL_FLAG;

	request = &new_cell->uac[0].request;
	if (dialog->forced_to_su.s.sa_family == AF_UNSPEC)
		request->dst.to = to_su;
	else
		request->dst.to = dialog->forced_to_su;
	request->dst.send_sock = dialog->send_sock;
	request->dst.proto = dialog->send_sock->proto;
	request->dst.proto_reserved1 = 0;

	hi=dlg2hash(dialog);
	LOCK_HASH(hi);
	insert_into_hash_table_unsafe(new_cell, hi);
	UNLOCK_HASH(hi);

	buf = build_uac_req(method, headers, body, dialog, 0, new_cell, &buf_len);
	if (!buf) {
		LM_ERR("failed to build message\n");
		ret=E_OUT_OF_MEM;
		goto error1;
	}

	if (local_rlist.a) {
		LM_DBG("building sip_msg from buffer\n");
		req = buf_to_sip_msg(buf, buf_len, dialog);
		if (req==NULL) {
			LM_ERR("failed to build sip_msg from buffer");
		} else {
			/* set transaction AVP list */
			backup = set_avp_list( &new_cell->user_avps );
			/* backup script flags */
			sflag_bk = getsflags();
			/* disable parallel forking */
			set_dset_state( 0 /*disable*/);

			/* run the route */
			swap_route_type( backup_route_type, LOCAL_ROUTE);
			run_top_route( local_rlist.a, req);
			set_route_type( backup_route_type );

			/* transfer current message context back to t */
			new_cell->uac[0].br_flags = req->flags;

			set_dset_state( 1 /*enable*/);
			setsflagsval(sflag_bk);
			set_avp_list( backup );

			/* check for changes - if none, do not regenerate the buffer */
			if (req->new_uri.s || req->add_rm || req->body_lumps || 
					req->dst_uri.len != dialog->hooks.next_hop->len ||
					memcmp(req->dst_uri.s,dialog->hooks.next_hop->s,req->dst_uri.len) != 0) {
				new_send_sock = NULL;

				/* do we also need to change the destination? */
				if (req->dst_uri.s || req->new_uri.s) {
					/* calculate the socket corresponding to next hop */
					new_send_sock = uri2sock(req,
						req->dst_uri.s ? &(req->dst_uri) : &req->new_uri,
						&new_to_su, PROTO_NONE );
					if (!new_send_sock) {
						LM_ERR("no socket found for the new destination\n");
						goto abort_update;
					}
				}

				/* if interface change, we need to re-build the via */
				if (new_send_sock && new_send_sock != dialog->send_sock) {
					LM_DBG("Interface change in local route. rebuilding via\n");
					if (!del_lump(req,req->h_via1->name.s - req->buf,req->h_via1->len,0)) {
						LM_ERR("Failed to remove initial via \n");
						goto abort_update;
					}

					memcpy(req->add_to_branch_s,req->via1->branch->value.s,req->via1->branch->value.len);
					req->add_to_branch_len = req->via1->branch->value.len;

					/* build the shm buffer now */
					buf1 = build_req_buf_from_sip_req(req,(unsigned int*)&buf_len1,
						new_send_sock?new_send_sock:dialog->send_sock,
						new_send_sock?new_send_sock->proto:dialog->send_sock->proto,
						MSG_TRANS_SHM_FLAG);
				} else {
					/* build the shm buffer now */
					buf1 = build_req_buf_from_sip_req(req,(unsigned int*)&buf_len1,
						new_send_sock?new_send_sock:dialog->send_sock,
						new_send_sock?new_send_sock->proto:dialog->send_sock->proto,
						MSG_TRANS_SHM_FLAG|MSG_TRANS_NOVIA_FLAG);
				}

				if (!buf1) {
					LM_ERR("no more shm mem\n");
					/* keep original buffer */
					goto abort_update;
				}
				/* update shortcuts */
				if(!req->add_rm && !req->new_uri.s) {
					/* headers are not affected, simply tranlate */
					new_cell->from.s = new_cell->from.s - buf + buf1;
					new_cell->to.s = new_cell->to.s - buf + buf1;
					new_cell->callid.s = new_cell->callid.s - buf + buf1;
					new_cell->cseq_n.s = new_cell->cseq_n.s - buf + buf1;
				} else {
					/* use heavy artilery :D */
					if (extract_ftc_hdrs( buf1, buf_len1, &h_from, &h_to,
					&h_cseq, &h_callid)!=0 ) {
						LM_ERR("failed to update shortcut pointers\n");
						shm_free(buf1);
						goto abort_update;
					}
					new_cell->from = h_from;
					new_cell->to = h_to;
					new_cell->callid = h_callid;
					new_cell->cseq_n = h_cseq;
				}
				/* here we rely on how build_uac_req() 
				   builds the first line */
				new_cell->uac[0].uri.s = buf1 +
					req->first_line.u.request.method.len + 1;
				new_cell->uac[0].uri.len = GET_RURI(req)->len;

				/* update also info about new destination and send sock */
				if (new_send_sock) {
					if (new_send_sock != dialog->send_sock) {
						dialog->send_sock = new_send_sock;
						request->dst.send_sock = new_send_sock;
						request->dst.proto = new_send_sock->proto;
						request->dst.proto_reserved1 = 0;
					}
					request->dst.to = new_to_su;
				}

				shm_free(buf);
				buf = buf1;
				buf_len = buf_len1;
				/* use new buffer */
			}
abort_update:
			free_sip_msg(req);
		}
	}

	new_cell->method.s = buf;
	new_cell->method.len = method->len;

	request->buffer.s = buf;
	request->buffer.len = buf_len;
	new_cell->nr_of_outgoings++;

	if(last_localT)
	{
		*last_localT = new_cell;
		REF_UNSAFE(new_cell);
	}

	if (SEND_BUFFER(request) == -1) {
		LM_ERR("attempt to send to '%.*s' failed\n", 
			dialog->hooks.next_hop->len,
			dialog->hooks.next_hop->s);
	}

	if (method->len==ACK_LEN && memcmp(method->s, ACK, ACK_LEN)==0 ) {
		t_release_transaction(new_cell);
	} else {
		start_retr(request);
	}


	return 1;

error1:
	LOCK_HASH(hi);
	remove_from_hash_table_unsafe(new_cell);
	UNLOCK_HASH(hi);
	free_cell(new_cell);
error2:
	return ret;
}


/*
 * Send a message within a dialog
 */
int req_within(str* method, str* headers, str* body, dlg_t* dialog,
		transaction_cb completion_cb,void* cbp,release_tmcb_param release_func)
{
	if (!method || !dialog) {
		LM_ERR("invalid parameter value\n");
		goto err;
	}

	if (dialog->state != DLG_CONFIRMED) {
		LM_ERR("dialog is not confirmed yet\n");
		goto err;
	}

	if ( !( ((method->len == 3) && !memcmp("ACK", method->s, 3)) ||
	        ((method->len == 6) && !memcmp("CANCEL", method->s, 6)) ) )  {
		dialog->loc_seq.value++; /* Increment CSeq */
	}

	return t_uac(method, headers, body, dialog,
		completion_cb, cbp, release_func);
err:
	return -1;
}


/*
 * Send an initial request that will start a dialog
 */
int req_outside(str* method, str* to, str* from,
	str* headers, str* body, dlg_t** dialog,
	transaction_cb cb, void* cbp,release_tmcb_param release_func)
{
	str callid, fromtag;

	if (check_params(method, to, from, dialog) < 0) goto err;
	
	generate_callid(&callid);
	generate_fromtag(&fromtag, &callid);

	if (new_dlg_uac(&callid, &fromtag, DEFAULT_CSEQ, from, to, dialog) < 0) {
		LM_ERR("failed to create new dialog\n");
		goto err;
	}

	return t_uac(method, headers, body, *dialog, cb, cbp, release_func);
err:
	return -1;
}


/*
 * Send a transactional request, no dialogs involved
 */
int request(str* m, str* ruri, str* to, str* from, str* h, str* b, str *oburi,
				transaction_cb cb, void* cbp,release_tmcb_param release_func)
{
	str callid, fromtag;
	dlg_t* dialog;
	int res;

	if (check_params(m, to, from, &dialog) < 0) goto err;

	generate_callid(&callid);
	generate_fromtag(&fromtag, &callid);

	if (new_dlg_uac(&callid, &fromtag, DEFAULT_CSEQ, from, to, &dialog) < 0) {
		LM_ERR("failed to create temporary dialog\n");
		goto err;
	}

	if (ruri) {
		dialog->rem_target.s = ruri->s;
		dialog->rem_target.len = ruri->len;
		dialog->hooks.request_uri = &dialog->rem_target;
	}
	
	if (oburi && oburi->s) dialog->hooks.next_hop = oburi;

	w_calculate_hooks(dialog);

	res = t_uac(m, h, b, dialog, cb, cbp, release_func);
	dialog->rem_target.s = 0;
	free_dlg(dialog);
	return res;

err:
	return -1;
}


void setlocalTholder(struct cell** holder)
{
	last_localT = holder;
}
