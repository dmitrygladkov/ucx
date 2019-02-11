/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include "tcp.h"

#include <ucs/async/async.h>


static void uct_tcp_ep_epoll_ctl(uct_tcp_ep_t *ep, int op)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    struct epoll_event epoll_event;
    int ret;

    memset(&epoll_event, 0, sizeof(epoll_event));
    epoll_event.data.ptr = ep;
    epoll_event.events   = ep->events;
    ret = epoll_ctl(iface->epfd, op, ep->fd, &epoll_event);
    if (ret < 0) {
        ucs_fatal("epoll_ctl(epfd=%d, op=%d, fd=%d) failed: %m",
                  iface->epfd, op, ep->fd);
    }
}

static inline int uct_tcp_ep_can_send(uct_tcp_ep_t *ep)
{
    ucs_assert(ep->offset <= ep->length);
    /* TODO optimize to allow partial sends/message coalescing */
    return ep->length == 0;
}

static UCS_CLASS_INIT_FUNC(uct_tcp_ep_t, uct_tcp_iface_t *iface,
                           int fd, const struct sockaddr_in *dest_addr)
{
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super)

    self->buf = NULL;
    self->events = 0;
    self->offset = 0;
    self->length = 0;
    ucs_queue_head_init(&self->pending_q);

    if (fd == -1) {
        status = ucs_tcpip_socket_create(&self->fd);
        if (status != UCS_OK) {
            goto err;
        }

        /* TODO use non-blocking connect */
        status = uct_tcp_socket_connect(self->fd, dest_addr);
        if (status != UCS_OK) {
            goto err_close;
        }
    } else {
        self->fd = fd;
    }

    status = ucs_sys_fcntl_modfl(self->fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        goto err_close;
    }

    status = uct_tcp_iface_set_sockopt(iface, self->fd);
    if (status != UCS_OK) {
        goto err_close;
    }

    uct_tcp_ep_epoll_ctl(self, EPOLL_CTL_ADD);

    UCS_ASYNC_BLOCK(iface->super.worker->async);
    ucs_list_add_tail(&iface->ep_list, &self->list);
    UCS_ASYNC_UNBLOCK(iface->super.worker->async);

    ucs_debug("tcp_ep %p: created on iface %p, fd %d", self, iface, self->fd);
    return UCS_OK;

err_close:
    close(self->fd);
err:
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_tcp_ep_t)
{
    uct_tcp_iface_t *iface = ucs_derived_of(self->super.super.iface,
                                            uct_tcp_iface_t);

    ucs_debug("tcp_ep %p: destroying", self);

    UCS_ASYNC_BLOCK(iface->super.worker->async);
    ucs_list_del(&self->list);
    UCS_ASYNC_UNBLOCK(iface->super.worker->async);

    if (self->length != 0)
        ucs_mpool_put(self->buf);

    close(self->fd);
}

UCS_CLASS_DEFINE(uct_tcp_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NAMED_NEW_FUNC(uct_tcp_ep_create, uct_tcp_ep_t, uct_tcp_ep_t,
                                uct_tcp_iface_t*, int,
                                const struct sockaddr_in*)
UCS_CLASS_DEFINE_NAMED_DELETE_FUNC(uct_tcp_ep_destroy, uct_tcp_ep_t, uct_ep_t)

ucs_status_t uct_tcp_ep_create_connected(const uct_ep_params_t *params,
                                         uct_ep_h *ep_p)
{
    uct_tcp_iface_t *iface = ucs_derived_of(params->iface, uct_tcp_iface_t);
    uct_tcp_ep_t *tcp_ep = NULL;
    struct sockaddr_in dest_addr;
    ucs_status_t status;

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port   = *(in_port_t*)params->iface_addr;
    dest_addr.sin_addr   = *(struct in_addr*)params->dev_addr;

    /* TODO try to reuse existing connection */
    status = uct_tcp_ep_create(iface, -1, &dest_addr, &tcp_ep);
    if (status == UCS_OK) {
        ucs_debug("tcp_ep %p: connected to %s:%d", tcp_ep,
                  inet_ntoa(dest_addr.sin_addr), ntohs(dest_addr.sin_port));
        *ep_p = &tcp_ep->super.super;
    }
    return status;
}

void uct_tcp_ep_mod_events(uct_tcp_ep_t *ep, uint32_t add, uint32_t remove)
{
    int new_events = (ep->events | add) & ~remove;

    if (new_events != ep->events) {
        ep->events = new_events;
        ucs_trace("tcp_ep %p: set events to %c%c", ep,
                  (new_events & EPOLLIN)  ? 'i' : '-',
                  (new_events & EPOLLOUT) ? 'o' : '-');
        uct_tcp_ep_epoll_ctl(ep, EPOLL_CTL_MOD);
    }
}

static unsigned uct_tcp_ep_send(uct_tcp_ep_t *ep)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface, uct_tcp_iface_t);
    size_t send_length;
    ucs_status_t status;

    send_length = ep->length - ep->offset;
    ucs_assert(send_length > 0);

    status = uct_tcp_send(ep->fd, ep->buf + ep->offset, &send_length);
    if (status < 0) {
        return 0;
    }

    ucs_trace_data("tcp_ep %p: sent %zu bytes", ep, send_length);

    iface->outstanding -= send_length;
    ep->offset         += send_length;
    if (ep->offset == ep->length) {
        ep->offset = 0;
        ep->length = 0;
        ucs_mpool_put_inline(ep->buf);
    }

    return send_length > 0;
}

