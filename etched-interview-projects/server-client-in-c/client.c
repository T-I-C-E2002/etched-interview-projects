#include "format.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
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
//Asked LLM to debug my writing the file logic in PUT operation and size_t size reading logic for parsing the responses
char **parse_args(int argc, char **argv);
verb check_args(char **args);
int write_to_server(int socket, char* verb, char** args);
int read_from_server(int socket, char** arguments);
void close_server_connection(int sock_fd);
void free_arguments(char** arguments, int argc);
int connect_to_server(char* host, char* port);

int main(int argc, char **argv) {
    //Plan!
    //For all 4 functionalities send their format correctly to server
    //Next, read from the server,
    //Be careful about the error messages, then you are done!.

    // Good luck! 
    char** arguments = parse_args(argc, argv); //parse the arguments // ./client [port] [VERB]  [remote] [local]
    char* port = arguments[1]; 

    //printf("Req %s\nPort %s\nVerb %s\n", arguments[0], port, arguments[2]);
 
    int sock_fd = connect_to_server(arguments[0], port); // connect to server
    int val = write_to_server(sock_fd, arguments[2], arguments); //create the message in the given format and send it to socket
    if (val == 0) {
        close_server_connection(sock_fd);
        //free_arguments(arguments, argc);
        free(arguments);
        return 0;
    }
    shutdown(sock_fd, SHUT_WR); //shut the half-socket
     
    //char* read_ = calloc(1, 255);
    //read_all_from_socket(sock_fd, read_, 255); 
    //printf("%s", read_);
    val = read_from_server(sock_fd, arguments);
    if (val == 0) {
        close_server_connection(sock_fd);
        //free_arguments(arguments, argc);
        free(arguments);
        return 0;
    }

    close_server_connection(sock_fd);
    //free_arguments(arguments, argc);
    free(arguments);
    return 0;
}
void free_arguments(char** args, int argc) {
    for (int i = 0; i < argc; ++i) {
        if (args[i]) {
            free(args[i]);
        }
    }
    free(args);
}
int read_from_server(int socket, char** arguments) {
    //check if the response is OK\n or ERROR\n
    char* if_ok = calloc(4, sizeof(char));
    read_all_from_socket(socket, if_ok, 3);
    if_ok[3] = '\0';
    //printf("IF OK is %s\n", if_ok);
    if (strcmp(if_ok, "OK\n") == 0) {
        puts(if_ok);
        
        if (!(strcmp(arguments[2], "GET") == 0 || strcmp(arguments[2], "LIST") == 0)) {
            if (strcmp(arguments[2], "DELETE") == 0 || strcmp(arguments[2], "PUT") == 0) {
                print_success();
                free(if_ok);
                return 1;
            }
            free(if_ok);
            
        } else {
            if (strcmp(arguments[2], "GET") == 0) {
                //write the contents of arg[3] to arg[4]
                FILE* file_local = fopen(arguments[4], "wb");
                if (!file_local) {
                    perror("fopen()");
                    close_server_connection(socket);
                    free(if_ok);
                    return 0;
                }

                size_t size_ = 0;
                read_all_from_socket(socket, (char*)&size_, sizeof(size_t));
                
                size_t count = 0;
                while (count < size_) {
                    size_t read_so_far = MIN(1024, size_ - count);
                    char buffer[1024];
                    size_t read_written = read_all_from_socket(socket, buffer, read_so_far);
                    if (read_written < read_so_far) {
                        print_too_little_data();
                        close_server_connection(socket);
                        free(if_ok);
                        fclose(file_local);
                        return 0;
                    }
                    fwrite(buffer, 1, read_so_far, file_local);
                    count += read_so_far;
                }
                char buffer[1024];
                if ((size_t)read_all_from_socket(socket, buffer, size_ + 1) > 0) {
                    print_received_too_much_data();
                    close_server_connection(socket);
                    free(if_ok);
                    fclose(file_local);
                    return 0;
                }
                free(if_ok);
                fclose(file_local);
                return 1;
            }
            if (strcmp(arguments[2], "LIST") == 0) {
                //list the files
                //char* size_ = calloc(1, sizeof(size_t));
                size_t size_ = 0;
                read_all_from_socket(socket, (char*) &size_, sizeof(size_t));
                //printf("Size is %zu\n", size_);
                char* files = calloc(1, size_ + 1);
                if ((size_t)read_all_from_socket(socket, files, size_) < size_) {
                    free(if_ok);
                    print_too_little_data();
                    close_server_connection(socket);
                    free(files);
                    return 0;
                }
                char buffer[1024];
                if ((size_t)read_all_from_socket(socket, buffer, size_ + 1) > 0) {
                    free(if_ok);
                    print_received_too_much_data();
                    close_server_connection(socket);
                    free(files);
                    return 0;
                }
                puts(files);
                free(files);
                free(if_ok);
                return 1;
            }
            free(if_ok);
        }
    }
    else if (strcmp(if_ok, "ERR") == 0) {
        read_all_from_socket(socket, if_ok, 3);
        if_ok[3] = '\0';
        if (strcmp(if_ok, "OR\n") == 0) {
            char buffer[1024] = {0};
            read_all_from_socket(socket, buffer, 1024);
           
            char* end = strchr(buffer, '\n');
            if (end) {
                end = "\0";
            }
        
            print_error_message(buffer);
            free(if_ok);
            return 0;
        }
    } else {
        print_invalid_response();
        free(if_ok);
        return 0;
    }
    free(if_ok);
    return 1;
}
int write_to_server(int socket, char* verb, char** args) {
    //create the message in the correct format for given verb!
    char* return_;
    if (strcmp(verb, "LIST") == 0) {
        //return_ = calloc(1, strlen(verb) + 1);
        return_ = calloc(strlen(verb) + 2, sizeof(char));
        sprintf(return_, "%s\n", verb);
    }
    else {
        size_t len = strlen(verb) + strlen(args[3]) + 3;
        return_ = calloc(len, sizeof(char));
        //return_ = calloc(1, strlen(verb) + strlen(args[3]) + 2);
        sprintf(return_, "%s %s\n", verb, args[3]);
    }
    
    ssize_t bytes_written = write_all_to_socket(socket, return_, strlen(return_)); //send to the server in the format one has used!
    //if (return_) {
        //free(return_);
    //}
    if ((size_t)(bytes_written) < strlen(return_)) {
        free(return_);
        close_server_connection(socket);
        return 0; //underwrite
    }
    free(return_);
    if (strcmp(verb, "PUT") == 0) {
        FILE* file = fopen(args[4], "rb");
        if (!file) {
            //printf("HERE!\n");
            perror("fopen()");
            close_server_connection(socket);
            return 0;
        }

        struct stat buf;
        if (stat(args[4], &buf) != 0) {
            perror("stat");
            fclose(file);
            close_server_connection(socket);
            return 0;
        }

        size_t file_size = buf.st_size;
        //printf("File size is %zu\n", file_size);
        if ((size_t)write_all_to_socket(socket, (char*)&file_size, sizeof(size_t)) < sizeof(size_t)) {
            close_server_connection(socket);
            fclose(file);
            return 0;
        } //write the size

        
        size_t count = 0;
        while (count < file_size) {
            size_t written_so_far = MIN(1024, file_size - count);
            //printf("Written so far is %zu\n", written_so_far);
            char buffer[1024] = {0};
           
            size_t written_read = fread(buffer, 1, written_so_far, file);
            if (written_read == 0 && ferror(file)) {
                perror("fread");
                fclose(file);
                close_server_connection(socket);
                return 0;
            }

            if ((size_t)write_all_to_socket(socket, buffer, written_read) < written_read) {
                close_server_connection(socket);
                fclose(file);
                return 0;
            }
            //printf("Buffer is %s\n", buffer);
            count += written_so_far;
        }
        //printf("Count is %zu\n", count);
        fclose(file);
    }
    //printf("Message is %s", return_);
    return 1;
}

