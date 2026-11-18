/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "WebNet.h"

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "System/Log/ILog.h"

// Shared-memory layout (read by the page, see web-harness/www/webnet.js):
//   ring:   u32 head (write offset), u32 tail (read offset), then RING_BYTES of data
//   record: u32 len, u32 ip, u16 remote port, u16 local port, payload, padded to 4 bytes.
//           len == 0xFFFFFFFF marks a wrap to offset 0; with fewer than 12 bytes left
//           before the end the reader wraps without a marker.
//   out ring: engine -> page; ip/remote port = destination, local port = source
//   in ring:  page -> engine; ip/remote port = source, local port = destination
// One producer and one consumer per ring (the engine side serializes its threads).

namespace {
	constexpr uint32_t RING_BYTES = 1u << 21;
	constexpr uint32_t WRAP = 0xFFFFFFFFu;
	constexpr uint32_t HDR = 12;
	constexpr size_t MAX_QUEUED = 4096;

	struct Ring {
		std::atomic<uint32_t> head;
		std::atomic<uint32_t> tail;
		uint8_t data[RING_BYTES];
	};
	// all zero, so it lives in .bss and not in the wasm file
	struct Shared {
		Ring out;
		Ring in;
		std::atomic<uint32_t> myIP; // set by the page (host byte order)
		std::atomic<uint32_t> dropped;
	} shared;

	static_assert(sizeof(std::atomic<uint32_t>) == 4, "the page reads these with Atomics on an Int32Array");

	constexpr uint32_t Pad4(uint32_t n) { return (n + 3u) & ~3u; }

	struct Datagram {
		uint32_t ip;
		uint16_t port;
		std::vector<uint8_t> data;
	};

	std::mutex mutex; // guards everything below and the producer side of the out ring
	std::unordered_map<const void*, uint16_t> socketPorts;
	std::unordered_map<uint16_t, std::deque<Datagram>> queues;
	uint16_t nextEphemeral = 40000;

	bool WriteRing(Ring& r, uint32_t ip, uint16_t remotePort, uint16_t localPort, const uint8_t* data, uint32_t len) {
		const uint32_t need = HDR + Pad4(len);
		const uint32_t head = r.head.load(std::memory_order_relaxed);
		const uint32_t tail = r.tail.load(std::memory_order_acquire);
		uint32_t pos = head;

		if (head >= tail) {
			// keep 4 bytes free so that head == tail always means empty
			const uint32_t endSpace = RING_BYTES - head - ((tail == 0) ? 4 : 0);
			if (endSpace < need) {
				if (tail < need + 4)
					return false;
				if (RING_BYTES - head >= HDR) {
					const uint32_t w = WRAP;
					std::memcpy(&r.data[head], &w, 4);
				}
				pos = 0;
			}
		} else if (tail - head < need + 4) {
			return false;
		}

		uint8_t* p = &r.data[pos];
		std::memcpy(p + 0, &len, 4);
		std::memcpy(p + 4, &ip, 4);
		std::memcpy(p + 8, &remotePort, 2);
		std::memcpy(p + 10, &localPort, 2);
		std::memcpy(p + HDR, data, len);

		uint32_t next = pos + need;
		if (next == RING_BYTES)
			next = 0;
		r.head.store(next, std::memory_order_release);
		return true;
	}

	void QueueLocked(uint16_t localPort, uint32_t ip, uint16_t remotePort, const uint8_t* data, size_t len) {
		const auto it = queues.find(localPort);
		if (it == queues.end())
			return; // nobody bound there
		if (it->second.size() >= MAX_QUEUED) {
			shared.dropped.fetch_add(1);
			return;
		}
		it->second.push_back({ip, remotePort, std::vector<uint8_t>(data, data + len)});
	}

	// move everything the page delivered into the per-port queues
	void PumpLocked() {
		Ring& r = shared.in;
		uint32_t tail = r.tail.load(std::memory_order_relaxed);
		const uint32_t head = r.head.load(std::memory_order_acquire);

		while (tail != head) {
			if (RING_BYTES - tail < HDR) { tail = 0; continue; }
			uint32_t len;
			std::memcpy(&len, &r.data[tail], 4);
			if (len == WRAP) { tail = 0; continue; }

			uint32_t ip;
			uint16_t remotePort, localPort;
			std::memcpy(&ip, &r.data[tail + 4], 4);
			std::memcpy(&remotePort, &r.data[tail + 8], 2);
			std::memcpy(&localPort, &r.data[tail + 10], 2);
			QueueLocked(localPort, ip, remotePort, &r.data[tail + HDR], len);

			tail += HDR + Pad4(len);
			if (tail == RING_BYTES)
				tail = 0;
		}
		r.tail.store(tail, std::memory_order_release);
	}

