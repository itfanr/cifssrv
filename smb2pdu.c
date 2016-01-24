/*
 *   fs/cifssrv/smb2pdu.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include "glob.h"
#include "export.h"
#include "smb2pdu.h"
#include "dcerpc.h"
#include "smbfsctl.h"
#include "oplock.h"

struct fs_type_info fs_type[] = {
	{ "ADFS",	0xadf5},
	{ "AFFS",	0xadff},
	{ "AFS",	0x5346414F},
	{ "AUTOFS",	0x0187},
	{ "CODA",	0x73757245},

	{ "CRAMFS",	0x28cd3d45},
	{ "CRAMFSW",	0x453dcd28},
	{ "DEBUGFS",	0x64626720},
	{ "SECURITYFS",	0x73636673},
	{ "SELINUX",	0xf97cff8c},

	{ "SMACK",	0x43415d53},
	{ "RAMFS",	0x858458f6},
	{ "TMPFS",	0x01021994},
	{ "HUGETLBFS",	0x958458f6},
	{ "SQUASHFS",	0x73717368},

	{ "ECRYPTFS",	0xf15f},
	{ "EFS",	0x414A53},
	{ "EXT2",	0xEF53},
	{ "EXT3",	0xEF53},
	{ "XENFS",	0xabba1974},

	{ "EXT4",	0xEF53},
	{ "BTRFS",	0x9123683E},
	{ "NILFS",	0x3434},
	{ "F2FS",	0xF2F52010},
	{ "HPFS",	0xf995e849},

	{ "ISOFS",	0x9660},
	{ "JFFS2",	0x72b6},
	{ "PSTOREFS",	0x6165676C},
	{ "EFIVARFS",	0xde5e81e4},
	{ "HOSTFS",	0x00c0ffee},

	{ "MINIX",	0x137F},        /* minix v1 fs, 14 char names */
	{ "MINIX_2",	0x138F},        /* minix v1 fs, 30 char names */
	{ "MINIX2",	0x2468},        /* minix v2 fs, 14 char names */
	{ "MINIX2_2",	0x2478},        /* minix v2 fs, 30 char names */
	{ "MINIX3",	0x4d5a},        /* minix v3 fs, 60 char names */

	{ "MSDOS",	0x4d44},        /* MD */
	{ "NCP",	0x564c},
	{ "NFS",	0x6969},
	{ "OPENPROM",	0x9fa1},
	{ "QNX4",	0x002f},        /* qnx4 fs detection */

	{ "QNX6",	0x68191122},    /* qnx6 fs detection */
	{ "REISERFS",	0x52654973},    /* used by gcc */
	{ "SMB",	0x517B},
	{ "CGROUP",	0x27e0eb},
	};

/**
 * check_session_id() - check for valid session id in smb header
 * @server:	TCP server instance of connection
 * @id:		session id from smb header
 *
 * Return:      1 if valid session id, otherwise 0
 */
static inline int check_session_id(struct tcp_server_info *server, __u64 id)
{
	WARN(server->sess_count > 1, "sess_count %d", server->sess_count);

	if (id == 0 || id == -1 || server->sess_id != id)
		return 0;

	return 1;
}

/**
 * smb2_set_err_rsp() - set error response code on smb response
 * @smb_work:	smb work containing response buffer
 */
void smb2_set_err_rsp(struct smb_work *smb_work)
{
	char *rsp = smb_work->rsp_buf;
	struct smb2_err_rsp *err_rsp;

	if (smb_work->next_smb2_rcv_hdr_off)
		err_rsp = (struct smb2_err_rsp *)((char *)rsp +
				smb_work->next_smb2_rsp_hdr_off);
	else
		err_rsp = (struct smb2_err_rsp *)rsp;

	if (err_rsp->hdr.Status != cpu_to_le32(NT_STATUS_STOPPED_ON_SYMLINK)) {
		err_rsp->StructureSize =
			cpu_to_le16(SMB2_ERROR_STRUCTURE_SIZE2);
		err_rsp->ByteCount = 0;
		err_rsp->ErrorData[0] = 0;
		inc_rfc1001_len(rsp, SMB2_ERROR_STRUCTURE_SIZE2);
	}
}

/**
 * is_smb2_neg_cmd() - is it smb2 negotiation command
 * @smb_work:	smb work containing smb header
 *
 * Return:      1 if smb2 negotiation command, otherwise 0
 */
int is_smb2_neg_cmd(struct smb_work *smb_work)
{
	struct smb2_hdr *hdr = (struct smb2_hdr *)smb_work->buf;

	/* is it SMB2 header ? */
	if (*(__le32 *)hdr->ProtocolId != SMB2_PROTO_NUMBER)
		return 0;

	/* make sure it is request not response message */
	if (hdr->Flags & SMB2_FLAGS_SERVER_TO_REDIR)
		return 0;

	if (hdr->Command != SMB2_NEGOTIATE)
		return 0;

	return 1;
}

/**
 * is_smb2_rsp() - is it smb2 response
 * @smb_work:	smb work containing smb response buffer
 *
 * Return:      1 if smb2 response, otherwise 0
 */
int is_smb2_rsp(struct smb_work *smb_work)
{
	struct smb2_hdr *hdr = (struct smb2_hdr *)smb_work->rsp_buf;

	/* is it SMB2 header ? */
	if (*(__le32 *)hdr->ProtocolId != SMB2_PROTO_NUMBER)
		return 0;

	/* make sure it is response not request message */
	if (!(hdr->Flags & SMB2_FLAGS_SERVER_TO_REDIR))
		return 0;

	return 1;
}

/**
 * get_smb2_cmd_val() - get smb command code from smb header
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      smb2 request command value
 */
int get_smb2_cmd_val(struct smb_work *smb_work)
{
	struct smb2_hdr *rcv_hdr = (struct smb2_hdr *)smb_work->buf;
	if (smb_work->next_smb2_rcv_hdr_off)
		rcv_hdr = (struct smb2_hdr *)((char *)rcv_hdr
					+ smb_work->next_smb2_rcv_hdr_off);
	return le16_to_cpu(rcv_hdr->Command);
}

/**
 * set_smb2_rsp_status() - set error response code on smb2 header
 * @smb_work:	smb work containing response buffer
 */
void set_smb2_rsp_status(struct smb_work *smb_work, unsigned int err)
{
	struct smb2_hdr *rsp_hdr = (struct smb2_hdr *) smb_work->rsp_buf;
	if (smb_work->next_smb2_rcv_hdr_off)
		rsp_hdr = (struct smb2_hdr *)((char *)rsp_hdr
					+ smb_work->next_smb2_rsp_hdr_off);
	rsp_hdr->Status = cpu_to_le32(err);
	smb2_set_err_rsp(smb_work);
}

/**
 * init_smb2_neg_rsp() - initialize smb2 response for negotiate command
 * @smb_work:	smb work containing smb request buffer
 *
 * smb2 negotiate response is sent in reply of smb1 negotiate command for
 * dialect auto-negotiation.
 */
void init_smb2_neg_rsp(struct smb_work *smb_work)
{
	struct smb2_hdr *rsp_hdr;
	struct smb2_negotiate_rsp *rsp;
	struct tcp_server_info *server = smb_work->server;
	init_smb2_0_server(server);
	rsp_hdr = (struct smb2_hdr *)smb_work->rsp_buf;

	memset(rsp_hdr, 0, sizeof(struct smb2_hdr) + 2);

	rsp_hdr->smb2_buf_length =
		cpu_to_be32(sizeof(struct smb2_hdr) - 4);

	rsp_hdr->ProtocolId[0] = 0xFE;
	rsp_hdr->ProtocolId[1] = 'S';
	rsp_hdr->ProtocolId[2] = 'M';
	rsp_hdr->ProtocolId[3] = 'B';

	rsp_hdr->StructureSize = SMB2_HEADER_STRUCTURE_SIZE;
	rsp_hdr->CreditRequest = cpu_to_le16(1);
	rsp_hdr->Command = 0;
	rsp_hdr->Flags = (SMB2_FLAGS_SERVER_TO_REDIR);
	rsp_hdr->NextCommand = 0;
	rsp_hdr->MessageId = 0;
	rsp_hdr->ProcessId = 0;
	rsp_hdr->TreeId = 0;
	rsp_hdr->SessionId = 0;
	memset(rsp_hdr->Signature, 0, 16);

	rsp = (struct smb2_negotiate_rsp *)smb_work->rsp_buf;

	WARN_ON(server->tcp_status == CifsGood);

	rsp->StructureSize = cpu_to_le16(65);
	rsp->SecurityMode = 0;
	cifssrv_debug("server->dialect 0x%x\n", server->dialect);
	rsp->DialectRevision = cpu_to_le16(server->dialect);
	rsp->Reserved = 0;
	/* Not setting server guid rsp->ServerGUID, as it
	 *          * not used by client for identifying server*/
	rsp->Capabilities = 0;
	/* Default Max Message Size till SMB2.0, 64K*/
	rsp->MaxTransactSize = SMBMaxBufSize;
	rsp->MaxReadSize = SMBMaxBufSize;
	rsp->MaxWriteSize = SMBMaxBufSize;
	rsp->SystemTime = cpu_to_le64(cifs_UnixTimeToNT(CURRENT_TIME));
	rsp->ServerStartTime = 0;

	rsp->SecurityBufferOffset = cpu_to_le16(128);
	rsp->SecurityBufferLength = 0;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(rsp, 65);
	server->tcp_status = CifsNeedNegotiate;
	rsp->hdr.CreditRequest = cpu_to_le16(2);
}

/**
 * init_smb2_rsp() - initialize smb2 chained response
 * @smb_work:	smb work containing smb response buffer
 */
void init_smb2_rsp(struct smb_work *smb_work)
{
	struct smb2_hdr *req;
	struct smb2_hdr *rsp;
	struct smb2_hdr *rsp_hdr;
	struct smb2_hdr *rcv_hdr;
	int next_hdr_offset = 0;
	int len, new_len;


	req = (struct smb2_hdr *)(smb_work->buf +
				  smb_work->next_smb2_rcv_hdr_off);
	rsp = (struct smb2_hdr *)(smb_work->rsp_buf +
				  smb_work->next_smb2_rsp_hdr_off);

	/* Len of this response = updated RFC len - offset of previous cmd
	   in the compound rsp */

	/* Storing the current local FID which may be needed by subsequent
	   command in the compound request */
	if (le16_to_cpu(req->Command) == SMB2_CREATE &&
			le32_to_cpu(rsp->Status) == NT_STATUS_OK) {
		smb_work->cur_local_fid =
			le64_to_cpu(((struct smb2_create_rsp *)rsp)->
				VolatileFileId);
		smb_work->cur_local_pfid =
			le64_to_cpu(((struct smb2_create_rsp *)rsp)->
				PersistentFileId);
		smb_work->cur_local_sess_id = rsp->SessionId;
	}

	len = get_rfc1002_length(smb_work->rsp_buf) -
				 smb_work->next_smb2_rsp_hdr_off;

	next_hdr_offset = le32_to_cpu(req->NextCommand);

	/* Align the length to 8Byte  */
	new_len = ((len + 7) & ~7);
	inc_rfc1001_len(smb_work->rsp_buf, ((sizeof(struct smb2_hdr) - 4)
			+ new_len - len));
	rsp->NextCommand = cpu_to_le32(new_len);

	smb_work->next_smb2_rcv_hdr_off += next_hdr_offset;
	smb_work->next_smb2_rsp_hdr_off += new_len;
	cifssrv_debug("Compound req new_len = %d rcv off = %d rsp off = %d\n",
		      new_len, smb_work->next_smb2_rcv_hdr_off,
		      smb_work->next_smb2_rsp_hdr_off);

	rsp_hdr = (struct smb2_hdr *)(((char *)smb_work->rsp_buf +
					smb_work->next_smb2_rsp_hdr_off));
	rcv_hdr = (struct smb2_hdr *)(((char *)smb_work->buf +
					smb_work->next_smb2_rcv_hdr_off));

	if (!(le32_to_cpu(rcv_hdr->Flags) & SMB2_FLAGS_RELATED_OPERATIONS)) {
		cifssrv_debug("related flag should be set\n");
		smb_work->cur_local_fid = -1;
		smb_work->cur_local_pfid = -1;
	}
	memset((char *)rsp_hdr + 4, 0, sizeof(struct smb2_hdr) + 2);
	memcpy(rsp_hdr->ProtocolId, rcv_hdr->ProtocolId, 4);
	rsp_hdr->StructureSize = SMB2_HEADER_STRUCTURE_SIZE;
	rsp_hdr->CreditRequest = rcv_hdr->CreditRequest;
	rsp_hdr->Command = rcv_hdr->Command;

	/*
	 * Message is response. We don't grant oplock yet.
	 */
	rsp_hdr->Flags = (SMB2_FLAGS_SERVER_TO_REDIR |
				SMB2_FLAGS_RELATED_OPERATIONS);
	rsp_hdr->NextCommand = 0;
	rsp_hdr->MessageId = rcv_hdr->MessageId;
	rsp_hdr->ProcessId = rcv_hdr->ProcessId;
	rsp_hdr->TreeId = rcv_hdr->TreeId;
	rsp_hdr->SessionId = rcv_hdr->SessionId;
	memcpy(rsp_hdr->Signature, rcv_hdr->Signature, 16);
}

/**
 * is_chained_smb2_message() - check for chained command
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      true if chained request, otherwise false
 */
bool is_chained_smb2_message(struct smb_work *smb_work)
{
	struct smb2_hdr *hdr = (struct smb2_hdr *)smb_work->buf;
	unsigned int len;

	if (*(__le32 *)(hdr->ProtocolId) != SMB2_PROTO_NUMBER)
		return false;

	hdr = (struct smb2_hdr *)(smb_work->buf +
			smb_work->next_smb2_rcv_hdr_off);
	if (le32_to_cpu(hdr->NextCommand) > 0) {
		cifssrv_debug("got SMB2 chained command\n");
		init_smb2_rsp(smb_work);
		return true;
	} else if (smb_work->next_smb2_rcv_hdr_off) {
		/*
		 * This is last request in chained command,
		 * align response to 8 byte
		 */
		len = ((get_rfc1002_length(smb_work->rsp_buf) + 7) & ~7);
		len = len - get_rfc1002_length(smb_work->rsp_buf);
		if (len) {
			cifssrv_debug("padding len %u\n", len);
			inc_rfc1001_len(smb_work->rsp_buf, len);
			if (smb_work->rdata_buf)
				smb_work->rrsp_hdr_size += len;
		}
	}
	return false;
}

/**
 * init_smb2_rsp_hdr() - initialize smb2 response
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0
 */
int init_smb2_rsp_hdr(struct smb_work *smb_work)
{
	struct smb2_hdr *rsp_hdr = (struct smb2_hdr *)smb_work->rsp_buf;
	struct smb2_hdr *rcv_hdr = (struct smb2_hdr *)smb_work->buf;
	struct tcp_server_info *server = smb_work->server;
	int next_hdr_offset = 0;

	next_hdr_offset = le32_to_cpu(rcv_hdr->NextCommand);
	memset(rsp_hdr, 0, sizeof(struct smb2_hdr) + 2);

	rsp_hdr->smb2_buf_length = cpu_to_be32(sizeof(struct smb2_hdr) - 4);

	memcpy(rsp_hdr->ProtocolId, rcv_hdr->ProtocolId, 4);
	rsp_hdr->StructureSize = SMB2_HEADER_STRUCTURE_SIZE;
	rsp_hdr->CreditRequest = rcv_hdr->CreditRequest;
	rsp_hdr->Command = rcv_hdr->Command;

	/*
	 * Message is response. We don't grant oplock yet.
	 */
	rsp_hdr->Flags = (SMB2_FLAGS_SERVER_TO_REDIR);
	if (next_hdr_offset)
		rsp_hdr->NextCommand = cpu_to_le32(next_hdr_offset);
	else
		rsp_hdr->NextCommand = 0;
	rsp_hdr->MessageId = rcv_hdr->MessageId;
	rsp_hdr->ProcessId = rcv_hdr->ProcessId;
	rsp_hdr->TreeId = rcv_hdr->TreeId;
	rsp_hdr->SessionId = rcv_hdr->SessionId;
	memcpy(rsp_hdr->Signature, rcv_hdr->Signature, 16);

	if (server->credits_granted) {
		if (le16_to_cpu(rcv_hdr->CreditCharge))
			server->credits_granted -=
				le16_to_cpu(rcv_hdr->CreditCharge);
		else
			server->credits_granted -= 1;
	}

	return 0;
}

/**
 * smb2_allocate_rsp_buf() - allocate smb2 response buffer
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
int smb2_allocate_rsp_buf(struct smb_work *smb_work)
{
	struct smb2_hdr *hdr = (struct smb2_hdr *)smb_work->buf;
	struct smb2_query_info_req *req;
	bool need_large_buf = false;

	/* allocate large response buf for chained commands */
	if (le32_to_cpu(hdr->NextCommand) > 0)
		need_large_buf = true;
	else {
		switch (le16_to_cpu(hdr->Command)) {
		case SMB2_READ:
			/* fall through */
		case SMB2_IOCTL_HE:
			/* fall through */
		case SMB2_QUERY_DIRECTORY_HE:
			need_large_buf = true;
			break;
		case SMB2_QUERY_INFO_HE:
			req = (struct smb2_query_info_req *)smb_work->buf;
			if (req->InfoType == SMB2_O_INFO_FILE &&
					req->FileInfoClass ==
					FILE_FULL_EA_INFORMATION) {
				need_large_buf = true;
			}
			break;
		}
	}

	if (need_large_buf) {
		smb_work->rsp_large_buf = true;
		smb_work->rsp_buf = mempool_alloc(cifssrv_rsp_poolp, GFP_NOFS);
	} else {
		smb_work->rsp_large_buf = false;
		smb_work->rsp_buf = mempool_alloc(cifssrv_sm_rsp_poolp,
								GFP_NOFS);
	}

	if (!smb_work->rsp_buf) {
		cifssrv_err("failed to alloc response buffer, large_buf %d\n",
				smb_work->rsp_large_buf);
		return -ENOMEM;
	}

	return 0;
}

/**
 * smb2_set_rsp_credits() - set number of credits iin response buffer
 * @smb_work:	smb work containing smb response buffer
 */
void smb2_set_rsp_credits(struct smb_work *smb_work)
{
	struct smb2_hdr *hdr = (struct smb2_hdr *)smb_work->rsp_buf;
	struct tcp_server_info *server = smb_work->server;
	unsigned int status = le32_to_cpu(hdr->Status);
	unsigned int flags = le32_to_cpu(hdr->Flags);
	unsigned short credits_requested = le16_to_cpu(hdr->CreditRequest);
	unsigned short cmd = le16_to_cpu(hdr->Command);
	unsigned short credit_charge = 1, credits_granted = 0;
	unsigned short aux_max, aux_credits, min_credits;

	BUG_ON(server->credits_granted >= server->max_credits);

	/* get default minimum credits by shifting maximum credits by 4 */
	min_credits = server->max_credits >> 4;

	if (flags & SMB2_FLAGS_ASYNC_COMMAND) {
		credits_granted = 0;
	} else if (credits_requested > 0) {
		aux_max = 0;
		aux_credits = credits_requested - 1;
		switch (cmd) {
		case SMB2_NEGOTIATE:
			break;
		case SMB2_SESSION_SETUP:
			aux_max = (status) ? 0 : 32;
			break;
		default:
			aux_max = 32;
			break;
		}
		aux_credits = (aux_credits < aux_max) ? aux_credits : aux_max;
		credits_granted = aux_credits + credit_charge;

		/* if credits granted per client is getting bigger than default
		 * minimum credits then we should wrap it up within the limits.
		 */
		if ((server->credits_granted + credits_granted) > min_credits)
			credits_granted = min_credits -	server->credits_granted;

	} else if (server->credits_granted == 0) {
		credits_granted = 1;
	}

	server->credits_granted += credits_granted;
	cifssrv_debug("credits: requested[%d] granted[%d] total_granted[%d]\n",
			credits_requested, credits_granted,
			server->credits_granted);
	/* set number of credits granted in SMB2 hdr */
	hdr->CreditRequest = cpu_to_le16(credits_granted);

}

