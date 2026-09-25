#include "turzx.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static const char *usb_err(int r)
{
    return libusb_strerror(r);
}

static void require_ok(int r, const char *what)
{
    if (r < 0)
        die("%s: %s", what, usb_err(r));
}

static char *suffix_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path)
        return NULL;
    char *s = strdup(dot + 1);
    for (char *p = s; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return s;
}

static int is_video_ext(const char *ext)
{
    static const char *v[] = {
        "mp4", "mkv", "webm", "mov", "avi", "m4v", "h264", "264", "ts", "m2ts", NULL
    };
    for (int i = 0; v[i]; i++)
        if (strcmp(ext, v[i]) == 0)
            return 1;
    return 0;
}

static int is_gif_ext(const char *ext)
{
    return ext && (strcmp(ext, "gif") == 0 || strcmp(ext, "apng") == 0);
}

static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
}

static int run_ffmpeg(char *const argv[])
{
    int r = run_argv(argv);
    if (r == 127)
        die("ffmpeg not found in PATH");
    return r;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static char *make_tmpdir(void)
{
    char *tmpl = strdup("/tmp/turzx-XXXXXX");
    if (!mkdtemp(tmpl))
        die("mkdtemp: %s", strerror(errno));
    return tmpl;
}

static void rm_rf(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)system(cmd);
}

static const char *scale_filter(turzx_fit_t fit, int w, int h)
{
    static char buf[256];
    if (fit == TURZX_FIT_STRETCH) {
        snprintf(buf, sizeof buf, "scale=%d:%d", w, h);
    } else if (fit == TURZX_FIT_COVER) {
        snprintf(buf, sizeof buf, "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d",
                 w, h, w, h);
    } else {
        snprintf(buf, sizeof buf,
                 "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black",
                 w, h, w, h);
    }
    return buf;
}

/* Build vf that produces native portrait pixels (always what the panel wants). */
static void build_vf(char *out, size_t n, const turzx_dev_t *d, turzx_orient_t o,
                     turzx_fit_t fit)
{
    int lw, lh;
    turzx_logical_size(d, o, &lw, &lh);
    const char *sc = scale_filter(fit, lw, lh);
    if (o == TURZX_ORIENT_LANDSCAPE)
        snprintf(out, n, "%s,transpose=1", sc); /* 90° CW == PIL ROTATE_270 */
    else
        snprintf(out, n, "%s", sc);
}

static int encode_still(const turzx_dev_t *d, const char *src, const char *dst_jpg,
                        turzx_orient_t o, turzx_fit_t fit, int quality)
{
    char vf[512];
    char q[16];
    build_vf(vf, sizeof vf, d, o, fit);
    snprintf(q, sizeof q, "%d", quality);
    char *argv[] = {
        "ffmpeg", "-y", "-i", (char *)src,
        "-vf", vf,
        "-frames:v", "1",
        "-q:v", q,
        (char *)dst_jpg,
        NULL
    };
    return run_ffmpeg(argv);
}

static int send_jpeg_file(turzx_dev_t *d, const char *path)
{
    size_t len = 0;
    uint8_t *data = read_file(path, &len);
    if (!data)
        die("cannot read %s", path);
    if (len > TURZX_MAX_IMAGE) {
        free(data);
        return LIBUSB_ERROR_OVERFLOW;
    }
    int r = turzx_send_image_bytes(d, data, len, 1);
    free(data);
    return r;
}

static int send_still(turzx_dev_t *d, const char *path, turzx_orient_t o,
                      turzx_fit_t fit)
{
    char *dir = make_tmpdir();
    char jpg[512];
    snprintf(jpg, sizeof jpg, "%s/frame.jpg", dir);
    int q = 3;
    int r = -1;
    for (; q <= 12; q += 2) {
        if (encode_still(d, path, jpg, o, fit, q) != 0)
            die("ffmpeg failed to convert %s", path);
        struct stat st;
        if (stat(jpg, &st) != 0)
            die("missing encoded jpeg");
        if (st.st_size > TURZX_MAX_IMAGE) {
            fprintf(stderr, "frame %ld bytes > 1 MiB, increasing compression\n",
                    (long)st.st_size);
            continue;
        }
        r = send_jpeg_file(d, jpg);
        break;
    }
    rm_rf(dir);
    free(dir);
    return r;
}

