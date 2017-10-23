#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "uv.h"

#include "utils.h"

#define N_BACKLOG 64

#define SENDBUF_SIZE 1024

typedef struct {
  char sendbuf[SENDBUF_SIZE];
  int sendbuf_end;
} peer_state_t;

void on_alloc_buffer(uv_handle_t* handle, size_t suggested_size,
                     uv_buf_t* buf) {
  buf->base = (char*)xmalloc(suggested_size);
  buf->len = suggested_size;
}

void on_client_closed(uv_handle_t* handle) {
  uv_tcp_t* client = (uv_tcp_t*)handle;
  // The client handle owns the peer state storing its address in the data
  // field, so we free it here.
  if (client->data) {
    free(client->data);
  }
  free(client);
}

void on_peer_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
  if (nread < 0) {
    if (nread != UV_EOF) {
      fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
    }
    uv_close((uv_handle_t*)client, on_client_closed);
  } else if (nread == 0) {
    // From the documentation of uv_read_cb: nread might be 0, which does not
    // indicate an error or EOF. This is equivalent to EAGAIN or EWOULDBLOCK
    // under read(2).
  } else {
    // nread > 0
    assert(buf->len >= nread);

    // Parse the number from client request: assume for simplicity the request
    // all arrives at the same time and contains only digits (possibly followed
    // by non-digits like a newline).
    uint64_t number = 0;
    for (int i = 0; i < nread; ++i) {
      char c = buf->base[i];
      if (isdigit(c)) {
        number = number * 10 + (c - '0');
      } else {
        break;
      }
    }

    /*peer_state_t* peerstate = (peer_state_t*)client->data;*/
    printf("Got %zu bytes\n", nread);
    printf("Num %" PRIu64 "\n", number);
  }
  free(buf->base);
}

void on_peer_connected(uv_stream_t* server, int status) {
  if (status < 0) {
    fprintf(stderr, "Peer connection error: %s\n", uv_strerror(status));
    return;
  }

  // client will represent this peer; it's allocated on the heap and only
  // released when the client disconnects. The client holds a pointer to
  // peer_state_t in its data field; this peer state tracks the protocol state
  // with this client throughout interaction.
  uv_tcp_t* client = (uv_tcp_t*)xmalloc(sizeof(*client));
  int rc;
  if ((rc = uv_tcp_init(uv_default_loop(), client)) < 0) {
    die("uv_tcp_init failed: %s", uv_strerror(rc));
  }
  client->data = NULL;

  if (uv_accept(server, (uv_stream_t*)client) == 0) {
    struct sockaddr_storage peername;
    int namelen = sizeof(peername);
    if ((rc = uv_tcp_getpeername(client, (struct sockaddr*)&peername,
                                 &namelen)) < 0) {
      die("uv_tcp_getpeername failed: %s", uv_strerror(rc));
    }
    report_peer_connected((const struct sockaddr_in*)&peername, namelen);

    // Initialize the peer state for a new client.
    peer_state_t* peerstate = (peer_state_t*)xmalloc(sizeof(*peerstate));
    peerstate->sendbuf_end = 0;
    client->data = peerstate;

    // Start reading on the peer socket.
    if ((rc = uv_read_start((uv_stream_t*)client, on_alloc_buffer,
                            on_peer_read)) < 0) {
      die("uv_read_start failed: %s", uv_strerror(rc));
    }
  } else {
    uv_close((uv_handle_t*)client, on_client_closed);
  }
}

int main(int argc, const char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  int portnum = 8070;
  if (argc >= 2) {
    portnum = atoi(argv[1]);
  }
  printf("Serving on port %d\n", portnum);

  int rc;
  uv_tcp_t server;
  if ((rc = uv_tcp_init(uv_default_loop(), &server)) < 0) {
    die("uv_tcp_init failed: %s", uv_strerror(rc));
  }

  struct sockaddr_in addr;
  if ((rc = uv_ip4_addr("0.0.0.0", portnum, &addr)) < 0) {
    die("uv_ip4_addr failed: %s", uv_strerror(rc));
  }

  if ((rc = uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0)) < 0) {
    die("uv_tcp_bind failed: %s", uv_strerror(rc));
  }

  // Listen on the socket for new peers to connect. When a new peer connects,
  // the on_peer_connected callback will be invoked.
  if ((rc = uv_listen((uv_stream_t*)&server, N_BACKLOG, on_peer_connected)) <
      0) {
    die("uv_listen failed: %s", uv_strerror(rc));
  }

  // Run the libuv event loop.
  uv_run(uv_default_loop(), UV_RUN_DEFAULT);

  // If uv_run returned, close the default loop before exiting.
  return uv_loop_close(uv_default_loop());
}
