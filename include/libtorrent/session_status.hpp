/*

Copyright (c) 2006, Arvid Norberg
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

#ifndef TORRENT_SESSION_STATUS_HPP_INCLUDED
#define TORRENT_SESSION_STATUS_HPP_INCLUDED

#include "libtorrent/config.hpp"
#include "libtorrent/size_type.hpp"

namespace libtorrent
{

#ifndef TORRENT_DISABLE_DHT

	struct dht_lookup
	{
		char const* type;
		int outstanding_requests;
		int timeouts;
		int responses;
		int branch_factor;
	};

#endif

	struct TORRENT_EXPORT session_status
	{
		bool has_incoming_connections;

		float upload_rate;
		float download_rate;
		size_type total_download;
		size_type total_upload;

		float payload_upload_rate;
		float payload_download_rate;
		size_type total_payload_download;
		size_type total_payload_upload;

		float ip_overhead_upload_rate;
		float ip_overhead_download_rate;
		size_type total_ip_overhead_download;
		size_type total_ip_overhead_upload;

		float dht_upload_rate;
		float dht_download_rate;
		size_type total_dht_download;
		size_type total_dht_upload;

		float tracker_upload_rate;
		float tracker_download_rate;
		size_type total_tracker_download;
		size_type total_tracker_upload;

		size_type total_redundant_bytes;
		size_type total_failed_bytes;

		int num_peers;
		int num_unchoked;
		int allowed_upload_slots;

		int up_bandwidth_queue;
		int down_bandwidth_queue;

		int up_bandwidth_bytes_queue;
		int down_bandwidth_bytes_queue;

		int optimistic_unchoke_counter;
		int unchoke_counter;

#ifndef TORRENT_DISABLE_DHT
		int dht_nodes;
		int dht_node_cache;
		int dht_torrents;
		size_type dht_global_nodes;
		std::vector<dht_lookup> active_requests;
#endif
	};

}

#endif // TORRENT_SESSION_STATUS_HPP_INCLUDED

