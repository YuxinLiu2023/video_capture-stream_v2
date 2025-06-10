#ifndef CAPTURE_HH
#define CAPTURE_HH

#include <stdint.h>

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
void capture_streaming_loop();

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

#endif // CAPTURE_HH