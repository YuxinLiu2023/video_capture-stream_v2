#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/statvfs.h>
#include <time.h>
#include <linux/videodev2.h>
#include <SDL2/SDL.h>

extern "C" {
  #include "libavutil/imgutils.h"
  #include "libavutil/pixfmt.h"
  #include "libavutil/avutil.h"
  #include "libswscale/swscale.h"
}

#include "capture.hh"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

const char *dev_name = "/dev/video0";
int fd = -1;
int width = 1920;
int height = 1080;
int fps = 30;
int pixel_mode = 422;
void *buffers = NULL;
unsigned n_buffers = 0;
int run = 1;

pthread_mutex_t preview_mtx = PTHREAD_MUTEX_INITIALIZER;
uint8_t *preview_rgb = NULL;
struct SwsContext *sws_ctx = NULL;
uint8_t *ff_in_data = NULL;
int ff_in_linesize = 0;
uint8_t *ff_out_data = NULL;
int ff_out_linesize[4];
struct SwsContext *sws_preview_ctx = NULL;
uint8_t *prev_in_data = NULL;
int prev_in_linesize = 0;
uint8_t *prev_out_data = NULL;
int prev_out_linesize = 0;

const int preview_w = 640;
const int preview_h = 480;

// Frame ring definition
YUV420PFrame frame_ring[FRAME_RING_SIZE];
int frame_ring_head = 0;
int frame_ring_tail = 0;
pthread_mutex_t frame_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t frame_available = PTHREAD_COND_INITIALIZER;
size_t yuv_frame_size = 0;

void sigint_handler(int s) {
  (void)s;
  run = 0;
}

void set_format() {
  struct v4l2_format fmt;
  CLEAR(fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = width;
  fmt.fmt.pix.height = height;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    perror("VIDIOC_S_FMT");
    exit(1);
  }

  struct v4l2_streamparm parm;
  CLEAR(parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = fps;
  if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
    perror("VIDIOC_S_PARM");
    exit(1);
  }
}

void init_mmap() {
  struct v4l2_requestbuffers req;
  CLEAR(req);
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    perror("VIDIOC_REQBUFS");
    exit(1);
  }

  n_buffers = req.count;
  buffers = calloc(n_buffers, sizeof(void*));

  for (unsigned i = 0; i < n_buffers; i++) {
    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    ioctl(fd, VIDIOC_QUERYBUF, &buf);
    buffers = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if (buffers == MAP_FAILED) {
      perror("mmap");
      exit(1);
    }
    ioctl(fd, VIDIOC_QBUF, &buf);
  }
}

int low_space() {
  struct statvfs s;
  if (statvfs(".", &s) < 0) return 0;
  unsigned long long free_b = s.f_bavail * (unsigned long long)s.f_frsize;
  return free_b < (1ULL << 30);
}

void *preview_thread(void *arg) {
  (void)arg;
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *win = SDL_CreateWindow("Preview", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, preview_w, preview_h, 0);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, preview_w, preview_h);

  SDL_Event ev;
  while (run) {
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q) run = 0;
    }

    pthread_mutex_lock(&preview_mtx);
    if (preview_rgb) {
      SDL_UpdateTexture(tex, NULL, preview_rgb, preview_w * 2);
      pthread_mutex_unlock(&preview_mtx);

      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, NULL, NULL);
      SDL_RenderPresent(ren);
    } else {
      pthread_mutex_unlock(&preview_mtx);
      SDL_Delay(33);
    }
  }

  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return NULL;
}

// ------------------ Disk Recording Capture Loop ------------------
void capture_disk_loop(const char *fname) {
  pthread_t tid;
  pthread_create(&tid, NULL, preview_thread, NULL);
  FILE *out = fopen(fname, "wb");
  if (!out) { perror("fopen"); exit(1); }
  fprintf(out, "YUV4MPEG2 W%d H%d F%d:1 Ip A0:0\n", width, height, fps);

  ff_in_linesize = width * 2;
  ff_in_data = (uint8_t *) av_malloc(ff_in_linesize * height);
  ff_out_linesize[0] = width;
  ff_out_linesize[1] = width / 2;
  ff_out_linesize[2] = width / 2;
  size_t ysz = width * height;
  size_t usz = ysz / 4;
  ff_out_data = (uint8_t *) av_malloc(ysz + 2 * usz);

  sws_ctx = sws_getContext(width, height, AV_PIX_FMT_YUYV422, width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

  struct pollfd pfd = { fd, POLLIN, 0 };
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd, VIDIOC_STREAMON, &type);
  
  time_t last_flush = time(NULL);

  while (run) {
    if (low_space()) break;
    if (poll(&pfd, 1, 1000) < 0) continue;

    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_DQBUF, &buf);
    uint8_t *data = (uint8_t *)buffers + buf.m.offset;

    memcpy(ff_in_data, data, ff_in_linesize * height);
    const uint8_t *in[1] = { ff_in_data };
    int in_ls[1] = { ff_in_linesize };
    uint8_t *out_planes[3] = {
      ff_out_data,
      ff_out_data + ysz,
      ff_out_data + ysz + usz
    };
    sws_scale(sws_ctx, in, in_ls, 0, height, out_planes, ff_out_linesize);

    fprintf(out, "FRAME\n");
    fwrite(out_planes[0], 1, ysz, out);
    fwrite(out_planes[1], 1, usz, out);
    fwrite(out_planes[2], 1, usz, out);

    ioctl(fd, VIDIOC_QBUF, &buf);
    if (difftime(time(NULL), last_flush) >= 5.0) { fflush(out); last_flush = time(NULL); }
  }

  int type_off = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd, VIDIOC_STREAMOFF, &type_off);

  fclose(out);
  pthread_join(tid, NULL);
  sws_freeContext(sws_ctx);
  av_free(ff_in_data);
  av_free(ff_out_data);
}

