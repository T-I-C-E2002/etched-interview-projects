
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define LOG(...)                      \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n");        \
    } while (0);

typedef enum { GET, PUT, DELETE, LIST, V_UNKNOWN } verb;


size_t get_message_size(int socket);

size_t write_message_size(size_t size, int socket);

ssize_t write_all_to_socket(int socket, const char* buffer, size_t count);

ssize_t read_all_from_socket(int socket, char* buffer, size_t count);