/**
 * smb2_check_user_session() - check for valid session for a user
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb2_check_user_session(struct smb_work *smb_work)
{
	struct smb2_hdr *req_hdr = (struct smb2_hdr *)smb_work->buf;
	struct tcp_server_info *server = smb_work->server;
	uint64_t incoming_sess_id;
	struct cifssrv_sess *sess;
	struct list_head *tmp, *t;
	int found = 0;

	if (server->tcp_status != CifsGood)
		return 0;

	incoming_sess_id = le64_to_cpu(req_hdr->SessionId);

	/* Check for validity of user session */
	list_for_each_safe(tmp, t, &server->cifssrv_sess) {
		sess = list_entry(tmp, struct cifssrv_sess,
					cifssrv_ses_list);
		if (sess->sess_id == incoming_sess_id) {
			if (!sess->valid) {
				cifssrv_err("Invalid user session\n");
				return -EINVAL;
			} else {
				found = 1;
				break;
			}
		}
	}

	/* Check if user is found */
	if (!found) {
		cifssrv_err("User session not found\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * smb2_invalidate_prev_session() - invalidate existing session of an user
 * @sess_id:	session id to be invalidated
 */
void smb2_invalidate_prev_session(uint64_t sess_id)
{
	struct cifssrv_sess *sess;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifssrv_session_list) {
		sess = list_entry(tmp, struct cifssrv_sess,
				cifssrv_ses_global_list);
		if (sess->sess_id == sess_id) {
			sess->valid = 0;
			break;
		}
	}
}

/**
 * get_cifssrv_tcon() - get tcon for a tree id
 * @sess:	session containing tree connect list
 * @tid:	tree id of desired tcon
 *
 * Return:      matching tcon on success, otherwise NULL
 */
static struct cifssrv_tcon *get_cifssrv_tcon(struct cifssrv_sess *sess,
			unsigned int tid)
{
	struct cifssrv_tcon *tcon;
	struct list_head *tmp;

	if (sess->tcon_count == 0) {
		cifssrv_debug("NO tree connected\n");
		return NULL;
	}

	list_for_each(tmp, &sess->tcon_list) {
		tcon = list_entry(tmp, struct cifssrv_tcon, tcon_list);
		if (tcon->share->tid == tid)
			return tcon;
	}

	return NULL;
}

/**
 * smb2_get_name() - get filename string from on the wire smb format
 * @src:	source buffer
 * @maxlen:	maxlen of source string
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      matching converted filename on success, otherwise error ptr
 */
char *
smb2_get_name(const char *src, const int maxlen, struct smb_work *smb_work)
{
	struct smb2_hdr *req_hdr = (struct smb2_hdr *)smb_work->buf;
	struct smb2_hdr *rsp_hdr = (struct smb2_hdr *)smb_work->rsp_buf;
	char *name, *unixname;
	char *wild_card_pos;

	if (smb_work->next_smb2_rcv_hdr_off)
		req_hdr = (struct smb2_hdr *)((char *)req_hdr
				+ smb_work->next_smb2_rcv_hdr_off);

	name = smb_strndup_from_utf16(src, maxlen, 1,
			smb_work->server->local_nls);
	if (IS_ERR(name)) {
		cifssrv_err("failed to get name %ld\n", PTR_ERR(name));
		if (PTR_ERR(name) == -ENOMEM)
			rsp_hdr->Status = NT_STATUS_NO_MEMORY;
		else
			rsp_hdr->Status = NT_STATUS_OBJECT_NAME_INVALID;
		return name;
	}

	/* change it to absolute unix name */
	convert_delimiter(name);

	/*Handling of dir path in FIND_FIRST2 having '*' at end of path*/
	wild_card_pos = strrchr(name, '*');

	if (wild_card_pos != NULL)
		*wild_card_pos = '\0';

	unixname = convert_to_unix_name(name, req_hdr->TreeId);
	kfree(name);
	if (!unixname) {
		cifssrv_err("can not convert absolute name\n");
		rsp_hdr->Status = NT_STATUS_NO_MEMORY;
		return ERR_PTR(-ENOMEM);
	}

	cifssrv_debug("absoulte name = %s\n", unixname);
	return unixname;
}

/**
 * smb2_get_name_from_filp() - get filename string from filp
 * @filp:	file pointer containing filename
 *
 * Reconstruct complete pathname from filp, required in cases e.g. durable
 * reconnect where incoming filename in SMB2 CREATE request need to be ignored
 *
 * Return:      filename on success, otherwise NULL
 */
char *
smb2_get_name_from_filp(struct file *filp)
{
	char *pathname, *name, *full_pathname;
	int namelen;

	pathname = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!pathname)
		return ERR_PTR(-ENOMEM);

	name = d_path(&filp->f_path, pathname, PATH_MAX);
	if (IS_ERR(name)) {
		kfree(pathname);
		return name;
	}

	namelen = strlen(name);
	full_pathname = kmalloc(namelen + 1, GFP_KERNEL);
	if (!full_pathname) {
		kfree(pathname);
		return ERR_PTR(-ENOMEM);
	}

	memcpy(full_pathname, name, namelen);
	full_pathname[namelen] = '\0';

	kfree(pathname);
	return full_pathname;
}

/**
 * smb2_get_dos_mode() - get file mode in dos format from unix mode
 * @stat:	kstat containing file mode
 *
 * Return:      converted dos mode
 */
int smb2_get_dos_mode(struct kstat *stat)
{
	int attr = 0;

	if (stat->mode & S_IXUSR)
		attr |= ATTR_ARCHIVE;

	if (S_ISDIR(stat->mode))
		attr = ATTR_DIRECTORY;

	if (!attr)
		attr = ATTR_NORMAL;

	return attr;
}

/**
 * smb2_negotiate() - handler for smb2 negotiate command
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0
 */
