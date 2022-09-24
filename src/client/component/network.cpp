#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "network.hpp"

#include "game/game.hpp"

#include "console.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace network
{
	std::unordered_map<std::string, callback>& get_callbacks()
	{
		static std::unordered_map<std::string, callback> callbacks{};
		return callbacks;
	}

	void on(const std::string& command, const callback& callback)
	{
		get_callbacks()[utils::string::to_lower(command)] = callback;
	}

	bool handle_command(game::netadr_s* address, const char* command, game::msg_t* message)
	{
		printf("hamdle_cmd: %s\n", command);
		const auto cmd_string = utils::string::to_lower(command);
		auto& callbacks = get_callbacks();
		const auto handler = callbacks.find(cmd_string);
		const auto offset = cmd_string.size() + 5;
		if (message->cursize < offset || handler == callbacks.end())
		{
			return false;
		}

		const std::string_view data(message->data + offset, message->cursize - offset);

#ifdef DEBUG
		console::info("[Network] Handling command %s\n", cmd_string.data());
#endif

		handler->second(*address, data);
		return true;
	}

	namespace
	{
		utils::hook::detour cl_main_connectionless_packet_connect_handshake_hook;
		bool cl_main_connectionless_packet_connect_handshake_stub(int client_num, game::netadr_s* from, game::msg_t* msg, int time, const char* c)
		{
			if (handle_command(from, c, msg))
			{
				return true;
			}

			return cl_main_connectionless_packet_connect_handshake_hook.invoke<bool>(client_num, from, msg, time, c);
		}

		int sys_send_packet_stub(const int size, const char* src, game::netadr_s* to)
		{
			console::info("Sys_SendPacket: type: %i, query_socket: %p\n", to->type, *game::query_socket);
			if (to->type == game::NA_BROADCAST || to->type == game::NA_IP)
			{
				if (*game::query_socket)
				{
					sockaddr s = {};
					game::NetadrToSockadr(to, &s);
					return sendto(*game::query_socket, src, size, 0, &s, sizeof(sockaddr)) >= 0;
				}
			}
			else
			{
				game::Com_Error(game::ERR_DROP, "Sys_SendPacket: bad address type %i", to->type);
			}
			return 0;
		}

		void sockadr_to_netadr(const sockaddr* s, game::netadr_s* a)
		{
			if (s->sa_family == 2)
			{
				a->type = game::NA_IP;
				*a->ip = *&s->sa_data[2];
				a->port = ntohs(*s->sa_data);
			}
		}

		int sys_get_packet_stub(game::netadr_s* net_from, game::msg_t* net_message)
		{
			int ret;
			int fromlen;
			sockaddr from;
			int datalen;

			if (!*game::query_socket)
			{
				return 0;
			}

			fromlen = sizeof(sockaddr);
			ret = recvfrom(*game::query_socket, net_message->data, net_message->maxsize, 0, &from, &fromlen);

			if (ret == SOCKET_ERROR)
			{
				return 0;
			}

			printf("%s\n", std::string(net_message->data, ret).data());

			sockadr_to_netadr(&from, net_from);
			net_message->readcount = 0;

			datalen = ret;
			if (!datalen)
			{
				console::warn("Sys_GetPacket: Empty or discarded packet from %s\n", net_adr_to_string(*net_from));
				return 0;
			}
			if (datalen == net_message->maxsize)
			{
				console::warn("Sys_GetPacket: Oversize packet from %s\n", net_adr_to_string(*net_from));
				return 0;
			}

			net_message->cursize = datalen - 1;
			net_message->targetLocalNetID = static_cast<game::netsrc_t>(net_message->data[net_message->cursize] >> 4);
			net_from->localNetID = static_cast<game::netsrc_t>(net_message->data[net_message->cursize] & 0xF);

			return 1;
		}

		int net_compare_base_address(const game::netadr_s* a, const game::netadr_s* b)
		{
			if (a->type != b->type)
			{
				return a->type - b->type;
			}

			if (a->type == b->type)
			{
				switch (a->type)
				{
				case game::netadrtype_t::NA_BOT:
				case game::netadrtype_t::NA_LOOPBACK:
					return a->port == b->port;

				case game::netadrtype_t::NA_IP:
					return !memcmp(a->ip, b->ip, 4);
				case game::netadrtype_t::NA_BROADCAST:
					return true;
				default:
					break;
				}
			}

			console::warn("NET_CompareBaseAdr: bad address type\n");
			return false;
		}

		int net_compare_address(const game::netadr_s* a, const game::netadr_s* b)
		{
			return net_compare_base_address(a, b) && a->port == b->port;
		}

		void string_to_sockaddr(const char* str, sockaddr_in* address)
		{
			game::netadr_s addr{};
			game::NET_StringToAdr(str, &addr);
			game::NetadrToSockadr(&addr, reinterpret_cast<sockaddr*>(address));
		}

		SOCKET create_socket(const char* net_interface, int port, int protocol)
		{
			sockaddr_in address{};

			if (net_interface && net_interface != "localhost"s)
			{
				string_to_sockaddr(net_interface, &address);
			}

			address.sin_family = AF_INET;
			address.sin_port = ntohs(static_cast<short>(port));

			const auto sock = socket(AF_INET, SOCK_DGRAM, protocol);

			if (sock == INVALID_SOCKET)
			{
				console::warn("WARNING: UDP_OpenSocket: socket");
				return INVALID_SOCKET;
			}

			u_long arg = 1;
			if (ioctlsocket(sock, FIONBIO, &arg) == SOCKET_ERROR)
			{
				console::warn("WARNING: UDP_OpenSocket: ioctl FIONBIO");
				return INVALID_SOCKET;
			}
			char optval[4] = { 0 };
			optval[0] = 1;
			if (setsockopt(sock, 0xFFFF, SO_BROADCAST, optval, 4) == SOCKET_ERROR)
			{
				console::warn("WARNING: UDP_OpenSocket: setsockopt SO_BROADCAST");
				return INVALID_SOCKET;
			}

			if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
			{
				console::warn("WARNING: UDP_OpenSocket: bind");
				closesocket(sock);
				return INVALID_SOCKET;
			}

			return sock;
		}

		void init_socket()
		{
			const auto net_ip = game::Dvar_FindVar("net_ip");
			const auto net_port = game::Dvar_FindVar("net_port");

			auto port_diff = 0;
			for (port_diff = 0; port_diff < 10; port_diff++)
			{
				*game::query_socket = create_socket(
					net_ip->current.string, net_port->current.integer + port_diff, IPPROTO_UDP);
				if (*game::query_socket)
				{
					break;
				}
			}

			if (!*game::query_socket)
			{
				console::warn("WARNING: Couldn't allocate IP/UDP port, LAN discovery will not work!\n");
				return;
			}

			game::Dvar_SetInt(net_port, net_port->current.integer + port_diff);
		}

		void net_init_stub()
		{
			init_socket();
			utils::hook::invoke<void>(0xD57A00_b);
		}
	}

	void send(const game::netadr_s& address, const std::string& command, const std::string& data, const char separator)
	{
		std::string packet = "\xFF\xFF\xFF\xFF";
		packet.append(command);
		packet.push_back(separator);
		packet.append(data);

		send_data(address, packet);
	}

	void send_data(const game::netadr_s& address, const std::string& data)
	{
		auto size = static_cast<int>(data.size());
		if (address.type == game::NA_LOOPBACK)
		{
			// TODO: Fix this for loopback
			if (size > 1280)
			{
				console::error("Packet was too long. Truncated!\n");
				size = 1280;
			}

			game::NET_SendLoopPacket(game::NS_CLIENT1, size, data.data(), &address);
		}
		else
		{
			game::Sys_SendPacket(size, data.data(), &address);
		}
	}

	bool are_addresses_equal(const game::netadr_s& a, const game::netadr_s& b)
	{
		return net_compare_address(&a, &b);
	}

	const char* net_adr_to_string(const game::netadr_s& a)
	{
		if (a.type == game::netadrtype_t::NA_LOOPBACK)
		{
			return "loopback";
		}

		if (a.type == game::netadrtype_t::NA_BOT)
		{
			return "bot";
		}

		if (a.type == game::netadrtype_t::NA_IP || a.type == game::netadrtype_t::NA_BROADCAST)
		{
			if (a.port)
			{
				return utils::string::va("%u.%u.%u.%u:%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3], htons(a.port));
			}

			return utils::string::va("%u.%u.%u.%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3]);
		}

		return "bad";
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// redirect packet sends to our stub
			utils::hook::jump(game::Sys_SendPacket, sys_send_packet_stub);

			// redirect packet receives to our stub
			utils::hook::jump(game::Sys_GetPacket, sys_get_packet_stub);

			// intercept command handling
			cl_main_connectionless_packet_connect_handshake_hook.create(0x9AFF90_b,
				cl_main_connectionless_packet_connect_handshake_stub);

			// handle xuid without secure connection
			utils::hook::nop(0xC53315_b, 2);

			utils::hook::jump(game::NET_CompareAdr, net_compare_address);
			utils::hook::jump(game::NET_CompareBaseAdr, net_compare_base_address);

			// don't establish secure conenction
			utils::hook::set<uint8_t>(0x9DBFDD_b, 0xEB);
			utils::hook::set<uint8_t>(0x9DC47D_b, 0xEB);
			utils::hook::set<uint8_t>(0x9DDC79_b, 0xEB);

			// ignore unregistered connection
			utils::hook::jump(0xC4F200_b, 0xC4F1AB_b);
			utils::hook::jump(0xC4F2F6_b, 0xC4F399_b);

			// disable xuid verification
			//utils::hook::set<uint8_t>(0x728BF_b, 0xEB);

			// ignore configstring mismatch
			utils::hook::set<uint8_t>(0x9B6F91_b, 0xEB);

			// ignore dw handle in SvClientMP::FindClientAtAddress
			utils::hook::set<uint8_t>(0xC58B2B_b, 0xEB);

			// ignore dw handle in SV_DirectConnect
			utils::hook::set<uint8_t>(0xC4EE1A_b, 0xEB);
			utils::hook::set<uint8_t>(0xC4F0FB_b, 0xEB);

			// ignore impure client
			utils::hook::jump(0xC500C8_b, 0xC500DE_b); // maybe add sv_pure dvar?

			// don't send checksum
			//utils::hook::set<uint8_t>(0x0, 0);

			// don't read checksum
			//utils::hook::set(0xCE6E60_b, 0xC301B0);

			// don't try to reconnect client
			//utils::hook::call(0x0, reconnect_migratated_client);
			//utils::hook::nop(0x0, 4); // this crashes when reconnecting for some reason

			// increase allowed packet size
			//const auto max_packet_size = 0x20000;
			//utils::hook::set<int>(0x0, max_packet_size);
			//utils::hook::set<int>(0x0, max_packet_size);
			//utils::hook::set<int>(0x0, max_packet_size);
			//utils::hook::set<int>(0x0, max_packet_size);

			// increase cl_maxpackets
			//dvars::override::register_int("cl_maxpackets", 1000, 1, 1000, game::DVAR_FLAG_SAVED);

			// increase snaps
			//dvars::override::register_int("sv_remote_client_snapshot_msec", 33, 33, 100, game::DVAR_FLAG_NONE);

			// ignore built in "print" oob command and add in our own
			utils::hook::set<uint8_t>(0x9B0326_b, 0xEB);
			network::on("print", [](const game::netadr_s&, const std::string_view& data)
			{
				const std::string message{ data };
				console::info(message.data());
			});

			// Use our own socket since the game's socket doesn't work with non localhost addresses
			// why? no idea
			//utils::hook::jump(0x0, create_socket);

			utils::hook::jump(0xD57C7E_b, net_init_stub);
		}
	};
}

REGISTER_COMPONENT(network::component)