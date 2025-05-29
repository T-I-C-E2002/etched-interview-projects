#include <stdio.h>
#include "format.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <sys/param.h>
#include <sys/vfs.h>
#include "common.h"
#include <sys/epoll.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <vector.h>
#include <sys/param.h>
#include "dictionary.h"

int set_socket_connection(char* port);
int init_epoll(int sockfd);
void handle_connections(int epoll_fd, int sockfd);
void make_socket_nonblocking(int sockfd);
void sigint_handler(int signum, siginfo_t* info, void* context);
void cleanup();
void handle_client(int fd);
void write_to_client(int fd);
void enable_epollout(int epoll_fd, int fd);
void disable_epollout(int epoll_fd, int fd);
void send_response(int fd);
void send_data(int fd);

//asked LLM to help me debug my sigaction logic and epoll iteration logic!
//asked LLM to help me modularize and reevaluate some of the write and read logic I had for EPOLLET.

#define MAX_EVENTS 1024
#define BUFSIZE 1024
#define HEADERSIZE 512

static char* temp_direct;
static int global_sockfd = -1;
static int global_epoll_fd = -1;
static vector* filenames = NULL; //global vector to store the filenames
static dictionary* fd_to_state = NULL;

typedef enum {
    REQUEST_HEADER,
    REQUEST_DATA,
    RESPONSE_HEADER,
    RESPONSE_DATA,
    CLOSE
} requestStateType;

typedef struct _request_state {
    char buffer[BUFSIZE];      //buffer to write errors and PUT to write data
    char header[HEADERSIZE];  //write the header OK\n ERROR\n etc. 
    size_t buffer_offset;    //for buffer offset
    size_t header_offset;   //for header offet
    char* verb;            // PUT LIST GET DELETE
    char* filename;       // the filename given in PUT
    int status;          //to indicate the progress of sending: 1:= STILL PROCESSING!
    size_t file_size;   //to be stored for PUT!
    size_t received_size; //this is the size we've read 
    size_t sent_bytes;            //bytes one has sent so far
    size_t total_response_bytes; //the total response bytes one requires for writing
    FILE* file; 
    requestStateType state;
} requestState;

int main(int argc, char **argv) {
    // good luck!
    if (argc > 2) {
        return -1; //we just have ./server [port]
    }
    signal(SIGPIPE, SIG_IGN); //to deal with SIGPIPE to avoid crushing the server.
    filenames = string_vector_create(); //will store the global list of files
    fd_to_state = int_to_shallow_dictionary_create(); //we'll use this to map from fd to request state to track the state of the fd

    char template[] = "XXXXXX";
    temp_direct = mkdtemp(template);
    print_temp_directory(temp_direct);
    //temp directory created!

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    
    sa.sa_sigaction = sigint_handler;
    sa.sa_flags = SA_SIGINFO;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigint setting failed!");
        return 0;
    }
    //SIGINT handling
    char* port = argv[1]; //this is the port
    printf("Port is %s\n",port);

    int sockfd = set_socket_connection(port);
    printf("Socket file descriptor acquired!\n");
    int epoll_fd = init_epoll(sockfd);
    printf("Epoll fd acquired!\n");

    global_sockfd = sockfd;
    global_epoll_fd = epoll_fd;

    if (epoll_fd == -1) {
        printf("ERROR\n");
        return 0;
    }
    
    handle_connections(epoll_fd, sockfd);
    
    //cleanup();
    //return 1;
}

void sigint_handler(int signum, siginfo_t *info, void* context) {
    printf("\nReceived SIGINT!\n");
    printf("signal number: %d\n", signum);

    cleanup(); //members are to be cleanedup. 
    exit(0);
}

void cleanup() {
    if (temp_direct) { //cleaning up the files in the temporary directory created
        VECTOR_FOR_EACH(filenames, file, {
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/%s", temp_direct, (char*)file);
            filepath[strlen(filepath)] = '\0';
            //printf("Length of the file to remove is %zu\n", strlen(file));
            //printf("Filepath to REMOVE is %s\n", filepath);
            if (remove(filepath) != 0) {
                perror("failed to remove the file");
            }
        });

        if (rmdir(temp_direct) != 0) {
            perror("failed to remove by rmdir!");
        } else {
            temp_direct = NULL;
        }
    }
    
    if (global_epoll_fd != -1) {
        close(global_epoll_fd);
        global_epoll_fd = -1;
    }

    if (global_sockfd != -1) {
        shutdown(global_sockfd, SHUT_RDWR);
        close(global_sockfd); //listening socket closed!
        global_sockfd = -1;
    }

    if (filenames) {
        vector_destroy(filenames);
        filenames = NULL;
    }

    if (fd_to_state) {
        dictionary_destroy(fd_to_state);
        fd_to_state = NULL;
    }
    print_connection_closed();
}

