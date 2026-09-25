#include "turzx.h"
#include "des.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

static const uint8_t DES_KEY[8] = { 's', 'l', 'v', '3', 't', 'u', 'z', 'x' };

static const turzx_model_t MODELS[] = {
    { 0x0028, 480,  480,  "TURZX 2.8\" round" },
    { 0x0046, 320,  960,  "TURZX 4.6\"" },
    { 0x0050, 720,  1280, "TURZX 5.2\"" },
    { 0x0080, 800,  1280, "TURZX 8.0\"" },
    { 0x0088, 480,  1920, "TURZX 8.8\"" },
    { 0x0092, 462,  1920, "TURZX 9.2\"" },
    { 0x0123, 720,  1920, "TURZX 12.3\"" },
    { 0, 0, 0, NULL }
};

const turzx_model_t *turzx_model_from_pid(uint16_t pid)
{
    for (int i = 0; MODELS[i].name; i++)
        if (MODELS[i].pid == pid)
            return &MODELS[i];
    return NULL;
}

int turzx_logical_size(const turzx_dev_t *d, turzx_orient_t o, int *w, int *h)
{
    if (!d->model)
        return -1;
    if (o == TURZX_ORIENT_LANDSCAPE) {
        *w = d->model->native_h;
        *h = d->model->native_w;
    } else {
        *w = d->model->native_w;
        *h = d->model->native_h;
    }
    return 0;
}

static uint32_t midnight_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    time_t mid = mktime(&tm);
    long ms = (tv.tv_sec - mid) * 1000L + tv.tv_usec / 1000L;
    if (ms < 0)
        ms = 0;
    return (uint32_t)ms;
}

static void build_header(uint8_t pkt[TURZX_CMD], uint8_t cmd)
{
    memset(pkt, 0, TURZX_CMD);
    pkt[0] = cmd;
    pkt[2] = 0x1A;
    pkt[3] = 0x6D;
    uint32_t ms = midnight_ms();
    pkt[4] = (uint8_t)(ms);
    pkt[5] = (uint8_t)(ms >> 8);
    pkt[6] = (uint8_t)(ms >> 16);
    pkt[7] = (uint8_t)(ms >> 24);
}

static void encrypt_packet(const uint8_t cmd[TURZX_CMD], uint8_t out[TURZX_PKT])
{
    uint8_t padded[504];
    memset(padded, 0, sizeof padded);
    memcpy(padded, cmd, TURZX_CMD);
    uint8_t enc[504];
    des_cbc_encrypt(DES_KEY, DES_KEY, padded, enc, sizeof padded);
    memset(out, 0, TURZX_PKT);
    memcpy(out, enc, sizeof enc);
    out[510] = 161;
    out[511] = 26;
}

static int flush_in(turzx_dev_t *d)
{
    uint8_t buf[TURZX_PKT];
    int xfer = 0;
    for (int i = 0; i < 8; i++) {
        int r = libusb_bulk_transfer(d->handle, TURZX_EP_IN, buf, sizeof buf,
                                     &xfer, 50);
        if (r == LIBUSB_ERROR_TIMEOUT)
            return 0;
        if (r < 0)
            return 0;
    }
    return 0;
}

int turzx_cmd(turzx_dev_t *d, uint8_t cmd, const uint8_t extra[16],
              const uint8_t *payload, size_t payload_len,
              uint8_t resp[TURZX_PKT], int timeout_ms)
{
    uint8_t header[TURZX_CMD];
    uint8_t pkt[TURZX_PKT];
    build_header(header, cmd);
    if (extra)
        memcpy(header + 8, extra, 16);
    encrypt_packet(header, pkt);

    size_t total = TURZX_PKT + payload_len;
    uint8_t *buf = malloc(total);
    if (!buf)
        return LIBUSB_ERROR_NO_MEM;
    memcpy(buf, pkt, TURZX_PKT);
    if (payload_len)
        memcpy(buf + TURZX_PKT, payload, payload_len);

    int xfer = 0;
    int r = libusb_bulk_transfer(d->handle, TURZX_EP_OUT, buf, (int)total,
                                 &xfer, timeout_ms);
    free(buf);
    if (r < 0)
        return r;

    uint8_t local[TURZX_PKT];
    uint8_t *rp = resp ? resp : local;
    memset(rp, 0, TURZX_PKT);
    r = libusb_bulk_transfer(d->handle, TURZX_EP_IN, rp, TURZX_PKT, &xfer,
                             timeout_ms);
    flush_in(d);
    return r;
}

static int cmd0(turzx_dev_t *d, uint8_t cmd)
{
    return turzx_cmd(d, cmd, NULL, NULL, 0, NULL, 2000);
}

int turzx_sync(turzx_dev_t *d)
{
    return cmd0(d, TURZX_CMD_SYNC);
}

int turzx_restart(turzx_dev_t *d)
{
    return cmd0(d, TURZX_CMD_RESTART);
}

int turzx_stop(turzx_dev_t *d)
{
    return cmd0(d, TURZX_CMD_STOP);
}