int smb2_negotiate(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_negotiate_req *req;
	struct smb2_negotiate_rsp *rsp;
	unsigned int limit;

	req = (struct smb2_negotiate_req *)smb_work->buf;
	rsp = (struct smb2_negotiate_rsp *)smb_work->rsp_buf;

	if (server->tcp_status == CifsGood) {
		cifssrv_err("server->tcp_status is already in CifsGood State\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	cifssrv_debug("%s: Recieved negotiate request\n", __func__);
	if (req->StructureSize != 36 || req->DialectCount == 0) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	server->dialect = negotiate_dialect(smb_work->buf);
	cifssrv_debug("server->dialect 0x%x\n", server->dialect);
	if (server->dialect == BAD_PROT_ID) {
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		return 0;
	}

	/* For stats */
	server->connection_type = server->dialect;
	rsp->Capabilities = 0;
	/* Default message size limit 64K till SMB2.0, no LargeMTU*/
	limit = SMBMaxBufSize;

	if (server->dialect == SMB30_PROT_ID) {
		init_smb3_0_server(server);
		rsp->Capabilities |= server->capabilities;
	}

	if (server->dialect == SMB21_PROT_ID) {
		init_smb2_1_server(server);
		rsp->Capabilities |= server->capabilities;
	}

	if (server->dialect > SMB20_PROT_ID) {
		memcpy(server->ClientGUID, req->ClientGUID,
				SMB2_CLIENT_GUID_SIZE);
		/* With LargeMTU above SMB2.0, default message limit is 1MB */
		limit = CIFS_DEFAULT_IOSIZE;
	}

	rsp->StructureSize = cpu_to_le16(65);
	rsp->SecurityMode = 0;
	rsp->DialectRevision = cpu_to_le16(server->dialect);
	rsp->Reserved = 0;
	/* Not setting server guid rsp->ServerGUID, as it
	 * not used by client for identifying server*/
	rsp->MaxTransactSize = SMBMaxBufSize;
	rsp->MaxReadSize = min(limit, (unsigned int)CIFS_DEFAULT_IOSIZE);
	rsp->MaxWriteSize = min(limit, (unsigned int)CIFS_DEFAULT_IOSIZE);
	rsp->SystemTime = cpu_to_le64(cifs_UnixTimeToNT(CURRENT_TIME));
	rsp->ServerStartTime = 0;

	rsp->SecurityBufferOffset = cpu_to_le16(128);
	rsp->SecurityBufferLength = 0;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(rsp, 65);
	server->tcp_status = CifsNeedNegotiate;
	server->need_neg = false;
	return 0;

}

/**
 * smb2_sess_setup() - handler for smb2 session setup command
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb2_sess_setup(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_sess_setup_req *req;
	struct smb2_sess_setup_rsp *rsp;
	struct cifssrv_sess *sess;
	NEGOTIATE_MESSAGE *negblob;
	CHALLENGE_MESSAGE *chgblob;
	AUTHENTICATE_MESSAGE *authblob;
	struct cifssrv_usr *usr;
	char *dup_name = NULL;
	int rc = 0;
	char key[CIFS_AUTH_RESP_SIZE];
	unsigned char p21[21];

	req = (struct smb2_sess_setup_req *)smb_work->buf;
	rsp = (struct smb2_sess_setup_rsp *)smb_work->rsp_buf;

	if (server->tcp_status != CifsNeedNegotiate) {
		cifssrv_err("server->tcp_status is not CifsNeedNegotiate\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	cifssrv_debug("%s: Recieved request for session setup\n", __func__);
	if (req->StructureSize != 25) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	rsp->StructureSize = cpu_to_le16(9);
	rsp->SessionFlags = 0;
	rsp->SecurityBufferOffset = cpu_to_le16(72);
	rsp->SecurityBufferLength = 0;
	inc_rfc1001_len(rsp, 9);

	if (!req->hdr.SessionId) {
		get_random_bytes(&server->sess_id, sizeof(__u64));
		rsp->hdr.SessionId = server->sess_id;
	}

	/* Check for previous session */

	if (le64_to_cpu(req->PreviousSessionId) != 0)
		smb2_invalidate_prev_session(
			le64_to_cpu(req->PreviousSessionId));

	negblob = (NEGOTIATE_MESSAGE *)(req->hdr.ProtocolId +
			req->SecurityBufferOffset);

	if (*(__le64 *)negblob->Signature == NTLMSSP_SIGNATURE_VAL) {
		cifssrv_debug("%s NTLMSSP present\n", __func__);
		if (negblob->NegotiateFlags == NTLMSSP_NEGOTIATE_56) {
			/* TBD: area for session sign/seal */
		}
	}

	if (negblob->MessageType == NtLmNegotiate) {
		cifssrv_debug("%s negotiate phase\n", __func__);

		chgblob = (CHALLENGE_MESSAGE *)(rsp->hdr.ProtocolId +
				rsp->SecurityBufferOffset);
		memset(chgblob, 0, sizeof(CHALLENGE_MESSAGE));

		rsp->SecurityBufferLength = build_ntlmssp_challenge_blob(
					chgblob, (__u8 *)rsp->hdr.ProtocolId,
			rsp->SecurityBufferOffset, server);

		rsp->hdr.Status = NT_STATUS_MORE_PROCESSING_REQUIRED;
		/* Note: here total size -1 is done as
		   an adjustment for 0 size blob */
		inc_rfc1001_len(rsp, rsp->SecurityBufferLength-1);
	} else if (negblob->MessageType == NtLmAuthenticate) {
		cifssrv_debug("%s authenticate phase\n", __func__);

		authblob = (AUTHENTICATE_MESSAGE *)(req->hdr.ProtocolId +
				req->SecurityBufferOffset);

		dup_name = smb_strndup_from_utf16((const char *)authblob +
				authblob->UserName.BufferOffset,
				authblob->UserName.Length, true,
				server->local_nls);

		if (IS_ERR(dup_name)) {
			cifssrv_err("%s-%d cannot allocate memory\n",
					__func__, __LINE__);
			rc = PTR_ERR(dup_name);
			goto out_err;
		}

		cifssrv_debug("session setup request for user %s\n", dup_name);
		usr = cifssrv_is_user_present(dup_name);
		if (!usr) {
			cifssrv_debug("user not present in database\n");
			kfree(dup_name);
			rc = -EINVAL;
			goto out_err;
		}

		kfree(dup_name);

		if (authblob->NtChallengeResponse.Length ==
				CIFS_AUTH_RESP_SIZE) {
			memset(p21, '\0', 21);
			memcpy(p21, usr->passkey, CIFS_NTHASH_SIZE);
			rc = E_P24(p21, server->cryptkey, key);
			if (rc) {
				cifssrv_err("%s password processing failed\n",
						__func__);
				goto out_err;
			}

			if (strncmp((const char *)authblob +
						authblob->NtChallengeResponse.
						BufferOffset, key,
						CIFS_AUTH_RESP_SIZE) != 0) {
				cifssrv_debug("ntlmv1 authentication failed\n");
				rc = -EINVAL;
				goto out_err;
			} else
				cifssrv_debug("ntlmv1 authentication pass\n");
		} else {
			char *srvname;

			srvname = kstrndup(netbios_name, strlen(netbios_name),
					GFP_KERNEL);
			if (!srvname) {
				cifssrv_err("%s-%d cannot allocate memory\n",
						__func__, __LINE__);
				rc = -ENOMEM;
				goto out_err;
			}

			rc = process_ntlmv2(server, (char *)authblob +
				authblob->NtChallengeResponse.
				BufferOffset, usr, srvname,
				authblob->NtChallengeResponse.Length -
				CIFS_ENCPWD_SIZE, server->local_nls);

			kfree(srvname);

			if (rc) {
				char *ntdomain;

				ntdomain = smb_strndup_from_utf16(
					(const char *)authblob +
					authblob->DomainName.BufferOffset,
					authblob->DomainName.Length, true,
					server->local_nls);
				if (IS_ERR(ntdomain)) {
					cifssrv_err("%s-%d cannot allocate memory\n",
							__func__, __LINE__);
					rc = PTR_ERR(ntdomain);
					goto out_err;
				}

				rc = process_ntlmv2(server, (char *)authblob +
					authblob->NtChallengeResponse.
					BufferOffset, usr, ntdomain,
					authblob->NtChallengeResponse.Length -
					CIFS_ENCPWD_SIZE, server->local_nls);

				if (rc) {
					cifssrv_debug("ntlmv2 authentication"
							"failed\n");
					rc = -EINVAL;
					kfree(ntdomain);
					goto out_err;
				}

				kfree(ntdomain);
			}

			cifssrv_debug("ntlmv2 authentication pass\n");
		}

		sess = kmalloc(sizeof(struct cifssrv_sess), GFP_KERNEL);
		if (sess == NULL) {
			cifssrv_err("cannot allocate memory to session\n");
			rc = -ENOMEM;
			goto out_err;
		}

		sess->usr = usr;
		INIT_LIST_HEAD(&sess->cifssrv_ses_list);
		INIT_LIST_HEAD(&sess->tcon_list);
		sess->tcon_count = 0;
		sess->valid = 1;
		list_add(&sess->cifssrv_ses_list, &server->cifssrv_sess);
		list_add(&sess->cifssrv_ses_global_list, &cifssrv_session_list);
		sess->server = server;
		sess->sess_id = server->sess_id;
		server->sess_count++;

		server->tcp_status = CifsGood;
	} else {
		cifssrv_err("%s Invalid phase\n", __func__);
		rc = -EINVAL;
	}

out_err:
	if (rc != 0)
		rsp->hdr.Status = NT_STATUS_LOGON_FAILURE;

	return rc;
}

/**
 * smb2_tree_connect() - handler for smb2 tree connect command
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb2_tree_connect(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_tree_connect_req *req;
	struct smb2_tree_connect_rsp *rsp;
	struct cifssrv_sess *sess;
	struct cifssrv_share *share;
	struct cifssrv_tcon *tcon;
	char *treename = NULL, *name = NULL;
	struct list_head *tmp;
	int rc = 0;
	bool tree_valid = false;
	int reject = 0;

	req = (struct smb2_tree_connect_req *)smb_work->buf;
	rsp = (struct smb2_tree_connect_rsp *)smb_work->rsp_buf;

	if (req->StructureSize != 9) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	sess = list_entry(server->cifssrv_sess.next,
			struct cifssrv_sess, cifssrv_ses_list);

	treename = smb_strndup_from_utf16(req->Buffer, req->PathLength,
					  true, server->local_nls);
	if (IS_ERR(treename)) {
		cifssrv_err("treename is NULL\n");
		rc = PTR_ERR(treename);
		goto out_err;
	}
	name = extract_sharename(treename);
	if (!name) {
		kfree(treename);
		goto out_err;
	}

	cifssrv_debug("tree connect request for tree %s treename %s\n",
		      name, treename);
	kfree(treename);

	if (!strncmp(name, "IPC", 3))
		cifssrv_debug("request for IPC, mark it invalid\n");
	list_for_each(tmp, &cifssrv_share_list) {
		share = list_entry(tmp, struct cifssrv_share, list);
		cifssrv_debug("comparing with treename %s\n", share->sharename);
		if (strcasecmp(share->sharename, name) == 0) {
			rc = validate_clip(server->peeraddr, share);
			if (rc <= 0) {
				if (!rc) {
					cifssrv_err(
					"[host:%s] not allowed for [share:%s]\n"
					, server->peeraddr, share->sharename);

					reject = 1;
					rc = -EINVAL;
				}

				kfree(name);
				goto out_err;
			}
			if (get_attr_guestok(&share->config.attr) == 1) {
				cifssrv_debug("guest login on to share %s\n",
							share->sharename);
				tree_valid = true;
				break;
			}
			rc = validate_usr(sess->usr->name, share);
			if (rc <= 0) {
				if (!rc) {
					cifssrv_err(
					"[user:%s] not authorised for [share:%s]\n",
					sess->usr->name, share->sharename);

					reject = 1;
					rc = -EINVAL;
				}

				kfree(name);
				goto out_err;
			}
			tree_valid = true;
			break;
		}
	}
	kfree(name);

	if (tree_valid ==  false) {
		cifssrv_err("tree not exported on server\n");
		rc = -EINVAL;
		goto out_err;
	}

	tcon = kmalloc(sizeof(struct cifssrv_tcon), GFP_KERNEL);
	if (!tcon) {
		cifssrv_err("cannot allocate memory\n");
		rc = -ENOMEM;
		goto out_err;
	}

	tcon->share = share;
	tcon->sess = sess;
	INIT_LIST_HEAD(&tcon->tcon_list);
	list_add(&tcon->tcon_list, &sess->tcon_list);
	sess->tcon_count++;

	rsp->hdr.TreeId = tcon->share->tid;

	if (tcon->share->tid == 1) {
		cifssrv_debug("IPC share path request\n");
		rsp->ShareType = 0x02;
		rsp->MaximalAccess = cpu_to_le32(0x001f00a9);
	} else {
		rsp->ShareType = 0x01;
		rsp->MaximalAccess = cpu_to_le32(0x001f01ff);
	}

	rsp->StructureSize = cpu_to_le16(16);
	rsp->Reserved = 0;
	rsp->ShareFlags = 0;
	rsp->Capabilities = 0;
	inc_rfc1001_len(rsp, 16);
	return 0;

out_err:
	cifssrv_debug("error while tree connect\n");
	rsp->StructureSize = cpu_to_le16(16);
	rsp->Reserved = 0;
	rsp->ShareFlags = 0;
	inc_rfc1001_len(rsp, 16);
	if (reject)
		rsp->hdr.Status = NT_STATUS_ACCESS_DENIED;
	else
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
	return rc;
}

/**
 * smb2_create_open_flags() - convert smb open flags to unix open flags
 * @file_present:	is file already present
 * @access:		file access flags
 * @disposition:	file disposition flags
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      file open flags
 */
int smb2_create_open_flags(bool file_present, __le32 access,
		__le32 disposition)
{
	int oflags = 0;

	if ((access & FILE_READ_DATA_LE || access & FILE_GENERIC_READ_LE) &&
			(access & FILE_WRITE_DATA_LE ||
			 access & FILE_GENERIC_WRITE_LE))
		oflags |= O_RDWR;
	else if (access & FILE_READ_DATA_LE  || access & FILE_GENERIC_READ_LE)
		oflags |= O_RDONLY;
	else if (access & FILE_WRITE_DATA_LE || access & FILE_GENERIC_WRITE_LE)
		oflags |= O_WRONLY;
	else
		oflags |= O_RDONLY;

	if (file_present) {
		switch (disposition & 0x00000007) {
		case FILE_OPEN_LE:
			/* fall through */
		case FILE_CREATE_LE:
			oflags &= ~O_CREAT;
			break;
		case FILE_SUPERSEDE_LE:
			/* fall through */
		case FILE_OVERWRITE_LE:
			/* fall through */
		case FILE_OVERWRITE_IF_LE:
			oflags |= O_TRUNC;
			break;
		default:
			break;
		}
	} else {
		switch (disposition & 0x00000007) {
		case FILE_SUPERSEDE_LE:
			/* fall through */
		case FILE_CREATE_LE:
			/* fall through */
		case FILE_OPEN_IF_LE:
			/* fall through */
		case FILE_OVERWRITE_IF_LE:
			oflags |= O_CREAT;
			break;
		case FILE_OPEN_LE:
			/* fall through */
		case FILE_OVERWRITE_LE:
			oflags &= ~O_CREAT;
			break;
		default:
			break;
		}
	}
	return oflags;
}

/**
 * smb2_tree_disconnect() - handler for smb tree connect request
 * @smb_work:	smb work containing request buffer
 *
 * Return:      0
 */
int smb2_tree_disconnect(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_tree_disconnect_req *req;
	struct smb2_tree_disconnect_rsp *rsp;
	struct cifssrv_sess *sess;
	struct cifssrv_tcon *tcon;

	req = (struct smb2_tree_disconnect_req *)smb_work->buf;
	rsp = (struct smb2_tree_disconnect_rsp *)smb_work->rsp_buf;

	if (req->StructureSize != 4) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}
	rsp->StructureSize = cpu_to_le16(4);
	inc_rfc1001_len(rsp, 4);

	cifssrv_debug("%s : request\n", __func__);
	sess = list_entry(server->cifssrv_sess.next,
			struct cifssrv_sess, cifssrv_ses_list);

	tcon = get_cifssrv_tcon(sess, req->hdr.TreeId);
	if (tcon == NULL) {
		cifssrv_err("Invalid tid %d\n", req->hdr.TreeId);
		rsp->hdr.Status = NT_STATUS_NETWORK_NAME_DELETED;
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	/* delete tcon from sess tcon list and decrease sess tcon count */
	list_del(&tcon->tcon_list);
	sess->tcon_count--;
	kfree(tcon);
	return 0;
}

/**
 * smb2_session_logoff() - handler for session log off request
 * @smb_work:	smb work containing request buffer
 *
 * Return:      0
 */
int smb2_session_logoff(struct smb_work *smb_work)
{

	struct tcp_server_info *server = smb_work->server;
	struct smb2_logoff_req *req;
	struct smb2_logoff_rsp *rsp;
	struct cifssrv_sess *sess;
	struct cifssrv_tcon *tcon;
	struct list_head *tmp, *t;

	req = (struct smb2_logoff_req *)smb_work->buf;
	rsp = (struct smb2_logoff_rsp *)smb_work->rsp_buf;

	if (req->StructureSize != 4) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	rsp->StructureSize = cpu_to_le16(4);
	inc_rfc1001_len(rsp, 4);

	cifssrv_err("%s : request\n", __func__);
	sess = list_entry(server->cifssrv_sess.next,
		struct cifssrv_sess, cifssrv_ses_list);

	/* Got a valid session, set server state */
	WARN_ON(sess->server != server || server->sess_count != 1);

	/* setting CifsExiting here may race with start_tcp_sess */
	server->tcp_status = CifsNeedReconnect;

	/*
	 * We cannot discard session in case some request are already running.
	 * Need to wait for them to finish and update req_running.
	 */
	wait_event(server->req_running_q,
			atomic_read(&server->req_running) == 1);

	/* Free the tree connection to session */
	list_for_each_safe(tmp, t, &sess->tcon_list) {
		tcon = list_entry(tmp, struct cifssrv_tcon, tcon_list);
		if (tcon == NULL) {
			cifssrv_err("Invalid tid %d\n", req->hdr.TreeId);
			rsp->hdr.Status = NT_STATUS_NETWORK_NAME_DELETED;
			smb2_set_err_rsp(smb_work);
			return 0;
		}
		list_del(&tcon->tcon_list);
		sess->tcon_count--;
		kfree(tcon);
	}

	WARN_ON(sess->tcon_count != 0);

	/* free all sessions, we have just 1 */
	list_del(&sess->cifssrv_ses_list);
	list_del(&sess->cifssrv_ses_global_list);
	kfree(sess);

	server->sess_count--;
	/* let start_tcp_sess free server info now */
	server->tcp_status = CifsNeedNegotiate;
	return 0;
}

/**
 * create_smb2_pipe() - create IPC pipe
 * @smb_work:	smb work containing request buffer
 *
 * Return:      0 on success, otherwise error
 */
static int create_smb2_pipe(struct smb_work *smb_work)
{
	struct smb2_create_rsp *rsp;
	struct smb2_create_req *req;
	int id;
	unsigned int pipe_type;
	char *name =  NULL;

	rsp = (struct smb2_create_rsp *)smb_work->rsp_buf;
	req = (struct smb2_create_req *)smb_work->buf;
	name = smb_strndup_from_utf16(req->Buffer, req->NameLength, 1,
				smb_work->server->local_nls);

	if (strcmp(name, "winreg") == 0) {
		cifssrv_debug("pipe: %s\n", name);
		pipe_type = WINREG;
	} else if (strcmp(name, "srvsvc") == 0) {
		cifssrv_debug("pipe: %s\n", name);
		pipe_type = SRVSVC;
	} else if (strcmp(name, "wkssvc") == 0) {
		cifssrv_debug("pipe: %s\n", name);
		pipe_type = SRVSVC;
	} else {
		cifssrv_debug("pipe %s not supported\n", name);
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		return -EOPNOTSUPP;
	}

	/* Assigning temporary fid for pipe */
	id = get_pipe_id(smb_work->server, pipe_type);
	if (id < 0) {
		cifssrv_debug("id is not correct\n");
		return id;
	}
	rsp->StructureSize = cpu_to_le16(89);
	rsp->OplockLevel = SMB2_OPLOCK_LEVEL_NONE;
	rsp->Reserved = 0;
	rsp->CreateAction = FILE_OPENED;

	rsp->CreationTime = cpu_to_le64(0);
	rsp->LastAccessTime = cpu_to_le64(0);
	rsp->ChangeTime = cpu_to_le64(0);
	rsp->AllocationSize = cpu_to_le64(0);
	rsp->EndofFile = cpu_to_le64(0);
	rsp->FileAttributes = ATTR_NORMAL;
	rsp->Reserved2 = 0;
	rsp->VolatileFileId = cpu_to_le64(id);
	rsp->PersistentFileId = 0;
	rsp->CreateContextsOffset = 0;
	rsp->CreateContextsLength = 0;

	inc_rfc1001_len(rsp, 88); /* StructureSize - 1*/
	kfree(name);
	return 0;
}

/**
 * smb2_open() - handler for smb file open request
 * @smb_work:	smb work containing request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb2_open(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_create_req *req;
	struct smb2_create_rsp *rsp, *rsp_org;
	struct path path, lpath;
	struct cifssrv_share *share;
	struct cifssrv_file *fp = NULL;
	struct file *filp = NULL, *lfilp = NULL;
	struct kstat stat;
	__u64 create_time;
	umode_t mode = 0;
	bool file_present = true, islink = false;
	int oplock, open_flags = 0, file_info = 0, len = 0;
	int volatile_id = 0;
	uint64_t persistent_id = 0;
	int rc = 0;
	char *name = NULL, *context_name, *lname = NULL, *pathname = NULL;
	struct create_context *context;
	int durable_open = false;
	int durable_reconnect = false, durable_reopened = false;
	struct create_durable *recon_state;
	struct cifssrv_durable_state *durable_state;
	char *lk = NULL;
	struct lease_ctx_info lc;
	bool attrib_only = false;
	int open_directory = 0;

	req = (struct smb2_create_req *)smb_work->buf;
	rsp = (struct smb2_create_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_create_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_create_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);
	}

	if (le32_to_cpu(req->hdr.NextCommand) &&
			!smb_work->next_smb2_rcv_hdr_off &&
			(le32_to_cpu(req->hdr.Flags) &
			 SMB2_FLAGS_RELATED_OPERATIONS)) {
		cifssrv_debug("invalid flag in chained command\n");
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		smb2_set_err_rsp(smb_work);
		return -EINVAL;
	}

	if (req->StructureSize != 57) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (rsp->hdr.TreeId == 1) {
		cifssrv_debug("IPC pipe create request\n");
		return create_smb2_pipe(smb_work);
	}

	if (!(le32_to_cpu(req->CreateOptions) & FILE_NON_DIRECTORY_FILE_LE)) {
		cifssrv_debug("GOT Opendir\n");
		open_directory = 1;
	}

	if (req->CreateContextsOffset != 0 && durable_enable) {
		context = smb2_find_context_vals(req,
				SMB2_CREATE_DURABLE_HANDLE_REQUEST);
		recon_state =
		  (struct create_durable *)smb2_find_context_vals(req,
				SMB2_CREATE_DURABLE_HANDLE_RECONNECT);

		if (recon_state != NULL) {

			durable_reconnect = true;
			persistent_id =
			  le64_to_cpu(recon_state->Data.Fid.PersistentFileId);
			durable_state =
			  cifssrv_get_durable_state(persistent_id);
			if (durable_state == NULL) {
				cifssrv_err("Failed to get Durable handle state\n");
				rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
				rc = -EINVAL;
				goto err_out1;
			}

			cifssrv_debug("Persistent-id from reconnect = %llu server = 0x%p\n",
				     persistent_id, durable_state->server);
			goto reconnect;
		} else if (context != NULL &&
			req->RequestedOplockLevel == SMB2_OPLOCK_LEVEL_BATCH) {

			context_name = (char *)context + context->NameOffset;

			cifssrv_debug("context name = %s name offset=%u\n",
					context_name, context->NameOffset);

			durable_open = true;
			cifssrv_debug("Request for durable open\n");
		}
	}

	/* Parse non-durable handle create contexts */
	if (req->CreateContextsOffset != 0) {
		context = smb2_find_context_vals(req, SMB2_CREATE_EA_BUFFER);
		if (context != NULL) {
			rsp->hdr.Status = NT_STATUS_EAS_NOT_SUPPORTED;
			rc = -EOPNOTSUPP;
			goto err_out1;
		}
	}

	if (!req->NameLength) {
		share = find_matching_share(rsp->hdr.TreeId);
		if (!share) {
			rsp->hdr.Status = NT_STATUS_NO_MEMORY;
			rc = -ENOMEM;
			goto err_out1;
		}

		len = strlen(share->path);
		cifssrv_debug("[%s] %d\n", __func__, len);
		name = kmalloc(len + 1, GFP_KERNEL);
		if (!name) {
			rsp->hdr.Status = NT_STATUS_NO_MEMORY;
			rc = -ENOMEM;
			goto err_out1;
		}
		memcpy(name, share->path, len);
		*(name + len) = '\0';

	} else {
		name = smb2_get_name(req->Buffer, req->NameLength, smb_work);
	}

	if (IS_ERR(name)) {
		rc = PTR_ERR(name);
		goto err_out1;
	}

	cifssrv_debug("converted name = %s\n", name);

	if (le32_to_cpu(req->CreateOptions) & FILE_DELETE_ON_CLOSE_LE) {
		/*
		 * On delete request, instead of following up, need to
		 * look the current entity
		 */
		rc = smb_kern_path(name, 0, &path, 1);
	} else {
		/*
		* Use LOOKUP_FOLLOW to follow the path of
		* symlink in path buildup
		*/
		rc = smb_kern_path(name, LOOKUP_FOLLOW, &path, 1);
		if (rc) { /* Case for broken link ?*/
			rc = smb_kern_path(name, 0, &path, 1);
		}
	}
	if (rc) {
		file_present = false;
		cifssrv_debug("can not get linux path for %s, rc = %d\n",
				name, rc);
	} else
		generic_fillattr(path.dentry->d_inode, &stat);

	if (file_present && !open_directory && S_ISDIR(stat.mode)) {
		cifssrv_debug("Can't open dir %s, request is to open file\n",
			      name);
		rsp->hdr.Status = NT_STATUS_FILE_IS_A_DIRECTORY;
		rc = -EINVAL;
		goto err_out;
	}

	/* set flags */
	open_flags = smb2_create_open_flags(file_present, req->DesiredAccess,
							req->CreateDisposition);

	/*create file if not present */
	mode |= S_IRWXUGO;
	if (!file_present && (open_flags & O_CREAT)) {
		cifssrv_debug("%s: file does not exist, so creating\n",
				__func__);
		if (le32_to_cpu(req->CreateOptions) & FILE_DIRECTORY_FILE_LE) {
			cifssrv_debug("%s: creating directory\n", __func__);
			mode |= S_IFDIR;
			rc = smb_vfs_mkdir(name, mode);
			if (rc) {
				rsp->hdr.Status = cpu_to_le32(
						NT_STATUS_DATA_ERROR);
				rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;
				kfree(name);
				return rc;
			}

		} else {
			cifssrv_debug("%s: creating regular file\n", __func__);
			mode |= S_IFREG;
			rc = smb_vfs_create(name, mode);
			if (rc) {
				rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;
				kfree(name);
				return rc;
			}
		}

		rc = smb_kern_path(name, 0, &path, 0);
		if (rc) {
			cifssrv_err("cannot get linux path (%s), err = %d\n",
				name, rc);
			rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;
			kfree(name);
			return rc;
		}
	} else if (!file_present && !(open_flags & O_CREAT)) {
		kfree(name);
		cifssrv_debug("%s: returning as file does not exist\n",
				__func__);
		rsp->hdr.Status = cpu_to_le32(NT_STATUS_OBJECT_NAME_NOT_FOUND);
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	if (!S_ISDIR(path.dentry->d_inode->i_mode) &&
			open_flags & O_TRUNC) {
		if (file_present && oplocks_enable)
			smb_break_all_oplock(server, NULL,
					path.dentry->d_inode);

		rc = vfs_truncate(&path, 0);
		if (rc) {
			cifssrv_err("vfs_truncate failed, rc %d\n", rc);
			goto err_out;
		}
	}

	filp = dentry_open(&path, open_flags|O_LARGEFILE, current_cred());
	if (IS_ERR(filp)) {
		rc = PTR_ERR(filp);
		cifssrv_err("dentry open for dir failed, rc %d\n", rc);
		goto err_out;
	}

	pathname = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!pathname) {
		rc = -ENOMEM;
		cifssrv_err("Failed to allocate memory for linkpath\n");
		goto err_out;
	}
	lname = d_path(&(filp->f_path), pathname, PATH_MAX);
	if (IS_ERR(lname)) {
		rc = PTR_ERR(lname);
		kfree(pathname);
		goto err_out;
	}
	if (strncmp(name, lname, PATH_MAX)) {
		islink = true;
		cifssrv_debug("Case for symlink follow, name(%s)->path(%s)\n",
				name, lname);
		rc = smb_kern_path(name, 0, &lpath, 0);
		if (rc) {
			cifssrv_err("cannot get linux path (%s), err = %d\n",
				name, rc);
			kfree(pathname);
			goto err_out;
		}
		lfilp = dentry_open(&lpath, open_flags
				| O_LARGEFILE, current_cred());
		if (IS_ERR(lfilp)) {
			rc = PTR_ERR(lfilp);
			cifssrv_err("dentry open for (%s) failed, rc %d\n",
				name, rc);
			kfree(pathname);
			path_put(&lpath);
			goto err_out;
		}
		path_put(&lpath);
	}
	kfree(pathname);

	if (file_present) {
		if (!(open_flags & O_TRUNC))
			file_info = FILE_OPENED;
		else
			file_info = FILE_OVERWRITTEN;

		if ((req->CreateDisposition & 0x00000007) == FILE_SUPERSEDE_LE)
			file_info = FILE_SUPERSEDED;
	} else if (open_flags & O_CREAT)
		file_info = FILE_CREATED;

	smb_vfs_set_fadvise(filp, le32_to_cpu(req->CreateOptions));

reconnect:

	if (durable_reconnect) {
		rc = cifssrv_durable_reconnect(server, durable_state, &filp);
		if (rc < 0) {
			rsp->hdr.Status = NT_STATUS_OBJECT_NAME_NOT_FOUND;
			goto err_out1;
		}
		cifssrv_debug("recovered filp = 0x%p\n", filp);
		path = filp->f_path;

		/* Fetch the filename */
		name = smb2_get_name_from_filp(filp);

		if (IS_ERR(name)) {
			rc = PTR_ERR(name);
			if (rc == -ENOMEM)
				rsp->hdr.Status = NT_STATUS_NO_MEMORY;
			else
				rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
			goto err_out1;
		}
		path_get(&path);
		durable_reopened = true;
	}

	/* Obtain Volatile-ID */
	volatile_id = cifssrv_get_unused_id(&server->fidtable);
	if (volatile_id < 0) {
		cifssrv_err("failed to get unused volatile_id for file\n");
		rc = volatile_id;
		goto err_out;
	}

	cifssrv_debug("volatile_id returned: %d\n", volatile_id);
	fp = insert_id_in_fidtable(server, volatile_id, filp);
	if (fp == NULL) {
		cifssrv_err("volatile_id insert failed\n");
		cifssrv_close_id(&server->fidtable, volatile_id);
		rc = -ENOMEM;
		goto err_out;
	}
	if (islink) {
		fp->lfilp = lfilp;
		fp->islink = islink;
	}

	generic_fillattr(path.dentry->d_inode, &stat);

	/* In case of durable reopen try to get BATCH oplock, irrespective
	   of the value of requested oplock in the request */
	if (durable_reopened)
		oplock = SMB2_OPLOCK_LEVEL_BATCH;
	else
		oplock = req->RequestedOplockLevel;

	if (oplock && (req->DesiredAccess & ~(FILE_READ_ATTRIBUTES_LE |
					FILE_WRITE_ATTRIBUTES_LE |
					FILE_SYNCHRONIZE_LE)) == 0)
		attrib_only = true;

	if (!oplocks_enable || S_ISDIR(file_inode(filp)->i_mode)) {
		oplock = SMB2_OPLOCK_LEVEL_NONE;
	} else if (oplock == SMB2_OPLOCK_LEVEL_LEASE) {
		if (!(server->capabilities & SMB2_GLOBAL_CAP_LEASING)
				|| stat.nlink != 1)
			oplock = SMB2_OPLOCK_LEVEL_NONE;
		else {
			oplock = parse_lease_state(req, &lc);
			if (oplock) {
				lk = (char *)&lc.LeaseKey;
				cifssrv_debug("lease req for(%s) oplock 0x%x, "
						"lease state 0x%x\n",
						name, oplock,
						lc.CurrentLeaseState);
				rc = smb_grant_oplock(server, &oplock,
					volatile_id, fp, req->hdr.TreeId,
					&lc, attrib_only);
				if (rc)
					oplock = SMB2_OPLOCK_LEVEL_NONE;
			}
		}
	} else if (oplock & (SMB2_OPLOCK_LEVEL_BATCH |
				SMB2_OPLOCK_LEVEL_EXCLUSIVE)) {
		rc = smb_grant_oplock(server, &oplock, volatile_id, fp,
				req->hdr.TreeId, NULL, attrib_only);
		if (rc)
			oplock = SMB2_OPLOCK_LEVEL_NONE;
	}

	if (S_ISDIR(stat.mode))
		fp->readdir_data.dirent = NULL;

	if (le32_to_cpu(req->CreateOptions) & FILE_DELETE_ON_CLOSE_LE)
		fp->delete_on_close = 1;

	/* Get Persistent-ID */
	if (durable_reopened == false) {
		durable_open = durable_open &&
				(oplock == SMB2_OPLOCK_LEVEL_BATCH);
		rc = cifssrv_insert_in_global_table(server, volatile_id,
						       filp, durable_open);

		if (rc < 0) {
			cifssrv_err("failed to get persistent_id for file\n");
			cifssrv_close_id(&server->fidtable, volatile_id);
			durable_open = false;
			goto err_out;
		} else {
			persistent_id = rc;
			rc = 0;
		}

		if (durable_open)
			fp->is_durable = 1;

	} else if (durable_reopened == true &&
		    oplock == SMB2_OPLOCK_LEVEL_BATCH) {
		/* During durable reconnect able to fetch/verify durable state
		   but couldn't get batch oplock then we will not come here */
		cifssrv_update_durable_state(server, persistent_id,
					     volatile_id, filp);
		fp->is_durable = 1;
		file_info = FILE_OPENED;
	}

	fp->persistent_id = persistent_id;

	rsp->StructureSize = cpu_to_le16(89);
	rsp->OplockLevel = oplock;
	rsp->Reserved = 0;
	rsp->CreateAction = file_info;

	create_time = min3(cifs_UnixTimeToNT(stat.ctime),
			cifs_UnixTimeToNT(stat.mtime),
			cifs_UnixTimeToNT(stat.atime));

	if (!create_time)
		create_time = min(cifs_UnixTimeToNT(stat.ctime),
				cifs_UnixTimeToNT(stat.mtime));

	rsp->CreationTime = cpu_to_le64(create_time);
	rsp->LastAccessTime = cpu_to_le64(cifs_UnixTimeToNT(stat.atime));
	rsp->LastWriteTime = cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
	rsp->ChangeTime = cpu_to_le64(cifs_UnixTimeToNT(stat.ctime));
	rsp->AllocationSize = cpu_to_le64(stat.blocks << 9);
	rsp->EndofFile = cpu_to_le64(stat.size);
	rsp->FileAttributes = cpu_to_le32(smb2_get_dos_mode(&stat));

	rsp->Reserved2 = 0;

	rsp->PersistentFileId = cpu_to_le64(persistent_id);
	rsp->VolatileFileId = cpu_to_le64(volatile_id);
	rsp->CreateContextsOffset = 0;
	rsp->CreateContextsLength = 0;
	inc_rfc1001_len(rsp_org, 88); /* StructureSize - 1*/

	/* If lease is request send lease context response */
	if (lk && (req->RequestedOplockLevel == SMB2_OPLOCK_LEVEL_LEASE)) {
		cifssrv_debug("lease granted on(%s) oplock 0x%x, "
				"lease state 0x%x\n",
				name, oplock,
				lc.CurrentLeaseState);
		rsp->OplockLevel = SMB2_OPLOCK_LEVEL_LEASE;
		create_lease_buf(rsp->Buffer, lk, oplock,
			lc.CurrentLeaseState & SMB2_LEASE_HANDLE_CACHING);
		rsp->CreateContextsOffset = offsetof(struct smb2_create_rsp,
				Buffer) - 4;
		rsp->CreateContextsLength = sizeof(struct create_lease);
		inc_rfc1001_len(rsp_org, sizeof(struct create_lease));
	}

	if (durable_open) {
		create_durable_rsp_buf(rsp->Buffer + rsp->CreateContextsLength);
		rsp->CreateContextsOffset = offsetof(struct smb2_create_rsp,
				Buffer) - 4 + rsp->CreateContextsLength;
		rsp->CreateContextsLength += sizeof(struct create_durable_rsp);
		inc_rfc1001_len(rsp_org, sizeof(struct create_durable_rsp));
	}

err_out:
	path_put(&path);
	kfree(name);
err_out1:
	if (rc) {
		if (!rsp->hdr.Status)
			rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;

		if (filp != NULL && !IS_ERR(filp))
			fput(filp);
		if (fp != NULL)
			kfree(fp);
		smb2_set_err_rsp(smb_work);
	} else
		server->stats.open_files_count++;

	return 0;
}

/**
 * fill_common_info() - convert unix stat information to smb stat format
 * @p:		destination buffer
 * @kstat:	file stat information
 */
static inline void *fill_common_info(char **p, struct kstat *kstat)
{
	FILE_DIRECTORY_INFO *info = (FILE_DIRECTORY_INFO *)(*p);
	info->FileIndex = 0;
	info->CreationTime = cpu_to_le64(
			cifs_UnixTimeToNT(kstat->ctime));
	info->LastAccessTime = cpu_to_le64(
			cifs_UnixTimeToNT(kstat->atime));
	info->LastWriteTime = cpu_to_le64(
			cifs_UnixTimeToNT(kstat->mtime));
	info->ChangeTime = cpu_to_le64(
			cifs_UnixTimeToNT(kstat->mtime));
	info->EndOfFile = cpu_to_le64(kstat->size);
	info->AllocationSize = cpu_to_le64(kstat->blocks << 9);
	info->ExtFileAttributes = S_ISDIR(kstat->mode) ?
		ATTR_DIRECTORY : ATTR_NORMAL;

	return info;
}