int set_socket_connection(char* port) {

    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status;
    if ((status = getaddrinfo(NULL, port, &hints, &result)) != 0) {
        perror("getaddrinfo()");
        exit(1);
    } 

    int sock_fd;

    if ((sock_fd = socket(result->ai_family, result->ai_socktype | SOCK_NONBLOCK, 0)) == -1){
        perror("socket()");
        freeaddrinfo(result);
        exit(1);
    }

    int optval = 1;
    int retval = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

    if (retval == -1) {
        perror("setsockopt()");
        close(sock_fd);
        exit(1);
    }

    retval = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (retval == -1) {
        perror("setsockopt()");
        close(sock_fd);
        exit(1);
    }


    if (bind(sock_fd, result->ai_addr, result->ai_addrlen) != 0) {
        perror("bind()");
        freeaddrinfo(result);
        close(sock_fd);
        exit(1);
    }

    if (listen(sock_fd, MAX_EVENTS) != 0) {
        perror("listen()");
        freeaddrinfo(result);
        close(sock_fd);
        exit(1);
    }

    freeaddrinfo(result);
    return sock_fd;
}

int init_epoll(int sockfd) { //if 0 not success
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return -1;
    }

    struct epoll_event event;
    event.events = EPOLLIN; //just as in coursebook we'll add server-socket in level-triggered mode
    event.data.fd = sockfd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sockfd, &event) == -1) {
        perror("epoll_ctl");
        close(epoll_fd);
        return -1;  
    }
    return epoll_fd;
}

void make_socket_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        exit(1);
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
        exit(1);
    }
}

