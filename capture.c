#include <stdint.h>           // for uint8_t and fixed-width integer types
#include <stdio.h>            // for I/O functions (printf, fprintf, fopen, etc.)
#include <stdlib.h>           // for malloc, calloc, av_malloc, exit, atoi, etc.
#include <string.h>           // for memset, memcpy, strcmp
#include <unistd.h>           // for getopt, close, sleep, etc.
#include <fcntl.h>            // for open flags (O_RDWR, O_NONBLOCK)
#include <errno.h>            // for errno definitions
#include <signal.h>           // for signal handling (SIGINT)
#include <pthread.h>          // for pthreads (thread creation, mutex)
#include <sys/ioctl.h>        // for ioctl system call (VIDIOC_*)
#include <sys/mman.h>         // for mmap, PROT_READ, PROT_WRITE, MAP_SHARED
#include <sys/poll.h>         // for poll, struct pollfd
#include <sys/statvfs.h>      // for statvfs, checking free disk space
#include <time.h>             // for time(), localtime(), strftime()
#include <linux/videodev2.h>  // for V4L2 definitions (v4l2_format, v4l2_buffer, etc.)
#include <SDL2/SDL.h>         // for SDL video preview (window, renderer, texture)

#include <libavutil/imgutils.h> // for av_image_get_buffer_size
#include <libavutil/pixfmt.h>   // for AVPixelFormat constants
#include <libavutil/avutil.h>   // for av_malloc, av_free
#include <libswscale/swscale.h> // for sws_getContext, sws_scale, sws_freeContext

#define CLEAR(x) memset(&(x), 0, sizeof(x))

// ---------------------- Globals & Config ----------------------

// Video device path (e.g., /dev/video0)
static const char *dev_name   = "/dev/video0";

// File descriptor for opened video device
static int fd = -1;

// Desired capture resolution and framerate
static int width      = 1920;
static int height     = 1080;
static int fps        = 30;

// Pixel format mode: 422 = YUY2 (no conversion), 420 = YUV420 (needs conversion)
static int pixel_mode = 422;

// Pointer to mmap'd buffer region for V4L2
static void *buffers = NULL;
static unsigned n_buffers = 0;

// Flag to indicate capture/preview loop should keep running
static int run = 1;

// Mutex to protect preview buffer access (RGB565 data)
static pthread_mutex_t preview_mtx = PTHREAD_MUTEX_INITIALIZER;

// Buffer holding converted preview frame (RGB565, 640×480)
static uint8_t *preview_rgb = NULL;

// FFmpeg libswscale context and buffers for writing YUV420P to disk
static struct SwsContext *sws_ctx = NULL;
static uint8_t           *ff_in_data;      // raw input buffer (YUYV) for conversion
static int                ff_in_linesize;  // number of bytes per input row
static uint8_t           *ff_out_data;     // output buffer (packed YUV420P)
static int                ff_out_linesize[4]; // output strides: Y, U, V

// Additional swscale context for preview (convert YUYV→RGB565, scaled to 640×480)
static struct SwsContext *sws_preview_ctx = NULL;
static uint8_t           *prev_in_data;     // raw input buffer for preview
static int                prev_in_linesize; // bytes per row of preview input (YUYV)
static uint8_t           *prev_out_data;    // output buffer for preview (RGB565)
static int                prev_out_linesize; // bytes per row of preview output (RGB565)

static const int preview_w = 640;  // preview width
static const int preview_h = 480;  // preview height

// Signal handler for SIGINT (Ctrl+C) to gracefully stop capture
static void sigint_handler(int s)
    {
        (void)s;   // suppress unused parameter warning
        run = 0;   // set flag to terminate loops
    }

// ---------------------- V4L2 Format & Parameters ----------------------

// Configure capture format: resolution, pixel format, field (progressive), etc.
void set_format()
    {
        struct v4l2_format fmt;
        CLEAR(fmt);  // zero-initialize the structure

        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = width;
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;   // request YUYV (YUY2) format
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;     // progressive scan

        // Issue ioctl to set the format on the device
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
            {
                perror("VIDIOC_S_FMT");
                exit(1);
            }

        // Also set the framerate (timeperframe)
        struct v4l2_streamparm parm;
        CLEAR(parm);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = fps;

        if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0)
            {
                perror("VIDIOC_S_PARM");
                exit(1);
            }
    }

// ---------------------- Memory Mapping ----------------------