static int encode_h264(const turzx_dev_t *d, const char *src, const char *dst,
                       turzx_orient_t o, turzx_fit_t fit, int fps)
{
    char vf[512];
    char rbuf[16];
    build_vf(vf, sizeof vf, d, o, fit);
    snprintf(rbuf, sizeof rbuf, "%d", fps);
    char *argv[] = {
        "ffmpeg", "-y", "-i", (char *)src,
        "-an",
        "-vf", vf,
        "-r", rbuf,
        "-c:v", "libx264",
        "-preset", "veryfast",
        "-profile:v", "baseline",
        "-level", "3.1",
        "-pix_fmt", "yuv420p",
        "-bsf:v", "h264_mp4toannexb",
        "-f", "h264",
        (char *)dst,
        NULL
    };
    return run_ffmpeg(argv);
}

static int send_video_file(turzx_dev_t *d, const char *h264_path, int loop)
{
    size_t len = 0;
    uint8_t *data = read_file(h264_path, &len);
    if (!data)
        die("cannot read encoded video");

    (void)turzx_cmd(d, 111, NULL, NULL, 0, NULL, 2000);
    (void)turzx_cmd(d, 112, NULL, NULL, 0, NULL, 2000);
    (void)turzx_cmd(d, 13, NULL, NULL, 0, NULL, 2000);
    (void)turzx_cmd(d, 41, NULL, NULL, 0, NULL, 2000);
    (void)turzx_framerate(d, 25);

    size_t chunk = 202752;
    uint8_t resp[TURZX_PKT];
    if (turzx_cmd(d, TURZX_CMD_H264_CHUNK_SIZE, NULL, NULL, 0, resp, 2000) == 0) {
        size_t n = ((size_t)resp[8] << 24) | ((size_t)resp[9] << 16) |
                   ((size_t)resp[10] << 8) | resp[11];
        if (n > 0 && n <= TURZX_MAX_IMAGE)
            chunk = n;
    }

    printf("streaming H.264 (%zu bytes, chunk %zu)\n", len, chunk);
    int r = 0;
    do {
        size_t off = 0;
        while (off < len && !g_stop) {
            size_t n = len - off;
            if (n > chunk)
                n = chunk;
            int last = (off + n >= len);
            r = turzx_send_h264(d, data + off, n, last);
            if (r < 0) {
                fprintf(stderr, "h264 chunk failed: %s\n", usb_err(r));
                break;
            }
            int depth = 0;
            if (turzx_stream_status(d, &depth) == 0 && depth > 3) {
                while (!g_stop) {
                    struct timespec ts = { 0, 50 * 1000 * 1000 };
                    nanosleep(&ts, NULL);
                    if (turzx_stream_status(d, &depth) < 0 || depth <= 2)
                        break;
                }
            }
            off += n;
        }
        if (r < 0 || !loop || g_stop)
            break;
        printf("loop\n");
    } while (!g_stop);

    (void)turzx_stop(d);
    free(data);
    return r;
}

static int send_gif(turzx_dev_t *d, const char *path, turzx_orient_t o,
                    turzx_fit_t fit, int fps, int loop)
{
    char *dir = make_tmpdir();
    char pattern[512];
    char vf[512];
    char fullvf[768];
    build_vf(vf, sizeof vf, d, o, fit);
    snprintf(fullvf, sizeof fullvf, "fps=%d,%s", fps, vf);
    snprintf(pattern, sizeof pattern, "%s/f%%06d.jpg", dir);
    char *argv[] = {
        "ffmpeg", "-y", "-i", (char *)path,
        "-vf", fullvf,
        "-q:v", "5",
        pattern,
        NULL
    };
    if (run_ffmpeg(argv) != 0) {
        rm_rf(dir);
        free(dir);
        die("ffmpeg failed to extract GIF frames from %s", path);
    }

    /* count frames */
    int nframes = 0;
    for (;;) {
        char f[512];
        snprintf(f, sizeof f, "%s/f%06d.jpg", dir, nframes + 1);
        if (access(f, R_OK) != 0)
            break;
        nframes++;
    }
    if (nframes == 0) {
        rm_rf(dir);
        free(dir);
        die("no frames extracted from %s", path);
    }
    printf("GIF: %d frames @ %d fps\n", nframes, fps);

    long delay_ns = 1000000000L / (fps > 0 ? fps : 10);
    int r = 0;
    do {
        for (int i = 1; i <= nframes && !g_stop; i++) {
            char f[512];
            snprintf(f, sizeof f, "%s/f%06d.jpg", dir, i);
            r = send_jpeg_file(d, f);
            if (r < 0) {
                fprintf(stderr, "frame %d failed: %s\n", i, usb_err(r));
                break;
            }
            struct timespec ts = { delay_ns / 1000000000L, delay_ns % 1000000000L };
            nanosleep(&ts, NULL);
        }
        if (r < 0 || !loop || g_stop)
            break;
    } while (!g_stop);

    rm_rf(dir);
    free(dir);
    return r;
}

