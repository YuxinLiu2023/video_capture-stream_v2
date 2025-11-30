#include <sys/sysinfo.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <algorithm>
//**********Yuxin: add package***********
#include <fstream>
#include <vector>
#include <iostream>
#include <string>
#include <iomanip>
// **************************************
#include "decoder.hh"
#include "exception.hh"
#include "conversion.hh"
#include "image.hh"
#include "timestamp.hh"

using namespace std;
using namespace chrono;

Frame::Frame(const uint32_t frame_id,
             const FrameType frame_type,
             const uint16_t frag_cnt)
  : id_(frame_id), type_(frame_type), frags_(frag_cnt), null_frags_(frag_cnt)
{
  if (frag_cnt == 0) {
    throw runtime_error("frame cannot have zero fragments");
  }
}

bool Frame::has_frag(const uint16_t frag_id) const
{
  return frags_.at(frag_id).has_value();
}

Datagram & Frame::get_frag(const uint16_t frag_id)
{
  return frags_.at(frag_id).value();
}

const Datagram & Frame::get_frag(const uint16_t frag_id) const
{
  return frags_.at(frag_id).value();
}

optional<size_t> Frame::frame_size() const
{
  if (not complete()) {
    return nullopt;
  }

  return frame_size_;
}

void Frame::validate_datagram(const Datagram & datagram) const
{
  if (datagram.frame_id != id_ or
      datagram.frame_type != type_ or
      datagram.frag_id >= frags_.size() or
      datagram.frag_cnt != frags_.size()) {
    throw runtime_error("unable to insert an incompatible datagram");
  }
}

void Frame::insert_frag(const Datagram & datagram)
{
  validate_datagram(datagram);

  // insert only if the datagram does not exist yet
  if (not frags_[datagram.frag_id]) {
    frame_size_ += datagram.payload.size();
    null_frags_--;
    frags_[datagram.frag_id] = datagram;
  }
}

void Frame::insert_frag(Datagram && datagram)
{
  validate_datagram(datagram);

  // insert only if the datagram does not exist yet
  if (not frags_[datagram.frag_id]) {
    frame_size_ += datagram.payload.size();
    null_frags_--;
    frags_[datagram.frag_id] = move(datagram);
  }
}

Decoder::Decoder(const uint16_t display_width,
                 const uint16_t display_height,
                 const int lazy_level,
                 const uint16_t frame_rate,
                 const string & output_path)
  : display_width_(display_width), display_height_(display_height),
    lazy_level_(), frame_rate_(frame_rate),
    output_fd_(), decoder_epoch_(steady_clock::now())
{
  // validate lazy level
  if (lazy_level < DECODE_DISPLAY or lazy_level > NO_DECODE_DISPLAY) {
    throw runtime_error("Invalid lazy level: " + to_string(lazy_level));
  }
  lazy_level_ = static_cast<LazyLevel>(lazy_level);

  // both main and worker threads start from the same time for stats output
  last_stats_time_ = decoder_epoch_;

  // open the output file
  if (not output_path.empty()) {
    output_fd_ = FileDescriptor(check_syscall(
        open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)));
  }

  // start the worker thread only if we are going to decode or display frames
  if (lazy_level <= DECODE_ONLY) {
    worker_ = thread(&Decoder::worker_main, this);
    cerr << "Spawned a new thread for decoding and displaying frames" << endl;
  }
}

bool Decoder::add_datagram_common(const Datagram & datagram)
{
  const auto frame_id = datagram.frame_id;
  const auto frame_type = datagram.frame_type;
  const auto frag_cnt = datagram.frag_cnt;

  // ignore any datagrams from the old frames
  if (frame_id < next_frame_) {
    return false;
  }

  if (not frame_buf_.count(frame_id)) {
    // initialize a Frame instance for frame 'frame_id'
    frame_buf_.emplace(piecewise_construct,
                       forward_as_tuple(frame_id),
                       forward_as_tuple(frame_id, frame_type, frag_cnt));
  }

  return true;
}

void Decoder::add_datagram(const Datagram & datagram)
{
  if (not add_datagram_common(datagram)) {
    return;
  }

  // copy the fragment into the frame
  frame_buf_.at(datagram.frame_id).insert_frag(datagram);
}

void Decoder::add_datagram(Datagram && datagram)
{
  if (not add_datagram_common(datagram)) {
    return;
  }

  // move the fragment into the frame
  frame_buf_.at(datagram.frame_id).insert_frag(move(datagram));
}

bool Decoder::next_frame_complete()
{
  {
    // check if the next frame to expect is complete
    auto it = as_const(frame_buf_).find(next_frame_);
    if (it != frame_buf_.end() and it->second.complete()) {
      return true;
    }
  }

  // seek forward if a key frame in the future is already complete
  for (auto it = frame_buf_.rbegin(); it != frame_buf_.rend(); it++) {
    const auto frame_id = it->first;
    const auto & frame = it->second;

    // found a complete key frame ahead of next_frame_
    if (frame.type() == FrameType::KEY and frame.complete()) {
      assert(frame_id > next_frame_);

      // set next_frame_ to frame_id and clean up old frames
      const auto frame_diff = frame_id - next_frame_;
      advance_next_frame(frame_diff);

      cerr << "* Recovery: skipped " << frame_diff
           << " frames ahead to key frame " << frame_id << endl;

      return true;
    }
  }

  return false;
}

