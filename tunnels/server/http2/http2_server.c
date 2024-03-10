#include "http2_server.h"
#include "nghttp2/nghttp2.h"
#include "buffer_stream.h"
#include "hv/httpdef.h"
#include "http2_def.h"
#include "grpc_def.h"
#include "loggers/network_logger.h"
#include "helpers.h"

#define STATE(x) ((http2_server_state_t *)((x)->state))
#define CSTATE(x) ((http2_server_con_state_t *)((((x)->line->chains_state)[self->chain_index])))
#define CSTATE_MUT(x) ((x)->line->chains_state)[self->chain_index]
#define ISALIVE(x) (CSTATE(x) != NULL)

typedef struct http2_server_state_s
{
    nghttp2_session_callbacks *cbs;
    tunnel_t *fallback;

} http2_server_state_t;

typedef struct http2_server_con_state_s
{
    bool init_sent;
    bool first_sent;
    bool is_first;
    buffer_stream_t *response_stream;

    nghttp2_session *session;
    http2_session_state state;
    int error;
    int stream_id;
    int stream_closed;
    int frame_type_when_stream_closed;
    enum http_content_type content_type;
    // http2_frame_hd + grpc_message_hd
    // at least HTTP2_FRAME_HDLEN + GRPC_MESSAGE_HDLEN = 9 + 5 = 14
    unsigned char frame_hdbuf[32];

    //since nghtt2 uses callbacks
    tunnel_t *_self;
    line_t* line;

} http2_server_con_state_t;

static int on_header_callback(nghttp2_session *session,
                              const nghttp2_frame *frame,
                              const uint8_t *_name, size_t namelen,
                              const uint8_t *_value, size_t valuelen,
                              uint8_t flags, void *userdata)
{
    if(userdata == NULL) return;

    LOGD("on_header_callback\n");
    print_frame_hd(&frame->hd);
    const char *name = (const char *)_name;
    const char *value = (const char *)_value;
    LOGD("%s: %s\n", name, value);

    http2_server_con_state_t *cstate = (http2_server_con_state_t *)userdata;

    if (*name == ':')
    {
        if (strcmp(name, ":method") == 0)
        {
            // req->method = http_method_enum(value);
        }
        else if (strcmp(name, ":path") == 0)
        {
            // req->url = value;
        }
        else if (strcmp(name, ":scheme") == 0)
        {
            // req->headers["Scheme"] = value;
        }
        else if (strcmp(name, ":authority") == 0)
        {
            // req->headers["Host"] = value;
        }
    }
    else
    {
        // hp->parsed->headers[name] = value;
        if (strcmp(name, "content-type") == 0)
        {
            cstate->content_type = http_content_type_enum(value);
        }
    }

    return 0;
}

static int on_data_chunk_recv_callback(nghttp2_session *session,
                                       uint8_t flags, int32_t stream_id, const uint8_t *data,
                                       size_t len, void *userdata)
{
    if(userdata == NULL) return;
    http2_server_con_state_t *cstate = (http2_server_con_state_t *)userdata;
    tunnel_t *self = cstate->self;
    LOGD("on_data_chunk_recv_callback\n");
    LOGD("stream_id=%d length=%d\n", stream_id, (int)len);
    // LOGD("%.*s\n", (int)len, data);

    if (cstate->content_type == APPLICATION_GRPC)
    {
        // grpc_message_hd
        if (len >= GRPC_MESSAGE_HDLEN)
        {
            grpc_message_hd msghd;
            grpc_message_hd_unpack(&msghd, data);
            LOGD("grpc_message_hd: flags=%d length=%d\n", msghd.flags, msghd.length);
            data += GRPC_MESSAGE_HDLEN;
            len -= GRPC_MESSAGE_HDLEN;
            // LOGD("%.*s\n", (int)len, data);
        }
    }

    shift_buffer_t *buf = popBuffer(buffer_pools[cstate->line->tid]);
    shiftl(buf, lCap(buf) / 1.25); // use some unsued space
    reserve(buf, len);
    memcpy(rawBuf(buf), data, len);
    free(*data);
    context_t *up_data = newContext(cstate->line);
    up_data->payload = buf;
    self->dw->downStream(self->dw, answer);
    // if (hp->parsed->http_cb)
    // {
    //     hp->parsed->http_cb(hp->parsed, HP_BODY, (const char *)data, len);
    // }
    // else
    // {
    //     hp->parsed->body.append((const char *)data, len);
    // }
    return 0;
}

