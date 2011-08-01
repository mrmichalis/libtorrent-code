/*

Copyright (c) 2008, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/peer_id.hpp"
#include "libtorrent/io_service.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/address.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/thread.hpp"
#include "libtorrent/create_torrent.hpp"
#include "libtorrent/hasher.hpp"
#include <cstring>
#include <boost/bind.hpp>
#include <iostream>
#include <boost/array.hpp>

using namespace libtorrent;
using namespace libtorrent::detail; // for write_* and read_*

void generate_block(boost::uint32_t* buffer, int piece, int start, int length)
{
	boost::uint32_t fill = (piece << 8) | ((start / 0x4000) & 0xff);
	for (int i = 0; i < length / 4; ++i)
	{
		buffer[i] = fill;
	}
}

// in order to circumvent the restricton of only
// one connection per IP that most clients implement
// all sockets created by this tester are bound to
// uniqe local IPs in the range (127.0.0.1 - 127.255.255.255)
// it's only enabled if the target is also on the loopback
int local_if_counter = 0;
bool local_bind = false;

struct peer_conn
{
	peer_conn(io_service& ios, int num_pieces, int blocks_pp, tcp::endpoint const& ep
		, char const* ih, bool seed_)
		: s(ios)
		, read_pos(0)
		, state(handshaking)
		, block(0)
		, blocks_per_piece(blocks_pp)
		, info_hash(ih)
		, outstanding_requests(0)
		, seed(seed_)
		, blocks_received(0)
		, blocks_sent(0)
		, num_pieces(num_pieces)
		, start_time(time_now_hires())
	{
		pieces.reserve(num_pieces);
		if (local_bind)
		{
			error_code ec;
			s.open(ep.protocol(), ec);
			if (ec)
			{
				close("ERROR OPEN: %s", ec);
				return;
			}
			tcp::endpoint bind_if(address_v4(
				(127 << 24)
				+ ((local_if_counter / 255) << 16)
				+ ((local_if_counter % 255) + 1)), 0);
			++local_if_counter;
			s.bind(bind_if, ec);
			if (ec)
			{
				close("ERROR BIND: %s", ec);
				return;
			}
		}
		s.async_connect(ep, boost::bind(&peer_conn::on_connect, this, _1));
	}

	stream_socket s;
	boost::uint32_t write_buffer[17*1024/4];
	boost::uint32_t buffer[17*1024/4];
	int read_pos;

	enum state_t
	{
		handshaking,
		sending_request,
		receiving_message
	};
	int state;
	std::vector<int> pieces;
	int block;
	int blocks_per_piece;
	char const* info_hash;
	int outstanding_requests;
	// if this is true, this connection is a seed
	bool seed;
	bool fast_extension;
	int blocks_received;
	int blocks_sent;
	int num_pieces;
	ptime start_time;
	ptime end_time;

	void on_connect(error_code const& ec)
	{
		if (ec)
		{
			close("ERROR CONNECT: %s", ec);
			return;
		}

		char handshake[] = "\x13" "BitTorrent protocol\0\0\0\0\0\0\0\x04"
			"                    " // space for info-hash
			"aaaaaaaaaaaaaaaaaaaa" // peer-id
			"\0\0\0\x01\x02"; // interested
		char* h = (char*)malloc(sizeof(handshake));
		memcpy(h, handshake, sizeof(handshake));
		std::memcpy(h + 28, info_hash, 20);
		std::generate(h + 48, h + 68, &rand);
		// for seeds, don't send the interested message
		boost::asio::async_write(s, libtorrent::asio::buffer(h, (sizeof(handshake) - 1) - (seed ? 5 : 0))
			, boost::bind(&peer_conn::on_handshake, this, h, _1, _2));
	}

	void on_handshake(char* h, error_code const& ec, size_t bytes_transferred)
	{
		free(h);
		if (ec)
		{
			close("ERROR SEND HANDSHAKE: %s", ec);
			return;
		}

		// read handshake
		boost::asio::async_read(s, libtorrent::asio::buffer((char*)buffer, 68)
			, boost::bind(&peer_conn::on_handshake2, this, _1, _2));
	}

	void on_handshake2(error_code const& ec, size_t bytes_transferred)
	{
		if (ec)
		{
			close("ERROR READ HANDSHAKE: %s", ec);
			return;
		}

		// buffer is the full 68 byte handshake
		// look at the extension bits

		fast_extension = ((char*)buffer)[27] & 4;

		if (seed)
		{
			write_have_all();
		}
		else
		{
			work_download();
		}
	}

	void write_have_all()
	{
		if (fast_extension)
		{
			// have_all and unchoke
			static char msg[] = "\0\0\0\x01\x0e\0\0\0\x01\x01";
			error_code ec;
			boost::asio::async_write(s, libtorrent::asio::buffer(msg, sizeof(msg) - 1)
				, boost::bind(&peer_conn::on_have_all_sent, this, _1, _2));
		}
		else
		{
			// bitfield
			int len = (num_pieces + 7) / 8;
			char* ptr = (char*)buffer;
			write_uint32(len + 1, ptr);
			write_uint8(5, ptr);
			memset(ptr, 255, len);
			ptr += len;
			// unchoke
			write_uint32(1, ptr);
			write_uint8(1, ptr);
			error_code ec;
			boost::asio::async_write(s, libtorrent::asio::buffer((char*)buffer, len + 10)
				, boost::bind(&peer_conn::on_have_all_sent, this, _1, _2));
		}
	
	}

	void on_have_all_sent(error_code const& ec, size_t bytes_transferred)
	{
		if (ec)
		{
			close("ERROR SEND HAVE ALL: %s", ec);
			return;
		}

		// read message
		boost::asio::async_read(s, asio::buffer((char*)buffer, 4)
			, boost::bind(&peer_conn::on_msg_length, this, _1, _2));
	}

	void write_request()
	{
		if (pieces.empty()) return;

		int piece = pieces.back();

		char msg[] = "\0\0\0\xd\x06"
			"    " // piece
			"    " // offset
			"    "; // length
		char* m = (char*)malloc(sizeof(msg));
		memcpy(m, msg, sizeof(msg));
		char* ptr = m + 5;
		write_uint32(piece, ptr);
		write_uint32(block * 16 * 1024, ptr);
		write_uint32(16 * 1024, ptr);
		error_code ec;
		boost::asio::async_write(s, libtorrent::asio::buffer(m, sizeof(msg) - 1)
			, boost::bind(&peer_conn::on_req_sent, this, m, _1, _2));
	
		++block;
		if (block == blocks_per_piece)
		{
			block = 0;
			pieces.pop_back();
		}
	}

	void on_req_sent(char* m, error_code const& ec, size_t bytes_transferred)
	{
		free(m);
		if (ec)
		{
			close("ERROR SEND REQUEST: %s", ec);
			return;
		}

		++outstanding_requests;
	
		work_download();
	}

	void close(char const* fmt, error_code const& ec)
	{
		end_time = time_now_hires();
		char tmp[1024];
		snprintf(tmp, sizeof(tmp), fmt, ec.message().c_str());
		int time = total_milliseconds(end_time - start_time);
		if (time == 0) time = 1;
		float up = (boost::int64_t(blocks_sent) * 0x4000) / time / 1000.f;
		float down = (boost::int64_t(blocks_received) * 0x4000) / time / 1000.f;
		printf("%s sent: %d received: %d duration: %d ms up: %.1fMB/s down: %.1fMB/s\n"
			, tmp, blocks_sent, blocks_received, time, up, down);
	}

	void work_download()
	{
		if (pieces.empty()
			&& outstanding_requests == 0
			&& blocks_received >= num_pieces * blocks_per_piece)
		{
			close("COMPLETED DOWNLOAD", error_code());
			return;
		}

		// send requests
		if (outstanding_requests < 20 && !pieces.empty())
		{
			write_request();
			return;
		}

		// read message
		boost::asio::async_read(s, asio::buffer((char*)buffer, 4)
			, boost::bind(&peer_conn::on_msg_length, this, _1, _2));
	}

	void on_msg_length(error_code const& ec, size_t bytes_transferred)
	{
		if (ec)
		{
			close("ERROR RECEIVE MESSAGE PREFIX: %s", ec);
			return;
		}
		char* ptr = (char*)buffer;
		unsigned int length = read_uint32(ptr);
		if (length > sizeof(buffer))
		{
			close("ERROR RECEIVE MESSAGE PREFIX: packet too big", error_code());
			return;
		}
		boost::asio::async_read(s, asio::buffer((char*)buffer, length)
			, boost::bind(&peer_conn::on_message, this, _1, _2));
	}

	void on_message(error_code const& ec, size_t bytes_transferred)
	{
		if (ec)
		{
			close("ERROR RECEIVE MESSAGE: %s", ec);
			return;
		}
		char* ptr = (char*)buffer;
		int msg = read_uint8(ptr);

		//printf("msg: %d len: %d\n", msg, int(bytes_transferred));

		if (seed)
		{
			if (msg == 6 && bytes_transferred == 13)
			{
				int piece = detail::read_int32(ptr);
				int start = detail::read_int32(ptr);
				int length = detail::read_int32(ptr);
				write_piece(piece, start, length);
			}
			else
			{
				// read another message
				boost::asio::async_read(s, asio::buffer(buffer, 4)
					, boost::bind(&peer_conn::on_msg_length, this, _1, _2));
			}
		}
		else
		{
			if (msg == 0xe) // have_all
			{
				// build a list of all pieces and request them all!
				pieces.resize(num_pieces);
				for (int i = 0; i < int(pieces.size()); ++i)
					pieces[i] = i;
				std::random_shuffle(pieces.begin(), pieces.end());
			}
			else if (msg == 4) // have
			{
				int piece = detail::read_int32(ptr);
				if (pieces.empty()) pieces.push_back(piece);
				else pieces.insert(pieces.begin() + (rand() % pieces.size()), piece);
			}
			else if (msg == 5) // bitfield
			{
				pieces.reserve(num_pieces);
				int piece = 0;
				for (int i = 0; i < bytes_transferred; ++i)
				{
					int mask = 0x80;
					for (int k = 0; k < 8; ++k)
					{
						if (piece > num_pieces) break;
						if (*ptr & mask) pieces.push_back(piece);
						mask >>= 1;
						++piece;
					}
					++ptr;
				}
				std::random_shuffle(pieces.begin(), pieces.end());
			}
			else if (msg == 7)
			{
				++blocks_received;
				--outstanding_requests;
			}
			work_download();
		}
	}

	void write_piece(int piece, int start, int length)
	{
		generate_block(write_buffer, piece, start, length);
		static char msg[] = "    \x07"
			"    " // piece
			"    "; // start
		char* ptr = msg;
		write_uint32(9 + length, ptr);
		assert(length == 0x4000);
		assert(*ptr == 7);
		++ptr; // skip message id
		write_uint32(piece, ptr);
		write_uint32(start, ptr);
		boost::array<libtorrent::asio::const_buffer, 2> vec;
		vec[0] = libtorrent::asio::buffer(msg, sizeof(msg)-1);
		vec[1] = libtorrent::asio::buffer(write_buffer, length);
		boost::asio::async_write(s, vec, boost::bind(&peer_conn::on_have_all_sent, this, _1, _2));
		++blocks_sent;
	}
};

void print_usage()
{
	fprintf(stderr, "usage: connection_tester command ...\n\n"
		"command is one of:\n"
		"  gen-torrent         generate a test torrent\n"
		"    this command takes two extra arguments:\n"
		"    1. the size of the torrent in megabytes\n"
		"    2. the file to save the .torrent file to\n\n"
		"  upload              start an uploader test\n"
		"  download            start a downloader test\n"
		"  dual                start a download and upload test\n"
		"    these commands set takes 4 additional arguments\n"
		"    1. num-connections - the number of connections to make to the target\n"
		"    2. destination-IP - the IP address of the target\n"
		"    3. destination-port - the port the target listens on\n"
		"    4. torrent-file - the torrent file previously generated by gen-torrent\n\n"
		"examples:\n\n"
		"connection_tester gen-torrent 1024 test.torrent\n"
		"connection_tester upload 200 127.0.0.1 6881 test.torrent\n"
		"connection_tester download 200 127.0.0.1 6881 test.torrent\n"
		"connection_tester dual 200 127.0.0.1 6881 test.torrent\n");
	exit(1);
}

// size is in megabytes
void generate_torrent(std::vector<char>& buf, int size)
{
	file_storage fs;
	// 1 MiB piece size
	const int piece_size = 1024 * 1024;
	const int num_pieces = size;
	const size_type total_size = size_type(piece_size) * num_pieces;
	size_type s = total_size;
	int i = 0;
	while (s > 0)
	{
		char buf[100];
		snprintf(buf, sizeof(buf), "t/stress_test%d", i);
		++i;
		fs.add_file(buf, (std::min)(s, size_type(20*1024*1024)));
		s -= 20*1024*1024;
	}
	libtorrent::create_torrent t(fs, piece_size);

	fprintf(stderr, "\n");
	boost::uint32_t piece[0x4000 / 4];
	for (int i = 0; i < num_pieces; ++i)
	{
		hasher ph;
		for (int j = 0; j < piece_size; j += 0x4000)
		{
			generate_block(piece, i, j, 0x4000);
			ph.update((char*)piece, 0x4000);
		}
		t.set_hash(i, ph.final());
		if (i & 1) fprintf(stderr, "\r%.1f %% ", float(i * 100) / float(num_pieces));
	}
	fprintf(stderr, "\n");

	std::back_insert_iterator<std::vector<char> > out(buf);

	bencode(out, t.generate());
}

void io_thread(io_service* ios)
{
	error_code ec;
	ios->run(ec);
	if (ec)
		fprintf(stderr, "ERROR: %s\n", ec.message().c_str());
}

int main(int argc, char* argv[])
{
	if (argc <= 1) print_usage();

	enum { none, upload_test, download_test, dual_test } test_mode = none;

	if (strcmp(argv[1], "gen-torrent") == 0)
	{
		if (argc != 4) print_usage();

		int size = atoi(argv[2]);
		std::vector<char> tmp;
		generate_torrent(tmp, size ? size : 1024);

		FILE* output = stdout;
		if (strcmp("-", argv[3]) != 0)
			output = fopen(argv[3], "wb+");
		fwrite(&tmp[0], 1, tmp.size(), output);
		if (output != stdout)
			fclose(output);

		return 0;
	}
	else if (strcmp(argv[1], "upload") == 0)
	{
		if (argc != 6) print_usage();
		test_mode = upload_test;
	}
	else if (strcmp(argv[1], "download") == 0)
	{
		if (argc != 6) print_usage();
		test_mode = download_test;
	}
	else if (strcmp(argv[1], "dual") == 0)
	{
		if (argc != 6) print_usage();
		test_mode = dual_test;
	}

	if (!download_test && !upload_test) print_usage();

	int num_connections = atoi(argv[2]);
	error_code ec;
	address_v4 addr = address_v4::from_string(argv[3], ec);
	if (ec)
	{
		fprintf(stderr, "ERROR RESOLVING %s: %s\n", argv[3], ec.message().c_str());
		return 1;
	}
	int port = atoi(argv[4]);
	tcp::endpoint ep(addr, port);
	
	unsigned long ip = addr.to_ulong();
	if ((ip & 0xff000000) == 0x7f000000)
	{
		local_bind = true;
	}

	torrent_info ti(argv[5], ec);
	if (ec)
	{
		fprintf(stderr, "ERROR LOADING .TORRENT: %s\n", ec.message().c_str());
		return 1;
	}
			
	std::list<peer_conn*> conns;
	io_service ios;
	for (int i = 0; i < num_connections; ++i)
	{
		bool seed = false;
		if (test_mode == upload_test) seed = true;
		else if (test_mode == dual_test) seed = (i & 1);
		conns.push_back(new peer_conn(ios, ti.num_pieces(), ti.piece_length() / 16 / 1024
			, ep, (char const*)&ti.info_hash()[0], seed));
		libtorrent::sleep(1);
		ios.poll_one(ec);
		if (ec)
		{
			fprintf(stderr, "ERROR: %s\n", ec.message().c_str());
			break;
		}
	}


	thread t1(boost::bind(&io_thread, &ios));
	thread t2(boost::bind(&io_thread, &ios));
 
	t1.join();
	t2.join();

	float up = 0.f;
	float down = 0.f;
	boost::uint64_t total_sent = 0;
	boost::uint64_t total_received = 0;
	
	for (std::list<peer_conn*>::iterator i = conns.begin()
		, end(conns.end()); i != end; ++i)
	{
		peer_conn* p = *i;
		int time = total_milliseconds(p->end_time - p->start_time);
		if (time == 0) time = 1;
		if (time == 0) time = 1;
		total_sent += p->blocks_sent;
		up += (boost::int64_t(p->blocks_sent) * 0x4000) / time / 1000.f;
		down += (boost::int64_t(p->blocks_received) * 0x4000) / time / 1000.f;
		delete p;
	}

	printf("=========================\ntotal sent: %.1f %% received: %.1f %%\n"
		, total_sent * 0x4000 / float(ti.total_size())
		, total_received * 0x4000 / float(ti.total_size()));

	return 0;
}


