#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <time.h>

// set non-blocking socket
static int set_socket_non_blocking(int fd) {
    int flags, result;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl");
        return -1;
    }
    result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (result == -1) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

static void remove_newline(const char *buf) {
    char *newline = strrchr(buf, '\n');
    if (newline)
        *newline = '\0';
    newline = strrchr(buf, '\r');
    if (newline)
        *newline = '\r';
}

static int create_and_bind(const char *host, char *port) {
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;

    memset (&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
    hints.ai_flags = AI_PASSIVE;     /* All interfaces */

    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1)
            continue;

        s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
        if (s == 0) {
            /* We managed to bind successfully! */
            break;
        }

        close(sfd);
    }

    if (rp == NULL) {
        /* fprintf(stderr, "Could not bind\n"); */
        perror("Could not bind");
        return -1;
    }

    freeaddrinfo(result);

    return sfd;
}

// start server instance, by setting hostname
void start_server(const char *host) {
    h_map *hashmap = m_create();
    int efd, sfd, s;
    struct epoll_event event, *events;

    sfd = create_and_bind(host, PORT);
    if (sfd == -1)
        abort();

    s = set_socket_non_blocking(sfd);
    if (s == -1)
        abort();

    s = listen(sfd, SOMAXCONN);
    if (s == -1) {
        perror("listen");
        abort();
    }

    efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create");
        abort();
    }

    event.data.fd = sfd;
    event.events = EPOLLIN | EPOLLET;
    s = epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &event);
    if (s == -1) {
        perror("epoll_ctl");
        abort();
    }

    /* Buffer where events are returned */
    events = calloc(MAX_EVENTS, sizeof event);

    if (host == NULL)
        printf("Server listening on 127.0.0.1:%s\n", PORT);
    else printf("Server listening on %s:%s\n", host, PORT);
    /* The event loop */
    while (1) {
        int n, i;

        n = epoll_wait(efd, events, MAX_EVENTS, -1);
        for (i = 0; i < n; i++) {
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                /* An error has occured on this fd, or the socket is not
                   ready for reading (why were we notified then?) */
                /* fprintf(stderr, "epoll error\n"); */
                perror("epoll error");
                close(events[i].data.fd);
                continue;
            }

            else if (sfd == events[i].data.fd) {
                /* We have a notification on the listening socket, which
                   means one or more incoming connections. */
                while (1) {
                    struct sockaddr in_addr;
                    socklen_t in_len;
                    int infd;
                    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                    in_len = sizeof in_addr;
                    infd = accept(sfd, &in_addr, &in_len);
                    if (infd == -1) {
                        if ((errno == EAGAIN) ||
                            (errno == EWOULDBLOCK)) {
                            /* We have processed all incoming
                               connections. */
                            break;
                        }
                        else {
                            perror("accept");
                            break;
                        }
                    }

                    s = getnameinfo(&in_addr, in_len,
                                    hbuf, sizeof hbuf,
                                    sbuf, sizeof sbuf,
                                    NI_NUMERICHOST | NI_NUMERICSERV);
                    if (s == 0) {
                        char time_buff[100];
                        time_t now = time(0);
                        strftime(time_buff, 100, "[%Y-%m-%d %H:%M:%S]", localtime(&now));
                        printf("%s - new connection from %s:%s on descriptor %d \n", time_buff,  hbuf, sbuf, infd);
                    }

                    /* Make the incoming socket non-blocking and add it to the
                       list of fds to monitor. */
                    s = set_socket_non_blocking(infd);
                    if (s == -1)
                        abort();

                    event.data.fd = infd;
                    event.events = EPOLLIN | EPOLLET;
                    s = epoll_ctl(efd, EPOLL_CTL_ADD, infd, &event);
                    if (s == -1) {
                        perror("epoll_ctl");
                        abort();
                    }
                }
                continue;
            }
            else {
                /* We have data on the fd waiting to be read. Read and
                   display it. We must read whatever data is available
                   completely, as we are running in edge-triggered mode
                   and won't get a notification again for the same
                   data. */
                int done = 0;

                while (1) {
                    ssize_t count;
                    /* char buf[MAX_DATA_SIZE]; */
                    char *buf = malloc(MAX_DATA_SIZE);

                    count = read(events[i].data.fd, buf, MAX_DATA_SIZE - 1);
                    if (count == -1) {
                        /* If errno == EAGAIN, that means we have read all
                           data. So go back to the main loop. */
                        if (errno != EAGAIN) {
                            perror("read");
                            done = 1;
                        }
                        free(buf);
                        break;
                    }
                    else if (count == 0) {
                        /* End of file. The remote has closed the
                           connection. */
                        done = 1;
                        free(buf);
                        break;
                    }

                    printf("Message: %s\n", buf);
                    int proc = process_command(hashmap, buf, events[i].data.fd);
                    switch(proc) {
                    case MAP_OK:
                        send(events[i].data.fd, "OK\n", 3, 0);
                        break;
                    case MAP_MISSING:
                        send(events[i].data.fd, "NOT FOUND\n", 10, 0);
                        break;
                    case MAP_FULL:
                        send(events[i].data.fd, "MAP FULL\n", 9, 0);
                        break;
                    case MAP_OMEM:
                        send(events[i].data.fd, "MAP FULL\n", 9, 0);
                        break;
                    default:
                        /* send(events[1].data.fd, "\n", 1, 0); */
                        break;
                    }
                    /* if (proc == 0) */
                    /*     send(events[i].data.fd, "OK\n", 3, 0); */
                    /* else if (proc != 1) send(events[i].data.fd, "ERROR\n", 6, 0); */
                    free(buf);
                }

                if (done) {
                    printf("Closed connection on descriptor %d\n",
                           events[i].data.fd);
                    m_release(hashmap);
                    /* Closing the descriptor will make epoll remove it
                       from the set of descriptors which are monitored. */
                    close(events[i].data.fd);
                }
            }
        }
    }

    free(events);
    close(sfd);

    return;
}