/**
 * calc_nl_and_neo() - calculates name_len(nl) and next_entry_offset(neo)
 * @buf:		target buffer for file name
 * @namestr:		source filename buffer
 * @len:		source buffer length
 * @size:		used buffer size
 * @local_nls		code page table
 * @name_len:		file name length after conversion
 * @next_entry_offset:	offset of dentry
 * @space_remaining:	space remaining in response buffer
 * @data_count:		used response buffer size
 *
 * Return:      false if next entry could not fit in current response buffer,
 *		otherwise true
 */
static inline bool calc_nl_and_neo(char *buf, char *namestr, int len, int size,
		const struct nls_table *local_nls, int *name_len,
		int *next_entry_offset, int *space_remaining, int *data_count)
{
	*name_len = smbConvertToUTF16((__le16 *)buf,
			namestr, len, local_nls, 0);
	(*name_len)++;/*for NULL character*/
	*name_len *= 2;
	*next_entry_offset = (size - 1 +
			*name_len + 7) & ~7;

	if (*next_entry_offset > *space_remaining) {
		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				*next_entry_offset, *data_count);
		*space_remaining = 0;
		return false;
	}
	return true;
}

/**
 * smb2_populate_readdir_entry() - encode directory entry in smb2 response buffer
 * @server:	TCP server instance of connection
 * @info_level:	smb information level
 * @p:		smb response buffer pointer
 * @namestr:	dirent name string
 * @space_remaining:	remaining space in readdir response buffer
 * @last_entry_offset:	offset of last entry in directory
 * @kstat:	dirent stat information
 * @data_count:	used buffer size
 *
 * if directory has many entries, find first can't read it fully.
 * find next might be called multiple times to read remaining dir entries
 *
 * Return:	0 on success, otherwise error
 */
static int smb2_populate_readdir_entry(struct tcp_server_info *server,
	int info_level, char **p, char *namestr, int *space_remaining,
		int *last_entry_offset,	struct kstat *kstat, int *data_count)
{
	int name_len;
	int next_entry_offset;

	SEARCH_ID_FULL_DIR_INFO *dinfo;
	FILE_DIRECTORY_INFO *fdinfo;
	FILE_ID_BOTH_DIRECTORY_INFO *fibdinfo;
	FILE_BOTH_DIRECTORY_INFO *fbdinfo;
	FILE_FULL_DIRECTORY_INFO *ffdinfo;
	FILE_NAMES_INFO *fninfo;

	char *name_size_check_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!name_size_check_buf)
		return -ENOMEM;
	switch (info_level) {
	case FILE_FULL_DIRECTORY_INFORMATION:
		if (!calc_nl_and_neo(name_size_check_buf, namestr, PATH_MAX,
					sizeof(FILE_FULL_DIRECTORY_INFO),
					server->local_nls, &name_len,
					&next_entry_offset,
					space_remaining, data_count))
			break;

		ffdinfo = (FILE_FULL_DIRECTORY_INFO *)
				fill_common_info(p, kstat);
		ffdinfo->FileNameLength = cpu_to_le32(name_len);
		ffdinfo->EaSize = 0;

		memcpy(ffdinfo->FileName, name_size_check_buf, name_len);
		ffdinfo->FileName[name_len - 2] = 0;
		ffdinfo->FileName[name_len - 1] = 0;

		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				next_entry_offset, *data_count);
		ffdinfo->NextEntryOffset = next_entry_offset;
		*last_entry_offset = *data_count;
		*data_count = *data_count + next_entry_offset;
		*space_remaining -= next_entry_offset;
		memset((char *)ffdinfo + sizeof(FILE_FULL_DIRECTORY_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				sizeof(FILE_FULL_DIRECTORY_INFO) - 1
								+ name_len);
		*p =  (char *)(*p) + next_entry_offset;
		break;
	case FILE_BOTH_DIRECTORY_INFORMATION:
		if (!calc_nl_and_neo(name_size_check_buf, namestr, PATH_MAX,
					sizeof(FILE_BOTH_DIRECTORY_INFO),
					server->local_nls, &name_len,
					&next_entry_offset,
					space_remaining, data_count))
			break;

		fbdinfo = (FILE_BOTH_DIRECTORY_INFO *)
				fill_common_info(p, kstat);
		fbdinfo->FileNameLength = cpu_to_le32(name_len);
		fbdinfo->EaSize = 0;
		fbdinfo->ShortNameLength = 0;
		fbdinfo->Reserved = 0;

		memcpy(fbdinfo->FileName, name_size_check_buf, name_len);
		fbdinfo->FileName[name_len - 2] = 0;
		fbdinfo->FileName[name_len - 1] = 0;

		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				next_entry_offset, *data_count);
		fbdinfo->NextEntryOffset = next_entry_offset;
		*last_entry_offset = *data_count;
		*data_count = *data_count + next_entry_offset;
		*space_remaining -= next_entry_offset;
		memset((char *)fbdinfo + sizeof(FILE_BOTH_DIRECTORY_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				sizeof(FILE_BOTH_DIRECTORY_INFO) - 1
								+ name_len);
		*p =  (char *)(*p) + next_entry_offset;
		break;
	case FILE_DIRECTORY_INFORMATION:
		if (!calc_nl_and_neo(name_size_check_buf, namestr, PATH_MAX,
					sizeof(FILE_DIRECTORY_INFO),
					server->local_nls, &name_len,
					&next_entry_offset,
					space_remaining, data_count))
			break;

		fdinfo = (FILE_DIRECTORY_INFO *)fill_common_info(p, kstat);
		fdinfo->FileNameLength = cpu_to_le32(name_len);

		memcpy(fdinfo->FileName, name_size_check_buf, name_len);
		fdinfo->FileName[name_len - 2] = 0;
		fdinfo->FileName[name_len - 1] = 0;

		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				next_entry_offset, *data_count);
		fdinfo->NextEntryOffset = next_entry_offset;
		*last_entry_offset = *data_count;
		*data_count = *data_count + next_entry_offset;
		*space_remaining -= next_entry_offset;
		memset((char *)fdinfo + sizeof(FILE_DIRECTORY_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				(sizeof(FILE_DIRECTORY_INFO) - 1 + name_len));
		*p =  (char *)(*p) + next_entry_offset;
		break;

	case FILE_NAMES_INFORMATION:
		if (!calc_nl_and_neo(name_size_check_buf, namestr, PATH_MAX,
					sizeof(FILE_NAMES_INFO),
					server->local_nls, &name_len,
					&next_entry_offset,
					space_remaining, data_count))
			break;

		fninfo = (FILE_NAMES_INFO *)fill_common_info(p, kstat);
		fninfo->FileNameLength = cpu_to_le32(name_len);

		memcpy(fninfo->FileName, name_size_check_buf, name_len);
		fninfo->FileName[name_len - 2] = 0;
		fninfo->FileName[name_len - 1] = 0;

		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				next_entry_offset, *data_count);
		fninfo->NextEntryOffset = next_entry_offset;
		*last_entry_offset = *data_count;
		*data_count = *data_count + next_entry_offset;
		*space_remaining -= next_entry_offset;
		memset((char *)fninfo + sizeof(FILE_NAMES_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				(sizeof(FILE_NAMES_INFO) - 1 + name_len));
		*p =  (char *)(*p) + next_entry_offset;
		break;

	case FILEID_FULL_DIRECTORY_INFORMATION:
		if (!calc_nl_and_neo(name_size_check_buf, namestr, PATH_MAX,
					sizeof(SEARCH_ID_FULL_DIR_INFO),
					server->local_nls, &name_len,
					&next_entry_offset,
					space_remaining, data_count))
			break;

		dinfo = (SEARCH_ID_FULL_DIR_INFO *)fill_common_info(p, kstat);
		dinfo->FileNameLength = cpu_to_le32(name_len);
		dinfo->EaSize = 0;
		dinfo->Reserved = 0;
		dinfo->UniqueId = cpu_to_le64(kstat->ino);

		memcpy(dinfo->FileName, name_size_check_buf, name_len);
		dinfo->FileName[name_len - 2] = 0;
		dinfo->FileName[name_len - 1] = 0;

		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				next_entry_offset, *data_count);
		dinfo->NextEntryOffset = next_entry_offset;
		*last_entry_offset = *data_count;
		*data_count = *data_count + next_entry_offset;
		*space_remaining -= next_entry_offset;
		memset((char *)dinfo + sizeof(SEARCH_ID_FULL_DIR_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				sizeof(SEARCH_ID_FULL_DIR_INFO) - 1 + name_len);
		*p =  (char *)(*p) + next_entry_offset;
		break;
	case FILEID_BOTH_DIRECTORY_INFORMATION:
		if (!calc_nl_and_neo(name_size_check_buf, namestr, PATH_MAX,
					sizeof(FILE_ID_BOTH_DIRECTORY_INFO),
					server->local_nls, &name_len,
					&next_entry_offset,
					space_remaining, data_count))
			break;

		fibdinfo = (FILE_ID_BOTH_DIRECTORY_INFO *)
			fill_common_info(p, kstat);
		fibdinfo->FileNameLength = cpu_to_le32(name_len);
		fibdinfo->EaSize = 0;
		fibdinfo->UniqueId = cpu_to_le64(kstat->ino);
		fibdinfo->ShortNameLength =
			smb2_get_shortname(server, namestr,
					&(fibdinfo->ShortName[0]));
		fibdinfo->Reserved = 0;
		fibdinfo->Reserved2 = cpu_to_le16(0);

		memcpy(fibdinfo->FileName, name_size_check_buf, name_len);
		fibdinfo->FileName[name_len - 2] = 0;
		fibdinfo->FileName[name_len - 1] = 0;

		cifssrv_debug("space_remaining %d next_entry_offset %d"
				" data_count=%d\n", *space_remaining,
				next_entry_offset, *data_count);
		fibdinfo->NextEntryOffset = next_entry_offset;
		*last_entry_offset = *data_count;
		*data_count = *data_count + next_entry_offset;
		*space_remaining -= next_entry_offset;
		memset((char *)fibdinfo + sizeof(FILE_ID_BOTH_DIRECTORY_INFO)
			- 1 + name_len, '\0', next_entry_offset -
			sizeof(FILE_ID_BOTH_DIRECTORY_INFO) - 1 + name_len);
		*p =  (char *)(*p) + next_entry_offset;
		break;

	default:
		kfree(name_size_check_buf);
		cifssrv_err("%s: failed\n", __func__);
		return -EOPNOTSUPP;
	}
	kfree(name_size_check_buf);
	return 0;
}

/**
 * read_next_entry() - read next directory entry and return absolute name string
 * @kstat:		stat of next dirent
 * @buf_p:		target response buffer
 * @dir_path_len:	directory path length
 * @dir_path_name:	directory path name
 *
 * Return:	on success return absolute path of directory entry,
 *		otherwise NULL
 */
static inline char *read_next_entry(struct kstat *kstat,
	struct smb_dirent *buf_p, int dir_path_len, char *dir_path_name)
{
	struct path path;
	int rc = 0, file_pathlen;
	char *namestr;

	/* 1 for '/'*/
	file_pathlen = dir_path_len +  buf_p->namelen + 1;
	namestr = kmalloc(file_pathlen + 1, GFP_KERNEL);
	if (!namestr) {
		cifssrv_err("Name memory failed for length %d,"
			" buf_name_len %d\n", dir_path_len, buf_p->namelen);
		return ERR_PTR(-ENOMEM);
	}

	memcpy(namestr, dir_path_name, dir_path_len);
	memset(namestr + dir_path_len, '/', 1);
	memcpy(namestr + dir_path_len + 1, buf_p->name, buf_p->namelen);
	namestr[file_pathlen] = '\0';

	rc = smb_kern_path(namestr, 0, &path, 1);
	if (rc) {
		cifssrv_err("look up failed for (%s) with rc=%d\n",
			namestr, rc);
		kfree(namestr);
		return ERR_PTR(rc);
	}

	generic_fillattr(path.dentry->d_inode, kstat);
	memcpy(namestr, buf_p->name, buf_p->namelen);
	namestr[buf_p->namelen] = '\0';
	path_put(&path);
	return namestr;
}

/**
 * smb2_query_dir() - handler for smb2 readdir i.e. query dir command
 * @smb_work:	smb work containing query dir request buffer
 *
 * Return:	0
 */
int smb2_query_dir(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_query_directory_req *req;
	struct smb2_query_directory_rsp *rsp, *rsp_org;
	int data_count = 0, srch_flag;
	int rc = 0, err = 0;
	uint64_t id = -1;
	int reclen, dir_path_len;
	struct smb_dirent *buf_p;
	int num_entry = 0;
	struct kstat kstat;
	struct inode *inode;
	char *dir_path_name, *p, *namestr, *srch_ptr = NULL, *pathname = NULL;
	int max_outbuf, space_remaining, used_count, srch_ptr_len;
	int dir_entry_bytes_count;
	struct cifssrv_file *dirdesc_fp;
	struct smb_readdir_data r_data = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		.ctx.actor = smb_filldir,
#endif
	};

	req = (struct smb2_query_directory_req *)smb_work->buf;
	rsp = (struct smb2_query_directory_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_query_directory_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_query_directory_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);
		if (le64_to_cpu(req->VolatileFileId == -1)) {
			cifssrv_debug("Compound request assigning stored FID = %llu\n",
				    smb_work->cur_local_fid);
			id = smb_work->cur_local_fid;
		}
	}

	if (req->StructureSize != 33) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (id == -1)
		id = le64_to_cpu(req->VolatileFileId);

	dirdesc_fp = get_id_from_fidtable(server, id);
	if (!dirdesc_fp) {
		/* Need to change the error */
		rsp->hdr.Status = NT_STATUS_NO_MEMORY;
		cifssrv_err("Invalid id for close: %llu\n", id);
		rc = -EINVAL;
		goto out;
	}

	rsp->StructureSize = cpu_to_le16(9);

	p = (char *)rsp->Buffer;

	inode = dirdesc_fp->filp->f_path.dentry->d_inode;
	if (!S_ISDIR(inode->i_mode)) {
		cifssrv_err("can't do query dir for a file\n");
		rc = -EINVAL;
		goto out;
	}
	srch_flag = req->Flags;
	srch_ptr = smb_strndup_from_utf16(req->Buffer,
		le32_to_cpu(req->FileNameLength), 1,
				smb_work->server->local_nls);

	if (IS_ERR(srch_ptr)) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		rc = -EINVAL;
		cifssrv_debug("Search Pattern not found\n");
		goto out;
	} else
		cifssrv_debug("Search pattern is %s\n", srch_ptr);

	srch_ptr_len = strlen(srch_ptr);
	max_outbuf = le32_to_cpu(req->OutputBufferLength);
	space_remaining = max_outbuf - sizeof(struct smb2_query_directory_rsp);
	pathname = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!pathname) {
		cifssrv_err("Failed to allocate memory\n");
		rsp->hdr.Status = NT_STATUS_NO_MEMORY;
		rc = -ENOMEM;
		goto out;
	}

	dir_path_name = d_path(&(dirdesc_fp->filp->f_path), pathname, PATH_MAX);
	if (IS_ERR(dir_path_name)) {
		cifssrv_err("Failed to get complete dir pathname\n");
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		rc = PTR_ERR(dir_path_name);
		goto out;
	}
	cifssrv_debug("Directory name is %s\n", dir_path_name);
	dir_path_len = strlen(dir_path_name);

	if (dirdesc_fp->readdir_data.dirent == NULL) {
		dirdesc_fp->readdir_data.dirent =
			(void *)__get_free_page(GFP_KERNEL);
		if (!dirdesc_fp->readdir_data.dirent) {
			cifssrv_err("Failed to allocate memory\n");
			rsp->hdr.Status = NT_STATUS_NO_MEMORY;
			rc = -ENOMEM;
			goto out;
		}
		dirdesc_fp->readdir_data.used = 0;
		dirdesc_fp->readdir_data.full = 0;
		dirdesc_fp->search_over = 0;
		dirdesc_fp->dirent_offset = 0;
	} else if (dirdesc_fp->search_over) {
		cifssrv_debug("search ended\n");
		goto no_more_files;
	}

	r_data.dirent = dirdesc_fp->readdir_data.dirent;
	r_data.used = dirdesc_fp->readdir_data.used;
	r_data.full = dirdesc_fp->readdir_data.full;

	if (srch_flag & SMB2_REOPEN) {
		cifssrv_debug("Reopen the directory\n");
		filp_close(dirdesc_fp->filp, NULL);
		dirdesc_fp->filp = filp_open(dir_path_name, O_RDONLY, 0666);
		if (!dirdesc_fp->filp) {
			cifssrv_debug("Reopening dir failed\n");
			rc = -EINVAL;
			goto out;
		}
		goto restart;
	}

	if (srch_flag & SMB2_RESTART_SCANS) {
		generic_file_llseek(dirdesc_fp->filp, 0, SEEK_SET);
		goto restart;
	}
	/* Check if previous buffer is sent completely or not */
	used_count = r_data.used;
	dir_entry_bytes_count = dirdesc_fp->dirent_offset;

	if (dir_entry_bytes_count < used_count) {
		buf_p = (struct smb_dirent *)
			((char *)r_data.dirent + dir_entry_bytes_count);
		while (dir_entry_bytes_count < used_count &&
				space_remaining > 0 &&
				!dirdesc_fp->search_over) {

			reclen = ALIGN(sizeof(struct smb_dirent) +
				buf_p->namelen, sizeof(__le64));

			namestr = read_next_entry(&kstat, buf_p,
					dir_path_len, dir_path_name);
			if (IS_ERR(namestr)) {
				rc = PTR_ERR(namestr);
				cifssrv_debug("Err while dirent read rc = %d\n",
					       rc);
				dir_entry_bytes_count += reclen;
				buf_p = (struct smb_dirent *)((char *)buf_p
					+ reclen);
				rc = 0;
				continue;
			}

			rc = smb2_populate_readdir_entry(server,
					req->FileInformationClass, &p,
					namestr, &space_remaining, &num_entry,
					&kstat, &data_count);
			if (rc) {
				kfree(namestr);
				goto out;
			}
			if (space_remaining != 0)
				dir_entry_bytes_count += reclen;

			if (srch_flag & SMB2_RETURN_SINGLE_ENTRY) {
				dirdesc_fp->dirent_offset =
						dir_entry_bytes_count;
				goto full_buf;
			}
			buf_p = (struct smb_dirent *)((char *)buf_p + reclen);
			kfree(namestr);
		}
		if (dir_entry_bytes_count == used_count) {
			cifssrv_debug("all data written, remaining size %d\n",
							space_remaining);
			dirdesc_fp->dirent_offset = 0;
			r_data.used = 0;
			r_data.full = 0;
		}
	}

	if (space_remaining == 0) {
		cifssrv_debug("Response Buffer is full. Send the data\n");
		dirdesc_fp->dirent_offset = dir_entry_bytes_count;
		goto full_buf;
	}
restart:
	r_data.used = 0;
	r_data.full = 0;
	err = smb_vfs_readdir(dirdesc_fp->filp, smb_filldir, &r_data);
	if (err < 0 || r_data.used == 0) {
		cifssrv_debug("err = %d dir buf used = %u\n",
				err, r_data.used);
		dirdesc_fp->search_over = 1;
	}

	used_count = r_data.used;
	dir_entry_bytes_count = 0;