static int mirror_x11(turzx_dev_t *d, turzx_orient_t o, turzx_fit_t fit, int fps,
                      const char *display, const char *region)
{
    char vf[512];
    build_vf(vf, sizeof vf, d, o, fit);
    char fpsbuf[16];
    snprintf(fpsbuf, sizeof fpsbuf, "%d", fps);
    char grab_input[128];
    char grab_size[64];
    const char *video_size = NULL;
    snprintf(grab_input, sizeof grab_input, "%s", display ? display : ":0");
    if (region) {
        int w = 0, h = 0, x = 0, y = 0;
        if (sscanf(region, "%dx%d+%d+%d", &w, &h, &x, &y) != 4 &&
            sscanf(region, "%dx%d", &w, &h) != 2)
            die("--region must be WxH or WxH+X+Y, e.g. 1280x800+0+0");
        snprintf(grab_size, sizeof grab_size, "%dx%d", w, h);
        video_size = grab_size;
        if (strchr(region, '+'))
            snprintf(grab_input, sizeof grab_input, "%s+%d,%d",
                     display ? display : ":0", x, y);
    }

    int pipefd[2];
    if (pipe(pipefd) != 0)
        die("pipe: %s", strerror(errno));

    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        char *argv[32];
        int i = 0;
        argv[i++] = "ffmpeg";
        argv[i++] = "-hide_banner";
        argv[i++] = "-nostdin";
        argv[i++] = "-f";
        argv[i++] = "x11grab";
        argv[i++] = "-framerate";
        argv[i++] = fpsbuf;
        if (video_size) {
            argv[i++] = "-video_size";
            argv[i++] = (char *)video_size;
        }
        argv[i++] = "-i";
        argv[i++] = grab_input;
        argv[i++] = "-vf";
        argv[i++] = vf;
        argv[i++] = "-q:v";
        argv[i++] = "6";
        argv[i++] = "-f";
        argv[i++] = "image2pipe";
        argv[i++] = "-vcodec";
        argv[i++] = "mjpeg";
        argv[i++] = "-";
        argv[i] = NULL;
        execvp("ffmpeg", argv);
        _exit(127);
    }
    close(pipefd[1]);

    printf("mirroring %s → %dx%d native @ %d fps (Ctrl+C to stop)\n",
           display ? display : ":0", d->model->native_w, d->model->native_h, fps);

    uint8_t *frame = malloc(TURZX_MAX_IMAGE);
    if (!frame)
        die("oom");
    size_t n = 0;
    int in_jpeg = 0;
    int prev = -1;
    int r = 0;
    uint8_t buf[4096];
    while (!g_stop) {
        ssize_t got = read(pipefd[0], buf, sizeof buf);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; i++) {
            uint8_t b = buf[i];
            if (!in_jpeg) {
                if (prev == 0xFF && b == 0xD8) {
                    in_jpeg = 1;
                    n = 0;
                    frame[n++] = 0xFF;
                    frame[n++] = 0xD8;
                }
                prev = b;
                continue;
            }
            if (n < TURZX_MAX_IMAGE)
                frame[n++] = b;
            if (n >= 2 && frame[n - 2] == 0xFF && frame[n - 1] == 0xD9) {
                r = turzx_send_image_bytes(d, frame, n, 1);
                if (r < 0)
                    fprintf(stderr, "frame send: %s\n", usb_err(r));
                in_jpeg = 0;
                prev = -1;
                n = 0;
            }
            if (n >= TURZX_MAX_IMAGE) {
                in_jpeg = 0;
                prev = -1;
                n = 0;
            }
        }
    }
    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    free(frame);
    return r;
}

static void print_usage(FILE *f)
{
    fprintf(f,
        "turzx — userspace driver for TURZX / Turing USB smart screens\n"
        "\n"
        "Usage:\n"
        "  turzx info\n"
        "  turzx sync\n"
        "  turzx brightness <0-100>\n"
        "  turzx clear\n"
        "  turzx stop\n"
        "  turzx restart\n"
        "  turzx show <file> [options]     jpeg/png/gif/webp/mp4/...\n"
        "  turzx image <file> [options]\n"
        "  turzx gif   <file> [options]\n"
        "  turzx video <file> [options]\n"
        "  turzx mirror [options]\n"
        "  turzx test\n"
        "\n"
        "Options:\n"
        "  --portrait          native portrait (e.g. 800x1280 on 8\")\n"
        "  --landscape         logical landscape (default)\n"
        "  --fit contain|cover|stretch   default: contain (letterbox)\n"
        "  --fps N             gif/video/mirror frame rate (default 10/25/10)\n"
        "  --loop              repeat gif or video until Ctrl+C\n"
        "  --display :0        X11 display for mirror (default $DISPLAY)\n"
        "  --region WxH[+X+Y]  capture a rectangle (mirror only)\n"
        "\n"
        "This panel is not a DRM/KMS monitor. The vendor protocol accepts\n"
        "JPEG/PNG stills and H.264 chunks over USB bulk. `turzx mirror`\n"
        "captures an X11/XWayland screen and pushes JPEG frames.\n"
        "\n"
        "USB access: install udev/99-turzx.rules (see README) or run as root.\n");
}