// Request and mmap buffers for V4L2 streaming (MMAP mode)
void init_mmap()
    {
        struct v4l2_requestbuffers req;
        CLEAR(req);
        req.count  = 4;                          // request 4 frame buffers
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;          // use memory mapping

        // Issue ioctl to request buffers
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
            {
                perror("VIDIOC_REQBUFS");
                exit(1);
            }

        // Number of buffers actually allocated by driver
        n_buffers = req.count;
        buffers   = calloc(n_buffers, sizeof(void*));

        // For each buffer, query its information and mmap it
        for (unsigned i = 0; i < n_buffers; i++)
            {
                struct v4l2_buffer buf;
                CLEAR(buf);
                buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index  = i;

                // Get buffer offset/length info
                ioctl(fd, VIDIOC_QUERYBUF, &buf);

                // Map the buffer into user space
                buffers = mmap(
                    NULL,
                    buf.length,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    fd,
                    buf.m.offset
                );

                if (buffers == MAP_FAILED)
                    {
                        perror("mmap");
                        exit(1);
                    }

                // Queue the buffer to the driver to be filled
                ioctl(fd, VIDIOC_QBUF, &buf);
            }
    }

// ---------------------- Disk Space Check ----------------------

// Returns 1 if free space < 1 GiB, else 0.
// Used to automatically stop capture if disk is nearly full.
int low_space()
    {
        struct statvfs s;
        if (statvfs(".", &s) < 0)
            return 0;
        unsigned long long free_b = s.f_bavail * (unsigned long long)s.f_frsize;
        return free_b < (1ULL << 30);
    }

// ---------------------- Preview Thread ----------------------

// Thread function: initialize SDL, create window/renderer/texture, 
// and continuously display the latest preview frame until 'run' is cleared.
void *preview_thread(void *arg)
    {
        (void)arg;  // suppress unused parameter warning

        // Initialize SDL video subsystem
        SDL_Init(SDL_INIT_VIDEO);

        // Create an SDL window of size preview_w × preview_h
        SDL_Window *win = SDL_CreateWindow(
            "Preview",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            preview_w,
            preview_h,
            0
        );

        // Create a renderer for the window (hardware-accelerated if possible)
        SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);

        // Create a streaming texture with pixel format RGB565
        SDL_Texture *tex = SDL_CreateTexture(
            ren,
            SDL_PIXELFORMAT_RGB565,
            SDL_TEXTUREACCESS_STREAMING,
            preview_w,
            preview_h
        );

        SDL_Event ev;
        while (run)  // run until 'run' is set to 0
            {
                // Handle SDL events (e.g., keypress to quit)
                while (SDL_PollEvent(&ev))
                    {
                        if (
                            ev.type == SDL_KEYDOWN &&
                            ev.key.keysym.sym == SDLK_q
                        )
                            {
                                run = 0;
                            }
                    }

                // Lock mutex and copy preview_rgb into texture, if available
                pthread_mutex_lock(&preview_mtx);
                if (preview_rgb)
                    {
                        SDL_UpdateTexture(
                            tex,
                            NULL,
                            preview_rgb,
                            preview_w * 2  // pitch = width×bytes-per-pixel (2 bytes for RGB565)
                        );
                        pthread_mutex_unlock(&preview_mtx);

                        SDL_RenderClear(ren);
                        SDL_RenderCopy(ren, tex, NULL, NULL);
                        SDL_RenderPresent(ren);
                    }
                else
                    {
                        pthread_mutex_unlock(&preview_mtx);
                        SDL_Delay(33);  // ~30 FPS idle delay
                    }
            }

        // Cleanup SDL resources
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return NULL;
    }

// ---------------------- Capture & Conversion Loop ----------------------

