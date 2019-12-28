#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>

#define MAX_CLIENTS 20

static struct epoll_event *events;
static unsigned int cli_count = 0;
static int uid = 10;

typedef struct {
    struct sockaddr_in addr;
    int connfd;
    int uid;
    char name[32];
} client_t;

client_t *clients[MAX_CLIENTS];

void error(const char *msg) 
{
    perror(msg);
    exit(1);
}

void strip_newline(char *s) {
    while (*s != '\0') {
        if (*s ==  '\r' || *s == '\n') {
            *s = '\0';
        }
        s++;
    }
}

void addfd(int epfd, int fd, int enable_et) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN;
    if (enable_et) {
        ev.events = EPOLLIN | EPOLLET;
    }
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    fcntl(fd, F_SETFL, O_NONBLOCK);
}

void queue_add(client_t *client) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) {
            clients[i] = client;
            return;
        }
    }
}

void queue_delete(int uid) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            if (clients[i]->uid == uid) {
                clients[i] = NULL;
                return;
            }
        }
    }
}

void send_message(char *str, int uid) {
    int i;
    int len = strlen(str);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            if (clients[i]->uid != uid) {
                send(clients[i]->connfd, str, len, 0); 
            }
        }
    }
}

void send_message_all(char *str) {
    int i;
    int len = strlen(str);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            send(clients[i]->connfd, str, len, 0);
        }
    }
}

void *handle_client(void *arg) {
    char buffer_out[128];
    char buffer_in[128];
    int rlen;

    cli_count++;
    client_t *client = (client_t *)arg;

    printf("ACCEPT");
    printf(" REFERENCED BY %d\n", client->uid);

    sprintf(buffer_out, "HELLO %s\n", client->name);
    send_message_all(buffer_out);

    while ((rlen = recv(client->connfd, buffer_in, sizeof(buffer_in), 0)) > 0) {
        buffer_in[rlen] = '\0';
        buffer_out[0] = '\0';
        strip_newline(buffer_in); 

        if (!strlen(buffer_in)) {
            continue;
        }

        sprintf(buffer_out, "[%s] %s \r\n", client->name, buffer_in);
        send_message(buffer_out, client->uid);
    }

    close(client->connfd);
    sprintf(buffer_out, "%s LEAVE\r\n", client->name);
    send_message_all(buffer_out);
    
    queue_delete(client->uid);
    printf("LEAVE ");
    printf(" REFERENCED BY %d\n", client->uid);
    free(client);
    cli_count--;
   // pthread_detach(pthread_self());
    
    return NULL;
}

int main(int argc, char *argv[]) 
{
    int serverfd, clientfd, epfd, eventfd, epoll_events_count;
    int i; 
    socklen_t clilen;
    char buffer[256];
    struct sockaddr_in cli_addr;
    struct addrinfo hints, *res;
    struct epoll_event ev; 
    pthread_t tid;

    if (argc < 2) {
        fprintf(stderr, "ERROR, no port provided\n");
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; 
    hints.ai_socktype = SOCK_STREAM; //use tcp
    hints.ai_flags = AI_PASSIVE;  //use my ip address

    getaddrinfo(NULL, argv[1], &hints, &res); 

    serverfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (serverfd < 0) {
        error("ERROR on opening socket");
    }
   
    if (fcntl(serverfd, F_SETFL, O_NONBLOCK) < 0) {
        error("ERROR on set non-blocking");
    }
         
    if (bind(serverfd, res->ai_addr, res->ai_addrlen) < 0) {
        error("ERROR on binding");
    }

    if (listen(serverfd, 5) < 0) {
        error("ERROR on listening");
    }

    freeaddrinfo(res);

    events = calloc(MAX_CLIENTS, sizeof(struct epoll_event));

    if ((epfd = epoll_create(MAX_CLIENTS)) == -1) {
        error("ERROR on epoll create");
    }

    ev.events = EPOLLIN;
    ev.data.fd = serverfd;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, serverfd, &ev) < 0) {
        error("ERROR on epollctl");
    }

    printf("SERVER STARTED\n");  

    //start server loop
    while (1) {
        epoll_events_count = epoll_wait(epfd, events, MAX_CLIENTS, -1);
        
        if (epoll_events_count < 0) {
            error("ERROR on epoll wait");
            break;
        }

        for (i = 0; i < epoll_events_count; i++) {
            eventfd = events[i].data.fd;
            //new connection
            if (eventfd == serverfd) {
                clilen = sizeof(cli_addr);
                clientfd = accept(serverfd, (struct sockaddr *)&cli_addr, &clilen);
                addfd(epfd, clientfd, 1);                
                
                client_t *client = (client_t *)malloc(sizeof (client_t));
                client->addr = cli_addr;
                client->connfd = clientfd;
                client->uid = uid++;
                queue_add(client);
                pthread_create(&tid, NULL, &handle_client, (void *)client);
           } 
        }
    }

    close(serverfd);
   
    return 0;
}
