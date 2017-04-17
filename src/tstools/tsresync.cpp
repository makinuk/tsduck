//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2017, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Resynchronize a transport stream at beginning of a packet.
//
//----------------------------------------------------------------------------

#include "tsArgs.h"
#include "tsInputRedirector.h"
#include "tsOutputRedirector.h"
#include "tsByteBlock.h"
#include "tsDecimal.h"
#include "tsFormat.h"
#include "tsFatal.h"
#include "tsMPEG.h"

using namespace ts;

#define MIN_SYNC_SIZE     (1024)              // 1 kB
#define MAX_SYNC_SIZE     (8 * 1024 * 1024)   // 8 MB
#define DEFAULT_SYNC_SIZE (1024 * 1024)       // 1 MB

#define MIN_CONTIG_SIZE     (2 * PKT_SIZE)    // 2 transport packets
#define MAX_CONTIG_SIZE     (8 * 1024 * 1024) // 8 MB
#define DEFAULT_CONTIG_SIZE (512 * 1024)      // 512 kB


//----------------------------------------------------------------------------
//  Command line options
//----------------------------------------------------------------------------

struct Options: public Args
{
    Options (int argc, char *argv[]);

    size_t      sync_size;   // number of initial bytes to analyze for resync
    size_t      contig_size; // required size of contiguous packets to accept a stream slice
    size_t      packet_size; // specific non-standard input packet size (zero means use standard sizes)
    size_t      header_size; // header size (when packet_size > 0)
    bool        verbose;     // verbose mode
    bool        cont_sync;   // continuous synchronization (default: stop on error)
    bool        keep;        // keep packet size (default: reduce to 188 bytes)
    std::string infile;      // Input file name
    std::string outfile;     // Output file name
};

Options::Options (int argc, char *argv[]) :
    Args ("MPEG Transport Stream Resynchronizer.", "[options] [filename]")
{
    option ("",                0,  STRING, 0, 1);
    option ("continue",       'c');
    option ("header-size",    'h', UNSIGNED);
    option ("keep",           'k');
    option ("min-contiguous", 'm', INTEGER, 0, 1, MIN_CONTIG_SIZE, MAX_CONTIG_SIZE);
    option ("packet-size",    'p', INTEGER, 0, 1, PKT_SIZE, 0x7FFFFFFFL);
    option ("output",         'o', STRING);
    option ("sync-size",      's', INTEGER, 0, 1, MIN_SYNC_SIZE, MAX_SYNC_SIZE);
    option ("verbose",        'v');

    setHelp ("Input file:\n"
             "\n"
             "  MPEG transport stream file (standard input if omitted).\n"
             "\n"
             "Options:\n"
             "\n"
             "  -c\n"
             "  --continue\n"
             "      Continue re-resynchronizing after loss of synchronization.\n"
             "      By default, stop after first packet not starting with 0x47.\n"
             "\n"
             "  -h value\n"
             "  --header-size value\n"
             "      When used with --packet-size, specifies the size of extra data preceeding\n"
             "      each packet in the input file. The default is zero.\n"
             "\n"
             "  --help\n"
             "      Display this help text.\n"
             "\n"
             "  -k\n"
             "  --keep\n"
             "      Keep TS packet size from input to output file. By default, strip extra\n"
             "      data and reduce packets to 188 bytes. See option --packet-size for a\n"
             "      description of supported input packet sizes.\n"
             "\n"
             "  -m value\n"
             "  --min-contiguous value\n"
             "      Minimum size containing contiguous valid packets to consider a slice of\n"
             "      input file as containing actual packets (default: 512 kB).\n"
             "\n"
             "  -o filename\n"
             "  --output filename\n"
             "      Output file name (standard output by default).\n"
             "\n"
             "  -p value\n"
             "  --packet-size value\n"
             "      Expected TS packet size in bytes. By default, try 188-byte (standard),\n"
             "      204-byte (trailing 16-byte Reed-Solomon outer FEC), 192-byte (leading\n"
             "      4-byte timestamp in M2TS/Blu-ray disc files). If the input file contains\n"
             "      any other type of packet encapsulation, use options --packet-size and\n"
             "      --header-size.\n"
             "\n"
             "  -s value\n"
             "  --sync-size value\n"
             "      Number of initial bytes to analyze to find start of packet\n"
             "      synchronization (default: 1 MB).\n"
             "\n"
             "  -v\n"
             "  --verbose\n"
             "      Display verbose information.\n"
             "\n"
             "  --version\n"
             "      Display the version number.\n");

    analyze (argc, argv);

    infile = value ("");
    outfile = value ("output");
    sync_size = intValue<size_t> ("sync-size", DEFAULT_SYNC_SIZE);
    contig_size = intValue<size_t> ("min-contiguous", DEFAULT_CONTIG_SIZE);
    header_size = intValue<size_t> ("header-size", 0);
    packet_size = intValue<size_t> ("packet-size", 0);
    verbose = present ("verbose");
    keep = present ("keep");
    cont_sync = present ("continue");

    if (packet_size > 0 && header_size + PKT_SIZE > packet_size) {
        error ("specified --header-size too large for specified --packet-size");
    }

    exitOnError();
}