static void cmd_info(turzx_dev_t *d)
{
    printf("TURZX USB panel\n");
    printf("  product : %s\n", d->product[0] ? d->product : "(none)");
    printf("  serial  : %s\n", d->serial[0] ? d->serial : "(none)");
    printf("  usb     : bus %u addr %u  vid=1cbe pid=%04x\n",
           d->bus, d->addr, d->model->pid);
    printf("  model   : %s\n", d->model->name);
    printf("  native  : %dx%d (portrait framebuffer)\n",
           d->model->native_w, d->model->native_h);
    printf("  logical landscape: %dx%d\n",
           d->model->native_h, d->model->native_w);
    int r = turzx_sync(d);
    require_ok(r, "sync");
    printf("  sync    : ok\n");
}

static int cmd_clear(turzx_dev_t *d, turzx_orient_t o)
{
    char *dir = make_tmpdir();
    char jpg[512];
    snprintf(jpg, sizeof jpg, "%s/black.jpg", dir);
    (void)o;
    char *argv[] = {
        "ffmpeg", "-y", "-f", "lavfi", "-i", "color=c=black:s=800x1280",
        "-frames:v", "1", "-q:v", "8", jpg, NULL
    };
    /* build the lavfi size from the actual panel */
    char lavfi[64];
    snprintf(lavfi, sizeof lavfi, "color=c=black:s=%dx%d",
             d->model->native_w, d->model->native_h);
    argv[5] = lavfi;
    if (run_ffmpeg(argv) != 0)
        die("ffmpeg failed to make a black frame");
    int r = send_jpeg_file(d, jpg);
    rm_rf(dir);
    free(dir);
    return r;
}

