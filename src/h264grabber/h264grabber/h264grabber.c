/*
 * Copyright (c) 2020 roleo.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Dump h264 content from /dev/shm/fshare_frame_buffer to stdout
 */

#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <getopt.h>

#define BUF_OFFSET_Y21GA 368
#define BUF_SIZE_Y21GA 1786224
#define FRAME_HEADER_SIZE_Y21GA 28
#define DATA_OFFSET_Y21GA 4
#define LOWRES_BYTE_Y21GA 8
#define HIGHRES_BYTE_Y21GA 4

#define BUF_OFFSET_R30GB 300
#define BUF_SIZE_R30GB 1786156
#define FRAME_HEADER_SIZE_R30GB 22
#define DATA_OFFSET_R30GB 0
#define LOWRES_BYTE_R30GB 8
#define HIGHRES_BYTE_R30GB 4
//#define HIGHRES_BYTE_R30GB 16

#define USLEEP 100000

#define RESOLUTION_LOW  360
#define RESOLUTION_HIGH 1080

#define BUFFER_FILE "/dev/shm/fshare_frame_buf"

int buf_offset;
int buf_size;
int frame_header_size;
int data_offset;
int lowres_byte;
int highres_byte;

unsigned char IDR4[]               = {0x65, 0xB8};
unsigned char NALx_START[]         = {0x00, 0x00, 0x00, 0x01};
unsigned char IDR4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
unsigned char IDR5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x26};
unsigned char PFR4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x41};
unsigned char PFR5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x02};
unsigned char SPS4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x67};
unsigned char SPS5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x42};
unsigned char PPS4_START[]         = {0x00, 0x00, 0x00, 0x01, 0x68};
unsigned char PPS5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x44};
unsigned char VPS5_START[]         = {0x00, 0x00, 0x00, 0x01, 0x40};

unsigned char SPS4_640X360[]       = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x14,
                                        0x96, 0x54, 0x05, 0x01, 0x7B, 0xCB, 0x37, 0x01,
                                        0x01, 0x01, 0x02};
unsigned char SPS4_1920X1080[]     = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x20,
                                        0x96, 0x54, 0x03, 0xC0, 0x11, 0x2F, 0x2C, 0xDC,
                                        0x04, 0x04, 0x04, 0x08};

unsigned char *addr;                      /* Pointer to shared memory region (header) */
int debug = 0;                            /* Set to 1 to debug this .c */
int resolution;

/* Locate a string in the circular buffer */
unsigned char * cb_memmem(unsigned char *src, int src_len, unsigned char *what, int what_len)
{
    unsigned char *p;

    if (src_len >= 0) {
        p = (unsigned char*) memmem(src, src_len, what, what_len);
    } else {
        // From src to the end of the buffer
        p = (unsigned char*) memmem(src, addr + buf_size - src, what, what_len);
        if (p == NULL) {
            // And from the start of the buffer size src_len
            p = (unsigned char*) memmem(addr + buf_offset, src + src_len - (addr + buf_offset), what, what_len);
        }
    }
    return p;
}

unsigned char * cb_move(unsigned char *buf, int offset)
{
    buf += offset;
    if ((offset > 0) && (buf > addr + buf_size))
        buf -= (buf_size - buf_offset);
    if ((offset < 0) && (buf < addr + buf_offset))
        buf += (buf_size - buf_offset);

    return buf;
}

// The second argument is the circular buffer
int cb_memcmp(unsigned char *str1, unsigned char *str2, size_t n)
{
    int ret;

    if (str2 + n > addr + buf_size) {
        ret = memcmp(str1, str2, addr + buf_size - str2);
        if (ret != 0) return ret;
        ret = memcmp(str1 + (addr + buf_size - str2), addr + buf_offset, n - (addr + buf_size - str2));
    } else {
        ret = memcmp(str1, str2, n);
    }

    return ret;
}

// The second argument is the circular buffer
void cb_memcpy(unsigned char *dest, unsigned char *src, size_t n)
{
    if (src + n > addr + buf_size) {
        memcpy(dest, src, addr + buf_size - src);
        memcpy(dest + (addr + buf_size - src), addr + buf_offset, n - (addr + buf_size - src));
    } else {
        memcpy(dest, src, n);
    }
}

void print_usage(char *progname)
{
    fprintf(stderr, "\nUsage: %s [-r RES] [-d]\n\n", progname);
    fprintf(stderr, "\t-m MODEL, --model MODEL\n");
    fprintf(stderr, "\t\tset model: y21ga or r30gb (default y21ga)\n");
    fprintf(stderr, "\t-r RES, --resolution RES\n");
    fprintf(stderr, "\t\tset resolution: LOW or HIGH (default HIGH)\n");
    fprintf(stderr, "\t-d, --debug\n");
    fprintf(stderr, "\t\tenable debug\n");
}

