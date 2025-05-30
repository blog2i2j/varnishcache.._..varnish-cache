/*-
 * Copyright (c) 2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

struct VSC_vbe;

/* cache_http1_fetch.c [V1F] */
int V1F_SendReq(struct worker *, struct busyobj *, uint64_t *ctr_hdrbytes,
    uint64_t *ctr_bodybytes);
int V1F_FetchRespHdr(struct busyobj *);
int V1F_Setup_Fetch(struct vfp_ctx *vfc, struct http_conn *htc);

/* cache_http1_fsm.c [HTTP1] */
extern const int HTTP1_Req[3];
extern const int HTTP1_Resp[3];

/* cache_http1_deliver.c */
enum vtr_deliver_e V1D_Deliver(struct req *, int sendbody);

/* cache_http1_pipe.c */
struct v1p_acct {
	uint64_t        req;
	uint64_t        bereq;
	uint64_t        in;
	uint64_t        out;
};

int V1P_Enter(void);
void V1P_Leave(void);
stream_close_t V1P_Process(const struct req *, int fd, struct v1p_acct *,
    vtim_real deadline);
void V1P_Charge(struct req *, const struct v1p_acct *, struct VSC_vbe *);

/* cache_http1_line.c */
void V1L_Chunked(struct v1l *v1l);
void V1L_EndChunk(struct v1l *v1l);
struct v1l * V1L_Open(struct ws *, int *fd, struct vsl_log *,
    vtim_real deadline, unsigned niov);
void V1L_NoRollback(struct v1l *v1l);
stream_close_t V1L_Flush(struct v1l *v1l);
stream_close_t V1L_Close(struct v1l **v1lp, uint64_t *cnt);
size_t V1L_Write(struct v1l *v1l, const void *ptr, ssize_t len);
extern const struct vdp * const VDP_v1l;