static int cmd_test(turzx_dev_t *d, turzx_orient_t o)
{
    char *dir = make_tmpdir();
    char png[512];
    snprintf(png, sizeof png, "%s/test.png", dir);
    int lw, lh;
    turzx_logical_size(d, o, &lw, &lh);
    char size[32];
    snprintf(size, sizeof size, "%dx%d", lw, lh);
    char *argv[] = {
        "ffmpeg", "-y", "-f", "lavfi", "-i", "smptebars=size=1280x800:rate=1",
        "-frames:v", "1", png, NULL
    };
    char lavfi[80];
    snprintf(lavfi, sizeof lavfi, "smptebars=size=%s:rate=1", size);
    argv[5] = lavfi;
    if (run_ffmpeg(argv) != 0)
        die("ffmpeg failed to make a test pattern");
    int r = send_still(d, png, o, TURZX_FIT_STRETCH);
    rm_rf(dir);
    free(dir);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        print_usage(stdout);
        return argc < 2 ? 1 : 0;
    }

    const char *cmd = argv[1];
    turzx_orient_t orient = TURZX_ORIENT_LANDSCAPE;
    turzx_fit_t fit = TURZX_FIT_CONTAIN;
    int fps = 0;
    int loop = 0;
    const char *display = getenv("DISPLAY");
    const char *region = NULL;
    const char *file = NULL;
    int brightness = -1;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--portrait"))
            orient = TURZX_ORIENT_PORTRAIT;
        else if (!strcmp(argv[i], "--landscape"))
            orient = TURZX_ORIENT_LANDSCAPE;
        else if (!strcmp(argv[i], "--loop"))
            loop = 1;
        else if (!strcmp(argv[i], "--fit") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "contain"))
                fit = TURZX_FIT_CONTAIN;
            else if (!strcmp(argv[i], "cover"))
                fit = TURZX_FIT_COVER;
            else if (!strcmp(argv[i], "stretch"))
                fit = TURZX_FIT_STRETCH;
            else
                die("unknown --fit %s", argv[i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--display") && i + 1 < argc)
            display = argv[++i];
        else if (!strcmp(argv[i], "--region") && i + 1 < argc)
            region = argv[++i];
        else if (argv[i][0] == '-')
            die("unknown option %s", argv[i]);
        else if (!file && strcmp(cmd, "brightness") == 0)
            brightness = atoi(argv[i]);
        else if (!file)
            file = argv[i];
        else
            die("unexpected argument %s", argv[i]);
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    turzx_dev_t dev;
    int r = turzx_open(&dev);
    if (r == LIBUSB_ERROR_ACCESS) {
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n < 0)
            snprintf(self, sizeof self, "%s", argv[0]);
        else
            self[n] = 0;
        fprintf(stderr,
                "permission denied opening TURZX USB device (1cbe:*)\n"
                "sudo does not search your user PATH, so use the full path:\n"
                "  sudo %s %s\n"
                "Permanent (after updating udev/99-turzx.rules):\n"
                "  sudo cp udev/99-turzx.rules /etc/udev/rules.d/\n"
                "  sudo udevadm control --reload-rules\n"
                "  sudo udevadm trigger -s usb -a idVendor=1cbe\n",
                self, cmd);
        return 1;
    }
    if (r == LIBUSB_ERROR_NO_DEVICE)
        die("no TURZX USB device found (vendor 1cbe)");
    require_ok(r, "open");

    int rc = 0;
    if (!strcmp(cmd, "info")) {
        cmd_info(&dev);
    } else if (!strcmp(cmd, "sync")) {
        require_ok(turzx_sync(&dev), "sync");
        printf("sync ok\n");
    } else if (!strcmp(cmd, "brightness")) {
        if (brightness < 0)
            die("usage: turzx brightness <0-100>");
        require_ok(turzx_brightness(&dev, brightness), "brightness");
        printf("brightness %d%%\n", brightness);
    } else if (!strcmp(cmd, "stop")) {
        require_ok(turzx_stop(&dev), "stop");
        printf("stopped\n");
    } else if (!strcmp(cmd, "restart")) {
        require_ok(turzx_restart(&dev), "restart");
        printf("restart sent\n");
    } else if (!strcmp(cmd, "clear")) {
        require_ok(cmd_clear(&dev, orient), "clear");
        printf("cleared\n");
    } else if (!strcmp(cmd, "test")) {
        require_ok(cmd_test(&dev, orient), "test");
        printf("test pattern sent\n");
    } else if (!strcmp(cmd, "image") || !strcmp(cmd, "show") ||
               !strcmp(cmd, "gif") || !strcmp(cmd, "video")) {
        if (!file)
            die("usage: turzx %s <file>", cmd);
        char *ext = suffix_of(file);
        int as_gif = !strcmp(cmd, "gif") || (ext && is_gif_ext(ext) && strcmp(cmd, "image"));
        int as_vid = !strcmp(cmd, "video") || (ext && is_video_ext(ext) && strcmp(cmd, "image") && strcmp(cmd, "gif"));
        if (!strcmp(cmd, "show")) {
            as_gif = ext && is_gif_ext(ext);
            as_vid = ext && is_video_ext(ext);
        }
        free(ext);

        require_ok(turzx_sync(&dev), "sync");
        (void)turzx_brightness(&dev, 40);

        if (as_vid) {
            if (fps <= 0)
                fps = 25;
            char *dir = make_tmpdir();
            char h264[512];
            snprintf(h264, sizeof h264, "%s/out.h264", dir);
            printf("encoding %s → H.264 %s...\n", file,
                   orient == TURZX_ORIENT_LANDSCAPE ? "landscape" : "portrait");
            if (encode_h264(&dev, file, h264, orient, fit, fps) != 0) {
                rm_rf(dir);
                free(dir);
                die("ffmpeg failed to encode %s", file);
            }
            r = send_video_file(&dev, h264, loop);
            rm_rf(dir);
            free(dir);
            require_ok(r, "video");
            printf("video done\n");
        } else if (as_gif) {
            if (fps <= 0)
                fps = 10;
            r = send_gif(&dev, file, orient, fit, fps, loop);
            require_ok(r, "gif");
        } else {
            r = send_still(&dev, file, orient, fit);
            require_ok(r, "image");
            printf("image sent\n");
        }
    } else if (!strcmp(cmd, "mirror")) {
        if (fps <= 0)
            fps = 10;
        require_ok(turzx_sync(&dev), "sync");
        (void)turzx_brightness(&dev, 40);
        r = mirror_x11(&dev, orient, fit, fps, display, region);
        if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED)
            fprintf(stderr, "mirror ended: %s\n", usb_err(r));
    } else {
        turzx_close(&dev);
        die("unknown command '%s' (try --help)", cmd);
    }

    turzx_close(&dev);
    return rc;
}
