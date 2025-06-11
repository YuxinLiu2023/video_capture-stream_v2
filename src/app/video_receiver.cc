#include <getopt.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <chrono>

#include "conversion.hh"
#include "udp_socket.hh"
#include "sdl.hh"
#include "protocol.hh"
#include "decoder.hh"

using namespace std;
using namespace chrono;

void print_usage(const string & program_name)
{
  cerr <<
  "Usage: " << program_name << " [options] host port width height\n\n"
  "Options:\n"
  "--fps <FPS>          frame rate to request from sender (default: 30)\n"
  "--cbr <bitrate>      request CBR from sender\n"
  "--lazy <level>       0: decode and display frames (default)\n"
  "                     1: decode but not display frames\n"
  "                     2: neither decode nor display frames\n"
  "-o, --output <file>  file to output performance results to\n"
  "-v, --verbose        enable more logging for debugging"
  << endl;
}

pair<Address, ConfigMsg> recv_config_msg(UDPSocket & udp_sock)
{
  // wait until a valid ConfigMsg is received
  while (true) {
    const auto & [peer_addr, raw_data] = udp_sock.recvfrom();

    const shared_ptr<Msg> msg = Msg::parse_from_string(raw_data.value());
    if (msg == nullptr or msg->type != Msg::Type::CONFIG) {
      continue; // ignore invalid or non-config messages
    }

    const auto config_msg = dynamic_pointer_cast<ConfigMsg>(msg);
    if (config_msg) {
      return {peer_addr, *config_msg};
    }
  }
}

int main(int argc, char * argv[])
{
  // // argument parsing
  // uint16_t frame_rate = 30;
  // unsigned int target_bitrate = 0; // kbps
  // int lazy_level = 0;
  string output_path;
  bool verbose = false;

  // const option cmd_line_opts[] = {
  //   {"fps",     required_argument, nullptr, 'F'},
  //   {"cbr",     required_argument, nullptr, 'C'},
  //   {"lazy",    required_argument, nullptr, 'L'},
  //   {"output",  required_argument, nullptr, 'o'},
  //   {"verbose", no_argument,       nullptr, 'v'},
  //   { nullptr,  0,                 nullptr,  0 },
  // };

  // while (true) {
  //   const int opt = getopt_long(argc, argv, "o:v", cmd_line_opts, nullptr);
  //   if (opt == -1) {
  //     break;
  //   }

  //   switch (opt) {
  //     case 'F':
  //       frame_rate = narrow_cast<uint16_t>(strict_stoi(optarg));
  //       break;
  //     case 'C':
  //       target_bitrate = strict_stoi(optarg);
  //       break;
  //     case 'L':
  //       lazy_level = strict_stoi(optarg);
  //       break;
  //     case 'o':
  //       output_path = optarg;
  //       break;
  //     case 'v':
  //       verbose = true;
  //       break;
  //     default:
  //       print_usage(argv[0]);
  //       return EXIT_FAILURE;
  //   }
  // }

  // if (optind != argc - 4) {
  //   print_usage(argv[0]);
  //   return EXIT_FAILURE;
  // }

  // ===== Argument parsing =====
  if (argc < 3) {
    cerr << "Usage: " << argv[0] << " <host> <port> [--cbr bitrate] [--lazy level] [--fps rate] [--output file] [--verbose]\n";
    return EXIT_FAILURE;
  }

  const string host = argv[optind];
  const auto port = narrow_cast<uint16_t>(strict_stoi(argv[optind + 1]));
  // const auto width = narrow_cast<uint16_t>(strict_stoi(argv[optind + 2]));
  // const auto height = narrow_cast<uint16_t>(strict_stoi(argv[optind + 3]));

  unsigned int target_bitrate = 0; // kbps
  int lazy_level = 0;

  optind = 3;
  const option cmd_line_opts[] = {
    {"cbr",  required_argument, nullptr, 'C'},
    {"lazy", required_argument, nullptr, 'L'},
    {nullptr, 0, nullptr, 0},
  };

  while (true) {
    const int opt = getopt_long(argc, argv, "C:L:", cmd_line_opts, nullptr);
    if (opt == -1) break;

    switch (opt) {
      case 'C':
        target_bitrate = strict_stoi(optarg);
        break;
      case 'L':
        lazy_level = strict_stoi(optarg);
        break;
      default:
        cerr << "Invalid option.\n";
        return EXIT_FAILURE;
    }
  }

  if (target_bitrate == 0) {
    cerr << "--cbr <bitrate> is required.\n";
    return EXIT_FAILURE;
  }

  Address peer_addr{host, port};
  cerr << "Peer address: " << peer_addr.str() << endl;

  // create a UDP socket and "connect" it to the peer (sender)
  UDPSocket udp_sock;
  udp_sock.connect(peer_addr);
  cerr << "Local address: " << udp_sock.local_address().str() << endl;

  // request a specific configuration
  const ConfigMsg config_msg(0, 0, 0, target_bitrate);
  udp_sock.send(config_msg.serialize_to_string());

  uint16_t width = 0, height = 0, frame_rate = 0;
  // wait for the sender to send a ConfigMsg with width, height, and frame rate
  while(true) {
    const auto & [peer_addr, config_msg] = recv_config_msg(udp_sock);
    width = config_msg.width;
    height = config_msg.height;
    frame_rate = config_msg.frame_rate;
    target_bitrate = config_msg.target_bitrate;

    cerr << "Received config: width=" << to_string(width)
         << " height=" << to_string(height)
         << " FPS=" << to_string(frame_rate)
         << " bitrate=" << to_string(target_bitrate) << endl;
    
    break;
  }

  // initialize decoder
  Decoder decoder(width, height, lazy_level, frame_rate, output_path);
  decoder.set_verbose(verbose);

  // main loop
  while (true) {
    // parse a datagram received from sender
    Datagram datagram;
    if (not datagram.parse_from_string(udp_sock.recv().value())) {
      throw runtime_error("failed to parse a datagram");
    }

    // send an ACK back to sender
    AckMsg ack(datagram);
    udp_sock.send(ack.serialize_to_string());

    if (verbose) {
      cerr << "Acked datagram: frame_id=" << datagram.frame_id
           << " frag_id=" << datagram.frag_id << endl;
    }

    // process the received datagram in the decoder
    decoder.add_datagram(move(datagram));

    // check if the expected frame(s) is complete
    while (decoder.next_frame_complete()) {
      // depending on the lazy level, might decode and display the next frame
      decoder.consume_next_frame();
    }
  }

  return EXIT_SUCCESS;
}
