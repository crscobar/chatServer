#include <fnmatch.h>
#include "csapp.h"

const char OK[] = "HTTP/1.1 200 OK\r\n\r\n";

const char NOTOK[] = "HTTP/1.1 400 Bad Request\r\n\r\n";

const char stream[] =
    "HTTP/1.1 200 OK\r\n"
    "Cache-Control: no-transform\r\n"
    "Content-Type: text/event-stream\r\n"
    "Connection: keep-alive\r\n\r\n"
    "retry: 10000\n\n";

char indexhtml[8192];

typedef struct {
  char requestbuf[8192];
  int first_empty_byte;
} recv_buffer;

typedef struct {
  /* represents a pool of connected descriptors */
  int maxfd;                /* largest descriptor in read_set */
  fd_set read_set;          /* set of all active descriptors */
  fd_set ready_set;         /* subset of descriptors ready for reading  */
  int nready;               /* number of ready descriptors from select */
  int maxi;                 /* highwater index into client array */
  int clientfd[FD_SETSIZE]; /* set of active descriptors */
  recv_buffer protos[FD_SETSIZE];
  int receiving_events[FD_SETSIZE];  // flag: set 1 if client is listening for
                                     // chat messages, 0 otherwise
} pool;

void init_pool(int listenfd, pool *p);
void add_client(int connfd, pool *p);
void check_clients(pool *p);

void extract_message(char *src, char *dst) {
  // given an HTTP POST message containing a message, format it and
  // write it into dst. both buffers are 8192 bytes max.
  // move to beginning of message
  char *end_of_uri;
  char *plus;
  src += strlen("POST /speak?");
  strncpy(dst, "data: ", MAXLINE);
  end_of_uri = strchr(src, ' ');
  strncpy(&dst[6], src, end_of_uri - src);
  strncat(dst, "\n\n", 3);
  // replace '+' with ' '
  for (; (plus = strchr(dst, '+')) != NULL; *plus = ' ')
    ;
}

// when you know you're done with a connection, close the socket,
// remove it from the set you're interested in reading from, and
// indicate that this offset in the pool is available.
void close_and_remove(pool *p, int offset) {
  Close(p->clientfd[offset]);
  FD_CLR(p->clientfd[offset], &p->read_set);
  p->clientfd[offset] = -1;
}

// courtesy Richard Stevens Unix Network Programming book.
ssize_t /* Write "n" bytes to a descriptor. */
writen(int fd, const void *vptr, size_t n) {
  size_t nleft;
  ssize_t nwritten;
  const char *ptr;
  ptr = vptr;
  nleft = n;
  while (nleft > 0) {
    if ((nwritten = write(fd, ptr, nleft)) <= 0) {
      if (nwritten < 0 && errno == EINTR)
        nwritten = 0; /* and call write() again */
      else
        return (-1); /* error */
    }
    nleft -= nwritten;
    ptr += nwritten;
  }
  return (n);
}

// this is very hacky, but it is good enough for our purposes.
typedef enum request_states {
  incomplete,
  root,
  speak,
  sse_listen,
  notSupported
} request_type;

request_type parse_request(recv_buffer *ps) {
  printf("%s\n",ps->requestbuf);
 
  // HTTP header + \r\n\r\n + data
  // GET /speak HTTP/1.1 \r\n fake: GET / \r\n\r\n
  // sse_listen should be used when you've received a complete request, and you know it's for the /listen endpoint.
  
  // ps->requesetbuf is an array and itself is a char* pointer
  // strstr -> returns a "pointer" to the first instance of the matched substring
  if (!strstr(ps->requestbuf, "\r\n\r\n")){
    fprintf(stderr, "INCOMPLETE\n");
    return incomplete;
  }

  else if (strstr(ps->requestbuf, "GET /listen HTTP/1.") == ps->requestbuf){
    fprintf(stderr, "LISTEN\n");
    return sse_listen;
  }

  // root should be used when you've received a complete request, and you know it's for index.html.
  // else if (strstr(ps->requestbuf, "GET / HTTP/1.") == ps->requestbuf){
  else if (fnmatch("GET * HTTP/1.*", ps->requestbuf, 0) == 0){
    fprintf(stderr, "ROOT\n");
    return root;
  }

  // speak should be used when you've received a complete request, and you know it's a POST message for the resource /speak.
  else if (strstr(ps->requestbuf, "POST /speak") == ps->requestbuf){
    fprintf(stderr, "SPEAK\n");
    return speak;
  }

  return notSupported;

}