void close_server_connection(int sock_fd) {
    shutdown(sock_fd, SHUT_RD);
    close(sock_fd);
    print_connection_closed();
}

int connect_to_server(char* host, char* port) {
    struct addrinfo current, *result;
    memset(&current, 0, sizeof(struct addrinfo));
    current.ai_family = AF_INET; //IPv4
    current.ai_socktype = SOCK_STREAM;
    current.ai_flags = AI_PASSIVE;

    int status;
    if ((status = getaddrinfo(host, port, &current, &result)) != 0) {
        perror("getaddrinfo()");
        exit(1);
    }

    int sock_fd;
    if ((sock_fd = socket(result->ai_family, result->ai_socktype, 0)) == -1){
        perror("socket()");
        freeaddrinfo(result);
        exit(1);
    }
    if ((connect(sock_fd, result->ai_addr, result->ai_addrlen)) == -1) {
        perror("connect()");
        freeaddrinfo(result);
        exit(1);
    }
    freeaddrinfo(result);
    return sock_fd;
}
/**
 * Given commandline argc and argv, parses argv.
 *
 * argc argc from main()
 * argv argv from main()
 *
 * Returns char* array in form of {host, port, method, remote, local, NULL}
 * where `method` is ALL CAPS
 */
char **parse_args(int argc, char **argv) {
    if (argc < 3) {
        return NULL;
    }

    char *host = strtok(argv[1], ":");
    char *port = strtok(NULL, ":");
    if (port == NULL) {
        return NULL;
    }

    char **args = calloc(1, 6 * sizeof(char *));
    args[0] = host;
    args[1] = port;
    args[2] = argv[2];
    char *temp = args[2];
    while (*temp) {
        *temp = toupper((unsigned char)*temp);
        temp++;
    }
    if (argc > 3) {
        args[3] = argv[3];
    }
    if (argc > 4) {
        args[4] = argv[4];
    }

    return args;
}

/**
 * Validates args to program.  If `args` are not valid, help information for the
 * program is printed.
 *
 * args     arguments to parse
 *
 * Returns a verb which corresponds to the request method
 */
verb check_args(char **args) {
    if (args == NULL) {
        print_client_usage();
        exit(1);
    }

    char *command = args[2];

    if (strcmp(command, "LIST") == 0) {
        return LIST;
    }

    if (strcmp(command, "GET") == 0) {
        if (args[3] != NULL && args[4] != NULL) {
            return GET;
        }
        print_client_help();
        exit(1);
    }

    if (strcmp(command, "DELETE") == 0) {
        if (args[3] != NULL) {
            return DELETE;
        }
        print_client_help();
        exit(1);
    }

    if (strcmp(command, "PUT") == 0) {
        if (args[3] == NULL || args[4] == NULL) {
            print_client_help();
            exit(1);
        }
        return PUT;
    }

    // Not a valid Method
    print_client_help();
    exit(1);
    //YB 5
}
