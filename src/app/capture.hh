#ifndef CAPTURE_HH
#define CAPTURE_HH

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

void set_format();
void init_mmap();
void *preview_thread(void *arg);
void capture_loop(const char *fname);
void sigint_handler(int s);
int low_space();
void capture_disk_loop(const char *fname);
void *capture_streaming_loop(void *arg);

extern const char *dev_name;
extern int fd;
extern int width;
extern int height;
extern int fps;
extern int pixel_mode;
extern void *buffers;
extern unsigned n_buffers;
extern int run;

#ifdef __cplusplus
}
#endif

// ===== Shared ring buffer between capturing thread and streaming thread =====
constexpr int FRAME_RING_SIZE = 500;

struct YUV420PFrame {
  uint8_t *data;
  size_t size;
  bool ready;
  pthread_mutex_t lock;
};

extern YUV420PFrame frame_ring[FRAME_RING_SIZE];
extern int frame_ring_head;
extern int frame_ring_tail;
extern pthread_cond_t frame_available;
extern pthread_mutex_t frame_ring_mutex;
extern size_t yuv_frame_size;

// ===== Capture parameters structure =====
struct CaptureParams {
  int width;
  int height;
  int fps;
};


#endif // CAPTURE_HH