void Decoder::consume_next_frame()
{
  Frame & frame = frame_buf_.at(next_frame_);
  if (not frame.complete()) {
    throw runtime_error("next frame must be complete before consuming it");
  }

  // found a decodable frame; update (and output) stats
  num_decodable_frames_++;
  const size_t frame_size = frame.frame_size().value();
  total_decodable_frame_size_ += frame_size;

  const auto stats_now = steady_clock::now();
  while (stats_now >= last_stats_time_ + 1s) {
    cerr << "Decodable frames in the last ~1s: "
         << num_decodable_frames_ << endl;

    const double diff_ms = duration<double, milli>(
                           stats_now - last_stats_time_).count();
    if (diff_ms > 0) {
      const uint32_t actual_bitrate = static_cast<uint32_t>(total_decodable_frame_size_ * 8 * 100 / diff_ms);
      lastest_bitrate_ = actual_bitrate;
      pending_bitrate_ = lastest_bitrate_;
      cerr << "  - Bitrate (kbps): "
           << double_to_string(total_decodable_frame_size_ * 8 / diff_ms)
           << endl;
    }else{
      lastest_bitrate_.reset();
      pending_bitrate_.reset();
    }

    // reset stats
    num_decodable_frames_ = 0;
    total_decodable_frame_size_ = 0;
    last_stats_time_ += 1s;
  }

  if (lazy_level_ <= DECODE_ONLY) {
    // dispatch the frame to worker thread
    {
      lock_guard<mutex> lock(mtx_);
      shared_queue_.emplace_back(move(frame));
    } // release the lock before notifying the worker thread

    // notify worker thread
    cv_.notify_one();
  } else {
    // main thread outputs frame information if no worker thread
    if (output_fd_) {
      const auto frame_decodable_ts = timestamp_us();

      output_fd_->write(to_string(next_frame_) + "," +
                        to_string(frame_size) + "," +
                        to_string(frame_decodable_ts) + "\n");
    }
  }

  // move onto the next frame
  advance_next_frame();
}

void Decoder::advance_next_frame(const unsigned int n)
{
  next_frame_ += n;

  // clean up state up to next_frame_
  clean_up_to(next_frame_);
}

void Decoder::clean_up_to(const uint32_t frontier)
{
  for (auto it = frame_buf_.begin(); it != frame_buf_.end(); ) {
    if (it->first >= frontier) {
      break;
    }

    it = frame_buf_.erase(it);
  }
}

double Decoder::decode_frame(vpx_codec_ctx_t & context, const Frame & frame)
{
  if (not frame.complete()) {
    throw runtime_error("frame must be complete before decoding");
  }

  // allocate a decoding buffer once
  static constexpr size_t MAX_DECODING_BUF = 1000000; // 1 MB
  static vector<uint8_t> decode_buf(MAX_DECODING_BUF);

  // copy the payload of the frame's datagrams to 'decode_buf'
  uint8_t * buf_ptr = decode_buf.data();
  const uint8_t * const buf_end = buf_ptr + decode_buf.size();

  for (const auto & datagram : frame.frags()) {
    const string & payload = datagram.value().payload;

    if (buf_ptr + payload.size() >= buf_end) {
      throw runtime_error("frame size exceeds max decoding buffer size");
    }

    memcpy(buf_ptr, payload.data(), payload.size());
    buf_ptr += payload.size();
  }

  const size_t frame_size = buf_ptr - decode_buf.data();

  // decode the compressed frame in 'decode_buf'
  const auto decode_start = steady_clock::now();
  check_call(vpx_codec_decode(&context, decode_buf.data(), frame_size,
                              nullptr, 1),
             VPX_CODEC_OK, "failed to decode a frame");
  const auto decode_end = steady_clock::now();

  return duration<double, milli>(decode_end - decode_start).count();
}

void Decoder::display_decoded_frame(vpx_codec_ctx_t & context,
                                    VideoDisplay & display)
{
  vpx_codec_iter_t iter = nullptr;
  vpx_image * raw_img;
  unsigned int frame_decoded = 0;

  // display the decoded frame stored in 'context_'
  while ((raw_img = vpx_codec_get_frame(&context, &iter))) {
    frame_decoded++;

    // there should be exactly one frame decoded
    if (frame_decoded > 1) {
      throw runtime_error("Multiple frames were decoded at once");
    }

    // construct a temporary RawImage that does not own the raw_img
    display.show_frame(RawImage(raw_img));
  }
}

