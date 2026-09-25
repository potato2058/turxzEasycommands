#ifndef TURZX_H
#define TURZX_H

#include <libusb-1.0/libusb.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TURZX_VID 0x1cbe
#define TURZX_EP_OUT 0x01
#define TURZX_EP_IN 0x81
#define TURZX_PKT 512
#define TURZX_CMD 500
#define TURZX_MAX_IMAGE (1024 * 1024)

#define TURZX_CMD_SYNC 10
#define TURZX_CMD_RESTART 11
#define TURZX_CMD_BRIGHTNESS 14
#define TURZX_CMD_FRAMERATE 15
#define TURZX_CMD_H264_CHUNK_SIZE 17
#define TURZX_CMD_JPEG 101
#define TURZX_CMD_PNG 102
#define TURZX_CMD_H264 121
#define TURZX_CMD_STREAM_STATUS 122
#define TURZX_CMD_STOP 123
#define TURZX_CMD_SAVE 125

typedef struct {
    uint16_t pid;
    int native_w; /* portrait framebuffer width  */
    int native_h; /* portrait framebuffer height */
    const char *name;
} turzx_model_t;

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *handle;
    const turzx_model_t *model;
    uint8_t bus;
    uint8_t addr;
    char serial[64];
    char product[64];
} turzx_dev_t;

typedef enum {
    TURZX_ORIENT_PORTRAIT = 0,
    TURZX_ORIENT_LANDSCAPE = 1
} turzx_orient_t;

typedef enum {
    TURZX_FIT_CONTAIN = 0,
    TURZX_FIT_COVER = 1,
    TURZX_FIT_STRETCH = 2
} turzx_fit_t;

const turzx_model_t *turzx_model_from_pid(uint16_t pid);
int turzx_open(turzx_dev_t *d);
void turzx_close(turzx_dev_t *d);

int turzx_cmd(turzx_dev_t *d, uint8_t cmd, const uint8_t extra[16],
              const uint8_t *payload, size_t payload_len,
              uint8_t resp[TURZX_PKT], int timeout_ms);

int turzx_sync(turzx_dev_t *d);
int turzx_brightness(turzx_dev_t *d, int percent);
int turzx_framerate(turzx_dev_t *d, int fps);
int turzx_stop(turzx_dev_t *d);
int turzx_restart(turzx_dev_t *d);
int turzx_send_image_bytes(turzx_dev_t *d, const uint8_t *data, size_t len,
                           int jpeg);
int turzx_send_h264(turzx_dev_t *d, const uint8_t *data, size_t len, int last);
int turzx_stream_status(turzx_dev_t *d, int *depth);

int turzx_logical_size(const turzx_dev_t *d, turzx_orient_t o, int *w, int *h);

#endif