// ------------------ Streaming-Oriented Capture Loop ------------------
void capture_streaming_loop() {

  // Initialize buffers for preview
  prev_in_linesize = width * 2;
  prev_out_linesize = preview_w * 2;
  prev_in_data = (uint8_t *) av_malloc(prev_in_linesize * height);
  prev_out_data = (uint8_t *) av_malloc(prev_out_linesize * preview_h);
  preview_rgb = (uint8_t *) malloc(preview_w * preview_h * 2);

  sws_preview_ctx = sws_getContext(
    width, height, AV_PIX_FMT_YUYV422, 
    preview_w, preview_h, AV_PIX_FMT_RGB565, 
    SWS_BILINEAR, NULL, NULL, NULL);

  // Initilize buffers for YUV420P conversion
  ff_in_linesize     = width * 2;
  ff_in_data         = (uint8_t *)av_malloc(ff_in_linesize * height);
  ff_out_linesize[0] = width;       // Y plane pitch = width
  ff_out_linesize[1] = width / 2;   // U plane pitch
  ff_out_linesize[2] = width / 2;   // V plane pitch
  // ff_out_data        = (uint8_t *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1));
  yuv_frame_size = width * height * 3 / 2;
  ff_out_data        = (uint8_t *)av_malloc(yuv_frame_size);

  // Initialize swscale context for YUYV422 → YUV420P
  sws_ctx = sws_getContext(
    width, height, AV_PIX_FMT_YUYV422, 
    width, height, AV_PIX_FMT_YUV420P, 
    SWS_BILINEAR, NULL, NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, preview_thread, NULL);

  // Start streaming on the V4L2 device
  struct pollfd pfd = { fd, POLLIN, 0 };
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd, VIDIOC_STREAMON, &type);

  while (run) {
    if (low_space()){
      fprintf(stderr, "Disk <1GiB, stopping.\n");
      break;
    }

    if (poll(&pfd, 1, 1000) < 0) continue;

    // Dequeue the next buffer containing a captured frame
    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_DQBUF, &buf);

    // Pointer to raw YUYV data from mapped buffers
    uint8_t *data = (uint8_t *)buffers + buf.m.offset;

    // --- Preview conversion: YUYV422 → RGB565 (scaled to 640×480) ---
    memcpy(prev_in_data, data, prev_in_linesize * height);
    const uint8_t *in[1] = { prev_in_data };
    int in_ls[1] = { prev_in_linesize };
    uint8_t *out[1] = { prev_out_data };
    int out_ls[1] = { prev_out_linesize };

    sws_scale(sws_preview_ctx, in, in_ls, 0, height, out, out_ls);
    pthread_mutex_lock(&preview_mtx);
    memcpy(preview_rgb, prev_out_data, preview_w * preview_h * 2);
    pthread_mutex_unlock(&preview_mtx);

    // YUV420P conversion
    memcpy(ff_in_data, data, ff_in_linesize * height);
    const uint8_t *src_planes[1] = { ff_in_data };
    int src_stride[1] = { ff_in_linesize };

    size_t y_sz = ff_out_linesize[0] * height;
    size_t u_sz = ff_out_linesize[1] * (height / 2);
    uint8_t *out_planes[3] = {
      ff_out_data,
      ff_out_data + y_sz,
      ff_out_data + y_sz + u_sz
    };

    sws_scale(
        sws_ctx,
        src_planes, src_stride,
        0, height,
        out_planes, ff_out_linesize
    );

    pthread_mutex_lock(&frame_ring[frame_ring_head].lock);
    if (!frame_ring[frame_ring_head].ready) {
      memcpy(frame_ring[frame_ring_head].data, ff_out_data, yuv_frame_size);
      frame_ring[frame_ring_head].size = yuv_frame_size;
      frame_ring[frame_ring_head].ready = true;

      pthread_mutex_lock(&frame_ring_mutex);
      frame_ring_head = (frame_ring_head + 1) % FRAME_RING_SIZE;
      pthread_cond_signal(&frame_available);
      pthread_mutex_unlock(&frame_ring_mutex);
    }
    pthread_mutex_unlock(&frame_ring[frame_ring_head].lock);

    ioctl(fd, VIDIOC_QBUF, &buf);
  }

  int type_off = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd, VIDIOC_STREAMOFF, &type_off);

  pthread_join(tid, NULL);
  sws_freeContext(sws_preview_ctx);
  av_free(prev_in_data);
  av_free(prev_out_data);
  free(preview_rgb);
}