static int on_frame_recv_callback(nghttp2_session *session,
                                  const nghttp2_frame *frame, void *userdata)
{
    if(userdata == NULL) return;

    LOGD("on_frame_recv_callback\n");
    print_frame_hd(&frame->hd);
    http2_server_con_state_t *cstate = (http2_server_con_state_t *)userdata;
    switch (frame->hd.type)
    {
    case NGHTTP2_DATA:
        cstate->state = H2_RECV_DATA;
        break;
    case NGHTTP2_HEADERS:
        cstate->state = H2_RECV_HEADERS;
        break;
    case NGHTTP2_SETTINGS:
        cstate->state = H2_RECV_SETTINGS;
        break;
    case NGHTTP2_PING:
        cstate->state = H2_RECV_PING;
        break;
    case NGHTTP2_RST_STREAM:
    case NGHTTP2_WINDOW_UPDATE:
        // ignore
        return 0;
    default:
        break;
    }
    // if (cstate->state == H2_RECV_HEADERS && cstate->parsed->http_cb)
    // {
    //     cstate->parsed->http_cb(cstate->parsed, HP_HEADERS_COMPLETE, NULL, 0);
    // }
    if (frame->hd.stream_id >= cstate->stream_id)
    {
        cstate->stream_id = frame->hd.stream_id;
        cstate->stream_closed = 0;
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
        {
            LOGD("on_stream_closed stream_id=%d\n", cstate->stream_id);
            cstate->stream_closed = 1;
            cstate->frame_type_when_stream_closed = frame->hd.type;
            if (cstate->state == H2_RECV_HEADERS || cstate->state == H2_RECV_DATA)
            {
                // todo upsteram
                //  if (cstate->parsed->http_cb)
                //  {
                //      cstate->parsed->http_cb(cstate->parsed, HP_MESSAGE_COMPLETE, NULL, 0);
                //  }
            }
        }
    }

    return 0;
}

static bool trySendResponse(tunnel_t *self, line_t *line)
{
    // HTTP2_MAGIC,HTTP2_SETTINGS,HTTP2_HEADERS
    http2_server_con_state_t *cstate = ((http2_server_con_state_t *)(((line->chains_state)[self->chain_index])));
    if (cstate == NULL)
        return false;

    char **data;
    size_t *len;
    *len = nghttp2_session_mem_send(cstate->session, (const uint8_t **)data);
    LOGD("nghttp2_session_mem_send %d\n", *len);
    if (*len != 0)
    {
        shift_buffer_t *buf = popBuffer(buffer_pools[line->tid]);
        shiftl(buf, lCap(buf) / 1.25); // use some unsued space
        reserve(buf, *len);
        memcpy(rawBuf(buf), *data, *len);
        free(*data);

        context_t *answer = newContext(line);
        answer->payload = buf;
        self->dw->downStream(self->dw, answer);
        return true;
    }

    // HTTP2_DATA
    if (cstate->state == H2_SEND_HEADERS)
    {
        int content_length = bufferStreamLen(cstate->response_stream);
        // HTTP2 DATA framehd
        cstate->state = H2_SEND_DATA_FRAME_HD;
        http2_frame_hd framehd;
        framehd.length = content_length;
        framehd.type = HTTP2_DATA;
        framehd.flags = HTTP2_FLAG_END_STREAM;
        framehd.stream_id = cstate->stream_id;
        // *data = (char *)cstate->frame_hdbuf;
        // *len = HTTP2_FRAME_HDLEN;
        LOGD("HTTP2 SEND_DATA_FRAME_HD...\n");
        if (cstate->content_type == APPLICATION_GRPC)
        {
            grpc_message_hd msghd;
            msghd.flags = 0;
            msghd.length = content_length;
            LOGD("grpc_message_hd: flags=%d length=%d\n", msghd.flags, msghd.length);

            // grpc server send grpc-status in HTTP2 header frame
            framehd.flags = HTTP2_FLAG_NONE;

            /*
            // @test protobuf
            // message StringMessage {
            //     string str = 1;
            // }
            int protobuf_taglen = 0;
            int tag = PROTOBUF_MAKE_TAG(1, WIRE_TYPE_LENGTH_DELIMITED);
            unsigned char* p = frame_hdbuf + HTTP2_FRAME_HDLEN + GRPC_MESSAGE_HDLEN;
            int bytes = varint_encode(tag, p);
            protobuf_taglen += bytes;
            p += bytes;
            bytes = varint_encode(content_length, p);
            protobuf_taglen += bytes;
            msghd.length += protobuf_taglen;
            framehd.length += protobuf_taglen;
            *len += protobuf_taglen;
            */

            grpc_message_hd_pack(&msghd, cstate->frame_hdbuf + HTTP2_FRAME_HDLEN);
            framehd.length += GRPC_MESSAGE_HDLEN;
            *len += GRPC_MESSAGE_HDLEN;
        }
        http2_frame_hd_pack(&framehd, cstate->frame_hdbuf);
        shift_buffer_t *buf = popBuffer(buffer_pools[line->tid]);
        reserve(buf, HTTP2_FRAME_HDLEN);
        memcpy(rawBuf(buf), cstate->frame_hdbuf, HTTP2_FRAME_HDLEN);
        context_t *answer = newContext(line);
        answer->payload = buf;
        self->dw->downStream(self->dw, answer);
        return true;
    }
    else if (cstate->state == H2_SEND_DATA_FRAME_HD)
    {
        // HTTP2 DATA
        int content_length = bufferStreamLen(cstate->response_stream);

        if (content_length == 0)
        {
            // skip send_data
            goto send_done;
        }
        else
        {
            LOGD("HTTP2 SEND_DATA... content_length=%d\n", content_length);
            cstate->state = H2_SEND_DATA;

            shift_buffer_t *buf = popBuffer(buffer_pools[line->tid]);
            shiftl(buf, lCap(buf) / 1.25); // use some unsued space
            reserve(buf, content_length);
            bufferStreamRead(rawBuf(buf), content_length, cstate->response_stream);
            context_t *answer = newContext(line);
            answer->payload = buf;
            self->dw->downStream(self->dw, answer);
            return true;
        }
    }
    else if (cstate->state == H2_SEND_DATA)
    {
    send_done:
        cstate->state = H2_SEND_DONE;
        if (cstate->content_type == APPLICATION_GRPC)
        {
            if (cstate->stream_closed)
            {
                // grpc HEADERS grpc-status
                LOGD("grpc HEADERS grpc-status: 0\n");
                int flags = NGHTTP2_FLAG_END_STREAM | NGHTTP2_FLAG_END_HEADERS;
                nghttp2_nv nv = make_nv("grpc-status", "0");
                nghttp2_submit_headers(cstate->session, flags, cstate->stream_id, NULL, &nv, 1, NULL);
                *len = nghttp2_session_mem_send(cstate->session, (const uint8_t **)data);
            }
        }
    }

    LOGD("GetSendData %d\n", *len);
    return false;
}