refill:
	buf_p = (struct smb_dirent *)r_data.dirent;
	cifssrv_debug("dentry_bytes_cnt = %d used_cnt = %d search_over = %d\n",
			dir_entry_bytes_count, used_count,
			dirdesc_fp->search_over);

	while (dir_entry_bytes_count < used_count &&
			space_remaining > 0 && !dirdesc_fp->search_over) {

		reclen = ALIGN(sizeof(struct smb_dirent) + buf_p->namelen,
				sizeof(__le64));

		namestr = read_next_entry(&kstat, buf_p, dir_path_len,
							dir_path_name);
		if (IS_ERR(namestr)) {
			rc = PTR_ERR(namestr);
			cifssrv_debug("Err while dirent read rc = %d\n", rc);
			dir_entry_bytes_count += reclen;
			buf_p = (struct smb_dirent *)((char *)buf_p + reclen);
			rc = 0;
			continue;
		}
		if (srch_flag & SMB2_RETURN_SINGLE_ENTRY) {
			cifssrv_debug("Single entry requested\n");
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
			if (strncmp(srch_ptr, "*", 1) &&
					(srch_ptr_len != buf_p->namelen ||
					 strncasecmp(buf_p->name, srch_ptr,
						 buf_p->namelen))) {
#else
			if (strncmp(srch_ptr, "*", 1) &&
					(srch_ptr_len != buf_p->namelen ||
					 strnicmp(buf_p->name, srch_ptr,
						 buf_p->namelen))) {
#endif
				dir_entry_bytes_count += reclen;
				goto next;
			}
		}
		rc = smb2_populate_readdir_entry(server,
				req->FileInformationClass, &p,
				namestr, &space_remaining, &num_entry,
				&kstat,	&data_count);
		if (rc) {
			kfree(namestr);
			goto out;
		}

		if (space_remaining != 0)
			dir_entry_bytes_count += reclen;

		if (srch_flag & SMB2_RETURN_SINGLE_ENTRY) {
			dirdesc_fp->dirent_offset = dir_entry_bytes_count;
			goto full_buf;
		}
next:
		buf_p = (struct smb_dirent *)((char *)buf_p + reclen);
		kfree(namestr);

	}

	if (dir_entry_bytes_count == used_count && space_remaining != 0 &&
			!dirdesc_fp->search_over) {
		r_data.used = 0;
		r_data.full = 0;
		err = smb_vfs_readdir(dirdesc_fp->filp, smb_filldir, &r_data);
		if (err < 0 || r_data.used == 0) {
			cifssrv_debug("err = %d dir buf used = %u\n",
					err, r_data.used);
			dirdesc_fp->search_over = 1;
		} else {
			dirdesc_fp->dirent_offset = dir_entry_bytes_count;
			dir_entry_bytes_count = 0;
			used_count = r_data.used;
			cifssrv_debug("Going to refill response buffer\n");
			goto refill;
		}
	} else if (dir_entry_bytes_count != used_count && space_remaining == 0
						&& !dirdesc_fp->search_over) {
		/*
		 * Dirent buffer is not passed to client completely.
		 * Save the next offset of buffer to be sent.
		 */
		dirdesc_fp->dirent_offset = dir_entry_bytes_count;
	}
out:
	if (rc) {
		if (dirdesc_fp && dirdesc_fp->readdir_data.dirent) {
			free_page((unsigned long)
					(dirdesc_fp->readdir_data.dirent));
			dirdesc_fp->readdir_data.dirent = NULL;
		}
		if (rsp->hdr.Status == 0)
			rsp->hdr.Status = NT_STATUS_NOT_IMPLEMENTED;
		smb2_set_err_rsp(smb_work);
		cifssrv_err("error while processing smb2 query dir rc = %d\n",
				rc);
		kfree(pathname);
		kfree(srch_ptr);
		return 0;
	}

full_buf:
	if (data_count == 0) {
		if (srch_flag & SMB2_RETURN_SINGLE_ENTRY
			&& strncmp(srch_ptr, "*", 1))
			rsp->hdr.Status = STATUS_OBJECT_NAME_NOT_FOUND;
		goto no_more_files;
	}
	if (dirdesc_fp && dirdesc_fp->readdir_data.dirent) {
		dirdesc_fp->readdir_data.used = r_data.used;
		dirdesc_fp->readdir_data.full = r_data.full;
	}

	if (data_count)
		((FILE_DIRECTORY_INFO *)
			((char *)rsp->Buffer + num_entry))->NextEntryOffset = 0;
	rsp->OutputBufferOffset = cpu_to_le16(72);
	rsp->OutputBufferLength = cpu_to_le32(data_count);
	inc_rfc1001_len(rsp_org, 8 + data_count);
	kfree(pathname);
	kfree(srch_ptr);
	return 0;

no_more_files:
	if (dirdesc_fp && dirdesc_fp->readdir_data.dirent) {
		free_page((unsigned long)(dirdesc_fp->readdir_data.dirent));
		dirdesc_fp->readdir_data.dirent = NULL;
	} else {
		dirdesc_fp->readdir_data.used = r_data.used;
		dirdesc_fp->readdir_data.full = r_data.full;
	}

	if (smb_work->next_smb2_rcv_hdr_off)
		rsp->hdr.Status = 0;
	else if (rsp->hdr.Status == 0)
		rsp->hdr.Status = STATUS_NO_MORE_FILES;

	rsp->StructureSize = cpu_to_le16(9);
	rsp->OutputBufferOffset = cpu_to_le16(0);
	rsp->OutputBufferLength = cpu_to_le32(0);
	rsp->Buffer[0] = 0;
	inc_rfc1001_len(rsp_org, 9);
	kfree(pathname);
	kfree(srch_ptr);
	return 0;
}

/**
 * smb2_query_dir() - handler for smb2 query info command
 * @smb_work:	smb work containing query info request buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_query_info(struct smb_work *smb_work)
{
	struct smb2_query_info_req *req;
	struct smb2_query_info_rsp *rsp, *rsp_org;
	int rc = 0;

	req = (struct smb2_query_info_req *)smb_work->buf;
	rsp = (struct smb2_query_info_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_query_info_req *)((char *)req +
				smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_query_info_rsp *)((char *)rsp +
				smb_work->next_smb2_rsp_hdr_off);

	}

	cifssrv_debug("GOT query info request\n");

	if (req->StructureSize != 41) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	switch (req->InfoType) {
	case SMB2_O_INFO_FILE:
		cifssrv_debug("GOT SMB2_O_INFO_FILE\n");
		rc = smb2_info_file(smb_work);
		break;
	case SMB2_O_INFO_FILESYSTEM:
		cifssrv_debug("GOT SMB2_O_INFO_FILESYSTEM\n");
		rc = smb2_info_filesystem(smb_work);
		break;
	default:
		cifssrv_debug("InfoType %d not supported yet\n", req->InfoType);
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		rc = -EOPNOTSUPP;
	}

	if (rc < 0) {
		if (rsp->hdr.Status == 0)
			rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		smb2_set_err_rsp(smb_work);

		cifssrv_debug("error while processing smb2 query rc = %d\n",
			      rc);
		return rc;
	}
	rsp->StructureSize = cpu_to_le16(9);
	rsp->OutputBufferOffset = cpu_to_le16(72);
	inc_rfc1001_len(rsp_org, 8);
	return 0;
}

/**
 * smb2_close_pipe() - handler for closing IPC pipe
 * @smb_work:	smb work containing close request buffer
 *
 * Return:	0
 */
static int smb2_close_pipe(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	int id;
	int rc = 0;

	struct smb2_close_req *req = (struct smb2_close_req *)smb_work->buf;
	struct smb2_close_rsp *rsp = (struct smb2_close_rsp *)smb_work->rsp_buf;

	id = req->VolatileFileId;

	if (!server->pipe_desc || id != server->pipe_desc->id) {
		cifssrv_debug("Pipe not opened or invalid in Pipe id\n");
		if (server->pipe_desc)
			cifssrv_err("Incoming id = %d opened pipe id = %d\n",
					id, server->pipe_desc->id);
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
		return 0;
	}
	rsp->StructureSize = cpu_to_le16(60);
	rsp->Flags = 0;
	rsp->Reserved = 0;
	rsp->CreationTime = 0;
	rsp->LastAccessTime = 0;
	rsp->LastWriteTime = 0;
	rsp->ChangeTime = 0;
	rsp->AllocationSize = 0;
	rsp->EndOfFile = 0;
	rsp->Attributes = 0;
	inc_rfc1001_len(rsp, 60);

	rc = close_pipe_id(server, id);
	if (rc < 0) {
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
	}

	return 0;
}

/**
 * smb2_close() - handler for smb2 close file command
 * @smb_work:	smb work containing close request buffer
 *
 * Return:	0
 */
int smb2_close(struct smb_work *smb_work)
{
	uint64_t volatile_id = -1, persistent_id = -1, sess_id;
	struct smb2_close_req *req = (struct smb2_close_req *)smb_work->buf;
	struct smb2_close_rsp *rsp = (struct smb2_close_rsp *)smb_work->rsp_buf;
	struct smb2_close_rsp *rsp_org;
	struct tcp_server_info *server = smb_work->server;
	int err = 0;

	rsp_org = rsp;
	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_close_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_close_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);
	}

	if (req->StructureSize != 24) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (rsp->hdr.TreeId == 1) {
		cifssrv_debug("IPC pipe close request\n");
		return smb2_close_pipe(smb_work);
	}

	sess_id = req->hdr.SessionId;
	if (le32_to_cpu(req->hdr.Flags) &
			SMB2_FLAGS_RELATED_OPERATIONS)
		sess_id = smb_work->cur_local_sess_id;

	smb_work->cur_local_sess_id = 0;
	if (check_session_id(server, sess_id))
		smb_work->cur_local_sess_id = sess_id;
	else {
		rsp->hdr.Status = NT_STATUS_USER_SESSION_DELETED;
		if (le32_to_cpu(req->hdr.Flags) &
				SMB2_FLAGS_RELATED_OPERATIONS)
			rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		err = -EBADF;
		goto out;
	}

	if (smb_work->next_smb2_rcv_hdr_off &&
			le64_to_cpu(req->VolatileFileId) == -1) {
		if (!smb_work->cur_local_fid) {
			/* file open failed, return EINVAL */
			cifssrv_debug("file open was failed\n");
			rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
			err = -EBADF;
			goto out;
		} else if (smb_work->cur_local_fid == -1) {
			/* file already closed, return FILE_CLOSED */
			cifssrv_debug("file already closed\n");
			rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
			err = -EBADF;
			goto out;
		} else {
			cifssrv_debug("Compound request assigning stored FID = %llu: %llu\n",
					smb_work->cur_local_fid,
					smb_work->cur_local_pfid);
			volatile_id = smb_work->cur_local_fid;
			persistent_id = smb_work->cur_local_pfid;

			/* file closed, stored id is not valid anymore */
			smb_work->cur_local_fid = -1;
			smb_work->cur_local_pfid = -1;
		}
	} else {
		volatile_id = le64_to_cpu(req->VolatileFileId);
		persistent_id = le64_to_cpu(req->PersistentFileId);
	}
	cifssrv_debug("volatile_id = %llu persistent_id = %llu\n",
			volatile_id, persistent_id);

	err = close_persistent_id(persistent_id);
	if (err)
		goto out;
	err = close_id(server, volatile_id);
	if (err)
		goto out;

	rsp->StructureSize = cpu_to_le16(60);
	rsp->Flags = 0;
	rsp->Reserved = 0;
	rsp->CreationTime = 0;
	rsp->LastAccessTime = 0;
	rsp->LastWriteTime = 0;
	rsp->ChangeTime = 0;
	rsp->AllocationSize = 0;
	rsp->EndOfFile = 0;
	rsp->Attributes = 0;

out:
	if (err) {
		if (rsp->hdr.Status == 0)
			rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
		smb2_set_err_rsp(smb_work);
	} else {
		server->stats.open_files_count--;
		inc_rfc1001_len(rsp_org, 60);
	}

	return 0;
}

/**
 * smb2_echo() - handler for smb2 echo(ping) command
 * @smb_work:	smb work containing echo request buffer
 *
 * Return:	0
 */
int smb2_echo(struct smb_work *smb_work)
{
	struct smb2_echo_req *req = (struct smb2_echo_req *)smb_work->buf;
	struct smb2_echo_rsp *rsp = (struct smb2_echo_rsp *)smb_work->rsp_buf;

	if (req->StructureSize != 4) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	rsp->StructureSize = cpu_to_le16(4);
	rsp->Reserved = 0;
	inc_rfc1001_len(rsp, 4);

	return 0;
}

/**
 * smb2_set_access_flags() - set smb access flags based on filp f_mode
 * @filp:	filp containing f_mode
 * @access:	smb access flags
 */
static void smb2_set_access_flags(struct file *filp, __le32 *access)
{
	struct inode *inode;

	inode = filp->f_path.dentry->d_inode;

	*access = 0;
	if (filp->f_mode & FMODE_READ) {
		*access |= FILE_READ_DATA_LE | FILE_READ_EA_LE
		| FILE_READ_ATTRIBUTES_LE;
	}
	if (filp->f_mode & FMODE_WRITE) {
		*access |= FILE_WRITE_DATA_LE | FILE_WRITE_EA_LE
		| FILE_WRITE_ATTRIBUTES_LE;
	} else
		*access &= ~FILE_DELETE_LE;
	if (IS_APPEND(inode))
		*access |= FILE_APPEND_DATA_LE;
	if (filp->f_mode & FMODE_EXEC)
		*access |= FILE_EXECUTE_LE;
}

/**
 * smb2_get_shortname() - get shortname from long filename
 * @server:	TCP server instance of connection
 * @longname:	source long filename
 * @shortname:	destination short filename
 *
 * Return:	0 or shortname length
 */
int smb2_get_shortname(struct tcp_server_info *server, char *longname,
		char *shortname)
{
	char *p, *sp;
	char base[9], extension[4];
	char out[13] = {0};
	int baselen = 0;
	int extlen = 0, len = 0;
	unsigned int csum = 0;
	unsigned char *ptr;
	bool dot_present = true;

	p = longname;
	if ((*p == '.') || (!(strcmp(p, "..")))) {
		/*no mangling required */
		shortname = NULL;
		return 0;
	}
	p = strrchr(longname, '.');
	if (p == longname) { /*name starts with a dot*/
		sp = "___";
		memcpy(extension, sp, 3);
		extension[3] = '\0';
	} else {
		if (p != NULL) {
			p++;
			while (*p && extlen < 3) {
				if (*p != '.')
					extension[extlen++] = toupper(*p);
				p++;
			}
			extension[extlen] = '\0';
		} else
			dot_present = false;
	}

	p = longname;
	if (*p == '.')
		*p++ = 0;
	while (*p && (baselen < 5)) {
		if (*p != '.')
			base[baselen++] = toupper(*p);
		p++;
	}
	base[baselen] = MAGIC_CHAR;

	memcpy(out, base, baselen+1);

	ptr = longname;
	len = strlen(longname);
	for (; len > 0; len--, ptr++)
		csum += *ptr;

	csum = csum % (MANGLE_BASE * MANGLE_BASE);
	out[baselen+1] = mangle(csum/MANGLE_BASE);
	out[baselen+2] = mangle(csum);
	out[baselen+3] = PERIOD;

	if (dot_present)
		memcpy(&out[baselen+4], extension, 4);
	else
		out[baselen+4] = '\0';
	smbConvertToUTF16((__le16 *)shortname, out, PATH_MAX,
			server->local_nls, 0);
	len = strlen(out) * 2;
	return len;
}

/**
 * smb2_get_ea() - handler for smb2 get extended attribute command
 * @smb_work:	smb work containing query info command buffer
 * @path:	path of file/dir to query info command
 * @rq:		get extended attribute request
 * @resp:	response buffer pointer
 * @resp_org:	base response buffer pointer in case of chained response
 *
 * Return:	0 on success, otherwise error
 */