int main(int argc, char **argv) {
  int listenfd, connfd, port, filefd;
  socklen_t clientlen = sizeof(struct sockaddr_in);
  struct sockaddr_in clientaddr;
  static pool pool;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(0);
  }
  //argv[1] = "5000";
  port = atoi(argv[1]);
  // read index into memory once to send to everyone
  filefd = open("index.html", O_RDONLY);
  read(filefd, indexhtml, 8192);
  close(filefd);
  // Open_listenfd calls socket(), bind(), and listen()
  listenfd = Open_listenfd(port);
  init_pool(listenfd, &pool);
  fprintf(stderr, "listening on http://localhost:%s\n", argv[1]);

  while (1) {
    /* Wait for listening/connected descriptor(s) to become ready */
    pool.ready_set = pool.read_set;
    pool.nready = Select(pool.maxfd + 1, &pool.ready_set, NULL, NULL, NULL);

    /* If listening descriptor ready, add new client to pool */
    if (FD_ISSET(listenfd, &pool.ready_set)) {
      connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
      add_client(connfd, &pool);
    }
    // perform the relevant protocol steps for each FD that has bytes available
    check_clients(&pool);
  }
}

void init_pool(int listenfd, pool *p) {
  /* Initially, there are no connected descriptors */
  int i;
  p->maxi = -1;
  for (i = 0; i < FD_SETSIZE; i++) p->clientfd[i] = -1;

  /* Initially, listenfd is only member of select read set */
  p->maxfd = listenfd;
  FD_ZERO(&p->read_set);
  FD_SET(listenfd, &p->read_set);
}

void add_client(int connfd, pool *p) {
  int i;
  p->nready--;
  for (i = 0; i < FD_SETSIZE; i++) /* Find an available slot */
    if (p->clientfd[i] < 0) {
      /* Add connected descriptor to the pool */
      p->clientfd[i] = connfd;
      /* Add the descriptor to descriptor set */
      FD_SET(connfd, &p->read_set);
      // wipe buffer and indicate it is empty
      memset(p->protos[i].requestbuf, 0, 8192);
      p->protos[i].first_empty_byte = 0;
      // set state for stateful connections
      p->receiving_events[i] = 0;

      /* Update max descriptor and pool highwater mark */
      if (connfd > p->maxfd) p->maxfd = connfd;
      if (i > p->maxi) p->maxi = i;
      break;
    }
  if (i == FD_SETSIZE) /* Couldn't find an empty slot */
    app_error("add_client error: Too many clients");
}

void check_clients(pool *p) {
  int i, j, connfd, n, offset;
  char *end_of_uri;
  char *uri_ptr;
  char buf[MAXLINE];
  recv_buffer *ps;

  for (i = 0; (i <= p->maxi) && (p->nready > 0); i++) {
    connfd = p->clientfd[i];
    ps = &p->protos[i];
    offset = p->protos[i].first_empty_byte;

    if ((connfd > 0) && (FD_ISSET(connfd, &p->ready_set))) {
      p->nready--;
      if ((n = read(connfd, &ps->requestbuf[offset], 8192 - offset)) != 0) {
        p->protos[i].first_empty_byte += n;
        fprintf(stderr, "Server received %d bytes on fd %d\n", n, connfd);

        request_type message;
        message = parse_request(ps);
        char *getCMD;
        
        if (message == incomplete){
          continue;
        }
        else if(message == root){
          // send ok, send index.html, close_and_remove()
          getCMD = strtok(ps->requestbuf, " ");
          getCMD = strtok(NULL, " ");
          fprintf(stderr, "TOKEN: %s\n", getCMD);

          if(access(getCMD, F_OK) != 0){
            writen(connfd, NOTOK, strlen(NOTOK));
            close_and_remove(p, i);
            break;
          }

          writen(connfd, OK, strlen(OK));
          // writen to server index.html
          writen(connfd, indexhtml, strlen(indexhtml));
          // close and remove
          close_and_remove(p, i);
        }
        else if(message == speak){
          // send OK, extract message into buf, close_and_remove, and send message to every client
          extract_message(ps->requestbuf, buf);
          writen(connfd, OK, strlen(OK));
          close_and_remove(p, i);

          for(int z = 0; z < p->maxi; z++){
            if(p->receiving_events[z] == 1){
              writen(p->clientfd[z], buf, strlen(buf));
            }
          }
        }
        else if(message == sse_listen){
          // listen flips p->recieveing_events[i] to 1 and send stream
          p->receiving_events[i] = 1;
          //send messagae to client
          writen(connfd, stream, strlen(stream));
        }
        else{
          writen(connfd, NOTOK, strlen(NOTOK));
          close_and_remove(p, i);
        }
      
      }
      /* EOF detected, remove descriptor from pool */
      else {
        close_and_remove(p, i);
      }
    }
  }
}
