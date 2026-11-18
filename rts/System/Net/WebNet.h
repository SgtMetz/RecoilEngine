/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _WEB_NET_H
#define _WEB_NET_H

// Datagram transport for the browser build (multiplayer). A browser can't open UDP sockets,
// so when the page sets RECOIL_WEB_NET=1 the engine's UDP traffic goes through two byte
// rings in the shared wasm memory instead: the page's main thread forwards them over a
// WebSocket to a relay, which routes them between the peers of one room. Every peer in the
// room has a virtual IPv4 address (10.77.x.y), so the engine's UDPConnection/UDPListener
// protocol, reliability layer and endpoints work unchanged. Without the variable (single
// player, replays, native builds) the asio sockets are used as before.

#include <asio/ip/udp.hpp>
#include <cstddef>
#include <cstdint>

namespace netcode {
namespace web {
#ifdef __EMSCRIPTEN__
	bool Active();
	// a socket's local port in the virtual network; 0 picks an ephemeral one
	void Bind(const asio::ip::udp::socket* s, int port);
	void Unbind(const asio::ip::udp::socket* s);
	// size of the next datagram for this socket, 0 if none
	size_t Available(const asio::ip::udp::socket* s);
	size_t ReceiveFrom(const asio::ip::udp::socket* s, std::uint8_t* buf, size_t cap, asio::ip::udp::endpoint& from);
	void SendTo(const asio::ip::udp::socket* s, const std::uint8_t* data, size_t len, const asio::ip::udp::endpoint& to);
	void Wait(int ms);
#else
	constexpr bool Active() { return false; }
	inline void Bind(const asio::ip::udp::socket*, int) {}
	inline void Unbind(const asio::ip::udp::socket*) {}
	inline size_t Available(const asio::ip::udp::socket*) { return 0; }
	inline size_t ReceiveFrom(const asio::ip::udp::socket*, std::uint8_t*, size_t, asio::ip::udp::endpoint&) { return 0; }
	inline void SendTo(const asio::ip::udp::socket*, const std::uint8_t*, size_t, const asio::ip::udp::endpoint&) {}
	inline void Wait(int) {}
#endif
}
}

#endif // _WEB_NET_H