// Main capture loop: capture raw frames from V4L2, optionally convert from
// YUYV422→YUV420P for disk, and from YUYV422→RGB565 for preview.
void capture_loop(const char *fname)
    {
        // Prepare to poll the file descriptor for readiness
        struct pollfd pfd = { fd, POLLIN, 0 };

        // Open output file (YUV4MPEG2 format) for writing frames
        FILE *out = fopen(fname, "wb");
        if (!out)
            {
                perror("fopen");
                exit(1);
            }

        // Write YUV4MPEG2 header (resolution, framerate, etc.)
        fprintf(
            out,
            "YUV4MPEG2 W%d H%d F%d:1 Ip A0:0\n",
            width,
            height,
            fps
        );

        // Allocate preview buffer (RGB565, 2 bytes per pixel)
        preview_rgb    = malloc(preview_w * preview_h * 2);
        prev_in_linesize  = width * 2;   // input (YUYV) has 2 bytes/pixel
        prev_in_data      = av_malloc(prev_in_linesize * height);
        prev_out_linesize = preview_w * 2;  // output (RGB565) also 2 bytes/pixel
        prev_out_data     = av_malloc(prev_out_linesize * preview_h);

        // Initialize swscale context for preview (YUYV422→RGB565, scaled to 640×480)
        sws_preview_ctx = sws_getContext(
            width,
            height,
            AV_PIX_FMT_YUYV422,
            preview_w,
            preview_h,
            AV_PIX_FMT_RGB565,
            SWS_BILINEAR,
            NULL,
            NULL,
            NULL
        );

        // Allocate FFmpeg input buffer (raw YUYV) and output for YUV420P
        ff_in_linesize     = width * 2;
        ff_in_data         = av_malloc(ff_in_linesize * height);
        ff_out_linesize[0] = width;       // Y plane pitch = width
        ff_out_linesize[1] = width / 2;   // U plane pitch = width/2
        ff_out_linesize[2] = width / 2;   // V plane pitch = width/2
        ff_out_data        = av_malloc(
            av_image_get_buffer_size(
                AV_PIX_FMT_YUV420P,
                width,
                height,
                1
            )
        );

        // Initialize swscale context for disk write (YUYV422→YUV420P, same size)
        sws_ctx = sws_getContext(
            width,
            height,
            AV_PIX_FMT_YUYV422,
            width,
            height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            NULL,
            NULL,
            NULL
        );

        // Launch preview thread
        pthread_t tid;
        pthread_create(&tid, NULL, preview_thread, NULL);

        // Start streaming on the V4L2 device
        ioctl(fd, VIDIOC_STREAMON, &(int){ V4L2_BUF_TYPE_VIDEO_CAPTURE });

        time_t last_flush = time(NULL);

        // Loop until run==0 (SIGINT or 'q' pressed) or low disk space
        while (run)
            {
                if (low_space())
                    {
                        fprintf(stderr, "Disk <1GiB, stopping.\n");
                        break;
                    }

                // Poll for new frame (timeout 1000ms)
                if (poll(&pfd, 1, 1000) < 0)
                    {
                        if (errno == EINTR)
                            continue;
                        perror("poll");
                        break;
                    }

                // Dequeue the next buffer containing a captured frame
                struct v4l2_buffer buf;
                CLEAR(buf);
                buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                ioctl(fd, VIDIOC_DQBUF, &buf);

                // Pointer to raw YUYV data from mapped buffers
                uint8_t *data = (uint8_t *)buffers + buf.m.offset;

                // --- Preview conversion: YUYV422 → RGB565 (scaled to 640×480) ---
                memcpy(prev_in_data, data, prev_in_linesize * height);
                const uint8_t *in_p[1]  = { prev_in_data };
                int            in_ls[1] = { prev_in_linesize };
                uint8_t       *out_p[1] = { prev_out_data };
                int            out_ls[1] = { prev_out_linesize };

                sws_scale(
                    sws_preview_ctx,
                    in_p,
                    in_ls,
                    0,
                    height,
                    out_p,
                    out_ls
                );

                // Copy preview output into shared preview_rgb buffer
                pthread_mutex_lock(&preview_mtx);
                memcpy(
                    preview_rgb,
                    prev_out_data,
                    prev_out_linesize * preview_h
                );
                pthread_mutex_unlock(&preview_mtx);

                // --- Write one frame to disk ---
                fprintf(out, "FRAME\n");

                if (pixel_mode == 422)
                    {
                        // If pixel_mode=422, write raw YUYV422 directly
                        fwrite(data, buf.bytesused, 1, out);
                    }
                else
                    {
                        // pixel_mode=420: convert YUYV422→YUV420P before writing
                        memcpy(
                            ff_in_data,
                            data,
                            ff_in_linesize * height
                        );
                        const uint8_t *in_planes[1] = { ff_in_data };
                        int in_ls2[1] = { ff_in_linesize };

                        // Calculate plane sizes for Y (full res) and U/V (half res)
                        size_t y_sz = ff_out_linesize[0] * height;
                        size_t u_sz = ff_out_linesize[1] * (height / 2);

                        // Pointers into ff_out_data buffer where Y, U, V planes reside
                        uint8_t *out_planes[3] = {
                            ff_out_data, 
                            ff_out_data + y_sz,
                            ff_out_data + y_sz + u_sz
                        };

                        sws_scale(
                            sws_ctx,
                            in_planes,
                            in_ls2,
                            0,
                            height,
                            out_planes,
                            ff_out_linesize
                        );

                        // Write Y, then U, then V data sequentially
                        fwrite(out_planes[0], 1, y_sz, out);
                        fwrite(out_planes[1], 1, u_sz, out);
                        fwrite(out_planes[2], 1, u_sz, out);
                    }

                // Re-queue the buffer so the driver can refill it
                ioctl(fd, VIDIOC_QBUF, &buf);

                // Periodically flush file to disk every 5 seconds
                if (difftime(time(NULL), last_flush) >= 5.0)
                    {
                        fflush(out);
                        last_flush = time(NULL);
                    }
            }

        // Stop streaming and clean up
        ioctl(fd, VIDIOC_STREAMOFF, &(int){ V4L2_BUF_TYPE_VIDEO_CAPTURE });
        run = 0;
        pthread_join(tid, NULL);
        fclose(out);

        // Free libswscale contexts and buffers
        sws_freeContext(sws_ctx);
        av_free(ff_in_data);
        av_free(ff_out_data);
        sws_freeContext(sws_preview_ctx);
        av_free(prev_in_data);
        av_free(prev_out_data);
    }