int turzx_brightness(turzx_dev_t *d, int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    uint8_t extra[16] = { 0 };
    extra[0] = (uint8_t)(percent * 102 / 100);
    return turzx_cmd(d, TURZX_CMD_BRIGHTNESS, extra, NULL, 0, NULL, 2000);
}

int turzx_framerate(turzx_dev_t *d, int fps)
{
    uint8_t extra[16] = { 0 };
    extra[0] = (uint8_t)fps;
    return turzx_cmd(d, TURZX_CMD_FRAMERATE, extra, NULL, 0, NULL, 2000);
}

int turzx_send_image_bytes(turzx_dev_t *d, const uint8_t *data, size_t len,
                           int jpeg)
{
    if (len == 0 || len > TURZX_MAX_IMAGE)
        return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t extra[16] = { 0 };
    extra[0] = (uint8_t)(len >> 24);
    extra[1] = (uint8_t)(len >> 16);
    extra[2] = (uint8_t)(len >> 8);
    extra[3] = (uint8_t)len;
    uint8_t cmd = jpeg ? TURZX_CMD_JPEG : TURZX_CMD_PNG;
    int timeout = 8000 + (int)(len / 1000);
    return turzx_cmd(d, cmd, extra, data, len, NULL, timeout);
}

int turzx_send_h264(turzx_dev_t *d, const uint8_t *data, size_t len, int last)
{
    uint8_t extra[16] = { 0 };
    extra[0] = (uint8_t)(len >> 24);
    extra[1] = (uint8_t)(len >> 16);
    extra[2] = (uint8_t)(len >> 8);
    extra[3] = (uint8_t)len;
    if (last)
        extra[4] = 1;
    int timeout = 8000 + (int)(len / 1000);
    return turzx_cmd(d, TURZX_CMD_H264, extra, data, len, NULL, timeout);
}

int turzx_stream_status(turzx_dev_t *d, int *depth)
{
    uint8_t resp[TURZX_PKT];
    int r = turzx_cmd(d, TURZX_CMD_STREAM_STATUS, NULL, NULL, 0, resp, 2000);
    if (r < 0)
        return r;
    if (depth)
        *depth = resp[8];
    return 0;
}

static int read_str(libusb_device_handle *h, uint8_t idx, char *out, size_t n)
{
    out[0] = 0;
    if (!idx)
        return 0;
    int r = libusb_get_string_descriptor_ascii(h, idx, (unsigned char *)out,
                                               (int)n - 1);
    if (r < 0) {
        out[0] = 0;
        return r;
    }
    out[r] = 0;
    return 0;
}

int turzx_open(turzx_dev_t *d)
{
    memset(d, 0, sizeof *d);
    int r = libusb_init(&d->ctx);
    if (r < 0)
        return r;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(d->ctx, &list);
    if (n < 0) {
        libusb_exit(d->ctx);
        d->ctx = NULL;
        return (int)n;
    }

    libusb_device *found = NULL;
    const turzx_model_t *model = NULL;
    struct libusb_device_descriptor desc;
    for (ssize_t i = 0; i < n; i++) {
        if (libusb_get_device_descriptor(list[i], &desc) < 0)
            continue;
        if (desc.idVendor != TURZX_VID)
            continue;
        model = turzx_model_from_pid(desc.idProduct);
        found = list[i];
        break;
    }

    if (!found) {
        libusb_free_device_list(list, 1);
        libusb_exit(d->ctx);
        d->ctx = NULL;
        return LIBUSB_ERROR_NO_DEVICE;
    }

    d->bus = libusb_get_bus_number(found);
    d->addr = libusb_get_device_address(found);
    r = libusb_open(found, &d->handle);
    libusb_free_device_list(list, 1);
    if (r < 0) {
        libusb_exit(d->ctx);
        d->ctx = NULL;
        return r;
    }

    if (libusb_kernel_driver_active(d->handle, 0) == 1)
        libusb_detach_kernel_driver(d->handle, 0);

    r = libusb_claim_interface(d->handle, 0);
    if (r < 0) {
        libusb_close(d->handle);
        libusb_exit(d->ctx);
        d->handle = NULL;
        d->ctx = NULL;
        return r;
    }

    if (libusb_get_device_descriptor(libusb_get_device(d->handle), &desc) == 0) {
        read_str(d->handle, desc.iSerialNumber, d->serial, sizeof d->serial);
        read_str(d->handle, desc.iProduct, d->product, sizeof d->product);
        d->model = turzx_model_from_pid(desc.idProduct);
        if (!d->model) {
            static turzx_model_t unknown = { 0, 800, 1280, "TURZX (unknown PID)" };
            unknown.pid = desc.idProduct;
            d->model = &unknown;
        }
    } else {
        d->model = model;
    }
    return 0;
}

void turzx_close(turzx_dev_t *d)
{
    if (!d)
        return;
    if (d->handle) {
        libusb_release_interface(d->handle, 0);
        libusb_close(d->handle);
        d->handle = NULL;
    }
    if (d->ctx) {
        libusb_exit(d->ctx);
        d->ctx = NULL;
    }
}
