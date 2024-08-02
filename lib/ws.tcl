# ./folk play/ws-play.tcl

set cc [C]
$cc include <errno.h>
$cc include <sys/socket.h>

$cc include <wslay/wslay.h>

$cc code {
    typedef struct WsSession {
        int fd;
        Jim_Obj* onMsgRecv;
    } WsSession;

    static ssize_t recv_callback(wslay_event_context_ptr ctx,
                                 uint8_t* buf, size_t len, int flags,
                                 void* user_data) {
        WsSession* session = (WsSession*) user_data;

        ssize_t r;
        while((r = recv(session->fd, buf, len, 0)) == -1 && errno == EINTR);
        if(r == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            } else {
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            }
        } else if(r == 0) {
            /* Unexpected EOF is also treated as an error */
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            r = -1;
        }
        return r;
    }
    static ssize_t send_callback(wslay_event_context_ptr ctx,
                                 const uint8_t* data, size_t len, int flags,
                                 void* user_data) {
        WsSession* session = (WsSession*) user_data;

        ssize_t r;
        int sflags = 0;
#ifdef MSG_MORE
        if(flags & WSLAY_MSG_MORE) {
            sflags |= MSG_MORE;
        }
#endif // MSG_MORE
        while((r = send(session->fd, data, len, sflags)) == -1 && errno == EINTR);
        if(r == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            } else {
                wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
            }
        }
        return r;
    }
    static void on_msg_recv_callback(wslay_event_context_ptr ctx,
                                     const struct wslay_event_on_msg_recv_arg* arg,
                                     void* user_data) {
        if (!wslay_is_ctrl_frame(arg->opcode)) {
            fprintf(stderr, "Got message: (%s)\n", arg->msg);
        }
    }

    struct wslay_event_callbacks callbacks = {
        .recv_callback = recv_callback,
        .send_callback = send_callback,
        .genmask_callback = NULL,
        .on_frame_recv_start_callback = NULL,
        .on_frame_recv_chunk_callback = NULL,
        .on_frame_recv_end_callback = NULL,
        .on_msg_recv_callback = on_msg_recv_callback
    };
}
$cc typedef {struct wslay_event_context*} wslay_event_context_ptr
$cc proc wsInit {Jim_Obj* chan Jim_Obj* onMsgRecv} wslay_event_context_ptr {
    // Convert chan to an int fd:
    FILE* fp = Jim_AioFilehandle(interp, chan);
    if (fp == NULL) {
        FOLK_ERROR("web: Unable to open channel as file\n");
    }
    int fd = fileno(fp);

    WsSession* session = (WsSession*) malloc(sizeof(WsSession));
    session->fd = fd;
    session->onMsgRecv = onMsgRecv;
    Jim_IncrRefCount(onMsgRecv);

    wslay_event_context_ptr ctx;
    wslay_event_context_server_init(&ctx, &callbacks, (void*) session);
    fprintf(stderr, "wsInit\n");
    return ctx;
}
$cc proc wsReadable {wslay_event_context_ptr ctx} void {
    wslay_event_recv(ctx);
}
$cc proc wsWritable {wslay_event_context_ptr ctx} void {
    wslay_event_send(ctx);
}
$cc proc wsDestroy {wslay_event_context_ptr ctx} void {
    wslay_event_context_free(ctx);
}
$cc endcflags -lwslay
set wsLib [$cc compile]

proc ::websocketUpgrade {chan acceptKey} {wsLib} {
    puts -nonewline $chan "HTTP/1.1 101 Switching Protocols\r
Upgrade: websocket\r
Connection: Upgrade\r
Sec-WebSocket-Accept: $acceptKey\r
\r
"
    set ctx [$wsLib wsInit $chan {
        puts stderr "ws.tcl: onMsgRecv"
    }]
    $chan readable [list $wsLib wsReadable $ctx]
    $chan writable [list $wsLib wsWritable $ctx]
}