int smb2_get_ea(struct smb_work *smb_work, struct path *path,
		void *rq, void *resp, void *resp_org)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_query_info_req *req;
	struct smb2_query_info_rsp *rsp_org, *rsp;
	struct smb2_ea_info *eainfo, *prev_eainfo;
	char *name, *ptr, *xattr_list = NULL;
	int rc, name_len, value_len, xattr_list_len;
	__u32 buf_free_len, alignment_bytes, rsp_data_cnt = 0;
	struct smb2_ea_info_req *ea_req = NULL;


	req = (struct smb2_query_info_req *)rq;
	rsp = (struct smb2_query_info_rsp *)resp;
	rsp_org = (struct smb2_query_info_rsp *)resp_org;

	/* single EA entry is requested with given user.* name */
	if (req->InputBufferLength)
		ea_req = (struct smb2_ea_info_req *)req->Buffer;
	else {
		/* need to send all EAs, if no specific EA is requested*/
		if (req->Flags & SL_RETURN_SINGLE_ENTRY)
			cifssrv_debug("Ambiguous, all EAs are requested but"
				"need to send single EA entry in rsp"
				"flags 0x%x\n", le32_to_cpu(req->Flags));
	}

	buf_free_len = SMBMaxBufSize + MAX_HEADER_SIZE(server) -
		(get_rfc1002_length(rsp_org) + 4)
		- sizeof(struct smb2_query_info_rsp);

	rc = smb_vfs_listxattr(path->dentry, &xattr_list, XATTR_LIST_MAX);
	if (rc < 0) {
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		goto out;
	} else if (!rc) { /* there is no EA in the file */
		cifssrv_debug("no ea data in the file\n");
		goto done;
	}
	xattr_list_len = rc;

	ptr = (char *)rsp->Buffer;
	eainfo = (struct smb2_ea_info *)ptr;
	prev_eainfo = eainfo;
	for (name = xattr_list; name - xattr_list < xattr_list_len;
			name += strlen(name) + 1) {
		cifssrv_debug("%s, len %zd\n", name, strlen(name));
		/*
		 * CIFS does not support EA other than user.* namespace,
		 * still keep the framework generic, to list other attrs
		 * in future.
		 */
		if (strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			continue;

		if (req->InputBufferLength &&
				(strncmp(&name[XATTR_USER_PREFIX_LEN],
					 ea_req->name, ea_req->EaNameLength)))
			continue;

		name_len = strlen(name);
		if (!strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			name_len -= XATTR_USER_PREFIX_LEN;

		ptr = (char *)(&eainfo->name + name_len + 1);
		buf_free_len -= (offsetof(struct smb2_ea_info, name) +
				name_len + 1);
		/* bailout if xattr can't fit in buf_free_len */
		value_len = smb_vfs_getxattr(path->dentry, name, ptr,
				buf_free_len);
		if (value_len < 0) {
			rc = value_len;
			rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
			goto out;
		}

		ptr += value_len;
		buf_free_len -= value_len;
		eainfo->Flags = 0;
		eainfo->EaNameLength = name_len;
		if (!strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			strncpy(eainfo->name, &name[XATTR_USER_PREFIX_LEN],
					name_len);
		else
			strncpy(eainfo->name, name, name_len);

		eainfo->name[name_len] = '\0';
		eainfo->EaValueLength = cpu_to_le16(value_len);
		rsp_data_cnt += offsetof(struct smb2_ea_info, name) +
			name_len + 1 + value_len;

		/* align next xattr entry at 4 byte bundary */
		alignment_bytes = ((rsp_data_cnt + 3) & ~3) - rsp_data_cnt;
		if (alignment_bytes) {
			memset(ptr, '\0', alignment_bytes);
			ptr += alignment_bytes;
			rsp_data_cnt += alignment_bytes;
			buf_free_len -= alignment_bytes;
		}
		eainfo->NextEntryOffset = cpu_to_le32(rsp_data_cnt);
		prev_eainfo = eainfo;
		eainfo = (struct smb2_ea_info *)ptr;

		if (req->InputBufferLength) {
			cifssrv_debug("single entry requested\n");
			break;
		}
	}

	/* no more ea entries */
	prev_eainfo->NextEntryOffset = 0;
done:
	rc = 0;
	rsp->OutputBufferLength = cpu_to_le32(rsp_data_cnt);
	inc_rfc1001_len(rsp_org, rsp_data_cnt);
out:
	if (xattr_list)
		vfree(xattr_list);
	return rc;
}

/**
 * smb2_info_file_pipe() - handler for smb2 query info on IPC pipe
 * @smb_work:	smb work containing query info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_info_file_pipe(struct smb_work *smb_work)
{
	struct smb2_query_info_req *req;
	struct smb2_query_info_rsp *rsp;
	struct tcp_server_info *server = smb_work->server;
	struct smb2_file_standard_info *sinfo;

	req = (struct smb2_query_info_req *)smb_work->buf;
	rsp = (struct smb2_query_info_rsp *)smb_work->rsp_buf;

	if (req->FileInfoClass != FILE_STANDARD_INFORMATION) {
		cifssrv_debug("smb2_info_file_pipe for %u not supported\n",
			    req->FileInfoClass);
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		return -EOPNOTSUPP;
	}

	cifssrv_debug("smb2 query info IPC pipe\n");
	/* Windows can sometime send query file info request on
	 * pipe without opening it, checking error condition here */
	if (!server->pipe_desc || req->VolatileFileId !=
			server->pipe_desc->id) {
		cifssrv_debug("Pipe not opened or invalid in Pipe id\n");
		if (server->pipe_desc)
			cifssrv_debug("Incoming id = %llu opened pipe id = %d\n",
				req->PersistentFileId, server->pipe_desc->id);
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		return -EINVAL;
	}


	sinfo = (struct smb2_file_standard_info *)rsp->Buffer;

	sinfo->AllocationSize = cpu_to_le64(4096);
	sinfo->EndOfFile = cpu_to_le64(0);
	sinfo->NumberOfLinks = cpu_to_le32(1);
	sinfo->DeletePending = 1;
	sinfo->Directory = 0;
	rsp->OutputBufferLength =
		cpu_to_le32(sizeof(struct smb2_file_standard_info));
	inc_rfc1001_len(rsp,
			sizeof(struct smb2_file_standard_info));

	return 0;
}

/**
 * buffer_check_err() - helper function to check buffer errors
 * @reqOutputBufferLength:	max buffer length expected in command response
 * @rsp:		query info response buffer contains output buffer length
 * @infoclass_size:	query info class response buffer size
 *
 * Return:	0 on success, otherwise error
 */
int buffer_check_err(int reqOutputBufferLength, struct smb2_query_info_rsp *rsp,
							int infoclass_size)
{
	if (reqOutputBufferLength < rsp->OutputBufferLength) {
		if (reqOutputBufferLength < infoclass_size) {
			cifssrv_err("Invalid Buffer Size Requested\n");
			rsp->hdr.Status = NT_STATUS_INFO_LENGTH_MISMATCH;
			rsp->hdr.smb2_buf_length = cpu_to_be32(
						sizeof(struct smb2_hdr) - 4);
			return -EINVAL;
		} else{
			cifssrv_err("Buffer Overflow\n");
			rsp->hdr.Status = NT_STATUS_BUFFER_OVERFLOW;
			rsp->hdr.smb2_buf_length = cpu_to_be32(
						sizeof(struct smb2_hdr) - 4
						+ reqOutputBufferLength);
			rsp->OutputBufferLength = cpu_to_le32(
							reqOutputBufferLength);
			return 0;
		}
	}
	return 0;
}

/**
 * smb2_info_file() - handler for smb2 query info command
 * @smb_work:	smb work containing query info request buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_info_file(struct smb_work *smb_work)
{
	struct smb2_query_info_req *req;
	struct smb2_query_info_rsp *rsp, *rsp_org;
	struct cifssrv_file *fp;
	struct tcp_server_info *server = smb_work->server;
	int fileinfoclass = 0;
	struct file *filp;
	struct kstat stat;
	uint64_t id = -1;
	int rc = 0;
	int file_infoclass_size;
	__u64 create_time;

	req = (struct smb2_query_info_req *)smb_work->buf;
	rsp = (struct smb2_query_info_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_query_info_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_query_info_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);

		if (le64_to_cpu(req->VolatileFileId) == -1) {
			cifssrv_debug("Compound request assigning stored FID = %llu\n",
				    smb_work->cur_local_fid);
			id = smb_work->cur_local_fid;
		}
	}

	if (id == -1)
		id = le64_to_cpu(req->VolatileFileId);

	if (rsp->hdr.TreeId == 1) {
		/* smb2 info file called for pipe */
		return smb2_info_file_pipe(smb_work);
	} else {
		fp = get_id_from_fidtable(server, id);
		if (!fp) {
			cifssrv_debug("Invalid id for file info : %llu\n", id);
			return -EINVAL;
		}


		filp = fp->filp;
		generic_fillattr(filp->f_path.dentry->d_inode, &stat);
	}
	fileinfoclass = req->FileInfoClass;

	switch (fileinfoclass) {
	case FILE_ACCESS_INFORMATION:
	{
		struct smb2_file_access_info *file_info;

		file_info = (struct smb2_file_access_info *)rsp->Buffer;

		smb2_set_access_flags(filp, &(file_info->AccessFlags));
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_access_info));
		inc_rfc1001_len(rsp_org, sizeof(struct smb2_file_access_info));
		file_infoclass_size = FILE_ACCESS_INFORMATION_SIZE;
		break;
	}
	case FILE_BASIC_INFORMATION:
	{
		struct smb2_file_all_info *basic_info;
		basic_info = (struct smb2_file_all_info *)rsp->Buffer;
		create_time = min3(cifs_UnixTimeToNT(stat.ctime),
				cifs_UnixTimeToNT(stat.mtime),
				cifs_UnixTimeToNT(stat.atime));
		if (!create_time)
			create_time = min(cifs_UnixTimeToNT(stat.ctime),
					cifs_UnixTimeToNT(stat.mtime));

		basic_info->CreationTime = cpu_to_le64(create_time);
		basic_info->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.atime));
		basic_info->LastWriteTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
		basic_info->ChangeTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
		basic_info->Attributes = S_ISDIR(stat.mode) ?
					ATTR_DIRECTORY : ATTR_NORMAL;
		basic_info->Pad1 = 0;
		rsp->OutputBufferLength =
			cpu_to_le32(offsetof(struct smb2_file_all_info,
						AllocationSize));
		inc_rfc1001_len(rsp_org, offsetof(struct smb2_file_all_info,
					AllocationSize));
		file_infoclass_size = FILE_BASIC_INFORMATION_SIZE;
		break;
	}
	case FILE_STANDARD_INFORMATION:
	{
		struct smb2_file_standard_info *sinfo;

		sinfo = (struct smb2_file_standard_info *)rsp->Buffer;

		sinfo->AllocationSize = cpu_to_le64(stat.blocks << 9);
		sinfo->EndOfFile = cpu_to_le64(stat.size);
		sinfo->NumberOfLinks = cpu_to_le32(stat.nlink);
		sinfo->DeletePending = 0;
		sinfo->Directory = S_ISDIR(stat.mode) ? 1 : 0;
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_standard_info));
		inc_rfc1001_len(rsp_org,
				sizeof(struct smb2_file_standard_info));
		file_infoclass_size = FILE_STANDARD_INFORMATION_SIZE;
		break;
	}
	case FILE_ALIGNMENT_INFORMATION:
	{
		struct smb2_file_alignment_info *file_info;

		file_info = (struct smb2_file_alignment_info *)rsp->Buffer;
		file_info->AlignmentRequirement = 0;
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_alignment_info));
		inc_rfc1001_len(rsp_org,
				sizeof(struct smb2_file_alignment_info));
		file_infoclass_size = FILE_ALIGNMENT_INFORMATION_SIZE;
		break;
	}
	case FILE_ALL_INFORMATION:
	{
		struct smb2_file_all_info *file_info;
		char *filename;
		int uni_filename_len;

		filename = (char *)filp->f_path.dentry->d_name.name;
		cifssrv_debug("filename = %s\n", filename);
		file_info = (struct smb2_file_all_info *)rsp->Buffer;

		create_time = min3(cifs_UnixTimeToNT(stat.ctime),
				cifs_UnixTimeToNT(stat.mtime),
				cifs_UnixTimeToNT(stat.atime));

		if (!create_time)
			create_time = min(cifs_UnixTimeToNT(stat.ctime),
					cifs_UnixTimeToNT(stat.mtime));

		file_info->CreationTime = cpu_to_le64(create_time);
		file_info->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.atime));
		file_info->LastWriteTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
		file_info->ChangeTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
		file_info->Attributes = S_ISDIR(stat.mode) ?
					ATTR_DIRECTORY : ATTR_NORMAL;
		file_info->Pad1 = 0;
		file_info->AllocationSize =
				cpu_to_le64(stat.blocks << 9);
		file_info->EndOfFile = cpu_to_le64(stat.size);
		file_info->NumberOfLinks = cpu_to_le32(stat.nlink);
		file_info->DeletePending = 0;
		file_info->Directory = S_ISDIR(stat.mode) ? 1 : 0;
		file_info->Pad2 = 0;
		file_info->IndexNumber = cpu_to_le64(stat.ino);
		file_info->EASize = 0;
		file_info->AccessFlags = cpu_to_le32(0x00000080);
		file_info->CurrentByteOffset = 0;
		file_info->Mode = cpu_to_le32(0x00000010);
		file_info->AlignmentRequirement = 0;
		uni_filename_len = smbConvertToUTF16(
				(__le16 *)file_info->FileName,
				filename, PATH_MAX,
				server->local_nls, 0);

		uni_filename_len *= 2;
		file_info->FileNameLength = cpu_to_le32(uni_filename_len);

		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_all_info) +
				    uni_filename_len - 1);
		inc_rfc1001_len(rsp_org, le32_to_cpu(rsp->OutputBufferLength));
		file_infoclass_size = FILE_ALL_INFORMATION_SIZE;
		break;
	}
	case FILE_ALTERNATE_NAME_INFORMATION:
	{
		struct smb2_file_alt_name_info *file_info;
		char *filename;
		int uni_filename_len;

		filename = (char *)filp->f_path.dentry->d_name.name;
		cifssrv_err("filename = %s\n", filename);

		file_info = (struct smb2_file_alt_name_info *)rsp->Buffer;
		uni_filename_len = smb2_get_shortname(server, filename,
				file_info->FileName);
		uni_filename_len *= 2;
		file_info->FileNameLength = cpu_to_le32(uni_filename_len);

		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_alt_name_info) +
				    uni_filename_len);
		inc_rfc1001_len(rsp_org, le32_to_cpu(rsp->OutputBufferLength));
		file_infoclass_size = FILE_ALTERNATE_NAME_INFORMATION_SIZE;
		break;
	}
	case FILE_STREAM_INFORMATION:
	{
		struct smb2_file_stream_info *file_info;
		struct inode *inode;
		struct kstat stat;
		char *Streamname, streamlen;

		file_info = (struct smb2_file_stream_info *)rsp->Buffer;

		inode = filp->f_path.dentry->d_inode;

		generic_fillattr(inode, &stat);

		Streamname = "::$DATA";
		file_info->NextEntryOffset = 0;
		streamlen  = smbConvertToUTF16(
				(__le16 *)file_info->StreamName,
				Streamname, PATH_MAX,
				server->local_nls, 0);
		streamlen *= 2;
		file_info->StreamNameLength = cpu_to_le32(streamlen);
		file_info->StreamSize = cpu_to_le64(stat.size);
		file_info->StreamAllocationSize = cpu_to_le64(stat.blocks << 9);
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_stream_info) +
					   streamlen);
		inc_rfc1001_len(rsp_org, cpu_to_le32(rsp->OutputBufferLength));
		file_infoclass_size = FILE_STREAM_INFORMATION_SIZE;
		break;
	}
	case FILE_INTERNAL_INFORMATION:
	{
		struct smb2_file_internal_info *file_info;

		file_info = (struct smb2_file_internal_info *)rsp->Buffer;

		file_info->IndexNumber = cpu_to_le64(stat.ino);
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_internal_info));
		inc_rfc1001_len(rsp_org,
				sizeof(struct smb2_file_internal_info));
		file_infoclass_size = FILE_INTERNAL_INFORMATION_SIZE;
		break;
	}
	case FILE_NETWORK_OPEN_INFORMATION:
	{
		struct smb2_file_ntwrk_info *file_info;
		file_info = (struct smb2_file_ntwrk_info *)rsp->Buffer;
		create_time = min3(cifs_UnixTimeToNT(stat.ctime),
				cifs_UnixTimeToNT(stat.mtime),
				cifs_UnixTimeToNT(stat.atime));

		if (!create_time)
			create_time = min(cifs_UnixTimeToNT(stat.ctime),
					cifs_UnixTimeToNT(stat.mtime));

		file_info->CreationTime = cpu_to_le64(create_time);
		file_info->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.atime));
		file_info->LastWriteTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
		file_info->ChangeTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
		file_info->Attributes = S_ISDIR(stat.mode) ?
					ATTR_DIRECTORY : ATTR_NORMAL;
		file_info->AllocationSize =
				cpu_to_le64(stat.blocks << 9);
		file_info->EndOfFile = cpu_to_le64(stat.size);
		file_info->Reserved = cpu_to_le32(0);
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_ntwrk_info));
		inc_rfc1001_len(rsp_org, sizeof(struct smb2_file_ntwrk_info));
		file_infoclass_size = FILE_NETWORK_OPEN_INFORMATION_SIZE;
		break;
	}
	case FILE_EA_INFORMATION:
	{
		struct smb2_file_ea_info *file_info;
		file_info = (struct smb2_file_ea_info *)rsp->Buffer;

		file_info->EASize = 0;
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_ea_info));
		inc_rfc1001_len(rsp_org, sizeof(struct smb2_file_ea_info));
		file_infoclass_size = FILE_EA_INFORMATION_SIZE;
		break;
	}
	case FILE_FULL_EA_INFORMATION:
		rc = smb2_get_ea(smb_work, &filp->f_path, req, rsp, rsp_org);
		file_infoclass_size = FILE_FULL_EA_INFORMATION_SIZE;
		if (rc < 0)
			return rc;
		break;
	case FILE_ALLOCATION_INFORMATION:
	{
		struct smb2_file_alloc_info *file_info;
		file_info = (struct smb2_file_alloc_info *)rsp->Buffer;

		file_info->Attributes = S_ISDIR(stat.mode) ?
			ATTR_DIRECTORY : ATTR_NORMAL;
		file_info->ReparseTag = 0;
		rsp->OutputBufferLength =
			cpu_to_le32(sizeof(struct smb2_file_alloc_info));
		inc_rfc1001_len(rsp_org, sizeof(struct smb2_file_alloc_info));
		file_infoclass_size = FILE_ALLOCATION_INFORMATION_SIZE;
		break;
	}
	default:
		cifssrv_debug("fileinfoclass %d not supported yet\n",
			fileinfoclass);
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		return -EOPNOTSUPP;
	}
	rc = buffer_check_err(req->OutputBufferLength, rsp,
					file_infoclass_size);
	return rc;
}

/**
 * fsTypeSearch() - get fs type string from fs magic number
 * @fs_type:		array of fs types
 * @magic_number:	match the magic number for fs type
 * @SIZE:		size of fs type table
 *
 * Return:	index of fs type
 */
inline int fsTypeSearch(struct fs_type_info fs_type[],
					int magic_number, int SIZE)
{
	int i;
	int dfault = 40;	/* setting MSDOS as default files system*/
	for (i = 0; i < SIZE; i++) {
		if (fs_type[i].magic_number == magic_number)
			return i;
	}
	return dfault;
}

/**
 * smb2_info_filesystem() - handler for smb2 query info command
 * @smb_work:	smb work containing query info request buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_info_filesystem(struct smb_work *smb_work)
{
	struct smb2_query_info_req *req;
	struct smb2_query_info_rsp *rsp, *rsp_org;
	struct tcp_server_info *server = smb_work->server;
	int fsinfoclass = 0;
	struct kstatfs stfs;
	struct cifssrv_share *share;
	struct path path;
	int rc = 0, len;
	int fs_infoclass_size = 0;
	int fs_type_idx;
	req = (struct smb2_query_info_req *)smb_work->buf;
	rsp = (struct smb2_query_info_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_query_info_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_query_info_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);
	}

	share = find_matching_share(req->hdr.TreeId);
	if (!share)
		return -ENOENT;

	rc = smb_kern_path(share->path, LOOKUP_FOLLOW, &path, 0);

	if (rc) {
		cifssrv_err("cannot create vfs path\n");
		rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;
		return rc;
	}

	rc = vfs_statfs(&path, &stfs);
	if (rc) {
		cifssrv_err("cannot do stat of path %s\n", share->path);
		rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;
		path_put(&path);
		return rc;
	}


	fsinfoclass = req->FileInfoClass;

	switch (fsinfoclass) {
	case FS_DEVICE_INFORMATION:
		{
			FILE_SYSTEM_DEVICE_INFO *fs_info;

			fs_info = (FILE_SYSTEM_DEVICE_INFO *)rsp->Buffer;

			fs_info->DeviceType = cpu_to_le32(stfs.f_type);
			fs_info->DeviceCharacteristics = (0x00000020);
			rsp->OutputBufferLength = cpu_to_le32(8);
			inc_rfc1001_len(rsp_org, 8);
			fs_infoclass_size = FS_DEVICE_INFORMATION_SIZE;
			break;
		}
	case FS_ATTRIBUTE_INFORMATION:
		{
			FILE_SYSTEM_ATTRIBUTE_INFO *fs_info;

			fs_info = (FILE_SYSTEM_ATTRIBUTE_INFO *)rsp->Buffer;
			fs_info->Attributes = cpu_to_le32(0x0001002f);
			fs_info->MaxPathNameComponentLength =
				cpu_to_le32(stfs.f_namelen);
			fs_type_idx = fsTypeSearch(fs_type, stfs.f_type,
							FS_TYPE_SUPPORT_SIZE);
			len = smbConvertToUTF16((__le16 *)
							fs_info->FileSystemName,
					fs_type[fs_type_idx].fs_name, PATH_MAX,
							server->local_nls, 0);
			len = len * 2;
			fs_info->FileSystemNameLen = len;
			rsp->OutputBufferLength = cpu_to_le32(sizeof
					(FILE_SYSTEM_ATTRIBUTE_INFO) -2 + len);
			inc_rfc1001_len(rsp_org,
				sizeof(FILE_SYSTEM_ATTRIBUTE_INFO) - 2 + len);
			fs_infoclass_size = FS_ATTRIBUTE_INFORMATION_SIZE;
			break;
		}
	case FS_VOLUME_INFORMATION:
		{
			FILE_SYSTEM_VOL_INFO *fsvinfo;
			fsvinfo = (FILE_SYSTEM_VOL_INFO *)(rsp->Buffer);
			fsvinfo->VolumeCreationTime = 0;
			/* Taking dummy value of serial number*/
			fsvinfo->SerialNumber = cpu_to_le32(0xbc3ac512);
			len = smbConvertToUTF16((__le16 *)fsvinfo->VolumeLabel,
				share->sharename, PATH_MAX,
					server->local_nls, 0);
			len = len * 2;
			fsvinfo->VolumeLabelSize = cpu_to_le32(len);
			fsvinfo->Reserved = 0;
			rsp->OutputBufferLength =
				cpu_to_le32(sizeof(FILE_SYSTEM_VOL_INFO)
								- 2 + len);
			inc_rfc1001_len(rsp_org, sizeof(FILE_SYSTEM_VOL_INFO)
								+ len - 2);
			fs_infoclass_size = FS_VOLUME_INFORMATION_SIZE;
			break;
		}
	case FS_SIZE_INFORMATION:
		{
			FILE_SYSTEM_INFO *fs_size_info;
			fs_size_info = (FILE_SYSTEM_INFO *)(rsp->Buffer);

			fs_size_info->TotalAllocationUnits =
						cpu_to_le64(stfs.f_blocks);
			fs_size_info->FreeAllocationUnits =
						cpu_to_le64(stfs.f_bfree);
			fs_size_info->SectorsPerAllocationUnit =
						cpu_to_le32(stfs.f_bsize >> 9);
			fs_size_info->BytesPerSector = cpu_to_le32(512);
			rsp->OutputBufferLength = cpu_to_le32(24);
			inc_rfc1001_len(rsp_org, 24);
			fs_infoclass_size = FS_SIZE_INFORMATION_SIZE;
			break;
		}
	case FS_FULL_SIZE_INFORMATION:
		{
			struct smb2_fs_full_size_info *fs_fullsize_info;
			fs_fullsize_info =
				(struct smb2_fs_full_size_info *)(rsp->Buffer);

			fs_fullsize_info->TotalAllocationUnits =
						cpu_to_le64(stfs.f_blocks);
			fs_fullsize_info->CallerAvailableAllocationUnits =
						cpu_to_le64(stfs.f_bavail);
			fs_fullsize_info->ActualAvailableAllocationUnits =
						cpu_to_le64(stfs.f_bfree);
			fs_fullsize_info->SectorsPerAllocationUnit =
						cpu_to_le32(stfs.f_bsize >> 9);
			fs_fullsize_info->BytesPerSector = cpu_to_le32(512);
			rsp->OutputBufferLength = cpu_to_le32(32);
			inc_rfc1001_len(rsp_org, 32);
			fs_infoclass_size = FS_FULL_SIZE_INFORMATION_SIZE;
			break;
		}

	default:
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		path_put(&path);
		return -1;
	}
	rc = buffer_check_err(req->OutputBufferLength, rsp,
					fs_infoclass_size);
	path_put(&path);
	return rc;

}

/**
 * smb2_set_info() - handler for smb2 set info command handler
 * @smb_work:	smb work containing set info request buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_set_info(struct smb_work *smb_work)
{
	struct smb2_set_info_req *req;
	struct smb2_set_info_rsp *rsp;
	int rc = 0;

	req = (struct smb2_set_info_req *)smb_work->buf;
	rsp = (struct smb2_set_info_rsp *)smb_work->rsp_buf;

	if (le16_to_cpu(req->StructureSize) != 33) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		return 0;
	}
	cifssrv_debug("%s: Recieved set info request\n", __func__);
	rsp->StructureSize = cpu_to_le16(33);

	switch (req->InfoType) {
	case SMB2_O_INFO_FILE:
		cifssrv_debug("GOT SMB2_O_INFO_FILE\n");
		rc = smb2_set_info_file(smb_work);
		break;
	default:
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
	}
	return rc;
}

/**
 * smb2_set_ea() - handler for setting extended attributes using set
 *		info command
 * @smb_work:	smb work containing set info command buffer
 * @path:	dentry path for get ea
 *
 * Return:	0 on success, otherwise error
 */
int smb2_set_ea(struct smb_work *smb_work, struct path *path)
{
	struct smb2_set_info_req *req;
	struct smb2_set_info_rsp *rsp;
	struct smb2_ea_info *eabuf;
	char *attr_name = NULL, *value;
	int rc = 0;

	req = (struct smb2_set_info_req *)smb_work->buf;
	rsp = (struct smb2_set_info_rsp *)smb_work->rsp_buf;
	eabuf = (struct smb2_ea_info *)(req->Buffer);
	if (!path) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		return -EINVAL;
	}

	cifssrv_debug("name: <%s>, name_len %u, value_len %u\n",
			eabuf->name, eabuf->EaNameLength,
			le16_to_cpu(eabuf->EaValueLength));

	if (strlen(eabuf->name) >
			(XATTR_NAME_MAX - XATTR_USER_PREFIX_LEN)) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		return -ERANGE;
	}

	attr_name = kmalloc(XATTR_NAME_MAX + 1, GFP_KERNEL);
	if (!attr_name) {
		rc = -ENOMEM;
		goto out;
	}

	memcpy(attr_name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN);
	memcpy(&attr_name[XATTR_USER_PREFIX_LEN], eabuf->name,
			eabuf->EaNameLength);
	attr_name[XATTR_USER_PREFIX_LEN + eabuf->EaNameLength] = '\0';
	value = (char *)&eabuf->name + eabuf->EaNameLength + 1;

	rc = smb_vfs_setxattr(NULL, path, attr_name, value,
			le16_to_cpu(eabuf->EaValueLength), 0);
	if (rc < 0)
		rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;

out:
	kfree(attr_name);
	return rc;
}

/**
 * smb2_create_link() - handler for creating hardlink using smb2
 *		set info command
 * @smb_work:	smb work containing set info command buffer
 * @filp:	file pointer of source file
 *
 * Return:	0 on success, otherwise error
 */
int smb2_create_link(struct smb_work *smb_work, struct file *filp)
{
	struct smb2_set_info_req *req = NULL;
	struct smb2_set_info_rsp *rsp = NULL;
	struct smb2_file_link_info *file_info = NULL;
	char *link_name = NULL, *target_name = NULL, *pathname = NULL;
	struct path path;
	bool file_present = true;
	int rc;

	req = (struct smb2_set_info_req *)smb_work->buf;
	rsp = (struct smb2_set_info_rsp *)smb_work->rsp_buf;
	file_info = (struct smb2_file_link_info *)req->Buffer;

	cifssrv_debug("setting FILE_LINK_INFORMATION\n");
	pathname = kmalloc(PATH_MAX, GFP_NOFS);
	if (!pathname) {
		rsp->hdr.Status = NT_STATUS_NO_MEMORY;
		return -ENOMEM;
	}

	link_name = smb2_get_name(file_info->FileName,
			le32_to_cpu(file_info->FileNameLength),
			smb_work);
	if (IS_ERR(link_name) || S_ISDIR(file_inode(filp)->i_mode)) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		rc = -EINVAL;
		goto out;
	}

	cifssrv_debug("link name is %s\n", link_name);
	target_name = d_path(&filp->f_path, pathname, PATH_MAX);
	if (IS_ERR(target_name)) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		rc = PTR_ERR(target_name);
		goto out;
	}

	cifssrv_debug("target name is %s\n", target_name);
	rc = smb_kern_path(link_name, 0, &path, 0);
	if (rc)
		file_present = false;
	else
		path_put(&path);

	if (file_info->ReplaceIfExists) {
		if (file_present) {
			rc = smb_vfs_unlink(link_name);
			if (rc) {
				rsp->hdr.Status =
					NT_STATUS_INVALID_PARAMETER;
				cifssrv_debug("cannot delete %s\n",
						link_name);
				goto out;
			}
		}
	} else {
		if (file_present) {
			rsp->hdr.Status =
				NT_STATUS_OBJECT_NAME_COLLISION;
			cifssrv_debug("link already exists\n");
			goto out;
		}
	}

	rc = smb_vfs_link(target_name, link_name);
	if (rc)
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;

