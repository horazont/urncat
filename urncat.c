#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <liburing.h>
#include <stdbool.h>

#define QUEUE_DEPTH (4)
#define BUFFER_SIZE (65536)

#define UD_FLAG_IS_WRITE     (0x100000000ULL)
#define UD_FLAG_SIZE_MASK    (0x0ffffffffULL)
#define UD_FLAG_BUFFER_MASK  (0x0000000f0ULL)
#define UD_FLAG_BUFFER_SHIFT (4)

int main(int argc, char **argv) {
    struct io_uring ring;

    /* if (argc != 4) {
        printf("%s: PORT ADDR PORT\n", argv[0]);
        return 2;
    }

    struct sockaddr_in listen_addr;
    struct sockaddr_in connect_addr; */

    int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (ret < 0) {
        perror("io_uring_queue_init");
        return 1;
    }


    void *buf_a = malloc(BUFFER_SIZE);
    void *buf_b = malloc(BUFFER_SIZE);
    void *buffers[2] = {buf_a, buf_b};

    int next_read_buffer = 0;

    // flow:
    // 1. submit a single read and wait for it to complete
    // 2. submit a write and the next read in the secondary buffer
    // 3. alternate until eof

    ssize_t bytes_read = read(STDIN_FILENO, buffers[next_read_buffer], BUFFER_SIZE);
    if (bytes_read < 0) {
        perror("read");
        return 1;
    }
    struct io_uring_cqe fake_completed_read = {
        .res = bytes_read,
        .flags = next_read_buffer << UD_FLAG_BUFFER_SHIFT,
    };
    next_read_buffer ^= 1;

    struct io_uring_cqe *last_completed_read = &fake_completed_read;
    size_t read_offset = bytes_read;
    size_t write_offset = 0;

    {
        int fds[2] = {STDIN_FILENO, STDOUT_FILENO};

        struct iovec iovecs[2];
        iovecs[0].iov_base = buffers[0];
        iovecs[0].iov_len = BUFFER_SIZE;
        iovecs[1].iov_base = buffers[1];
        iovecs[1].iov_len = BUFFER_SIZE;

        ret = io_uring_register_files(&ring, fds, sizeof(fds) / sizeof(int));
        if (ret != 0) {
            perror("io_uring_register_files");
            return 1;
        }
        ret = io_uring_register_buffers(&ring, iovecs, sizeof(iovecs) / sizeof(struct iovec));
        if (ret != 0) {
            perror("io_uring_register_buffers");
            return 1;
        }
    }

    while (last_completed_read != NULL) {
        {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read_fixed(sqe, 0, buffers[next_read_buffer], BUFFER_SIZE, read_offset, next_read_buffer);
            // io_uring_prep_read(sqe, 0, buffers[next_read_buffer], BUFFER_SIZE, read_offset);
            sqe->user_data = next_read_buffer << UD_FLAG_BUFFER_SHIFT;
            sqe->flags |= IOSQE_FIXED_FILE;
            next_read_buffer ^= 1;
        }

        {
            const int read_buffer_index = (last_completed_read->flags & UD_FLAG_BUFFER_MASK) >> UD_FLAG_BUFFER_SHIFT;
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_write_fixed(sqe, 1, buffers[read_buffer_index], last_completed_read->res, write_offset, read_buffer_index);
            // io_uring_prep_write(sqe, 1, buffers[read_buffer_index], last_completed_read->res, write_offset);
            sqe->flags |= IOSQE_FIXED_FILE;
            sqe->user_data = UD_FLAG_IS_WRITE | ((size_t)last_completed_read->res);
            read_offset += last_completed_read->res;
            write_offset += last_completed_read->res;
            if (last_completed_read != &fake_completed_read) {
                io_uring_cqe_seen(&ring, last_completed_read);
            }
        }

        io_uring_submit(&ring);

        for (int i = 0; i < 2; ++i) {
            // wait for both read and write to complete
            struct io_uring_cqe *completed_op = NULL;
            io_uring_wait_cqe(&ring, &completed_op);
            /* fprintf(stderr, "op: sz=%d flags=%d ud=%llx\n",
                    completed_op->res, completed_op->flags,
                    completed_op->user_data); */
            if (completed_op->user_data & UD_FLAG_IS_WRITE) {
                // TODO: handle write errors
                const ssize_t expected_size = completed_op->user_data & UD_FLAG_SIZE_MASK;
                if (completed_op->res < expected_size) {
                    fprintf(stderr, "io_uring partial write\n");
                    return 1;
                }
                io_uring_cqe_seen(&ring, completed_op);
                continue;
            }
            last_completed_read = completed_op;
            if (last_completed_read->res == 0) {
                last_completed_read = NULL;
            }
            // TODO: handle read errors
        }
    }

    io_uring_queue_exit(&ring);
}
