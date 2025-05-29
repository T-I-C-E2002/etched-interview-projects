
#include "common.h"



size_t get_message_size(int sock_fd) {
    size_t size;
    ssize_t read = read_all_from_socket(sock_fd, (char*)&size, 8);
    if (read == 0 || read == -1) {
        return read;
    }
    return size;
}

size_t write_message_write(size_t size, int sock_fd) {
    size_t net_size = size;
    ssize_t bytes_written = write_all_to_socket(sock_fd, (char*)&net_size, 8);
    return bytes_written;
}

ssize_t write_all_to_socket(int socket, const char* buffer, size_t count) {
    size_t total_write = 0;
    while (total_write < count) {
        //printf("HERE!");
        ssize_t bytes_written = write(socket, buffer + total_write, count - total_write);
        if (bytes_written == 0) {
            return total_write;
        } else if (bytes_written > 0) {
            total_write += bytes_written;
        } else if (bytes_written == -1 && errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return total_write;
        } else {
            perror(NULL);
            return -1;
        }
    }
    //printf("HERE\n");
    return total_write;
}

ssize_t read_all_from_socket(int socket, char* buffer, size_t count) {
    size_t total_read = 0;
    while (total_read < count) {
        ssize_t bytes_read = read(socket, buffer + total_read, count - total_read);
        if (bytes_read == 0) {
            return total_read;
        } else if (bytes_read > 0) {
            total_read += bytes_read;
        } else if (bytes_read == -1 && errno == EINTR) {
            continue;
        }
        else if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
            return total_read;
        } else {
            perror(NULL);
            return -1;
        }
    }
    return total_read;
}


