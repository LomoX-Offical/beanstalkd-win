#include "dat_w32.h"

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "dat.h"


#if defined WIN32_IOCP_MODE

#define WRITE_LOG(...)              \
    do {                            \
        if (verbose) {              \
            printf(__VA_ARGS__);    \
        }                           \
    }while(0);

#define IDLE_BUF_SIZE (1024)

static HANDLE iocph;
static Socket*            listens[100];
static iocp_event_ovlp_t* ovlps[100];
static int listens_size = 0;

static char buf0[512]; /* buffer of zeros */ 

#define SOCKS_SIZE (10000)

int max_id = 1;

int max_seq = 0;

typedef struct sock_list {
    u_int   count;               /* how many are SET? */
    int     id_array[SOCKS_SIZE];   /* an array of rw */
    Socket* sock_array[SOCKS_SIZE];   /* an array of SOCKETs */
} sock_list;


static sock_list socks = {0};

#define GEN_ID  (max_id++)
#define GEN_SEQ (max_seq++)

int
socks_delete(int id) {
    u_int __i;
    for (__i = 0; __i < socks.count; __i++) {
        if (socks.id_array[__i] == (id)) {
            while (__i < socks.count - 1) {
                socks.id_array[__i] = socks.id_array[__i+1];
                socks.sock_array[__i] = socks.sock_array[__i+1];
                __i++;
            } 
            socks.count--;
            return 0;
        }
    }

    return 0;
} 

int 
socks_insert(int id, Socket* sock) {
    u_int __i;
    for (__i = 0; __i < socks.count; __i++) {
        if (socks.id_array[__i] == id) {
            socks.sock_array[__i] = sock;
            return 0;
        }
    }

    if (__i == socks.count) {
        if (socks.count < SOCKS_SIZE) {
            socks.id_array[__i] = id;
            socks.sock_array[__i] = sock;
            socks.count++;
        } else {
            return -1;
        }
    }

    return 0;
}

static Socket*
get_sock(int id) {
    int i = 0;
    for (i = 0; i < socks.count; i++) {
        if (id == socks.id_array[i]) {
            return socks.sock_array[i];
        }
    }

    return NULL;
}


#define GET_SOCK get_sock


#define SOCKS_CLR(_id) do { \
    int __i; \
    for (__i = 0; __i < socks_size; __i++) { \
        if (socks[__i].id == (_id)) { \
            while (__i < socks_size - 1) { \
                socks[__i] = socks[__i+1]; \
                __i++; \
            } \
            socks_size--; \
            break; \
        } \
    } \
} while(0)

/* Allocate disk space.
 * Expects fd's offset to be 0; may also reset fd's offset to 0.
 * Returns 0 on success, and a positive errno otherwise. */
int
rawfalloc(int fd, int len)
{
    int i, w;

    WRITE_LOG("rawfalloc len\n");

    for (i = 0; i < len; i += w) {
        w = write(fd, buf0, sizeof buf0);
        if (w == -1) return errno;
    }

    lseek(fd, 0, 0); /* do not care if this fails */

    return 0;
}


int
sockinit(void)
{
    WRITE_LOG("[sockinit] sockinit\n");

    /* create a single IOCP to be shared by all sockets */
    iocph = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
        NULL,
        0,
        1);
    if (iocph == NULL) {
        return -1;
    }

    WRITE_LOG("[sockinit] done\n");

    return 0;
}