void handle_connections(int epoll_fd, int sockfd) {
    printf("Handling connections!\n");
    struct epoll_event ev, events[MAX_EVENTS];
    int client_fd;
    while (true) { // as suggested in the textbook we wait and see if epoll has any events
        int n_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_ready == -1) { //return -1 if errno, 0 if no fd is ready for I/O
            if (errno == EINTR) {
                //cleanup();
                //exit(1);          //per the suggestion by Timmy catching the EINTR in case of SIGINT
                continue;
            }
            perror("epoll_wait() 2" );
            //cleanup();
            //exit(1);
        }
        for (int n = 0; n < n_ready; ++n) {
            if (events[n].data.fd == sockfd) { 
                struct sockaddr_storage local;
                socklen_t addrlen = sizeof(local);
                client_fd = accept(sockfd, (struct sockaddr*) &local, &addrlen);
                if (client_fd == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { //no more connection!
                        printf("No connection!\n");
                        continue;
                        //return;
                    }
                    perror("accept()");
                    break;
                    
                }
                make_socket_nonblocking(client_fd);      //make socket nonblocking
                ev.events = EPOLLIN;                    //no more of EPOLLET
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) { 
                    perror("epoll_ctl: client_fd"); 
                    //error adding the epoll
                    return;
                }
            } else { //client socket
                printf("CONNECTION!\n");
                int client_fd = events[n].data.fd;
                handle_client(client_fd); //NOW HERE WE GO!
                requestState* state = dictionary_get(fd_to_state, &client_fd);
                if (state->state == RESPONSE_HEADER) {
                    printf("The verb is %s\n", state->verb);
                    write_to_client(client_fd);
                } else if (state->state == RESPONSE_DATA) {
                    printf("The verb {RESPONSE_DATA} is %s\n", state->verb);
                    send_data(client_fd);
                } else if (state->state == CLOSE) {
                    printf("Now getting deleted..\n");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, &ev); //remove the epoll as it is closed now!
                    shutdown(client_fd, SHUT_RDWR);
                    close(client_fd);
                    dictionary_remove(fd_to_state, &client_fd);

                    if (state->filename) free(state->filename);
                    if (state->verb) free(state->verb);
                    free(state);
                }
            }
        }
    }
}
void handle_client(int fd) {
    requestState* state = NULL;
    if (!dictionary_contains(fd_to_state, &fd)) {
        state = (requestState*)malloc(sizeof(requestState)); //create the struct
        if (!state) {
            perror("malloc failed!"); //be careful about deleting the temp directory
            close(fd);
            epoll_ctl(global_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            return;
        }
        memset(state, 0, sizeof(requestState));
        state->state = REQUEST_HEADER;
        dictionary_set(fd_to_state, &fd, state);
    }
    state = dictionary_get(fd_to_state, &fd);
    if (state->state == REQUEST_HEADER) {
        ssize_t bytes_read = read(fd, state->header + state->header_offset, 1); //read until finding the '\n' HEADERS are short so one can read them one byte one one byte
        if (bytes_read <= 0) {
            //could be check to write
            //send response(fd) per status check!
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return; //no more data for now 
                } else {
                    return; //no use
                    //might close the fd as well LOOK HERE AGAIN
                }
            }
            return; //no connection
        }
        state->header_offset += bytes_read;
        //at this point state->header with '\n' is parsed
        //printf("The state header {REQUEST HEADER} is %s\n", state->header);
        char* nline = strchr(state->header, '\n');
        if (nline) {
            puts("NEW LINE FOUND!\n");
            *nline = '\0'; 
            char* verb = strtok(state->header, " ");
            state->verb = strdup(verb);
            if (strcmp(state->verb, "LIST") && strcmp(state->verb, "PUT") && strcmp(state->verb, "GET") && strcmp(state->verb, "DELETE")) { //to check interesting names!
                if (state->status != 1) { //if not still processing!
                    const char* err_head = "ERROR\n";
                    memcpy(state->buffer, err_head, strlen(err_head));
                    state->buffer_offset += strlen(err_head);
                    const char* error = err_bad_request;
                    state->total_response_bytes = strlen(error) + strlen(err_head);
                    memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                    state->buffer_offset += strlen(error);
                }
                send_response(fd);
                if (state->state != CLOSE) {
                    state->status = 1; //still processing
                }
                //note that we might need to put a message here indicating the type of the error!
                return;
            }
            if (strcmp(state->verb, "LIST")) {
                char* filename = strtok(NULL, " ");
                state->filename = strdup(filename);
                if (!strcmp(state->verb, "PUT")) {
                    memset(state->header, 0, sizeof(state->header)); //cleanup to reuse!
                    state->header_offset = 0;
                    state->state = REQUEST_DATA;
                    state->file_size = 0;
                    state->received_size = 0;
                } else {
                    memset(state->header, 0, sizeof(state->header)); //cleanup to reuse!
                    state->header_offset = 0;
                    state->state = RESPONSE_HEADER;
                }
            } else {
                state->total_response_bytes = 0;
                state->sent_bytes = 0;
                memset(state->buffer, 0, sizeof(state->buffer));
                state->buffer_offset = 0;
                state->state = RESPONSE_HEADER;
            }
        } else {
            if (strlen(state->header) >= HEADERSIZE) {
                if (state->status != 1) {
                    const char* err_head = "ERROR\n";
                    memcpy(state->buffer, err_head, strlen(err_head));
                    state->buffer_offset += strlen(err_head);
                    const char* error = err_bad_request;
                    state->total_response_bytes = strlen(error) + strlen(err_head);
                    memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                    state->buffer_offset += strlen(error);
                }
                send_response(fd);
                if (state->state != CLOSE) {
                    state->status = 1; //still processing!
                }
                //note that we might need to put a message here indicating the type of the error!
                return;
            }
        }
    } else if (state->state == REQUEST_DATA && strcmp(state->verb, "PUT") == 0) { //this works with PUT
        if (state->file_size == 0 && state->received_size < sizeof(size_t)) {
            ssize_t ret = read(fd, ((char*)&state ->file_size) + state->received_size, sizeof(size_t) - state->received_size);
            if (ret <= 0) {
                if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return; // no more data, wait for next event
                } else {
                     // error or closed
                    state->state = CLOSE;
                    return;
                }
            }
            state->received_size += ret;
            if (state->received_size == sizeof(size_t)) {
                char filepath[PATH_MAX];
                snprintf(filepath, sizeof(filepath), "%s/%s", temp_direct, state->filename);
                filepath[sizeof(filepath)-1] = '\0';
                state->file = fopen(filepath, "wb+");
                if (!state->file) {
                    printf("Couldnt't get the file for {PUT}!\n");
                    if (state->status != -1) {
                        const char* err_head = "ERROR\n";
                        memcpy(state->buffer, err_head, strlen(err_head));
                        state->buffer_offset += strlen(err_head);
                        const char* error = err_no_such_file;
                        state->total_response_bytes = strlen(error) + strlen(err_head);
                        memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                        state->buffer_offset += strlen(error);
                    }
                    send_response(fd);
                    if (state->state != CLOSE) {
                        state->status = 1;
                    }
                    return;
                }
                state->received_size = 0;
            } else {
                return;
            }
        }
        if (state->file_size > 0 && state->received_size < state->file_size) {
            size_t remaining = state->file_size - state->received_size;
            char buf[1024];
            size_t to_read = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
            ssize_t ret = read(fd, buf, to_read);
            if (ret <= 0) {
                if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return; // no data now
                } else {
                    // client closed or error
                    state->state = CLOSE;
                    return;
                }
            }

            size_t written = fwrite(buf, 1, ret, state->file);
            if (written < (size_t)ret) {
                // disk error? //underwrite
                if (state->status != 1) {
                    const char* err_head = "ERROR\n";
                    memcpy(state->buffer, err_head, strlen(err_head));
                    state->buffer_offset += strlen(err_head);
                    const char* error = err_bad_file_size;
                    state->total_response_bytes = strlen(error) + strlen(err_head);
                    memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                    state->buffer_offset += strlen(error);
                }
                send_response(fd);
                if (state->state != CLOSE) {
                    state->status = 1;
                }
                return;
            }
            state->received_size += ret;
            if (state->received_size == state->file_size) {
                // full file received!
                fclose(state->file);
                state->file = NULL;
                vector_push_back(filenames, state->filename); // successfully saved file
                // prepare "OK\n" response
                memset(state->buffer, 0, sizeof(state->buffer));
                const char* ok = "OK\n";
                memcpy(state->buffer, ok, strlen(ok));
                state->total_response_bytes = strlen(ok);
                state->sent_bytes = 0;
                state->buffer_offset = 0; // no leftover data needed here

                state->state = RESPONSE_HEADER;
            }
        }
    }
}