	uint16_t PortLocked(const void* s) {
		const auto it = socketPorts.find(s);
		if (it != socketPorts.end())
			return it->second;
		// a socket nobody bound explicitly: give it an ephemeral port
		const uint16_t port = nextEphemeral++;
		socketPorts[s] = port;
		queues[port];
		return port;
	}

	uint32_t ToIP(const asio::ip::address& a) {
		if (a.is_v4())
			return uint32_t(a.to_v4().to_ulong());
		if (a.is_v6() && a.to_v6().is_v4_mapped())
			return uint32_t(a.to_v6().to_v4().to_ulong());
		if (a.is_loopback())
			return 0x7F000001u;
		return 0;
	}
}

// for the page: where the rings are
extern "C" {
	EMSCRIPTEN_KEEPALIVE uint32_t recoil_webnet_out() { return uint32_t(reinterpret_cast<uintptr_t>(&shared.out)); }
	EMSCRIPTEN_KEEPALIVE uint32_t recoil_webnet_in() { return uint32_t(reinterpret_cast<uintptr_t>(&shared.in)); }
	EMSCRIPTEN_KEEPALIVE uint32_t recoil_webnet_myip() { return uint32_t(reinterpret_cast<uintptr_t>(&shared.myIP)); }
	EMSCRIPTEN_KEEPALIVE uint32_t recoil_webnet_ring_bytes() { return RING_BYTES; }
}

namespace netcode {
namespace web {

bool Active() {
	static const bool active = [] {
		const char* v = std::getenv("RECOIL_WEB_NET");
		const bool on = (v != nullptr && v[0] == '1');
		if (on)
			LOG("[WebNet] UDP traffic goes through the page's relay connection");
		return on;
	}();
	return active;
}

void Bind(const asio::ip::udp::socket* s, int port) {
	std::lock_guard<std::mutex> lock(mutex);
	const uint16_t p = (port > 0) ? uint16_t(port) : nextEphemeral++;
	socketPorts[s] = p;
	queues[p].clear();
	LOG("[WebNet] socket bound to virtual port %u", unsigned(p));
}

void Unbind(const asio::ip::udp::socket* s) {
	std::lock_guard<std::mutex> lock(mutex);
	const auto it = socketPorts.find(s);
	if (it == socketPorts.end())
		return;
	queues.erase(it->second);
	socketPorts.erase(it);
}

size_t Available(const asio::ip::udp::socket* s) {
	std::lock_guard<std::mutex> lock(mutex);
	PumpLocked();
	const auto& q = queues[PortLocked(s)];
	return q.empty() ? 0 : std::max<size_t>(q.front().data.size(), 1);
}

size_t ReceiveFrom(const asio::ip::udp::socket* s, uint8_t* buf, size_t cap, asio::ip::udp::endpoint& from) {
	std::lock_guard<std::mutex> lock(mutex);
	PumpLocked();
	auto& q = queues[PortLocked(s)];
	if (q.empty())
		return 0;
	const Datagram d = std::move(q.front());
	q.pop_front();
	const size_t n = std::min(cap, d.data.size());
	std::memcpy(buf, d.data.data(), n);
	from = asio::ip::udp::endpoint(asio::ip::address_v4(d.ip), d.port);
	return n;
}

void SendTo(const asio::ip::udp::socket* s, const uint8_t* data, size_t len, const asio::ip::udp::endpoint& to) {
	std::lock_guard<std::mutex> lock(mutex);
	const uint16_t localPort = PortLocked(s);
	const uint32_t ip = ToIP(to.address());
	const uint32_t myIP = shared.myIP.load(std::memory_order_relaxed);

	// to ourselves: no round trip through the relay
	if (ip == 0x7F000001u || (myIP != 0 && ip == myIP)) {
		QueueLocked(to.port(), (myIP != 0) ? myIP : 0x7F000001u, localPort, data, len);
		return;
	}
	if (!WriteRing(shared.out, ip, to.port(), localPort, data, uint32_t(len)))
		shared.dropped.fetch_add(1);
}

void Wait(int ms) {
	if (ms > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}
}

#endif // __EMSCRIPTEN__
