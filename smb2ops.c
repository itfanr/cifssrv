/*
 *   fs/cifssrv/smb2ops.c
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

#include <linux/slab.h>
#include "glob.h"
#include "smb2pdu.h"

struct smb_version_values smb20_server_values = {
	.version_string = SMB20_VERSION_STRING,
	.protocol_id = SMB20_PROT_ID,
	.req_capabilities = 0, /* MBZ */
	.large_lock_type = 0,
	.exclusive_lock_type = SMB2_LOCKFLAG_EXCLUSIVE,
	.shared_lock_type = SMB2_LOCKFLAG_SHARED,
	.unlock_lock_type = SMB2_LOCKFLAG_UNLOCK,
	.header_size = sizeof(struct smb2_hdr),
	.max_header_size = MAX_SMB2_HDR_SIZE,
	.read_rsp_size = sizeof(struct smb2_read_rsp) - 1,
	.lock_cmd = SMB2_LOCK,
	.cap_unix = 0,
	.cap_nt_find = SMB2_NT_FIND,
	.cap_large_files = SMB2_LARGE_FILES,
};

struct smb_version_values smb21_server_values = {
	.version_string = SMB21_VERSION_STRING,
	.protocol_id = SMB21_PROT_ID,
	.req_capabilities = 0, /* MBZ */
	.large_lock_type = 0,
	.exclusive_lock_type = SMB2_LOCKFLAG_EXCLUSIVE,
	.shared_lock_type = SMB2_LOCKFLAG_SHARED,
	.unlock_lock_type = SMB2_LOCKFLAG_UNLOCK,
	.header_size = sizeof(struct smb2_hdr),
	.max_header_size = MAX_SMB2_HDR_SIZE,
	.read_rsp_size = sizeof(struct smb2_read_rsp) - 1,
	.lock_cmd = SMB2_LOCK,
	.cap_unix = 0,
	.cap_nt_find = SMB2_NT_FIND,
	.cap_large_files = SMB2_LARGE_FILES,
};

struct smb_version_values smb30_server_values = {
	.version_string = SMB30_VERSION_STRING,
	.protocol_id = SMB30_PROT_ID,
	.req_capabilities = SMB2_GLOBAL_CAP_DFS | SMB2_GLOBAL_CAP_LEASING
						| SMB2_GLOBAL_CAP_LARGE_MTU,
	.large_lock_type = 0,
	.exclusive_lock_type = SMB2_LOCKFLAG_EXCLUSIVE,
	.shared_lock_type = SMB2_LOCKFLAG_SHARED,
	.unlock_lock_type = SMB2_LOCKFLAG_UNLOCK,
	.header_size = sizeof(struct smb2_hdr),
	.max_header_size = MAX_SMB2_HDR_SIZE,
	.read_rsp_size = sizeof(struct smb2_read_rsp) - 1,
	.lock_cmd = SMB2_LOCK,
	.cap_unix = 0,
	.cap_nt_find = SMB2_NT_FIND,
	.cap_large_files = SMB2_LARGE_FILES,
};

struct smb_version_ops smb2_0_server_ops = {
	.get_cmd_val		=	get_smb2_cmd_val,
	.init_rsp_hdr		=	init_smb2_rsp_hdr,
	.set_rsp_status		=	set_smb2_rsp_status,
	.allocate_rsp_buf       =       smb2_allocate_rsp_buf,
	.set_rsp_credits        =       smb2_set_rsp_credits,
	.check_user_session	=	smb2_check_user_session,
};

struct smb_version_cmds smb2_0_server_cmds[NUMBER_OF_SMB2_COMMANDS] = {
	[SMB2_NEGOTIATE_HE]	=	{ .proc = smb2_negotiate, },
	[SMB2_SESSION_SETUP_HE] =	{ .proc = smb2_sess_setup, },
	[SMB2_TREE_CONNECT_HE]  =	{ .proc = smb2_tree_connect,},
	[SMB2_TREE_DISCONNECT_HE]  =	{ .proc = smb2_tree_disconnect,},
	[SMB2_LOGOFF_HE]	=	{ .proc = smb2_session_logoff,},
	[SMB2_CREATE_HE]	=	{ .proc = smb2_open},
	[SMB2_QUERY_INFO_HE]	=	{ .proc = smb2_query_info},
	[SMB2_QUERY_DIRECTORY_HE] =	{ .proc = smb2_query_dir},
	[SMB2_CLOSE_HE]		=	{ .proc = smb2_close},
	[SMB2_ECHO_HE]		=	{ .proc = smb2_echo},
	[SMB2_SET_INFO_HE]      =       { .proc = smb2_set_info},
	[SMB2_READ_HE]		=	{ .proc = smb2_read},
	[SMB2_WRITE_HE]		=	{ .proc = smb2_write},
	[SMB2_FLUSH_HE]		=	{ .proc = smb2_flush},
	[SMB2_CANCEL_HE]	=	{ .proc = smb2_cancel},
	[SMB2_LOCK_HE]		=	{ .proc = smb2_lock},
	[SMB2_IOCTL_HE]		=	{ .proc = smb2_ioctl},
	[SMB2_OPLOCK_BREAK_HE]	=	{ .proc = smb2_oplock_break},
	[SMB2_CHANGE_NOTIFY_HE]	=	{ .proc = smb2_notify},
};

/**
 * init_smb2_0_server() - initialize a smb server connection with smb2.0
 *			command dispatcher
 * @server:	TCP server instance of connection
 */
void init_smb2_0_server(struct tcp_server_info *server)
{
	if (!server)
		return;

	server->vals = &smb20_server_values;
	server->ops = &smb2_0_server_ops;
	server->cmds = smb2_0_server_cmds;
	server->max_cmds =
		sizeof(smb2_0_server_cmds)/sizeof(smb2_0_server_cmds[0]);
	server->max_credits = SMB2_MAX_CREDITS;
	server->credits_granted = 0;
}

/**
 * init_smb2_1_server() - initialize a smb server connection with smb2.1
 *			command dispatcher
 * @server:	TCP server instance of connection
 */
void init_smb2_1_server(struct tcp_server_info *server)
{
	if (!server)
		return;

	server->vals = &smb21_server_values;
	server->ops = &smb2_0_server_ops;
	server->cmds = smb2_0_server_cmds;
	server->max_cmds =
		sizeof(smb2_0_server_cmds)/sizeof(smb2_0_server_cmds[0]);
	if (lease_enable)
		server->capabilities = SMB2_GLOBAL_CAP_LEASING;

	server->capabilities |= SMB2_GLOBAL_CAP_LARGE_MTU;
}

/**
 * init_smb3_0_server() - initialize a smb server connection with smb3.0
 *			command dispatcher
 * @server:	TCP server instance of connection
 */
void init_smb3_0_server(struct tcp_server_info *server)
{
	if (!server)
		return;

	server->vals = &smb30_server_values;
	server->ops = &smb2_0_server_ops;
	server->cmds = smb2_0_server_cmds;
	server->max_cmds =
		sizeof(smb2_0_server_cmds)/sizeof(smb2_0_server_cmds[0]);
	if (lease_enable)
		server->capabilities = SMB2_GLOBAL_CAP_LEASING;

	server->capabilities |= SMB2_GLOBAL_CAP_LARGE_MTU;
}