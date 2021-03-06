/*******************************************************************************
 * This file contains the smbdirect driver for Samba
 *
 * (c) Richard Sharpe <rsharpe@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/

#include <linux/spinlock.h>
#include <linux/cdev.h>
#include <rdma/rdma_cm.h>

/*
 * The port number we listen on
 */
#define SMB_DIRECT_PORT 5445

/* Max CQ depth per queue ... */
#define MAX_CQ_DEPTH 128

#define SMBD_IOC_VAL 's'
#define SMBD_LISTEN         _IOR(SMBD_IOC_VAL, 1, void *)
#define SMBD_SET_PARAMS     _IOR(SMBD_IOC_VAL, 2, void *)
#define SMBD_GET_MEM_PARAMS _IOW(SMBD_IOC_VAL, 3, void *)
#define SMBD_SET_SESSION_ID _IOW(SMBD_IOC_VAL, 4, void *)

enum smbd_states {
	SMBD_NEGOTIATE = 0, /* This is the PASSIVE state in the spec */
	SMBD_TRANSFER,      /* This is the ESTABLISHED state in the spec */
	SMBD_ERROR,      /* There's been an error, drop the connection */
};

struct smbd_negotiate_req {
	uint16_t min_version;
	uint16_t max_version;
	uint16_t reserved;
	uint16_t credits_requested;
	uint32_t preferred_send_size;
	uint32_t max_receive_size;
	uint32_t max_fragmented_size;
} __attribute__((packed));

struct smbd_negotiate_resp {
	uint16_t min_version;
	uint16_t max_version;
	uint16_t negotiated_version;
	uint16_t reserved;
	uint16_t credits_requested;
	uint16_t credits_granted;
	uint32_t status;
	uint32_t max_read_write_size;
} __attribute__((packed));

struct smbd_params {
	unsigned int send_credits;
	unsigned int recv_credits;
	unsigned int recv_credit_max;
	unsigned int send_credit_target;
	unsigned int max_snd_size;
	unsigned int max_fragmented_size;
	unsigned int max_receive_size;
	unsigned int max_read_write_size;
	unsigned int keepalive_interval;
	unsigned int sec_blob_size;
	void *sec_blob;
};

struct smbd_device {
	bool initialized;
	dev_t smbdirect_dev_no;
	struct class *kio_class;
	struct device *kio_device;

	int connection_count;
	struct smbd_params params;
	struct mutex connection_list_mutex; /* Controls access to the list */
	/*
	 * List of connections or pending connections
	 */
	wait_queue_head_t conn_queue;
	struct list_head connection_list;
	struct cdev cdev;
	/*
	 * RDMA Related stuff, including our listen port.
	 */
	struct rdma_cm_id *cm_lid;

	struct workqueue_struct *smbd_wq;
};

/*
 * Defines connections or pending connections 
 */
struct connection_struct {
	struct smbd_device *smbd_device;

	struct list_head connect_ent;
	wait_queue_head_t wait_queue;
	enum smbd_states state;
	unsigned long long session_id;
	struct work_struct cq_work;
	struct work_struct disconnect_work;

	/*
	 * RDMA stuff
	 */
	struct rdma_cm_id *cm_id;
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	/*
	 * Structures for sending and receiving RDMA stuff
	 */
	struct ib_recv_wr recv_wr; /* Initial receive ... */
	struct ib_sge recv_sge;    /* Single SGE for now */
	struct ib_mr *recv_mr;

	struct ib_send_wr send_wr;
	struct ib_sge send_sge;
	struct ib_mr *send_mr;

	/*
	 * Buffers ...
	 */
	u64 recv_buf_dma;
	u64 send_buf_dma;
	char recv_buf[20];         /* an SMB Direct  Negotiate req */
	char gap[32];
	char send_buf[32];	   /* The SMB Direct Neg response  */
	char gap1[32];
};

int smbd_listen(struct smbd_device *smbd_dev);
int smbd_teardown_listen_and_connections(struct smbd_device *smbd_dev);

