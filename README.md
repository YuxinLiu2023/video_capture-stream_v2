# Pipeline Version 1

Sender side: the Jetson board, Our video capturing-streaming-storage platform.

Receiver side: the server.

## Updates

On the sender side, the capture and stream parts are separated.

# Pipeline Version 2

## Description

On the sender side, bridge the capture and stream parts, make them two thread.

## Compile process

```bash
// after code update
// under video_capture-stream_v2/
./autogen.sh
./configure
make clean
make -j
```

## Running process

First, run the sender side under `src/app/` with command

```bash
./video_sender [port] -w [width] -h [height] -r [fps]
```

Then, run the receiver side under `src/app/` with command

```bash
./video_receiver [sender ip] [port] --cbr [target bitrate] --lazy 1
// [sender ip] can be checked via "ifconfig" command on the sender side.
// [port] should be set to the same value on the both side.
// [lazy] is set for decoding and display.
```

## Parameter setting
### Sender side
FPS choices = {120, 60, 50, 20, 14, 3}

| Width | Height | FPS   | Remark |
|-------|--------|-------|--------|
| 1280  | 720    | ≤ 120 | 720P   |
| 1920  | 1080   | ≤ 60  | 1080P  |
| 2000  | 1500   | ≤ 50  | 2K     |
| 3840  | 2160   | ≤ 20  | 4K     |
| 4000  | 3000   | ≤ 14  | 4:3 4K |
| 8000  | 6000   | ≤ 3   | 8K     |

The parameters are limited by V4L2.

### Receiver side
| Width | Height | FPS | Bitrate possible range |
|-------|--------|-----|------------------------|
| 1280  | 720    | 60  | 4–8 Mbps               |
| 1280  | 720    | 120 | 6–12 Mbps              |
| 1920  | 1080   | 50  | 5–10 Mbps              |
| 1920  | 1080   | 60  | 6–12 Mbps              |
| 2000  | 1500   | 50  | 8–14 Mbps              |
| 3840  | 2160   | 20  | 12–24 Mbps             |
| 4000  | 3000   | 14  | 12–24 Mbps             |
| 8000  | 6000   | 3   | 20–40 Mbps             |

## Display process

The raw y4m video file will be saved on the receiver side `src/app/data/` after the transmission. To display the video, use `ffmpeg` and `ffplay`. For example:

```bash
ffmpeg -i output_raw.y4m -c:v libx264 -preset fast -crf 23 output.mp4
ffplay output.mp4
```

# Pipeline Version 3

## Description