//----------------------------------------------------------------------------
// Resynchronization class
//----------------------------------------------------------------------------

enum Status {RS_OK, RS_SYNC_LOST, RS_EOF, RS_ERROR};

class Resynchronizer
{
public:

    // Reset the analysis of input data.
    void reset ()
    {
        _status = RS_OK;
        _in_pkt_size = 0;
        _in_header_size = 0;
    }

    // Look for MPEG packets in a buffer, according to an assumed packet size.
    // If the complete buffer matches the packet size, set input and output packet
    // sizes and return true. Return false otherwise.
    bool checkSync (const uint8_t* buf, size_t buf_size, size_t pkt_size, size_t header_size);

    // Get packet sizes, as determined by checkSync(). Size is zero if no valid packet size found.
    size_t inputPacketSize() const {return _in_pkt_size;}
    size_t inputHeaderSize() const {return _in_header_size;}
    size_t outputPacketSize() const {return _out_pkt_size;}
    size_t outputHeaderSize() const {return _out_header_size;}

    // Get output size so far in bytes and packets
    uint64_t outputFileBytes() const {return _out_size;}
    uint64_t outputFilePackets() const {return _out_pkt_size == 0 ? 0 : _out_size / _out_pkt_size;}

    // Get/set status
    Status status() const {return _status;}
    void setStatus (Status status) {_status = status;}

    // Read input data, return read size (zero on end of file or error)
    size_t readData (uint8_t* buf, size_t size);

    // Write one output packet from input packet.
    bool writePacket (const uint8_t* input_packet);

    // Constructor
    Resynchronizer (bool keep_packet_size) :
        _status (RS_OK),
        _keep_packet_size (keep_packet_size),
        _out_size (0),
        _in_pkt_size (0),
        _in_header_size (0),
        _out_pkt_size (0),
        _out_header_size (0)
    {
    }

private:
    Status _status;            // Processing status
    bool   _keep_packet_size;  // Same packet size on output file
    uint64_t _out_size;          // Size of output file
    size_t _in_pkt_size;       // TS packet size in input stream (188, 204, 192)
    size_t _in_header_size;    // Header size before TS packet in input stream (0, 4)
    size_t _out_pkt_size;      // TS packet size in output stream
    size_t _out_header_size;   // Header size before TS packet in output stream
};


//----------------------------------------------------------------------------
// Read input data, return read size (zero on end of file or error)
//----------------------------------------------------------------------------

size_t Resynchronizer::readData (uint8_t* buf, size_t size)
{
    std::streamsize got = 0;
    std::streamsize remain = std::streamsize (size);
    while (remain > 0) {
        if (std::cin.read (reinterpret_cast <char*> (buf + got), remain)) {
            const std::streamsize count = std::cin.gcount();
            got += count;
            remain -= count;
        }
        else {
            if (got == 0) {
                _status = RS_EOF;
            }
            break;
        }
    }
    return size_t (got);
}


//----------------------------------------------------------------------------
// Write one output packet from input packet.
//----------------------------------------------------------------------------

bool Resynchronizer::writePacket (const uint8_t* input_packet)
{
    const char* out_pkt = reinterpret_cast <const char*> (input_packet + _in_header_size - _out_header_size);
    if (std::cout.write (out_pkt, static_cast <std::streamsize> (_out_pkt_size))) {
        _out_size += _out_pkt_size;
        return true;
    }
    else {
        std::cerr << "* Error writing output file" << std::endl;
        _status = RS_ERROR;
        return false;
    }
}


//----------------------------------------------------------------------------
//  Look for MPEG packets in a buffer, according to an assumed packet size.
//----------------------------------------------------------------------------

bool Resynchronizer::checkSync (const uint8_t* buf, size_t buf_size, size_t pkt_size, size_t header_size)
{
    assert (pkt_size >= header_size + PKT_SIZE);
    const uint8_t* end = buf + buf_size - pkt_size + 1;

    // Check if the buffer contains packets with the appropriate size
    while (buf < end) {
        if (buf[header_size] != SYNC_BYTE) {
            return false; // not found
        }
        buf += pkt_size;
    }

    // Packets found all along the buffer
    _in_pkt_size = pkt_size;
    _in_header_size = header_size;
    _out_pkt_size = _keep_packet_size ? pkt_size : PKT_SIZE;
    _out_header_size = _keep_packet_size ? header_size : 0;
    return true;
}


//----------------------------------------------------------------------------
//  Program entry point
//----------------------------------------------------------------------------