out:
	if (!IS_ERR(link_name))
		smb_put_name(link_name);
	kfree(pathname);
	return rc;
}

/**
 * smb2_rename() - handler for rename using smb2 setinfo command
 * @smb_work:	smb work containing set info command buffer
 * @filp:	file pointer of source file
 * @old_fid:	file id of source file
 *
 * Return:	0 on success, otherwise error
 */
int smb2_rename(struct smb_work *smb_work, struct file *filp, int old_fid)
{
	struct smb2_set_info_req *req = NULL;
	struct smb2_set_info_rsp *rsp = NULL;
	struct smb2_file_rename_info *file_info = NULL;
	struct tcp_server_info *server = smb_work->server;
	char *new_name = NULL, *abs_oldname = NULL, *old_name = NULL;
	char *tmp_name = NULL, *pathname = NULL;
	struct path path;
	bool file_present = true;
	int rc;

	req = (struct smb2_set_info_req *)smb_work->buf;
	rsp = (struct smb2_set_info_rsp *)smb_work->rsp_buf;
	file_info = (struct smb2_file_rename_info *)req->Buffer;

	cifssrv_debug("setting FILE_RENAME_INFO\n");
	pathname = kmalloc(PATH_MAX, GFP_NOFS);
	if (!pathname) {
		rsp->hdr.Status = NT_STATUS_NO_MEMORY;
		return -ENOMEM;
	}

	abs_oldname = d_path(&filp->f_path, pathname, PATH_MAX);
	if (IS_ERR(abs_oldname)) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		rc = PTR_ERR(old_name);
		goto out;
	}
	old_name = strrchr(abs_oldname, '/');
	if (old_name && old_name[1] != '\0')
		old_name++;
	else {
		cifssrv_debug("can't get last component in path %s\n",
				abs_oldname);
		rc = -ENOENT;
		goto out;
	}

	new_name = smb2_get_name(file_info->FileName,
			le32_to_cpu(file_info->FileNameLength),
			smb_work);
	if (IS_ERR(new_name)) {
		rc = PTR_ERR(new_name);
		goto out;
	}

	tmp_name = kmalloc(PATH_MAX, GFP_NOFS);
	if (!tmp_name) {
		rsp->hdr.Status = NT_STATUS_NO_MEMORY;
		rc = -ENOMEM;
		goto out;
	}
	strncpy(tmp_name, new_name, strlen(new_name) + 1);
	cifssrv_debug("new name %s\n", new_name);
	rc = smb_kern_path(tmp_name, 0, &path, 1);
	if (rc)
		file_present = false;
	else
		path_put(&path);

	if (file_info->ReplaceIfExists) {
		if (file_present) {
			if (S_ISDIR(path.dentry->d_inode->i_mode))
				rc = smb_vfs_rmdir(new_name);
			else
				rc = smb_vfs_unlink(new_name);
			if (rc) {
				if (rc == -ENOTEMPTY)
					rsp->hdr.Status =
						NT_STATUS_DIRECTORY_NOT_EMPTY;
				else
					rsp->hdr.Status =
						NT_STATUS_INVALID_PARAMETER;
				cifssrv_debug("cannot delete %s, rc %d\n",
						new_name, rc);
				goto out;
			}
		}
	} else {
		if (file_present &&
				strncmp(old_name, path.dentry->d_name.name,
					strlen(old_name))) {
			rc = -EEXIST;
			rsp->hdr.Status =
				NT_STATUS_OBJECT_NAME_COLLISION;
			cifssrv_debug("cannot rename already existing file\n");
			goto out;
		}
	}

	rc = smb_vfs_rename(server, NULL, new_name, old_fid);
	if (rc)
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;

out:
	kfree(pathname);
	kfree(tmp_name);
	if (!IS_ERR(new_name))
		smb_put_name(new_name);
	return rc;
}

/**
 * smb2_set_info_file() - handler for smb2 set info command
 * @smb_work:	smb work containing set info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_set_info_file(struct smb_work *smb_work)
{
	struct smb2_set_info_req *req;
	struct smb2_set_info_rsp *rsp;
	struct cifssrv_file *fp;
	struct tcp_server_info *server = smb_work->server;
	uint64_t id;
	int rc = 0;
	struct file *filp;
	struct inode *inode;

	req = (struct smb2_set_info_req *)smb_work->buf;
	rsp = (struct smb2_set_info_rsp *)smb_work->rsp_buf;

	id = le64_to_cpu(req->VolatileFileId);
	fp = get_id_from_fidtable(server, id);
	if (!fp) {
		cifssrv_debug("Invalid id for close: %llu\n", id);
		return -EINVAL;
	}
	filp = fp->filp;
	inode = filp->f_path.dentry->d_inode;

	switch (req->FileInfoClass) {
	case FILE_BASIC_INFORMATION:
	{
		struct smb2_file_all_info *file_info;
		struct iattr attrs;

		file_info = (struct smb2_file_all_info *)req->Buffer;
		attrs.ia_valid = 0;
		if (le64_to_cpu(file_info->LastAccessTime)) {
			attrs.ia_atime = cifs_NTtimeToUnix(
					le64_to_cpu(file_info->LastAccessTime));
			attrs.ia_valid |= (ATTR_ATIME | ATTR_ATIME_SET);
		}

		if (le64_to_cpu(file_info->ChangeTime)) {
			attrs.ia_ctime = cifs_NTtimeToUnix(
					le64_to_cpu(file_info->ChangeTime));
			attrs.ia_valid |= ATTR_CTIME;
		}

		if (le64_to_cpu(file_info->LastWriteTime)) {
			attrs.ia_mtime = cifs_NTtimeToUnix(
					le64_to_cpu(file_info->LastWriteTime));
			attrs.ia_valid |= (ATTR_MTIME | ATTR_MTIME_SET);
		}

		if (attrs.ia_valid) {
			rc = smb_vfs_setattr(server, NULL, id, &attrs);
			if (rc) {
				cifssrv_debug("failed to set time\n");
				rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
				smb2_set_err_rsp(smb_work);
				return rc;
			}
		}
		break;
	}
	case FILE_ALLOCATION_INFORMATION:
		/* fall through */
	case FILE_END_OF_FILE_INFORMATION:
	{
		struct smb2_file_eof_info *file_eof_info;
		loff_t newsize;

		file_eof_info = (struct smb2_file_eof_info *)req->Buffer;

		newsize = le64_to_cpu(file_eof_info->EndOfFile);

		if (newsize != i_size_read(inode)) {
			rc = smb_vfs_truncate(server, NULL, id, newsize);
			if (rc) {
				cifssrv_err("truncate failed! fid %llu err %d\n",
						id, rc);
				if (rc == -EAGAIN)
					rsp->hdr.Status =
						NT_STATUS_FILE_LOCK_CONFLICT;
				else
					rsp->hdr.Status =
						NT_STATUS_INVALID_HANDLE;
				smb2_set_err_rsp(smb_work);
				return rc;
			}
			cifssrv_debug("fid %llu truncated to newsize %lld\n",
					id, newsize);
		}
		break;
	}

	case FILE_RENAME_INFORMATION:
	rc = smb2_rename(smb_work, filp, id);
	break;
	case FILE_LINK_INFORMATION:
	rc = smb2_create_link(smb_work, fp->filp);
	break;
	case FILE_DISPOSITION_INFORMATION:
	{
		struct smb2_file_disposition_info *file_info;

		file_info = (struct smb2_file_disposition_info *)req->Buffer;
		if (file_info->DeletePending) {
			if (S_ISDIR(fp->filp->f_path.dentry->
				d_inode->i_mode) && !is_dir_empty(fp)) {
				rsp->hdr.Status = NT_STATUS_DIRECTORY_NOT_EMPTY;
				rc = -1;
			} else
				fp->delete_on_close = true;
		}
		break;
	}
	case FILE_FULL_EA_INFORMATION:
	{
		rc = smb2_set_ea(smb_work, &filp->f_path);
		break;
	}
	default:
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		cifssrv_err("Unimplemented Fileinfoclass :%d\n",
			    req->FileInfoClass);
		rc = -1;
	}

	if (rc) {
		smb2_set_err_rsp(smb_work);
		return rc;
	}

	rsp->StructureSize = cpu_to_le16(2);
	inc_rfc1001_len(rsp, 2);
	return 0;
}

/**
 * smb2_read_pipe() - handler for smb2 read from IPC pipe
 * @smb_work:	smb work containing read IPC pipe command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_read_pipe(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	int ret = 0, nbytes = 0;
	char *data_buf;
	int id;
	struct smb2_read_req *req;
	struct smb2_read_rsp *rsp;
	size_t length;
	req = (struct smb2_read_req *)smb_work->buf;
	rsp = (struct smb2_read_rsp *)smb_work->rsp_buf;

	id = le64_to_cpu(req->VolatileFileId);

	length = le32_to_cpu(req->Length);
	if (!server->pipe_desc || id != server->pipe_desc->id) {
		cifssrv_debug("Pipe not opened or invalid in Pipe id\n");
		if (server->pipe_desc)
			cifssrv_debug("Incoming id = %d opened pipe id = %d\n",
					id, server->pipe_desc->id);
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
		return ret;
	}

	data_buf = (char *)(rsp->Buffer);

	nbytes = process_rpc_rsp(smb_work->server, data_buf, length);

	if (nbytes <= 0) {
		cifssrv_err("Pipe data not present\n");
		rsp->hdr.Status = NT_STATUS_UNEXPECTED_IO_ERROR;
		smb2_set_err_rsp(smb_work);
		return -EINVAL;
	}

	rsp->StructureSize = cpu_to_le16(17);
	rsp->DataOffset = 80;
	rsp->Reserved = 0;
	rsp->DataLength = cpu_to_le32(nbytes);
	rsp->DataRemaining = 0;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(rsp, 16 + nbytes);
	return 0;

}

/**
 * smb2_read() - handler for smb2 read from file
 * @smb_work:	smb work containing read command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_read(struct smb_work *smb_work)
{
	struct smb2_read_req *req;
	struct smb2_read_rsp *rsp, *rsp_org;
	struct tcp_server_info *server = smb_work->server;
	loff_t offset;
	size_t length, mincount;
	ssize_t nbytes = 0;
	uint64_t id = -1;
	int err = 0;

	req = (struct smb2_read_req *)smb_work->buf;
	rsp = (struct smb2_read_rsp *)smb_work->rsp_buf;

	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_read_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_read_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);
		if (le64_to_cpu(req->VolatileFileId) == -1) {
			cifssrv_debug("Compound request assigning stored FID = %llu\n",
				    smb_work->cur_local_fid);
			id = smb_work->cur_local_fid;
		}
	}

	if (req->StructureSize != 49) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (rsp->hdr.TreeId == 1) {
		cifssrv_debug("IPC pipe read request\n");
		return smb2_read_pipe(smb_work);
	}

	if (id == -1)
		id = le64_to_cpu(req->VolatileFileId);

	offset = le32_to_cpu(req->Offset);
	length = le32_to_cpu(req->Length);
	mincount = le32_to_cpu(req->MinimumCount);

	if (length > CIFS_DEFAULT_IOSIZE) {
		cifssrv_debug("read size(%zu) exceeds max size(%u)\n",
				length, CIFS_DEFAULT_IOSIZE);
		cifssrv_debug("limiting read size to max size(%u)\n",
				CIFS_DEFAULT_IOSIZE);
		length = CIFS_DEFAULT_IOSIZE;
	}

	cifssrv_debug("fid %llu, offset %lld, len %zu\n", id, offset, length);
	nbytes = smb_vfs_read(server, id, &smb_work->rdata_buf, length,
			&offset);
	if (nbytes < 0) {
		err = nbytes;
		goto out;
	}
	if ((nbytes == 0 && length != 0) || nbytes < mincount) {
		vfree(smb_work->rdata_buf);
		smb_work->rdata_buf = NULL;
		rsp->hdr.Status = NT_STATUS_END_OF_FILE;
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	cifssrv_debug("nbytes %zu, offset %lld mincount %zu\n",
						nbytes, offset, mincount);

	rsp->StructureSize = cpu_to_le16(17);
	rsp->DataOffset = 80;
	rsp->Reserved = 0;
	rsp->DataLength = cpu_to_le32(nbytes);
	rsp->DataRemaining = 0;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(rsp_org, 16);
	smb_work->rrsp_hdr_size = get_rfc1002_length(rsp_org) + 4;
	smb_work->rdata_cnt = nbytes;
	inc_rfc1001_len(rsp_org, nbytes);
	return 0;

out:

	if (err) {
		if (err == -EISDIR)
			rsp->hdr.Status = NT_STATUS_INVALID_DEVICE_REQUEST;
		else if (err == -EAGAIN)
			rsp->hdr.Status = NT_STATUS_FILE_LOCK_CONFLICT;
		else
			rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;

		smb2_set_err_rsp(smb_work);
	}
	return err;
}

/**
 * smb2_write_pipe() - handler for smb2 write on IPC pipe
 * @smb_work:	smb work containing write IPC pipe command buffer
 *
 * Return:	0 on success, otherwise error
 */
static int smb2_write_pipe(struct smb_work *smb_work)
{
	struct smb2_write_req *req;
	struct smb2_write_rsp *rsp;
	struct tcp_server_info *server = smb_work->server;
	int id = 0, err = 0, ret = 0;
	char *data_buf;
	size_t length;
	req = (struct smb2_write_req *)smb_work->buf;
	rsp = (struct smb2_write_rsp *)smb_work->rsp_buf;

	length = le32_to_cpu(req->Length);
	id = le64_to_cpu(req->VolatileFileId);

	if (!server->pipe_desc || id != server->pipe_desc->id) {
		cifssrv_debug("Pipe not opened or invalid in Pipe id\n");
		if (server->pipe_desc)
			cifssrv_debug("Incoming id = %d opened pipe id = %d\n",
					id, server->pipe_desc->id);
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
		return ret;
	}

	if (le16_to_cpu(req->DataOffset) ==
			(offsetof(struct smb2_write_req, Buffer) - 4)) {
		data_buf = (char *)&req->Buffer[0];
	} else {
		if ((le16_to_cpu(req->DataOffset) > get_rfc1002_length(req)) ||
				(le16_to_cpu(req->DataOffset) +
				 length > get_rfc1002_length(req))) {
			cifssrv_err("invalid write data offset %u, smb_len %u\n",
					le16_to_cpu(req->DataOffset),
					get_rfc1002_length(req));
			err = -EINVAL;
			goto out;
		}

		data_buf = (char *)(((char *)&req->hdr.ProtocolId) +
				le16_to_cpu(req->DataOffset));
	}

	ret = process_rpc(server, data_buf);
	if (ret == -EOPNOTSUPP) {
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		smb2_set_err_rsp(smb_work);
		return ret;
	} else if (ret) {
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
		return ret;
	}
	rsp->StructureSize = cpu_to_le16(17);
	rsp->DataOffset = 0;
	rsp->Reserved = 0;
	rsp->DataLength = le32_to_cpu(req->Length);
	rsp->DataRemaining = 0;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(rsp, 16);
	return 0;
out:
	if (err) {
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
	}

	return err;
}

/**
 * smb2_write() - handler for smb2 write from file
 * @smb_work:	smb work containing write command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_write(struct smb_work *smb_work)
{
	struct smb2_write_req *req;
	struct smb2_write_rsp *rsp, *rsp_org;
	struct tcp_server_info *server = smb_work->server;
	loff_t offset;
	size_t length;
	ssize_t nbytes;
	char *data_buf;
	bool writethrough = false;
	uint64_t id = -1;
	int err = 0;

	req = (struct smb2_write_req *)smb_work->buf;
	rsp = (struct smb2_write_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_write_req *)((char *)req +
				smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_write_rsp *)((char *)rsp +
				smb_work->next_smb2_rsp_hdr_off);
		if (le64_to_cpu(req->VolatileFileId) == -1) {
			cifssrv_debug("Compound request assigning stored FID  = %llu\n",
				    smb_work->cur_local_fid);
			id = smb_work->cur_local_fid;
		}
	}

	if (req->StructureSize != 49) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (rsp->hdr.TreeId == 1) {
		cifssrv_debug("IPC pipe write request\n");
		return smb2_write_pipe(smb_work);
	}

	if (id == -1)
		id = le64_to_cpu(req->VolatileFileId);

	offset = le64_to_cpu(req->Offset);
	length = le32_to_cpu(req->Length);

	if (le16_to_cpu(req->DataOffset) ==
			(offsetof(struct smb2_write_req, Buffer) - 4)) {
		data_buf = (char *)&req->Buffer[0];
	} else {
		if ((le16_to_cpu(req->DataOffset) > get_rfc1002_length(req)) ||
				(le16_to_cpu(req->DataOffset) +
				 length > get_rfc1002_length(req))) {
			cifssrv_err("invalid write data offset %u, smb_len %u\n",
					le16_to_cpu(req->DataOffset),
					get_rfc1002_length(req));
			err = -EINVAL;
			goto out;
		}

		data_buf = (char *)(((char *)&req->hdr.ProtocolId) +
				le16_to_cpu(req->DataOffset));
	}

	cifssrv_debug("flags %u\n", le32_to_cpu(req->Flags));
	if (le32_to_cpu(req->Flags) & SMB2_WRITEFLAG_WRITE_THROUGH)
		writethrough = true;

	cifssrv_debug("fid %llu, offset %lld, len %zu\n", id, offset, length);
	err = smb_vfs_write(server, id, data_buf, length, &offset,
			writethrough, &nbytes);
	if (err < 0)
		goto out;

	rsp->StructureSize = cpu_to_le16(17);
	rsp->DataOffset = 0;
	rsp->Reserved = 0;
	rsp->DataLength = cpu_to_le32(nbytes);
	rsp->DataRemaining = 0;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(rsp_org, 16);
	return 0;

out:
	if (err == -EAGAIN)
		rsp->hdr.Status = NT_STATUS_FILE_LOCK_CONFLICT;
	else
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;

	smb2_set_err_rsp(smb_work);
	return err;
}

/**
 * smb2_flush() - handler for smb2 flush file - fsync
 * @smb_work:	smb work containing flush command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_flush(struct smb_work *smb_work)
{
	struct smb2_flush_req *req;
	struct smb2_flush_rsp *rsp;
	int err;

	req = (struct smb2_flush_req *)smb_work->buf;
	rsp = (struct smb2_flush_rsp *)smb_work->rsp_buf;

	if (req->StructureSize != 24) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	cifssrv_debug("SMB2_FLUSH called for fid %llu\n",
			le64_to_cpu(req->VolatileFileId));
	if (!find_matching_share((unsigned short)req->hdr.TreeId)) {
		cifssrv_err("invalid TreeId %u\n", req->hdr.TreeId);
		err = -ENXIO;
		goto out;
	}

	err = smb_vfs_fsync(smb_work->server,
			(unsigned short)le64_to_cpu(req->VolatileFileId));
	if (err)
		goto out;

	rsp->StructureSize = cpu_to_le16(4);
	rsp->Reserved = 0;
	inc_rfc1001_len(rsp, 4);
	return 0;

out:
	if (err) {
		rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
		smb2_set_err_rsp(smb_work);
	}

	return err;
}

/**
 * smb2_cancel() - handler for smb2 cancel command
 * @smb_work:	smb work containing cancel command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_cancel(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_hdr *hdr = (struct smb2_hdr *)smb_work->buf;
	struct smb2_hdr *work_hdr;
	struct smb_work *work;
	struct list_head *tmp;

	cifssrv_debug("smb2 cancel called on mid %llu\n", hdr->MessageId);

	spin_lock(&server->request_lock);
	list_for_each(tmp, &server->requests) {
		work = list_entry(tmp, struct smb_work, request_entry);
		work_hdr = (struct smb2_hdr *)work->buf;
		if (work_hdr->MessageId == hdr->MessageId) {
			cifssrv_debug("smb2 with mid %llu cancelled command = 0x%x\n",
					hdr->MessageId, work_hdr->Command);
			work->send_no_response = 1;
			list_del_init(&work->request_entry);
			work->added_in_request_list = 0;
			break;
		}
	}
	spin_unlock(&server->request_lock);

	/* For SMB2_CANCEL command itself send no response*/
	smb_work->send_no_response = 1;

	return 0;

}