unsigned uct_tcp_ep_progress_tx(uct_tcp_ep_t *ep)
{
    unsigned                     count = 0;
    uct_pending_req_priv_queue_t *priv;

    ucs_trace_func("ep=%p", ep);

    if (ep->length > 0) {
        count += uct_tcp_ep_send(ep);
    }

    uct_pending_queue_dispatch(priv, &ep->pending_q, uct_tcp_ep_can_send(ep));

    if (uct_tcp_ep_can_send(ep)) {
        ucs_assert(ucs_queue_is_empty(&ep->pending_q));
        uct_tcp_ep_mod_events(ep, 0, EPOLLOUT);
    }

    return count;
}

static inline void
uct_tcp_ep_complete_recv_am(uct_tcp_iface_t *iface, uct_tcp_ep_t *ep,
                            uct_tcp_am_hdr_t *hdr)
{
    ucs_assertv(hdr->am_id < UCT_AM_ID_MAX, "invalid am id: %d", hdr->am_id);

    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_RECV, hdr->am_id,
                       hdr + 1, hdr->length, "RECV fd %d", ep->fd);
    uct_iface_invoke_am(&iface->super, hdr->am_id, hdr + 1,
                        hdr->length, 0);
}

static inline unsigned uct_tcp_ep_do_next_rx(uct_tcp_ep_t *ep)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    uct_tcp_am_hdr_t *hdr;
    ucs_status_t status;
    size_t recv_length;
    ssize_t remainder;

    ucs_assertv(ep->length == 0, "ep=%p", ep);

    ep->buf = ucs_mpool_get_inline(&iface->rx_buf_mp);
    if (ucs_unlikely(!ep->buf)) {
        return 0;
    }

    recv_length = iface->config.buf_size - ep->length;
    ucs_assertv(recv_length > 0, "ep=%p", ep);

    status = uct_tcp_recv(ep->fd, ep->buf, &recv_length);
    if (status != UCS_OK) {
        if (status == UCS_ERR_CANCELED) {
            ucs_debug("tcp_ep %p: remote disconnected", ep);
            uct_tcp_ep_mod_events(ep, 0, EPOLLIN);
            ucs_mpool_put(ep->buf);
            uct_tcp_ep_destroy(&ep->super.super);
        }
        return 0;
    }

    ep->length += recv_length;
    ucs_trace_data("tcp_ep %p: recvd %zu bytes", ep, recv_length);

    /* Parse received active messages */
    while (ep->offset < ep->length) {
        remainder = ep->length - ep->offset;
        if (remainder < sizeof(*hdr)) {
	    return recv_length > 0;
        }

        hdr = ep->buf + ep->offset; 
        ucs_assert(hdr->length <= (iface->config.buf_size - sizeof(uct_tcp_am_hdr_t)));

        if (remainder < sizeof(*hdr) + hdr->length) {
            return recv_length > 0;
        }

        /* Full message was received */
        ep->offset += sizeof(*hdr) + hdr->length;

        uct_tcp_ep_complete_recv_am(iface, ep, hdr);
    }

    ep->length = ep->offset = 0;
    ucs_mpool_put_inline(ep->buf);

    return recv_length > 0;
}