// ---------------------- Main ----------------------
int main(int argc, char **argv)
    {
        int opt;
        // Parse command-line options: -w width, -h height, -r fps, -p [422|420]
        while ((opt = getopt(argc, argv, "w:h:r:p:")) != -1)
            {
                switch (opt)
                    {
                        case 'w':
                            width = atoi(optarg);
                            break;
                        case 'h':
                            height = atoi(optarg);
                            break;
                        case 'r':
                            fps = atoi(optarg);
                            break;
                        case 'p':
                            // If user specified "420", set pixel_mode=420; otherwise default to 422
                            pixel_mode = (strcmp(optarg, "420") == 0) ? 420 : 422;
                            break;
                        default:
                            fprintf(
                                stderr,
                                "Usage: %s -w width -h height -r fps -p [422|420]\n",
                                argv[0]
                            );
                            exit(1);
                    }
            }

        // ---------------------- Resolution & Framerate Validation ----------------------
        struct Tier { int w, h, max_fps; };
        static const struct Tier tiers[] = {
            { 1280,  720, 120 },
            { 1920, 1080,  60 },
            { 2000, 1500,  50 },
            { 3840, 2160,  20 },
            { 4000, 3000,  14 },
            { 8000, 6000,   3 },
        };
        static const int allowed_fps[] = { 120, 60, 50, 20, 14, 3 };
        int tier_max = 0;

        // Find max allowed FPS for the chosen resolution
        for (size_t i = 0; i < sizeof(tiers) / sizeof(*tiers); i++)
            {
                if (width <= tiers[i].w && height <= tiers[i].h)
                    {
                        tier_max = tiers[i].max_fps;
                        break;
                    }
            }
        if (!tier_max)
            {
                fprintf(
                    stderr,
                    "Unsupported resolution %dx%d\n",
                    width,
                    height
                );
                return 1;
            }

        // Check if desired FPS is allowed for this resolution
        int fps_ok = 0;
        for (size_t i = 0; i < sizeof(allowed_fps) / sizeof(*allowed_fps); i++)
            {
                if (fps == allowed_fps[i] && fps <= tier_max)
                    {
                        fps_ok = 1;
                        break;
                    }
            }
        if (!fps_ok)
            {
                fprintf(
                    stderr,
                    "Unsupported frame rate %dfps for %dx%d (max %dfps)\n",
                    fps,
                    width,
                    height,
                    tier_max
                );
                return 1;
            }

        // ---------------------- Prepare Output Filename ----------------------
        // Use timestamp to name file: YYYYMMDD_HHMMSS.y4m
        char      fname[64];
        time_t    t0 = time(NULL);
        struct tm *tm = localtime(&t0);
        strftime(fname, sizeof(fname), "%Y%m%d_%H%M%S.y4m", tm);

        // Register SIGINT handler to gracefully terminate on Ctrl+C
        signal(SIGINT, sigint_handler);

        // Open video device in non-blocking mode
        fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
        if (fd < 0)
            {
                perror("open");
                return 1;
            }

        // Set V4L2 format & parameters, initialize memory mapping, and start capture
        set_format();
        init_mmap();
        capture_loop(fname);
        close(fd);
        return 0;
    }