/*
 * trim string, removing leading and trailing spaces
 */

static void trim(char *str) {
    int i;
    int begin = 0;
    int end = strlen(str) - 1;

    while (isspace(str[begin]))
        begin++;

    while ((end >= begin) && isspace(str[end]))
        end--;

    // Shift all characters back to the start of the string array.
    for (i = begin; i <= end; i++)
        str[i - begin] = str[i];

    str[i - begin] = '\0'; // Null terminate string.
}

static int is_number(char *s) {
    while(*s) {
        if (isdigit(*s++) == 0) return 0;
    }
    return 1;
}

static int to_int(char *p) {
    int len = 0;
    while(isdigit(*p)) {
        len = (len * 10) + (*p - '0');
        p++;
    }
    return len;
}

int process_command(h_map *hashmap, char *buffer, int sock_fd) {
    char *command;
    char *key;
    void *value;
    int ret = 0;
    command = strtok(buffer, " \r\n");
    // print nothing on an empty command
    if (!command)
        return 1;
    if (strcasecmp(command, "SET") == 0) {
        key = strtok(NULL, " ");
        value = key + strlen(key) + 1;
        remove_newline(value);
        char *key_holder = malloc(strlen(key));
        char *value_holder = malloc(strlen((char *) value));
        strcpy(key_holder, key);
        strcpy(value_holder, value);
        ret = m_put(hashmap, key_holder, value_holder);
    } else if (strcasecmp(command, "GET") == 0) {
        key = strtok(NULL, " ");
        remove_newline(key);
        trim(key);
        int get = m_get(hashmap, key, &value);
        if (get == MAP_OK && value) {
            send(sock_fd, value, strlen((char *) value), 0);
            return 1;
        } else ret = MAP_MISSING;
    } else if (strcasecmp(command, "DEL") == 0) {
        key = strtok(NULL, " ");
        remove_newline(key);
        trim(key);
        ret = m_remove(hashmap, key);
    } else if (strcasecmp(command, "SUB") == 0) {
        key = strtok(NULL, " ");
        remove_newline(key);
        trim(key);
        ret = m_sub(hashmap, key, sock_fd);
    } else if (strcasecmp(command, "PUB") == 0) {
        key = strtok(NULL, " ");
        value = key + strlen(key) + 1;
        remove_newline(value);
        char *key_holder = malloc(strlen(key));
        char *value_holder = malloc(strlen((char *) value));
        strcpy(key_holder, key);
        strcpy(value_holder, value);
        ret = m_pub(hashmap, key_holder, value_holder);
    } else if (strcasecmp(command, "INC") == 0) {
        key = strtok(NULL, " ");
        remove_newline(key);
        trim(key);
        ret = m_get(hashmap, key, &value);
        if (ret == MAP_OK && value) {
            char *s = (char *) value;
            if(is_number(s) == 1) {
                int v = to_int(s);
                v++;
                sprintf(value, "%d", v);
                ret = m_put(hashmap, key, value);
            }
            else return -1;
        } else return ret;
    } else if (strcasecmp(command, "DEC") == 0) {
        key = strtok(NULL, " ");
        remove_newline(key);
        trim(key);
        ret = m_get(hashmap, key, &value);
        if (ret == MAP_OK && value) {
            char *s = (char *) value;
            if(is_number(s) == 1) {
                int v = to_int(s);
                v--;
                sprintf(value, "%d", v);
                ret = m_put(hashmap, key, value);
            }
            else return -1;
        } else return ret;
    } else if (strcasecmp(command, "COUNT") == 0) {
        int len = m_length(hashmap);
        char c_len[5];
        sprintf(c_len, "%d", len);
        send(sock_fd, c_len, 5, 0);
        return 1;
    } else if (strcasecmp(command, "QUIT") == 0 || strcasecmp(command, "EXIT") == 0) {
        printf("Connection closed on descriptor %d\n", sock_fd);
        close(sock_fd);
        return 1;
    } else ret = 1;
    return ret;
}