static inline
unsigned uct_tcp_ep_do_partial_rx(uct_tcp_ep_t *ep)
{
    uct_tcp_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                            uct_tcp_iface_t);
    ucs_status_t status;
    uct_tcp_am_hdr_t *hdr;
    size_t cur_recvd_length = ep->length - ep->offset;
    size_t recv_length;

    if (cur_recvd_length < sizeof(*hdr)) {
        recv_length = sizeof(*hdr) - cur_recvd_length;

        status = uct_tcp_recv(ep->fd, ep->buf + ep->length, &recv_length);
        if (status != UCS_OK) {
            if (status == UCS_ERR_CANCELED) {
                ucs_debug("tcp_ep %p: remote disconnected", ep);
                uct_tcp_ep_mod_events(ep, 0, EPOLLIN);
                /* No need to put ep::buf to mpool, it will be done in
		 * destroy function, because `ep::length != 0` */
                uct_tcp_ep_destroy(&ep->super.super);
            }
            return 0;
        }

        ep->length += recv_length;
        ucs_trace_data("tcp_ep %p: recvd %zu bytes", ep, recv_length);

        if (ep->length - ep->offset < sizeof(*hdr)) {
	    return recv_length > 0;
        }

        hdr = ep->buf + ep->offset;

        if (hdr->length == 0) {
            uct_tcp_ep_complete_recv_am(iface, ep, hdr);
            ep->length = ep->offset = 0;
            ucs_mpool_put_inline(ep->buf);
	    return 1;
        }
    } else {
        hdr = ep->buf + ep->offset;
    }

    ucs_assert(hdr->length <= (iface->config.buf_size - sizeof(uct_tcp_am_hdr_t)));

    cur_recvd_length = ep->length - ep->offset - sizeof(*hdr);
    recv_length = hdr->length - cur_recvd_length;

    ucs_assertv(recv_length > 0, "ep=%p", ep);

    status = uct_tcp_recv(ep->fd, ep->buf + ep->length, &recv_length);
    if (status != UCS_OK) {
        if (status == UCS_ERR_CANCELED) {
            ucs_debug("tcp_ep %p: remote disconnected", ep);
            uct_tcp_ep_mod_events(ep, 0, EPOLLIN);
            /* No need to put ep::buf to mpool, it will be done in
	     * destroy function, because `ep::length != 0` */
            uct_tcp_ep_destroy(&ep->super.super);
        }
        return 0;
    }

    ucs_trace_data("tcp_ep %p: recvd %zu bytes", ep, recv_length);

    if (recv_length == (hdr->length - cur_recvd_length)) {
        uct_tcp_ep_complete_recv_am(iface, ep, hdr);
        ep->length = ep->offset = 0;
        ucs_mpool_put_inline(ep->buf);
    } else {
        ep->length += recv_length;
    }

    return recv_length > 0;
}

unsigned uct_tcp_ep_progress_rx(uct_tcp_ep_t *ep)
{
    ucs_trace_func("ep=%p", ep);

    if (ep->length == 0) {
        /* Receive next chunk of data */
        return uct_tcp_ep_do_next_rx(ep);
    } else {
        /* Receive remaining part of AM data */
        return uct_tcp_ep_do_partial_rx(ep);
    }
}

ssize_t uct_tcp_ep_am_bcopy(uct_ep_h uct_ep, uint8_t am_id,
                            uct_pack_callback_t pack_cb, void *arg,
                            unsigned flags)
{
    uct_tcp_ep_t *ep = ucs_derived_of(uct_ep, uct_tcp_ep_t);
    uct_tcp_iface_t *iface = ucs_derived_of(uct_ep->iface, uct_tcp_iface_t);
    uct_tcp_am_hdr_t *hdr;
    size_t packed_length;

    if (!uct_tcp_ep_can_send(ep)) {
        return UCS_ERR_NO_RESOURCE;
    }

    ep->buf = ucs_mpool_get_inline(&iface->tx_buf_mp);
    if (ucs_unlikely(!ep->buf)) {
        return UCS_ERR_NO_RESOURCE;
    }

    hdr         = ep->buf;
    hdr->am_id  = am_id;
    hdr->length = packed_length = pack_cb(hdr + 1, arg);
    ep->length  = sizeof(*hdr) + packed_length;

    UCT_CHECK_LENGTH(hdr->length, 0,
                     iface->config.buf_size - sizeof(uct_tcp_am_hdr_t),
                     "am_bcopy");
    UCT_TL_EP_STAT_OP(&ep->super, AM, BCOPY, hdr->length);
    uct_iface_trace_am(&iface->super, UCT_AM_TRACE_TYPE_SEND, hdr->am_id,
                       hdr + 1, hdr->length, "SEND fd %d", ep->fd);
    iface->outstanding += ep->length;

    uct_tcp_ep_send(ep);
    if (ep->length > 0) {
        uct_tcp_ep_mod_events(ep, EPOLLOUT, 0);
    }
    return packed_length;
}

ucs_status_t uct_tcp_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req,
                                    unsigned flags)
{
    uct_tcp_ep_t *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);

    if (uct_tcp_ep_can_send(ep)) {
        return UCS_ERR_BUSY;
    }

    uct_pending_req_queue_push(&ep->pending_q, req);
    UCT_TL_EP_STAT_PEND(&ep->super);
    return UCS_OK;
}

void uct_tcp_ep_pending_purge(uct_ep_h tl_ep, uct_pending_purge_callback_t cb,
                             void *arg)
{
    uct_tcp_ep_t                 *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);
    uct_pending_req_priv_queue_t *priv;

    uct_pending_queue_purge(priv, &ep->pending_q, 1, cb, arg);
}

ucs_status_t uct_tcp_ep_flush(uct_ep_h tl_ep, unsigned flags,
                              uct_completion_t *comp)
{
    uct_tcp_ep_t *ep = ucs_derived_of(tl_ep, uct_tcp_ep_t);

    if (!uct_tcp_ep_can_send(ep)) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_TL_EP_STAT_FLUSH(&ep->super);
    return UCS_OK;
}