void write_to_client(int fd) {
    requestState* state = dictionary_get(fd_to_state, &fd);
    if (!state) {
        printf("Does not exist!");
        return;
    }
    if (!strcmp(state->verb, "DELETE")) { //might need to look further for client buffering!
        //now check if the given filename exists in the vector of files we store
        printf("DELETE ENTERED\n");
        if (state->filename) {
            if (state->status != 1) {
                printf("STATE FILENAME is %s\n", state->filename);
                if (vector_size(filenames) <= 0) {
                    if (state->status != 1) {
                        const char* err_head = "ERROR\n";
                        memcpy(state->buffer, err_head, strlen(err_head));
                        state->buffer_offset += strlen(err_head);
                        const char* error = err_no_such_file;
                        state->total_response_bytes = strlen(error) + strlen(err_head);
                        memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                        state->buffer_offset += strlen(error);
                    }
                    send_response(fd);
                    if (state->state != CLOSE) {
                        state->status = 1;
                    }
                    //note that we might need to put a message here indicating the type of the error!
                    return;
                }
                int match = 0;
                for (size_t i = 0; i < vector_size(filenames); ++i) {
                    if (!strcmp(vector_get(filenames, i), state->filename)) {
                        match = 1;
                        printf("MATCH FOUND!\n");
                        vector_erase(filenames, i); //erase the ith file
                        char filepath[PATH_MAX];
                        snprintf(filepath, sizeof(filepath), "%s/%s", temp_direct, state->filename);
                        filepath[sizeof(filepath) - 1] = '\0';
                        if (remove(filepath) != 0) {
                            printf("NOT HERE!\n");
                            if (state->status != 1) {
                                const char* err_head = "ERROR\n";
                                memcpy(state->buffer, err_head, strlen(err_head));
                                state->buffer_offset += strlen(err_head);
                                const char* error = err_no_such_file;
                                state->total_response_bytes = strlen(error) + strlen(err_head);
                                memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                                state->buffer_offset += strlen(error);
                            }
                            send_response(fd);
                            if (state->state != CLOSE) {
                                state->status = 1;
                            }
                            //note that we might need to put a message here indicating the type of the error!
                            return;
                        }
                    }
                }
                if (match == 0) {
                    if (state->status != 1) {
                        const char* err_head = "ERROR\n";
                        memcpy(state->buffer, err_head, strlen(err_head));
                        state->buffer_offset += strlen(err_head);
                        const char* error = err_no_such_file;
                        state->total_response_bytes = strlen(error) + strlen(err_head);
                        memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                        state->buffer_offset += strlen(error);
                    }
                    send_response(fd);
                    if (state->state != CLOSE) {
                        state->status = 1;
                    }
                }

                if (state->status != 1) {
                    char* ok = "OK\n";
                    memcpy(state->buffer, ok, strlen(ok));
                    state->buffer_offset += strlen(ok);
                    state->total_response_bytes = strlen(ok);
                }
            }
            send_response(fd); //the message OK\n is sent!
            if (state->state != CLOSE) {
                state->status = 1;
            }

            return;
        } else {
            //printf("ERROR write");
            if (state->status != 1) {
                const char* err_head = "ERROR\n";
                memcpy(state->buffer, err_head, strlen(err_head));
                state->buffer_offset += strlen(err_head);
                const char* error = err_no_such_file;
                state->total_response_bytes = strlen(error) + strlen(err_head);
                memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                state->buffer_offset += strlen(error);
            }
            send_response(fd);
            if (state->state != CLOSE) {
                state->status = 1;
            }
            //note that we might need to put a message here indicating the type of the error!
            return;
        }
    } else if (!strcmp(state->verb, "GET")) {
        if (state->filename) {
            if (vector_size(filenames) <= 0) {
                if (state->status != 1) {
                       const char* err_head = "ERROR\n";
                       memcpy(state->buffer, err_head, strlen(err_head));
                       state->buffer_offset += strlen(err_head);
                       const char* error = err_no_such_file;
                       state->total_response_bytes = strlen(error) + strlen(err_head);
                       memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                       state->buffer_offset += strlen(error);
                }
                send_response(fd);
                if (state->state != CLOSE) {
                    state->status = 1;
                }
                return;
            }
            if (state->status != 1) {
                char filepath[PATH_MAX];
                snprintf(filepath, sizeof(filepath), "%s/%s", temp_direct, state->filename); //directory formed!
                filepath[sizeof(filepath) - 1] = '\0'; 
                printf("Filepath is %s\n", filepath);
                state->file = fopen(filepath, "rb");
                if (!state->file) {
                    if (state->status != 1) {
                        const char* err_head = "ERROR\n";
                        memcpy(state->buffer, err_head, strlen(err_head));
                        state->buffer_offset += strlen(err_head);
                        const char* error = err_no_such_file;
                        state->total_response_bytes = strlen(error) + strlen(err_head);
                        memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                        state->buffer_offset += strlen(error);
                    }
                    send_response(fd);
                    if (state->state != CLOSE) {
                        state->status = 1;
                    }
                    return;
                }
                struct stat buf;
                if (stat(filepath, &buf) != 0) {
                    perror("stat() file for GET");
                    state->state = CLOSE;
                    return;
                }
                //state->file = get_file;
                size_t file_size = buf.st_size; //size of the file;
                state->file_size = file_size;
                char* ok = "OK\n";
                state->total_response_bytes = strlen(ok) + sizeof(size_t);
                memcpy(state->buffer, ok, strlen(ok));
                state->buffer_offset += strlen(ok);
                memcpy(state->buffer + state->buffer_offset, (char*)&state->file_size, sizeof(size_t));
                state->buffer_offset += sizeof(size_t);
            }
            send_response(fd); //the message OK\n is sent!
            if (state->state != CLOSE) {
                state->status = 1;
            }
            if (state->state == CLOSE) {
                memset(state->buffer, 0, sizeof(state->buffer));
                state->buffer_offset = 0;
                printf("File size is %zu\n", state->file_size);
                state->total_response_bytes = state->file_size;
                state->sent_bytes = 0;
                state->status = 0;
                state->state = RESPONSE_DATA;
                return;
            }
            ///RESPONSE->DATA; WE'll migrate this to a another function;
        } else {
            if (state->status != 1) {
                const char* err_head = "ERROR\n";
                memcpy(state->buffer, err_head, strlen(err_head));
                state->buffer_offset += strlen(err_head);
                const char* error = err_no_such_file;
                state->total_response_bytes = strlen(error) + strlen(err_head);
                memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                state->buffer_offset += strlen(error);
            }
            send_response(fd);
            if (state->state != CLOSE) {
                state->status = 1;
            }
            return;
        }
    } else if (!strcmp(state->verb, "PUT")) {
        send_response(fd);
    } else if (!strcmp(state->verb, "LIST")) {
        char buf[1024];
        size_t off = 0;
        memset(buf, 0, sizeof(buf));
        size_t total_length = 0;
        if (vector_size(filenames) > 0) {
           // memset(buf, 0, sizeof(buf));
            //state->file_size = 0;
            if (state->status != 1) {
                for (size_t i = 0; i < vector_size(filenames); ++i) {
                    char* list_file = vector_get(filenames, i);
                    printf("File name is %s\n", list_file);
                    printf("Strlen file name is %zu\n", strlen(list_file));
                    memcpy(buf + off, list_file, strlen(list_file));
                    printf("Filled buf is %s\n", buf);
                    total_length += strlen(list_file);
                    off += strlen(list_file);
                    if (i < vector_size(filenames) - 1) {
                        memcpy(buf + off, "\n", 1);
                        off += 1;
                        total_length += 1;
                    }
                }
                state->file_size = total_length;
                if (state->status != 1) {
                    printf("Total_length is %zu\n", total_length);
                    state->total_response_bytes = 3 + sizeof(size_t); 
                    const char* ok = "OK\n";
                    memcpy(state->buffer, ok, strlen(ok));
                    state->buffer_offset += strlen(ok);
                    memcpy(state->buffer + state->buffer_offset, (char*)&state->file_size, sizeof(size_t));
                    state->buffer_offset += sizeof(size_t);
                }
            }
            send_response(fd);
            if (state->state != CLOSE) { //client still waits for my writes
                state->status = 1;
            }
            if (state->state == CLOSE) {
                state->total_response_bytes = total_length;
                state->sent_bytes = 0;
                printf("New total response bytes is %zu\n", state->total_response_bytes);
                state->state = RESPONSE_DATA;
                memset(state->buffer, 0, sizeof(state->buffer));
                state->buffer_offset = 0;
                memcpy(state->buffer + state->buffer_offset, buf, total_length);
                printf("BUFFER TO SEND IS %s\n", state->buffer);
                return;
            }
            ///send these to RESPONSE DATA
        } else {
            if (state->status != 1) {
                printf("Couldnt't get the file!\n");
                const char* err_head = "ERROR\n";
                memcpy(state->buffer, err_head, strlen(err_head));
                state->buffer_offset += strlen(err_head);
                const char* error = err_no_such_file;
                state->total_response_bytes = strlen(error) + strlen(err_head);
                memcpy(state->buffer + state->buffer_offset, error, strlen(error));
                state->buffer_offset += strlen(error);
            }
            send_response(fd);
            if (state->state != CLOSE) {
                state->status = 1;
            }
            return;
        }
    }
}