static inline void upStream(tunnel_t *self, context_t *c)
{
    http2_server_state_t *state = STATE(self);
    if (c->payload != NULL)
    {
        http2_server_con_state_t *cstate = CSTATE(c);
    }
    else
    {
        if (c->init)
        {
            CSTATE_MUT(c) = malloc(sizeof(http2_server_con_state_t));
            memset(CSTATE(c), 0, sizeof(http2_server_con_state_t));
            http2_server_con_state_t *cstate = CSTATE(c);
            cstate->response_stream = newBufferStream(buffer_pools[c->line->tid]);
            nghttp2_session_server_new(&cstate->session, state->cbs, cstate);
            cstate->state = H2_WANT_RECV;
            cstate->stream_id = -1;
            cstate->stream_closed = 0;
            cstate->_self = self;

            nghttp2_settings_entry settings[] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
            nghttp2_submit_settings(cstate->session, NGHTTP2_FLAG_NONE, settings, ARRAY_SIZE(settings));

            cstate->state = H2_SEND_SETTINGS;
            destroyContext(c);
        }
        else if (c->fin)
        {
            bool init_sent = CSTATE(c)->init_sent;
            destroyBufferStream( CSTATE(c)->)
            free(CSTATE(c));
            CSTATE_MUT(c) = NULL;
            if (init_sent)
                self->up->upStream(self->up, c);
            else
                destroyContext(c);
        }
    }
}

static inline void downStream(tunnel_t *self, context_t *c)
{
    if (c->fin)
    {
        free(CSTATE(c));
        CSTATE_MUT(c) = NULL;
    }
    self->dw->downStream(self->dw, c);

    return;
}

static void http2ServerUpStream(tunnel_t *self, context_t *c)
{
    upStream(self, c);
}
static void http2ServerPacketUpStream(tunnel_t *self, context_t *c)
{
    LOGF("Http2Server: Http2 protocol dose not run on udp");
    exit(1);
}
static void http2ServerDownStream(tunnel_t *self, context_t *c)
{
    downStream(self, c);
}
static void http2ServerPacketDownStream(tunnel_t *self, context_t *c)
{
    LOGF("Http2Server: Http2 protocol dose not run on udp");
    exit(1);
}

tunnel_t *newHttp2Server(node_instance_context_t *instance_info)
{
    http2_server_state_t *state = malloc(sizeof(http2_server_state_t));
    memset(state, 0, sizeof(http2_server_state_t));
    cJSON *settings = instance_info->node_settings_json;

    nghttp2_session_callbacks_new(&(state->cbs));
    nghttp2_session_callbacks_set_on_header_callback(state->cbs, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(state->cbs, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(state->cbs, on_frame_recv_callback);

    tunnel_t *t = newTunnel();
    t->state = state;
    t->upStream = &http2ServerUpStream;
    t->packetUpStream = &http2ServerPacketUpStream;
    t->downStream = &http2ServerDownStream;
    t->packetDownStream = &http2ServerPacketDownStream;

    atomic_thread_fence(memory_order_release);
    return t;
}

api_result_t apiHttp2Server(tunnel_t *self, char *msg)
{
    LOGE("http2-server API NOT IMPLEMENTED");
    return (api_result_t){0}; // TODO
}

tunnel_t *destroyHttp2Server(tunnel_t *self)
{
    LOGE("http2-server DESTROY NOT IMPLEMENTED"); // TODO
    return NULL;
}

tunnel_metadata_t getMetadataHttp2Server()
{
    return (tunnel_metadata_t){.version = 0001, .flags = 0x0};
}
