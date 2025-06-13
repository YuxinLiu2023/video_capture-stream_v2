# Pipeline Version 1

Sender side: Jetson board, Ringmaster

Receiver side: the server

## Description

Now the capture and stream parts are separated.

# Pipeline Version 2

## Description

Update the sender side, bridge the capture and stream parts, make them two thread.

## Compile process

```bash
// after code update
// under video_capture-stream_v2/
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
```

## Display process

The raw y4m video file will be saved on the receiver side `src/app/data/` after the transmission. To display the video, use `ffmpeg` and `ffplay`.