int
sockaccept(int fd, struct sockaddr *addr, int *addrlen) 
{
    Socket* s = NULL;
    int result = 0;
    SOCKADDR *plocalsa = NULL;
    SOCKADDR *premotesa = NULL;
    int locallen = 0;
    int remotelen = 0;
    int acceptfd = 0;
    int i;
    iocp_accept_t*     acpt;
    iocp_event_ovlp_t* ovlp;
    
    for (i = 0; i < listens_size; ++i) {
        if (listens[i]->fd == fd) {
            s = listens[i];
            ovlp = ovlps[i];
            break;
        }
    }

    acpt = (iocp_accept_t*)ovlp->data;

    if (s == NULL) {
        return -1;
    }

    acceptfd = acpt->fd;


    GetAcceptExSockaddrs(
        acpt->buf,
        0,
        ACCEPT_LENGTH,
        ACCEPT_LENGTH,
        &plocalsa, &locallen,
        &premotesa, &remotelen);


    if (addr != NULL) {
        if (remotelen > 0) {
            if (remotelen < *addrlen) {
                *addrlen = remotelen;
            }
            memcpy(addr, premotesa, *addrlen);
            WRITE_LOG("[sockaccept] fd:%d, remote port:%d\n", acceptfd, ntohs(((struct sockaddr_in*)addr)->sin_port));
        } else {
            *addrlen = 0;
            WRITE_LOG("[sockaccept] fd:%d, no remote info..\n", acceptfd);
        }
    }
  

    /* queue another accept */
    if (sockqueueaccept(s, ovlp) == -1) {
        return -1;
    }

    return acceptfd;
}

int
sockqueueaccept(Socket *s, void* ovlp_ptr)
{
    iocp_accept_t* acpt;
    iocp_event_ovlp_t* ovlp = (iocp_event_ovlp_t*)ovlp_ptr;
    int result = 0;

    acpt = (iocp_accept_t*)ovlp->data;

    acpt->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acpt->fd == -1) {
        errno = WSAEINVAL;
        return -1;
    }

    if(!AcceptEx(s->fd, acpt->fd, acpt->buf, 0, 
        ACCEPT_LENGTH, ACCEPT_LENGTH, 0, (LPOVERLAPPED)ovlp))
    {
        result = WSAGetLastError();
        if(result == WSA_IO_PENDING) {
            result = NO_ERROR;
        }else{
            result = -1;
        }
    } else {
        WRITE_LOG("[sockqueueaccept] fd:%d, AcceptEx success!\n", acpt->fd);
    }

    return result;
}

