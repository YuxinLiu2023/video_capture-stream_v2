# Pipeline Version 1
## Platform Structure

Sender side: Jetson board running our real-time video capture–encoding–streaming–storage platform.

Receiver side: Server receiving and storing the streamed video.

## Updates

[x] Separated the capture and streaming components on the sender side.

# Pipeline Version 2
## Updates

[x] Integrated the capture and streaming components on the sender side and implemented them as two separate threads.

## Compilation Process

```bash
# After updating the code
# Under video_capture-stream_v2/
./autogen.sh
./configure
make clean
make -j
```

## Running Process

First, run the sender side under `src/app/` with command:

```bash
./video_sender [port] -w [width] -h [height] -r [fps]
```

Then, run the receiver side under `src/app/` with command:

```bash
./video_receiver [sender ip] [port] --cbr [target bitrate] --lazy 1
```
Notes:
- `[sender_ip]` can be obtained using `ifconfig` on the sender.
- `[port]` must match on both sender and receiver.
- `--lazy` enables decoding and display optimizations.

## Parameter Settings
### Sender Side (V4L2-limited)
FPS choices = {120, 60, 50, 20, 14, 3}

| Width | Height | FPS   | Remark |
|-------|--------|-------|--------|
| 1280  | 720    | ≤ 120 | 720P   |
| 1920  | 1080   | ≤ 60  | 1080P  |
| 2000  | 1500   | ≤ 50  | 2K     |
| 3840  | 2160   | ≤ 20  | 4K     |
| 4000  | 3000   | ≤ 14  | 4:3 4K |
| 8000  | 6000   | ≤ 3   | 8K     |

### Receiver Side (Practical Bitrate Ranges)
| Width | Height | FPS | Bitrate Range |
|-------|--------|-----|------------------------|
| 1280  | 720    | 60  | 4–8 Mbps               |
| 1280  | 720    | 120 | 6–12 Mbps              |
| 1920  | 1080   | 50  | 5–10 Mbps              |
| 1920  | 1080   | 60  | 6–12 Mbps              |
| 2000  | 1500   | 50  | 8–14 Mbps              |
| 3840  | 2160   | 20  | 12–24 Mbps             |
| 4000  | 3000   | 14  | 12–24 Mbps             |
| 8000  | 6000   | 3   | 20–40 Mbps             |

## Display Process

The raw Y4M video file is saved on the receiver side under `src/app/data/`. To convert and display the video, use `ffmpeg` and `ffplay`. For example:

```bash
ffmpeg -i output_raw.y4m -c:v libx264 -preset fast -crf 23 output.mp4
ffplay output.mp4
```

# Pipeline Version 3
## Updates (Planned)
[ ] Integrate an ACK-based adaptive bitrate algorithm; design bitrate adaptation strategy based on actual throughput and RTT.
[ ] Extend the platform to support multi-threaded encoding.
[ ] Add support for direct storage without decoding on the receiver side.

# Comparison with Ringmaster
- Ringmaster only supports video streaming, whereas our platform additionally enables real-time video capture from cameras, along with integrated encoding and streaming.
- Our platform supports separate threads for video capture and encoding, and we will further extend it to support multi-threaded encoding.
- An ACK-based adaptive bitrate (ABR) algorithm is implemented in our platform.
- Our platform will be further extended to support GPU-based encoding.