void Decoder::worker_main()
{
  // worker does nothing if not decode or display
  if (lazy_level_ == NO_DECODE_DISPLAY) {
    return;
  }

  // initialize a VP9 decoding context
  const unsigned int max_threads = min(get_nprocs(), 4);
  vpx_codec_dec_cfg_t cfg {max_threads, display_width_, display_height_};

  vpx_codec_ctx_t context;
  check_call(vpx_codec_dec_init(&context, &vpx_codec_vp9_dx_algo, &cfg, 0),
             VPX_CODEC_OK, "vpx_codec_dec_init");

  cerr << "[worker] Initialized decoder (max threads: "
       << max_threads << ")" << endl;

  // video display
  unique_ptr<VideoDisplay> display;
  if (lazy_level_ == DECODE_DISPLAY) {
    display = make_unique<VideoDisplay>(display_width_, display_height_);
  }

  // **********************************************************
  // Yuxin: generate a timestamp-based filename for the Y4M file
  const auto now = chrono::system_clock::now();
  const auto now_time_t = chrono::system_clock::to_time_t(now);
  const auto now_tm = *localtime(&now_time_t);

  char y4m_filename[64];
  strftime(y4m_filename, sizeof(y4m_filename), "./data/output_%Y%m%d_%H%M%S.y4m", &now_tm);

  // Open the Y4M file
  ofstream y4m_file(y4m_filename, ios::binary);
  if (!y4m_file.is_open()) {
    throw runtime_error("Failed to open Y4M file for writing");
  }

  // Improvement: set an 8MB buffer for the output file
  std::vector<char> y4m_buffer(32 * 1024 * 1024); // 8MB buffer
  y4m_file.rdbuf()->pubsetbuf(y4m_buffer.data(), y4m_buffer.size());

  // Write the Y4M header
  y4m_file << "YUV4MPEG2 W" << display_width_ << " H" << display_height_
           << " F" << frame_rate_ << ":1 Ip A128:117\n";
  // **********************************************************

  // local queue of frames
  deque<Frame> local_queue;

  // stats maintained by the worker thread
  unsigned int num_decoded_frames = 0;
  double total_decode_time_ms = 0.0;
  double max_decode_time_ms = 0.0;
  auto last_stats_time = decoder_epoch_;

  while (true) {
    // destroy display if it has been signalled to quit
    if (display and display->signal_quit()) {
      display.reset(nullptr);
    }

    {
      // wait until the shared queue is not empty
      unique_lock<mutex> lock(mtx_);
      cv_.wait(lock, [this] { return not shared_queue_.empty(); });

      // worker owns the lock after wait and should copy shared queue quickly
      while (not shared_queue_.empty()) {
        local_queue.emplace_back(move(shared_queue_.front()));
        shared_queue_.pop_front();
      }
    } // worker releases the lock so it doesn't block the main thread anymore

    // now worker can take its time to decode and render the frames kept locally
    while (not local_queue.empty()) {
      const Frame & frame = local_queue.front();
      const double decode_time_ms = decode_frame(context, frame);

      if (output_fd_) {
        const auto frame_decoded_ts = timestamp_us();

        output_fd_->write(to_string(frame.id()) + "," +
                          to_string(frame.frame_size().value()) + "," +
                          to_string(frame_decoded_ts) + "\n");
      }

      // **********************************************************
      // Yuxin: write the decoded frame to the Y4M file
      vpx_codec_iter_t iter = nullptr;
      vpx_image * raw_img;
      while ((raw_img = vpx_codec_get_frame(&context, &iter))) {
        // Write the Y4M frame header
        y4m_file << "FRAME\n";

        // Write the YUV data to the Y4M file
        for (int plane = 0; plane < 3; ++plane) {
          const uint8_t * data = raw_img->planes[plane];
          const int stride = raw_img->stride[plane];
          const int height = (plane == 0) ? raw_img->d_h : (raw_img->d_h + 1) / 2;
          const int width = (plane == 0) ? raw_img->d_w : (raw_img->d_w + 1) / 2;

          for (int y = 0; y < height; ++y) {
            y4m_file.write(reinterpret_cast<const char *>(data), width);
            data += stride;
          }
        }
      }
      // **********************************************************

      if (display) {
        display_decoded_frame(context, *display);
      }

      local_queue.pop_front();

      // update stats
      num_decoded_frames++;
      total_decode_time_ms += decode_time_ms;
      max_decode_time_ms = max(max_decode_time_ms, decode_time_ms);

      // worker thread also outputs stats roughly every second
      const auto stats_now = steady_clock::now();
      while (stats_now >= last_stats_time + 1s) {
        // if (num_decoded_frames > 0) {
        //   cerr << "[worker] Avg/Max decoding time (ms) of "
        //        << num_decoded_frames << " frames: "
        //        << double_to_string(total_decode_time_ms / num_decoded_frames)
        //        << "/" << double_to_string(max_decode_time_ms) << endl;
        // }

        // reset stats
        num_decoded_frames = 0;
        total_decode_time_ms = 0.0;
        max_decode_time_ms = 0.0;
        last_stats_time += 1s;
      }
    }
  }

  check_call(vpx_codec_destroy(&context), VPX_CODEC_OK, "vpx_codec_destroy");
}