int
sockwant(Socket *s, int rw)
{
    iocp_event_ovlp_t* listen_ovlp = NULL;
    int seq = 0;
    int is_hang = 0;

    WRITE_LOG("[sockwant] fd:%d, want:%c\n", s->fd, (char)rw);
    
    if (rw == 'h') {
        rw = 'r';
        is_hang = 1;
    }

    if (s->bind_id == 0) {
        int id = GEN_ID;
        if (socks_insert(id, s) == -1) {
            WRITE_LOG("[sockwant] socks is full!:fd:%d\n", s->fd);
            return -1;
        }
        if (CreateIoCompletionPort((HANDLE) s->fd, iocph, (ULONG_PTR)id, 0) == NULL) {
            WRITE_LOG("[sockwant] CreateIoCompletionPort Fail!:fd:%d\n", s->fd);
            return -1;
        }

        if (s->type == SOCK_TYPE_LISTEN) {
            listens[listens_size] = s;
            listen_ovlp = new(iocp_event_ovlp_t);
            ovlps[listens_size] = listen_ovlp;
            listens_size++;
        }

        s->bind_id = id;
        WRITE_LOG("[sockwant] bound id!:fd:%d, id:%d\n", s->fd, s->bind_id);
    }

    if (rw == s->added && rw != 0) 
    {
        if (is_hang == 1) {
            s->added = 'h';
        }
        WRITE_LOG("[sockwant] event is already setted:fd:%d\n", s->fd);
        return 0;
    }


    if (s->added == 'r' && s->reading == 1) {
        s->added = 0;
        s->reading = 0;
        WRITE_LOG("[sockwant] cancel read:fd:%d\n", s->fd);
    }
    else if (s->added == 'w' && s->writing == 1) {
        s->added = 0;
        s->writing = 0;
        WRITE_LOG("[sockwant] cancel write:fd:%d\n", s->fd);
    }

    switch (rw) {
    case 'r':
        if (s->read_seq != 0) {
            s->reading = 1;
            s->added = rw;
            break;
        }

        if (s->type == SOCK_TYPE_LISTEN) {

            if (listen_ovlp == NULL){
                return -1;
            }

            seq = GEN_SEQ;

            s->reading = 1;
            s->added = rw;
            s->read_seq = seq;

            listen_ovlp->seq = seq;
            listen_ovlp->event = rw;


            listen_ovlp->data = new(iocp_accept_t);

            return sockqueueaccept(s, listen_ovlp);

        } else {

            WSABUF             wsabuf;
            uint32             bytes, flags;
            int                rc;
            iocp_event_ovlp_t* ovlp = new(iocp_event_ovlp_t);

            seq = GEN_SEQ;
            s->read_seq = seq;

            ovlp->event = rw;
            ovlp->seq = seq;

            wsabuf.buf = NULL;
            wsabuf.len = 0;
            flags = 0;
            bytes = 0;

            rc = WSARecv(s->fd, &wsabuf, 1, &bytes, &flags, (LPWSAOVERLAPPED)ovlp, NULL);
            if (rc == -1 && GetLastError() != ERROR_IO_PENDING) {
                WRITE_LOG("[sockwant] WSARecv fail!:fd:%d, err:%d\n", s->fd, GetLastError());
                return -1;
            }
            if (rc == 0) {
                WRITE_LOG("[sockwant] fd:%d, WSARecv success!, seq:%d\n", s->fd, s->read_seq);
            }

            s->added = rw;
            s->reading = 1;

            if (is_hang == 1) {
                s->added = 'h';
            }

            WRITE_LOG("[sockwant] WSARecv has been called:fd:%d, event:%c, seq:%d\n", s->fd, s->added, seq);
        }
        break;
    case 'w':
        {
            WSABUF             wsabuf;
            uint32             bytes, flags;
            int                rc;
            iocp_event_ovlp_t* ovlp;

            if (s->write_seq != 0) {
                break;
            }

            ovlp = new(iocp_event_ovlp_t);

            seq = GEN_SEQ;
            s->write_seq = seq;

            ovlp->event = rw;
            ovlp->seq = seq;

            wsabuf.buf = NULL;
            wsabuf.len = 0;
            flags = 0;
            bytes = 0;

            rc = WSASend(s->fd, &wsabuf, 1, &bytes, flags, (LPWSAOVERLAPPED)ovlp, NULL);
            if (rc == -1 && GetLastError() != ERROR_IO_PENDING) {
                WRITE_LOG("[sockwant] WSASend fail!:fd:%d, err:%d\n", s->fd, GetLastError());
                return -1;
            }
            if (rc == 0) {
                WRITE_LOG("[sockwant] fd:%d, WSASend success!, seq:%d\n", s->fd, s->write_seq);
            }

            s->added = rw;
            s->writing = 1;
            WRITE_LOG("[sockwant] WSASend has been called:fd:%d, event:%c, seq:%d\n", s->fd, rw, seq);
        }
        break;
    case 0:
        CancelIo((HANDLE)s->fd);
        socks_delete(s->bind_id);
        break;
    }


    WRITE_LOG("[sockwant] done:fd:%d, event:%c, seq:%d\n", s->fd, rw, seq);
    return 0;
}


int
idle_read(Socket *s)
{
    char bucket[IDLE_BUF_SIZE] = {0};
    int r = 0, c = 0;

    WRITE_LOG("[idle_read] begin:fd:%d\n", s->fd);

    do {
        r = net_read(s->fd, bucket, IDLE_BUF_SIZE);
        c++;
    } while (r > 0);

    WRITE_LOG("[idle_read] idle read socket %d in %d times\n", (s)->fd, c);

    return r;
}