void send_response(int fd) {
    printf("SENDING SENDING!\n");
    enable_epollout(global_epoll_fd, fd);
    requestState* state = (requestState*)dictionary_get(fd_to_state, &fd);
    printf("State->buffer is %s\n", state->buffer); 
    ssize_t write_so_far = write(fd, state->buffer + state->sent_bytes, state->total_response_bytes - state->sent_bytes);
    if (write_so_far <= 0) {
        if (write_so_far == -1) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return; //data is yet to arrive
            }
        }
        printf("Connection is closed so no SEND RESPONSE!\n");
        return;
    }
    state->sent_bytes += write_so_far;
    if (state->sent_bytes == state->total_response_bytes) {
        printf("Mission accomplished!\n");
        state->state = CLOSE; //mission accomplished!
        return;
    } else {
        printf("Still more data to write!\n");
    }
    disable_epollout(global_epoll_fd, fd); //close the lights when going out!
}

void enable_epollout(int epoll_fd, int fd) {
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLOUT;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1) {
        perror("Problem with epoll mod");
        return;
    }
}

void disable_epollout(int epoll_fd, int fd) {
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event)) {
        perror("Problem with epoll mod 2");
        return;
    }
}
void send_data(int fd) { //for LIST and GET!
    requestState* state = (requestState*)dictionary_get(fd_to_state, &fd);
    if (!strcmp(state->verb, "GET")) {
        ssize_t binary_data_read = fread(state->buffer + state->buffer_offset, 1, state->file_size - state->buffer_offset, state->file);
        if (binary_data_read <= 0) {
            if (binary_data_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return; //no more data for now 
                } else {
                    return; //no use
                        //might close the fd as well LOOK HERE AGAIN
                }
            }
            return; //no connection
        }

        state->buffer_offset += binary_data_read;
        if (state->buffer_offset == state->file_size) { //this can be improved for large files!
            send_response(fd);
            if (state->state != CLOSE) {
                state->status = 1;
            } 
        }
    } else if (!strcmp(state->verb, "LIST")) {
        send_response(fd);
    }
}

// Make sure your error handlers work!
// Check GET's permission, only PUT will open a file!
//YB 5