/**
 * smb2_lock() - handler for smb2 file lock command
 * @smb_work:	smb work containing lock command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_lock(struct smb_work *smb_work)
{
	struct smb2_lock_req *req;
	struct smb2_lock_rsp *rsp;
	struct smb2_lock_element *lock_ele;
	struct cifssrv_file *fp;
	struct tcp_server_info *server = smb_work->server;
	struct file_lock *flock;
	struct file *filp;
	int16_t lock_count;
	int flags;
	unsigned int cmd = 0;
	bool pending = false;
	int err = 0;

	req = (struct smb2_lock_req *)smb_work->buf;
	rsp = (struct smb2_lock_rsp *)smb_work->rsp_buf;

	if (le16_to_cpu(req->StructureSize) != 48) {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		return 0;
	}

	cifssrv_debug("Recieved lock request\n");
	fp = get_id_from_fidtable(server,
			le64_to_cpu(req->VolatileFileId));
	if (!fp) {
		cifssrv_debug("Invalid file id for lock : %llu\n",
				le64_to_cpu(req->VolatileFileId));
		rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
		smb2_set_err_rsp(smb_work);
		return 0;
	}
	filp = fp->filp;
	lock_count = le16_to_cpu(req->LockCount);
	lock_ele = req->locks;

	cifssrv_debug("lock count is %d\n", lock_count);
	if (!lock_count)  {
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
		smb2_set_err_rsp(smb_work);
		return 0;
	}
	if (fp->persistent_id != le64_to_cpu(req->PersistentFileId)) {
		rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
		smb2_set_err_rsp(smb_work);
		return 0;
	}
	flock = locks_alloc_lock();
	if (!flock) {
		rsp->hdr.Status = NT_STATUS_LOCK_NOT_GRANTED;
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	locks_init_lock(flock);

	while (lock_count > 0) {
		flags = lock_ele->Flags;
		/* Checking for wrong flag combination during lock request*/
		switch (flags) {
		case SMB2_LOCKFLAG_SHARED:
		case SMB2_LOCKFLAG_EXCLUSIVE:
		case SMB2_LOCKFLAG_SHARED|SMB2_LOCKFLAG_FAIL_IMMEDIATELY:
		case SMB2_LOCKFLAG_EXCLUSIVE|SMB2_LOCKFLAG_FAIL_IMMEDIATELY:
		case SMB2_LOCKFLAG_UNLOCK:
			break;
		default:
			rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
			smb2_set_err_rsp(smb_work);
			locks_free_lock(flock);
			return 0;
		}
		flock->fl_start = le64_to_cpu(lock_ele->Offset);
		flock->fl_end = flock->fl_start +
			le64_to_cpu(lock_ele->Length) - 1;
		if (flock->fl_end < flock->fl_start) {
			rsp->hdr.Status = NT_STATUS_INVALID_LOCK_RANGE;
			smb2_set_err_rsp(smb_work);
			locks_free_lock(flock);
			return 0;
		}

		if ((flags & SMB2_LOCKFLAG_UNLOCK) == 0) {
			cifssrv_debug("recieved locking requests\n");

			flock->fl_owner = (struct files_struct *)filp;
			flock->fl_pid = current->tgid;
			flock->fl_file = filp;
			flock->fl_flags = FL_POSIX;
			if ((flags & SMB2_LOCKFLAG_FAIL_IMMEDIATELY) == 0)
				flock->fl_flags |= FL_SLEEP;
			flock->fl_ops = NULL;
			flock->fl_lmops = NULL;
			if (flags & SMB2_LOCKFLAG_SHARED) {
				flock->fl_type = F_RDLCK;
				if (!(filp->f_mode & FMODE_READ)) {
					rsp->hdr.Status =
						NT_STATUS_ACCESS_DENIED;
					smb2_set_err_rsp(smb_work);
					locks_free_lock(flock);
					return 0;
				}
			} else if (flags & SMB2_LOCKFLAG_EXCLUSIVE) {
				flock->fl_type = F_WRLCK;
				if (!(filp->f_mode & FMODE_WRITE)) {
					rsp->hdr.Status =
						NT_STATUS_ACCESS_DENIED;
					smb2_set_err_rsp(smb_work);
					locks_free_lock(flock);
					return 0;
				}
			}
			if ((flags & SMB2_LOCKFLAG_FAIL_IMMEDIATELY) != 0)
				cmd = F_SETLK;
			else
				cmd = F_SETLKW;
			for (;;) {
				err = smb_vfs_lock(filp, cmd, flock);
				if (err == FILE_LOCK_DEFERRED) {
					cifssrv_debug("would have to wait"
					" for getting lock\n");
					pending = true;
					rsp->hdr.Status	=
						NT_STATUS_PENDING;
					rsp->StructureSize = cpu_to_le16(4);
					rsp->Reserved = 0;
					inc_rfc1001_len(rsp, 4);
					smb_send_rsp(smb_work);
					err = wait_event_interruptible(
					      flock->fl_wait, !flock->fl_next);
					if (!err) {
						lock_count--;
						continue;
					}
				} else if (!err) {
					cifssrv_err("successful in taking"
						" lock\n");
					rsp->hdr.Status =
						NT_STATUS_OK;
					break;
				} else {
					rsp->hdr.Status =
						NT_STATUS_LOCK_NOT_GRANTED;
					smb2_set_err_rsp(smb_work);
					locks_free_lock(flock);
					return 0;
				}
			}
			lock_ele++;
		} else if ((flags & SMB2_LOCKFLAG_UNLOCK) != 0) {
			cifssrv_debug("recieved unlock request\n");
			flock->fl_owner = (struct files_struct *)filp;
			flock->fl_pid = current->tgid;
			flock->fl_file = filp;
			flock->fl_flags = FL_POSIX;
			flock->fl_ops = NULL;
			flock->fl_lmops = NULL;
			flock->fl_type = F_UNLCK;
			err = smb_vfs_lock(filp, cmd, flock);
			if (!err)
				cifssrv_debug("File unlocked\n");
			else if (err == -ENOENT) {
				rsp->hdr.Status = NT_STATUS_NOT_LOCKED;
				smb2_set_err_rsp(smb_work);
				locks_free_lock(flock);
				return 0;
			}
			lock_ele++;
		}

		lock_count--;
	}

	locks_free_lock(flock);
	rsp->StructureSize = cpu_to_le16(4);
	rsp->Reserved = 0;
	if (!pending)
		inc_rfc1001_len(rsp, 4);
	return err;
}

/**
 * smb2_ioctl() - handler for smb2 ioctl command
 * @smb_work:	smb work containing ioctl command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb2_ioctl(struct smb_work *smb_work)
{
	struct smb2_ioctl_req *req;
	struct smb2_ioctl_rsp *rsp, *rsp_org;
	int cnt_code, nbytes = 0;
	int out_buf_len;
	char *data_buf;
	uint64_t id = -1;
	int ret = 0;
	struct tcp_server_info *server = smb_work->server;
	req = (struct smb2_ioctl_req *)smb_work->buf;
	rsp = (struct smb2_ioctl_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_ioctl_req *)((char *)req +
				smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_ioctl_rsp *)((char *)rsp +
				smb_work->next_smb2_rsp_hdr_off);
		if (le64_to_cpu(req->VolatileFileId) == -1) {
			cifssrv_debug("Compound request assigning stored FID = %llu\n",
					smb_work->cur_local_fid);
			id = smb_work->cur_local_fid;
		}
	}

	if (req->StructureSize != 57) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (id == -1)
		id = le64_to_cpu(req->VolatileFileId);

	cnt_code = le32_to_cpu(req->CntCode);
	out_buf_len = le32_to_cpu(req->maxoutputresp);
	data_buf = (char *)&req->Buffer[0];

	switch (cnt_code) {
	case FSCTL_DFS_GET_REFERRALS:
		rsp->hdr.Status = FSCTL_DFS_GET_REFERRALS;
		goto out;
	case FSCTL_PIPE_TRANSCEIVE:
		if (rsp->hdr.TreeId == 1) {
			cifssrv_debug("Pipe transcevie\n");
			if (!server->pipe_desc || id != server->pipe_desc->id) {
				cifssrv_debug("Pipe not opened or"
					       "invalid in Pipe id\n");
				if (server->pipe_desc)
					cifssrv_debug(
					"Incoming id = %llu opened pipeid = %d\n",
					 id, server->pipe_desc->id);
				rsp->hdr.Status = NT_STATUS_INVALID_HANDLE;
				smb2_set_err_rsp(smb_work);
				return ret;
			}

			ret = process_rpc(server, data_buf);
			if (!ret) {
				nbytes = process_rpc_rsp(server,
					    (char *)rsp->Buffer, out_buf_len);
				if (nbytes > out_buf_len) {
					rsp->hdr.Status =
						NT_STATUS_BUFFER_OVERFLOW;
					nbytes = out_buf_len;
				} else if (nbytes < 0) {
					rsp->hdr.Status =
						NT_STATUS_INVALID_PARAMETER;
					goto out;
				}
			} else if (ret == -EOPNOTSUPP) {
				rsp->hdr.Status =
					NT_STATUS_NOT_SUPPORTED;
				goto out;
			} else {
				rsp->hdr.Status =
					NT_STATUS_INVALID_PARAMETER;
				goto out;
			}

		}
		break;

	default:
		cifssrv_debug("not implemented yet ioctl command 0x%x\n",
				cnt_code);
		rsp->hdr.Status = NT_STATUS_NOT_SUPPORTED;
		goto out;
	}
	rsp->CntCode = cpu_to_le32(cnt_code);
	rsp->VolatileFileId = cpu_to_le64(id);
	rsp->inputcount = cpu_to_le32(0);
	rsp->inputoffset = cpu_to_le32(112);
	rsp->outputcount = cpu_to_le32(nbytes);
	rsp->outputoffset = cpu_to_le32(112);
	rsp->StructureSize = cpu_to_le16(49);
	rsp->Reserved = cpu_to_le16(0);
	rsp->flags = cpu_to_le32(0);
	rsp->Reserved2 = cpu_to_le32(0);
	inc_rfc1001_len(rsp_org, 48 + nbytes);
	return 0;

out:
	if (rsp->hdr.Status == 0)
		rsp->hdr.Status = NT_STATUS_INVALID_PARAMETER;
	smb2_set_err_rsp(smb_work);
	return 0;
}

/**
 * smb20_oplock_break() - handler for smb2.0 oplock break command
 * @smb_work:	smb work containing oplock break command buffer
 *
 * Return:	0
 */
int smb20_oplock_break(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_oplock_break *req;
	struct smb2_oplock_break *rsp;
	struct cifssrv_file *fp;
	struct ofile_info *ofile;
	struct oplock_info *opinfo;
	int err = 0, ret = 0;
	uint64_t volatile_id, persistent_id;
	char oplock;
	unsigned int oplock_change_type;

	req = (struct smb2_oplock_break *)smb_work->buf;
	rsp = (struct smb2_oplock_break *)smb_work->rsp_buf;
	volatile_id = le64_to_cpu(req->VolatileFid);
	persistent_id = le64_to_cpu(req->PersistentFid);
	oplock = req->OplockLevel;
	cifssrv_debug("SMB2_OPLOCK_BREAK v_id %llu, p_id %llu oplock %d\n",
			volatile_id, persistent_id, oplock);

	mutex_lock(&ofile_list_lock);
	fp = get_id_from_fidtable(server, volatile_id);
	if (!fp) {
		mutex_unlock(&ofile_list_lock);
		rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
		goto err_out;
	}

	ofile = fp->ofile;
	if (ofile == NULL) {
		mutex_unlock(&ofile_list_lock);
		cifssrv_err("unexpected null ofile_info\n");
		goto err_out;
	}

	opinfo = get_matching_opinfo(server, ofile, volatile_id, 0);
	if (opinfo == NULL) {
		mutex_unlock(&ofile_list_lock);
		cifssrv_err("unexpected null oplock_info\n");
		goto err_out;
	}

	if (opinfo->state == OPLOCK_NOT_BREAKING) {
		mutex_unlock(&ofile_list_lock);
		rsp->hdr.Status =
			NT_STATUS_INVALID_DEVICE_STATE;
		cifssrv_err("unexpected oplock state 0x%x\n", opinfo->state);
		goto err_out;
	}

	if (((opinfo->lock_type == SMB2_OPLOCK_LEVEL_EXCLUSIVE) ||
			(opinfo->lock_type == SMB2_OPLOCK_LEVEL_BATCH)) &&
			((oplock != SMB2_OPLOCK_LEVEL_II) &&
			 (oplock != SMB2_OPLOCK_LEVEL_NONE))) {
		err = NT_STATUS_INVALID_OPLOCK_PROTOCOL;
		oplock_change_type = OPLOCK_WRITE_TO_NONE;
	} else if ((opinfo->lock_type == SMB2_OPLOCK_LEVEL_II) &&
			(oplock != SMB2_OPLOCK_LEVEL_NONE)) {
		err = NT_STATUS_INVALID_OPLOCK_PROTOCOL;
		oplock_change_type = OPLOCK_READ_TO_NONE;
	} else if ((oplock == SMB2_OPLOCK_LEVEL_II) ||
			(oplock == SMB2_OPLOCK_LEVEL_NONE)) {
		err = NT_STATUS_INVALID_DEVICE_STATE;
		if (((opinfo->lock_type == SMB2_OPLOCK_LEVEL_EXCLUSIVE) ||
			(opinfo->lock_type == SMB2_OPLOCK_LEVEL_BATCH)) &&
			(oplock == SMB2_OPLOCK_LEVEL_II)) {
			oplock_change_type = OPLOCK_WRITE_TO_READ;
		} else if (((opinfo->lock_type == SMB2_OPLOCK_LEVEL_EXCLUSIVE)
			|| (opinfo->lock_type == SMB2_OPLOCK_LEVEL_BATCH)) &&
			(oplock == SMB2_OPLOCK_LEVEL_NONE)) {
			oplock_change_type = OPLOCK_WRITE_TO_NONE;
		} else if ((opinfo->lock_type == SMB2_OPLOCK_LEVEL_II) &&
				(oplock == SMB2_OPLOCK_LEVEL_NONE)) {
			oplock_change_type = OPLOCK_READ_TO_NONE;
		} else
			oplock_change_type = 0;
	} else
		oplock_change_type = 0;

	switch (oplock_change_type) {
	case OPLOCK_WRITE_TO_READ:
		ret = opinfo_write_to_read(ofile, opinfo, 0);
		oplock = SMB2_OPLOCK_LEVEL_II;
		break;
	case OPLOCK_WRITE_TO_NONE:
		ret = opinfo_write_to_none(ofile, opinfo);
		oplock = SMB2_OPLOCK_LEVEL_NONE;
		break;
	case OPLOCK_READ_TO_NONE:
		ret = opinfo_read_to_none(ofile, opinfo);
		oplock = SMB2_OPLOCK_LEVEL_NONE;
		break;
	default:
		cifssrv_err("unknown oplock change 0x%x -> 0x%x\n",
				opinfo->lock_type, oplock);
	}

	opinfo->state = OPLOCK_NOT_BREAKING;
	wake_up_interruptible(&server->oplock_q);
	wake_up(&ofile->op_end_wq);
	mutex_unlock(&ofile_list_lock);

	if (ret < 0) {
		rsp->hdr.Status = err;
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	rsp->StructureSize = cpu_to_le16(24);
	rsp->OplockLevel = oplock;
	rsp->Reserved = 0;
	rsp->Reserved2 = 0;
	rsp->VolatileFid = cpu_to_le64(volatile_id);
	rsp->PersistentFid = cpu_to_le64(persistent_id);
	inc_rfc1001_len(rsp, 24);
	return 0;

err_out:
	rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
	smb2_set_err_rsp(smb_work);
	return 0;
}

/**
 * smb21_lease_break() - handler for smb2.1 lease break command
 * @smb_work:	smb work containing lease break command buffer
 *
 * Return:	0
 */
int smb21_lease_break(struct smb_work *smb_work)
{
	struct tcp_server_info *server = smb_work->server;
	struct smb2_lease_ack *req, *rsp;
	struct ofile_info *ofile = NULL;
	struct oplock_info *opinfo;
	int err = 0, ret = 0;
	unsigned int lease_change_type;
	__le32 lease_state;

	req = (struct smb2_lease_ack *)smb_work->buf;
	rsp = (struct smb2_lease_ack *)smb_work->rsp_buf;

	cifssrv_debug("smb21 lease break, lease state(0x%x)\n",
			req->LeaseState);
	mutex_lock(&ofile_list_lock);
	opinfo = get_matching_opinfo_lease(server, &ofile, req->LeaseKey,
			NULL, 0);
	if (ofile == NULL || opinfo == NULL) {
		mutex_unlock(&ofile_list_lock);
		cifssrv_debug("file not opened\n");
		goto err_out;
	}

	if (opinfo->state == OPLOCK_NOT_BREAKING) {
		mutex_unlock(&ofile_list_lock);
		rsp->hdr.Status =
			NT_STATUS_INVALID_DEVICE_STATE;
		cifssrv_debug("unexpected lease break state 0x%x\n",
				opinfo->state);
		goto err_out;
	}

	/* check for bad lease state */
	if (req->LeaseState & (~(SMB2_LEASE_READ_CACHING |
					SMB2_LEASE_HANDLE_CACHING))) {
		err = NT_STATUS_INVALID_OPLOCK_PROTOCOL;
		if (opinfo->CurrentLeaseState & SMB2_LEASE_WRITE_CACHING)
			lease_change_type = OPLOCK_WRITE_TO_NONE;
		else
			lease_change_type = OPLOCK_READ_TO_NONE;
		cifssrv_debug("handle bad lease state 0x%x -> 0x%x\n",
				opinfo->CurrentLeaseState, req->LeaseState);
	} else if ((opinfo->CurrentLeaseState == SMB2_LEASE_READ_CACHING) &&
			(req->LeaseState != SMB2_LEASE_NONE)) {
		err = NT_STATUS_INVALID_OPLOCK_PROTOCOL;
		lease_change_type = OPLOCK_READ_TO_NONE;
		cifssrv_debug("handle bad lease state 0x%x -> 0x%x\n",
				opinfo->CurrentLeaseState, req->LeaseState);
	} else {
		/* valid lease state changes */
		err = NT_STATUS_INVALID_DEVICE_STATE;
		if (req->LeaseState == SMB2_LEASE_NONE) {
			if (opinfo->CurrentLeaseState &
					SMB2_LEASE_WRITE_CACHING)
				lease_change_type = OPLOCK_WRITE_TO_NONE;
			else
				lease_change_type = OPLOCK_READ_TO_NONE;
		} else if (req->LeaseState & SMB2_LEASE_READ_CACHING)
			lease_change_type = OPLOCK_WRITE_TO_READ;
		else
			lease_change_type = 0;
	}

	switch (lease_change_type) {
	case OPLOCK_WRITE_TO_READ:
		ret = opinfo_write_to_read(ofile, opinfo, req->LeaseState);
		break;
	case OPLOCK_WRITE_TO_NONE:
		ret = opinfo_write_to_none(ofile, opinfo);
		break;
	case OPLOCK_READ_TO_NONE:
		ret = opinfo_read_to_none(ofile, opinfo);
		break;
	default:
		cifssrv_debug("unknown lease change 0x%x -> 0x%x\n",
				opinfo->CurrentLeaseState, req->LeaseState);
	}

	lease_state = opinfo->CurrentLeaseState;
	opinfo->state = OPLOCK_NOT_BREAKING;
	wake_up_interruptible(&server->oplock_q);
	wake_up(&ofile->op_end_wq);
	mutex_unlock(&ofile_list_lock);

	if (ret < 0) {
		rsp->hdr.Status = err;
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	rsp->StructureSize = cpu_to_le16(36);
	rsp->Reserved = 0;
	rsp->Flags = 0;
	memcpy(rsp->LeaseKey, req->LeaseKey, 16);
	rsp->LeaseState = lease_state;
	rsp->LeaseDuration = 0;
	inc_rfc1001_len(rsp, 36);
	return 0;

err_out:
	rsp->hdr.Status = NT_STATUS_FILE_CLOSED;
	smb2_set_err_rsp(smb_work);
	return 0;
}

/**
 * smb2_oplock_break() - dispatcher for smb2.0 and 2.1 oplock/lease break
 * @smb_work:	smb work containing oplock/lease break command buffer
 *
 * Return:	0
 */
int smb2_oplock_break(struct smb_work *smb_work)
{
	struct smb2_oplock_break *req;
	struct smb2_oplock_break *rsp;
	int err;

	req = (struct smb2_oplock_break *)smb_work->buf;
	rsp = (struct smb2_oplock_break *)smb_work->rsp_buf;

	switch (le16_to_cpu(req->StructureSize)) {
	case OP_BREAK_STRUCT_SIZE_20:
		err = smb20_oplock_break(smb_work);
		break;
	case OP_BREAK_STRUCT_SIZE_21:
		err = smb21_lease_break(smb_work);
		break;
	default:
		cifssrv_debug("invalid break cmd %d\n", req->StructureSize);
		err = NT_STATUS_INVALID_PARAMETER;
		goto err_out;
	}

	if (err)
		goto err_out;

	return 0;

err_out:
	rsp->hdr.Status = err;
	smb2_set_err_rsp(smb_work);
	return 0;
}

/**
 * smb2_notify() - handler for smb2 notify request
 * @smb_work:	smb work containing notify command buffer
 *
 * Return:	0
 */
int smb2_notify(struct smb_work *smb_work)
{
	struct smb2_notify_req *req;
	struct smb2_notify_rsp *rsp, *rsp_org;

	req = (struct smb2_notify_req *)smb_work->buf;
	rsp = (struct smb2_notify_rsp *)smb_work->rsp_buf;
	rsp_org = rsp;

	if (smb_work->next_smb2_rcv_hdr_off) {
		req = (struct smb2_notify_req *)((char *)req +
					smb_work->next_smb2_rcv_hdr_off);
		rsp = (struct smb2_notify_rsp *)((char *)rsp +
					smb_work->next_smb2_rsp_hdr_off);
	}

	if (req->StructureSize != 32) {
		cifssrv_err("malformed packet\n");
		smb_work->send_no_response = 1;
		return 0;
	}

	if (smb_work->next_smb2_rcv_hdr_off &&
			le32_to_cpu(req->hdr.NextCommand)) {
		rsp->hdr.Status = NT_STATUS_INTERNAL_ERROR;
		smb2_set_err_rsp(smb_work);
		return 0;
	}

	rsp->hdr.Status = NT_STATUS_OK;
	rsp->StructureSize = cpu_to_le16(9);
	rsp->OutputBufferLength = cpu_to_le32(0);
	rsp->OutputBufferOffset = cpu_to_le16(0);
	rsp->Buffer[0] = 0;
	inc_rfc1001_len(rsp_org, 9);
	return 0;
}