int
socknext(Socket **s, int64 timeout)
{
    OVERLAPPED_ENTRY entry;
    iocp_event_ovlp_t* ovlp;
    int ret = 0;
    int rc  = 0;
    int id = 0;
    int err = 0;

    rc = GetQueuedCompletionStatus(iocph,
        &entry.dwNumberOfBytesTransferred,
        &entry.lpCompletionKey,
        &entry.lpOverlapped,
        timeout / 1000000);
    if (!rc && entry.lpOverlapped == NULL) {
        // timeout. Return.
        *s = NULL;
        return 0;
    }

    id = (int)entry.lpCompletionKey;
    err = WSAGetLastError();

    *s = GET_SOCK(id);
    if ((*s) == NULL) {
        free(entry.lpOverlapped);
        WRITE_LOG("[socknext] id is not found in socks:id:%d\n", id);
        return 0;
    }

    ovlp = (iocp_event_ovlp_t*)(entry.lpOverlapped);
    if (ovlp->event == 'w') {
        (*s)->write_seq = 0;
    } else {
        (*s)->read_seq = 0;
    }


    if (err == ERROR_NETNAME_DELETED || err == WSAECONNRESET) /* the socket was closed */
    {
        WRITE_LOG("[socknext] half close:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);
        free(entry.lpOverlapped);

        /*
         * the WSA_OPERATION_ABORTED completion notification
         * for a file descriptor that was closed
         */
        return 'h';
    }
    else if (err == ERROR_OPERATION_ABORTED) /* the operation was canceled */
    {
        if ((*s)->type == SOCK_TYPE_LISTEN) { // listen sock not need free ovlp
            int ret = 0;
            WRITE_LOG("[socknext] accept operation aborted:fd:%d\n", (*s)->fd);


            ret = sockqueueaccept(*s, (void*)ovlp);
            if (ret == -1) {
                WRITE_LOG("[socknext] accept operation created failed:fd:%d\n", (*s)->fd);
                return -1;
            }

            return 0;
        }

        WRITE_LOG("[socknext] normal operation aborted:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);

        if (ovlp->event == 'r') {
            if ((*s)->added == 'h') {
                (*s)->reading = 0;
                (*s)->added   = 0;
                sockwant(*s, 'h');
            } else {
                (*s)->reading = 0;
                (*s)->added   = 0;
                sockwant(*s, 'r');
            }
        } else {
            /* ovlp->event == 'w' */

            WRITE_LOG("[socknext] normal operation aborted, close socket :fd:%d, event:%c\n", (*s)->fd, ovlp->seq);
            free(entry.lpOverlapped);
            return 'h';
        }

        *s = NULL;
        free(entry.lpOverlapped);
        return 0;
    }


    switch (ovlp->event) {
    case 'r':
        if ((*s)->reading == 0) {
            WRITE_LOG("[socknext] read event bury:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);
            free(entry.lpOverlapped);
            return 0;
        }

        if ((*s)->type == SOCK_TYPE_LISTEN) { // listen sock not need free ovlp
            WRITE_LOG("[socknext] accept event accepted:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);
            ret = ovlp->event;
            return ret;
        }

        if ((*s)->added == 'h') {
            idle_read(*s);

            (*s)->reading = 0;
            (*s)->added   = 0;

            WRITE_LOG("[socknext] hang event accepted:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);

            sockwant(*s, 'h');
            *s = NULL;
            free(entry.lpOverlapped);

            return 0;
        }


        (*s)->reading = 0;
        (*s)->added   = 0;

        WRITE_LOG("[socknext] read event accepted:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);
        sockwant(*s, ovlp->event);
        break;
    case 'w':
        if ((*s)->writing == 0) {
            WRITE_LOG("[socknext] write event bury:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);
            free(entry.lpOverlapped);
            return 0;
        }
        (*s)->writing = 0;
        (*s)->added   = 0;
        WRITE_LOG("[socknext] write event accepted:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);
        break;
    case 'h':
        idle_read(*s);
        (*s)->reading = 0;
        (*s)->added   = 0;
        WRITE_LOG("[socknext] hang event accepted:fd:%d, seq:%d\n", (*s)->fd, ovlp->seq);

        sockwant(*s, ovlp->event);
        *s = NULL;
        free(entry.lpOverlapped);
        return 0;
    }

    ret = ovlp->event;
    free(entry.lpOverlapped);
    return ret;
}

#endif