int main(int argc, char **argv) {
    unsigned char *buf_idx_1, *buf_idx_2;
    unsigned char *buf_idx_w, *buf_idx_tmp;
    unsigned char *buf_idx_start, *buf_idx_end;
    unsigned char *sps_addr;
    int sps_len;
    FILE *fFid;

    int frame_res, frame_len, frame_counter, frame_counter_prev = -1;

    int i, c;
    int write_enable = 0;
    int sync_lost = 1;

    time_t ta, tb;

    resolution = RESOLUTION_HIGH;
    debug = 0;

    buf_offset = BUF_OFFSET_Y21GA;
    buf_size = BUF_SIZE_Y21GA;
    frame_header_size = FRAME_HEADER_SIZE_Y21GA;
    data_offset = DATA_OFFSET_Y21GA;
    lowres_byte = LOWRES_BYTE_Y21GA;
    highres_byte = HIGHRES_BYTE_Y21GA;

    while (1) {
        static struct option long_options[] =
        {
            {"model",  required_argument, 0, 'm'},
            {"resolution",  required_argument, 0, 'r'},
            {"debug",  no_argument, 0, 'd'},
            {"help",  no_argument, 0, 'h'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv, "m:r:dh",
                         long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c) {
        case 'm':
            if (strcasecmp("y21ga", optarg) == 0) {
                buf_offset = BUF_OFFSET_Y21GA;
                buf_size = BUF_SIZE_Y21GA;
                frame_header_size = FRAME_HEADER_SIZE_Y21GA;
                data_offset = DATA_OFFSET_Y21GA;
                lowres_byte = LOWRES_BYTE_Y21GA;
                highres_byte = HIGHRES_BYTE_Y21GA;
            } else if (strcasecmp("r30gb", optarg) == 0) {
                buf_offset = BUF_OFFSET_R30GB;
                buf_size = BUF_SIZE_R30GB;
                frame_header_size = FRAME_HEADER_SIZE_R30GB;
                data_offset = DATA_OFFSET_R30GB;
                lowres_byte = LOWRES_BYTE_R30GB;
                highres_byte = HIGHRES_BYTE_R30GB;
            }
            break;

        case 'r':
            if (strcasecmp("low", optarg) == 0) {
                resolution = RESOLUTION_LOW;
            } else if (strcasecmp("high", optarg) == 0) {
                resolution = RESOLUTION_HIGH;
            }
            break;

        case 'd':
            fprintf (stderr, "debug on\n");
            debug = 1;
            break;

        case 'h':
            print_usage(argv[0]);
            return -1;
            break;

        case '?':
            /* getopt_long already printed an error message. */
            break;

        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    // Opening an existing file
    fFid = fopen(BUFFER_FILE, "r") ;
    if ( fFid == NULL ) {
        fprintf(stderr, "could not open file %s\n", BUFFER_FILE) ;
        return -1;
    }

    // Map file to memory
    addr = (unsigned char*) mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fileno(fFid), 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "error mapping file %s\n", BUFFER_FILE);
        fclose(fFid);
        return -2;
    }
    if (debug) fprintf(stderr, "mapping file %s, size %d, to %08x\n", BUFFER_FILE, buf_size, (unsigned int) addr);

    // Closing the file
    if (debug) fprintf(stderr, "closing the file %s\n", BUFFER_FILE) ;
    fclose(fFid) ;

    memcpy(&i, addr + 16, sizeof(i));
    buf_idx_w = addr + buf_offset + i;
    buf_idx_1 = buf_idx_w;

    if (debug) fprintf(stderr, "starting capture main loop\n");

    // Infinite loop
    while (1) {
        memcpy(&i, addr + 16, sizeof(i));
        buf_idx_w = addr + buf_offset + i;
//        if (debug) fprintf(stderr, "buf_idx_w: %08x\n", (unsigned int) buf_idx_w);
        buf_idx_tmp = cb_memmem(buf_idx_1, buf_idx_w - buf_idx_1, NALx_START, sizeof(NALx_START));
        if (buf_idx_tmp == NULL) {
            usleep(USLEEP);
            continue;
        } else {
            buf_idx_1 = buf_idx_tmp;
        }
//        if (debug) fprintf(stderr, "found buf_idx_1: %08x\n", (unsigned int) buf_idx_1);

        buf_idx_tmp = cb_memmem(buf_idx_1 + 1, buf_idx_w - (buf_idx_1 + 1), NALx_START, sizeof(NALx_START));
        if (buf_idx_tmp == NULL) {
            usleep(USLEEP);
            continue;
        } else {
            buf_idx_2 = buf_idx_tmp;
        }
//        if (debug) fprintf(stderr, "found buf_idx_2: %08x\n", (unsigned int) buf_idx_2);

        if ((write_enable) && (!sync_lost)) {
            tb = time(NULL);
            if (buf_idx_start + frame_len > addr + buf_size) {
                fwrite(buf_idx_start, 1, addr + buf_size - buf_idx_start, stdout);
                fwrite(addr + buf_offset, 1, frame_len - (addr + buf_size - buf_idx_start), stdout);
            } else {
                fwrite(buf_idx_start, 1, frame_len, stdout);
            }
            ta = time(NULL);
            if ((frame_counter - frame_counter_prev != 1) && (frame_counter_prev != -1)) {
                fprintf(stderr, "frames lost: %d\n", frame_counter - frame_counter_prev);
            }
            if (ta - tb > 3) {
                sync_lost = 1;
                fprintf(stderr, "sync lost\n");
                sleep(3);
            }
            frame_counter_prev = frame_counter;
        }

        if ((cb_memcmp(SPS4_START, buf_idx_1, sizeof(SPS4_START)) == 0) ||
                (cb_memcmp(SPS5_START, buf_idx_1, sizeof(SPS5_START)) == 0)) {
            // SPS frame
            write_enable = 1;
            sync_lost = 0;
            buf_idx_1 = cb_move(buf_idx_1, - (6 + frame_header_size));
            if (buf_idx_1[17 + data_offset] == lowres_byte) {
                frame_res = RESOLUTION_LOW;
            } else if (buf_idx_1[17 + data_offset] == highres_byte) {
                frame_res = RESOLUTION_HIGH;
            } else {
                write_enable = 0;
            }
            if (frame_res == resolution) {
                cb_memcpy((unsigned char *) &frame_len, buf_idx_1, 4);
                frame_len -= 6;                                                              // -6 only for SPS
                frame_counter = (int) buf_idx_1[18 + data_offset] + (int) buf_idx_1[19 + data_offset] * 256;
                buf_idx_1 = cb_move(buf_idx_1, 6 + frame_header_size);
                buf_idx_start = buf_idx_1;
                if (debug) fprintf(stderr, "SPS detected - frame_res: %d - frame_len: %d - frame_counter: %d\n", frame_res, frame_len, frame_counter);
            } else {
                write_enable = 0;
            }
        } else if ((cb_memcmp(PPS4_START, buf_idx_1, sizeof(PPS4_START)) == 0) ||
                        (cb_memcmp(PPS5_START, buf_idx_1, sizeof(PPS5_START)) == 0) ||
                        (cb_memcmp(VPS5_START, buf_idx_1, sizeof(VPS5_START)) == 0) ||
                        (cb_memcmp(IDR4_START, buf_idx_1, sizeof(IDR4_START)) == 0) ||
                        (cb_memcmp(IDR5_START, buf_idx_1, sizeof(IDR5_START)) == 0) ||
                        (cb_memcmp(PFR4_START, buf_idx_1, sizeof(PFR4_START)) == 0) ||
                        (cb_memcmp(PFR5_START, buf_idx_1, sizeof(PFR5_START)) == 0)) {
            // PPS, VPS, IDR and PFR frames
            write_enable = 1;
            buf_idx_1 = cb_move(buf_idx_1, -frame_header_size);
            if (buf_idx_1[17 + data_offset] == lowres_byte) {
                frame_res = RESOLUTION_LOW;
            } else if (buf_idx_1[17 + data_offset] == highres_byte) {
                frame_res = RESOLUTION_HIGH;
            } else {
                write_enable = 0;
            }
            if (frame_res == resolution) {
                cb_memcpy((unsigned char *) &frame_len, buf_idx_1, 4);
                frame_counter = (int) buf_idx_1[18 + data_offset] + (int) buf_idx_1[19 + data_offset] * 256;
                buf_idx_1 = cb_move(buf_idx_1, frame_header_size);
                buf_idx_start = buf_idx_1;
                if (debug) fprintf(stderr, "frame detected - frame_res: %d - frame_len: %d - frame_counter: %d\n", frame_res, frame_len, frame_counter);
            } else {
                write_enable = 0;
            }
        } else {
            write_enable = 0;
        }

        buf_idx_1 = buf_idx_2;
    }

    // Unreacheable path

    // Unmap file from memory
    if (munmap(addr, buf_size) == -1) {
        if (debug) fprintf(stderr, "error munmapping file");
    } else {
        if (debug) fprintf(stderr, "unmapping file %s, size %d, from %08x\n", BUFFER_FILE, buf_size, addr);
    }

    return 0;
}