int main (int argc, char *argv[])
{
    Options opt (argc, argv);
    InputRedirector input (opt.infile, opt);
    OutputRedirector output (opt.outfile, opt);
    Resynchronizer resync (opt.keep);

    // Synchronization buffer
    ByteBlock sync_buf_bb (opt.sync_size + opt.contig_size);
    uint8_t* const sync_buf = sync_buf_bb.data();
    size_t const sync_buf_size = sync_buf_bb.size();

    size_t sync_pre_size = 0; // Pre-loaded in synchronization buffer
    const char* prefix_fn = "first";

    // Loop on synchronization start. This occurs once at the
    // beginning of the file. Then, if option --continue is specified,
    // it occurs again each time the synchronization is lost.
    do {
        resync.reset();

        // Read the initial buffer. We use these data to look for packet sync.
        size_t const read_size = resync.readData (sync_buf + sync_pre_size, sync_buf_size - sync_pre_size);
        size_t const sync_size = sync_pre_size + read_size;
        uint8_t* const sync_end = sync_buf + sync_size;

        if (opt.verbose) {
            std::cerr << "* Analyzing " << prefix_fn << " " << Decimal (sync_size) << " bytes" << std::endl;
            prefix_fn = "next";
        }

        // Look for a range of packets for at least --min-contiguous bytes
        size_t const search_size = std::min (opt.contig_size, sync_size);
        uint8_t* const end_search = sync_end - search_size + 1;

        // Search a range of valid packets. Try all expected packet sizes.
        const uint8_t* start;
        for (start = sync_buf; start < end_search; start++) {
            if (opt.packet_size > 0) {
                if (resync.checkSync (start, search_size, opt.packet_size, opt.header_size)) {
                    // Found user-specified encapsulation of TS packets
                    break;
                }
            }
            else {
                if (resync.checkSync (start, search_size, PKT_SIZE, 0)) {
                    // Found standard TS packets
                    break;
                }
                if (resync.checkSync (start, search_size, PKT_RS_SIZE, 0)) {
                    // Found TS packets with trailing Reed-Solomon outer FEC
                    break;
                }
                if (resync.checkSync (start, search_size, PKT_M2TS_SIZE, M2TS_HEADER_SIZE)) {
                    // Found TS packets with leading 4-byte timestamp (M2TS format, blu-ray discs)
                    break;
                }
            }
        }
        if (resync.inputPacketSize() == 0) {
            std::cerr << "* Cannot find MPEG TS packets after " << Decimal (search_size) << " bytes" << std::endl;
            resync.setStatus (RS_ERROR);
            break;
        }
        if (opt.verbose) {
            std::cerr << "* Found synchronization after " << Decimal (start - sync_buf) << " bytes" << std::endl
                      << "* Packet size is " << resync.inputPacketSize() << " bytes";
            if (resync.inputHeaderSize() > 0) {
                std::cerr << " (" << resync.inputHeaderSize() << "-byte header)";                
            }
            std::cerr << std::endl;
        }

        // Output initial sync buffer, starting at first valid packet, writing all valid packets
        while (start <= sync_end - resync.inputPacketSize() && start[resync.inputHeaderSize()] == SYNC_BYTE) {
            if (!resync.writePacket (start)) {
                break;
            }
            start += resync.inputPacketSize();
        }
        if (resync.status() != RS_OK) {
            break;
        }

        // Compact sync buffer
        if (start >= sync_end) {
            sync_pre_size = 0;
        }
        else {
            sync_pre_size = sync_end - start;
            ::memmove (sync_buf, start, sync_pre_size);
        }

        // If more than one packet left, out of sync
        if (sync_pre_size >= resync.inputPacketSize()) {
            resync.setStatus (RS_SYNC_LOST);
        }

        // Read the rest of the input file
        while (resync.status() == RS_OK) {
            assert (sync_pre_size < resync.inputPacketSize());
            // Read the next packet
            const size_t remain_size = resync.inputPacketSize() - sync_pre_size;
            if (resync.readData (sync_buf + sync_pre_size, remain_size) != remain_size) {
                resync.setStatus (RS_EOF);
            }
            else if (sync_buf[resync.inputHeaderSize()] != SYNC_BYTE) {
                std::cerr << "*** Synchronization lost after "
                          << Decimal (resync.outputFilePackets()) << " TS packets" << std::endl
                          << Format ("*** Got 0x%02X instead of 0x%02X at start of TS packet", int (sync_buf[resync.inputHeaderSize()]), SYNC_BYTE)
                          << std::endl;
                resync.setStatus (RS_SYNC_LOST);
                // Will resynchronize with sync buffer pre-loaded
                sync_pre_size = resync.inputPacketSize();
            }
            else {
                resync.writePacket (sync_buf);
                sync_pre_size = 0;
            }
        }

    } while (resync.status() == RS_OK || (resync.status() == RS_SYNC_LOST && opt.cont_sync));

    if (opt.verbose) {
        std::cerr << "* Output " << Decimal (resync.outputFileBytes()) << " bytes, "
                  << Decimal (resync.outputFilePackets()) << " " << resync.outputPacketSize() << "-byte packets" << std::endl;
    }

    return resync.status() == RS_EOF ? EXIT_SUCCESS : EXIT_FAILURE;
}