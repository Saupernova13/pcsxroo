// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/server/DebugServer.h"

#include "pcsxroo/server/DebugServerCommands.h"
#include "pcsxroo/server/DebugServerJson.h"
#include "pcsxroo/server/DebuggerControl.h"

#include "common/Console.h"
#include "common/Threading.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <WinSock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using pollfd_t = WSAPOLLFD;
#define PCSXROO_INVALID_SOCKET INVALID_SOCKET
#define PCSXROO_POLL_READ POLLRDNORM
#define socket_recv(s, buf, len) recv((s), (char*)(buf), (int)(len), 0)
#define socket_send(s, buf, len) send((s), (const char*)(buf), (int)(len), 0)
#define socket_shutdown(s) shutdown((s), SD_BOTH)
#define socket_close(s) closesocket(s)
#define socket_poll(fds, count, timeout_ms) WSAPoll((fds), (count), (timeout_ms))
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
using pollfd_t = pollfd;
#define PCSXROO_INVALID_SOCKET (-1)
#define PCSXROO_POLL_READ POLLIN
// A send to a client that has just disconnected raises SIGPIPE, which would take the whole
// emulator down. Linux suppresses it per call with MSG_NOSIGNAL; macOS has no such flag and
// gets SO_NOSIGPIPE on each accepted socket instead.
#ifdef MSG_NOSIGNAL
#define PCSXROO_SEND_FLAGS MSG_NOSIGNAL
#else
#define PCSXROO_SEND_FLAGS 0
#endif
#define socket_recv(s, buf, len) recv((s), (buf), (len), 0)
#define socket_send(s, buf, len) send((s), (buf), (len), PCSXROO_SEND_FLAGS)
#define socket_shutdown(s) shutdown((s), SHUT_RDWR)
#define socket_close(s) close(s)
#define socket_poll(fds, count, timeout_ms) poll((fds), (count), (timeout_ms))
#endif

namespace
{
	// A client that never sends a newline must not be able to grow the server's memory
	// without bound.
	constexpr size_t MAX_LINE_BYTES = 1024 * 1024;
	constexpr size_t MAX_CLIENTS = 8;

	// How long the accept loop waits for a connection before checking whether to stop.
	constexpr int ACCEPT_POLL_MS = 250;

	// One connected client. Reader and writer are separate threads so a subscriber stalled
	// on a full socket buffer cannot hold up whatever produced the event.
	struct Client final : public DebugServerConnection
	{
		// Set once at accept and not closed until both threads have been joined, so they can
		// use it without a lock. Closing it earlier, from another thread, would let the system
		// hand the same descriptor to a new connection while these threads still wrote to it.
		socket_t sock = PCSXROO_INVALID_SOCKET;
		std::thread reader;
		std::thread writer;

		std::mutex out_mutex;
		std::condition_variable out_cv;
		std::deque<std::string> out_queue;
		bool closing = false;

		std::atomic_bool subscribed{false};
		// Set by the reader thread on its way out so the accept loop can reap this entry.
		std::atomic_bool finished{false};

		void SetSubscribed(bool value) override { subscribed.store(value, std::memory_order_release); }
	};

	std::atomic_bool s_end{true};
	socket_t s_listen_sock = PCSXROO_INVALID_SOCKET;
	std::thread s_accept_thread;
	int s_port = -1;
	size_t s_stop_callback = 0;

	std::mutex s_clients_mutex;
	std::vector<std::shared_ptr<Client>> s_clients;

#ifdef _WIN32
	bool s_winsock_initialized = false;
#endif

	void QueueLine(const std::shared_ptr<Client>& client, std::string line)
	{
		{
			std::lock_guard lock(client->out_mutex);
			if (client->closing)
				return;

			client->out_queue.push_back(std::move(line));
		}

		client->out_cv.notify_one();
	}

	// Stops a client's threads without releasing its socket. shutdown wakes a reader blocked
	// in recv and a writer blocked in send on every platform; close from another thread does
	// not (on Linux recv stays blocked). The socket is released by JoinAndRelease.
	void CloseClient(const std::shared_ptr<Client>& client)
	{
		{
			std::lock_guard lock(client->out_mutex);
			client->closing = true;
		}

		client->out_cv.notify_all();
		socket_shutdown(client->sock);
	}

	// Never called from the client's own reader or writer.
	void JoinAndRelease(const std::shared_ptr<Client>& client)
	{
		CloseClient(client);

		if (client->reader.joinable())
			client->reader.join();
		if (client->writer.joinable())
			client->writer.join();

		if (client->sock != PCSXROO_INVALID_SOCKET)
		{
			socket_close(client->sock);
			client->sock = PCSXROO_INVALID_SOCKET;
		}
	}

	void BroadcastStopEvent(const DebuggerControl::StopEvent& stop)
	{
		std::vector<std::shared_ptr<Client>> clients;
		{
			std::lock_guard lock(s_clients_mutex);
			clients = s_clients;
		}

		std::string line;
		for (const auto& client : clients)
		{
			if (!client->subscribed.load(std::memory_order_acquire))
				continue;

			if (line.empty())
				line = DebugServerCommands::MakeStopEventLine(stop);

			QueueLine(client, line);
		}
	}

	std::string HandleLine(std::string_view line, DebugServerConnection& connection)
	{
		DebugServerRequest request;
		std::string error;

		if (!DebugServerJson::ParseRequest(line, request, error))
		{
			const rapidjson::Value null_id;
			return DebugServerJson::MakeError(null_id, "parse_error", error);
		}

		const DebugServerHandler* handler = DebugServerCommands::Find(request.cmd);
		if (!handler)
			return DebugServerJson::MakeError(request.id, "unknown_command", "no such command: " + request.cmd);

		// Handlers must never throw, and nothing here can catch it if one does: PCSX2 builds with
		// exceptions disabled (-fno-exceptions, or _HAS_EXCEPTIONS=0 on MSVC), where a try block
		// does not compile on GCC or Clang and a throw ends the whole emulator. Bad input is
		// reported through Fail, and nothing that throws on bad input may be called with it.
		return (*handler)(request, connection);
	}

	void WriterLoop(std::shared_ptr<Client> client)
	{
		Threading::SetNameOfCurrentThread("PCSXROO Debug Writer");

		for (;;)
		{
			std::string line;
			{
				std::unique_lock lock(client->out_mutex);
				client->out_cv.wait(lock, [&client] { return client->closing || !client->out_queue.empty(); });

				if (client->out_queue.empty())
					return; // closing, nothing left to flush

				line = std::move(client->out_queue.front());
				client->out_queue.pop_front();
			}

			line.push_back('\n'); // framing lives here, and only here

			size_t sent = 0;
			while (sent < line.size())
			{
				const auto written = socket_send(client->sock, line.data() + sent, line.size() - sent);
				if (written <= 0)
					return;

				sent += static_cast<size_t>(written);
			}
		}
	}

	void ReaderLoop(std::shared_ptr<Client> client)
	{
		Threading::SetNameOfCurrentThread("PCSXROO Debug Client");

		std::string buffer;
		char chunk[8192];

		while (!s_end.load(std::memory_order_acquire))
		{
			const auto received = socket_recv(client->sock, chunk, sizeof(chunk));
			if (received <= 0)
				break;

			buffer.append(chunk, static_cast<size_t>(received));

			size_t newline;
			while ((newline = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, newline);
				buffer.erase(0, newline + 1);

				if (!line.empty() && line.back() == '\r')
					line.pop_back();

				if (line.empty())
					continue;

				QueueLine(client, HandleLine(line, *client));
			}

			if (buffer.size() > MAX_LINE_BYTES)
			{
				const rapidjson::Value null_id;
				QueueLine(client, DebugServerJson::MakeError(null_id, "parse_error",
										"request line exceeds 1 MiB; closing connection"));

				// Let the writer flush the explanation before the socket goes away.
				Threading::Sleep(50);
				break;
			}
		}

		CloseClient(client);

		// Announce that this connection is over. Without this the entry stayed in s_clients
		// for the life of the server: every command leaked a slot, and once eight had been
		// run the accept loop refused everything else. It looked like the emulator hanging
		// after a long command sequence, because that is exactly when the cap was reached.
		client->finished.store(true, std::memory_order_release);
	}

	// Joins and drops connections whose reader has exited. Called from the accept loop, so
	// it never runs on a thread it is about to join.
	void ReapFinishedClients()
	{
		std::vector<std::shared_ptr<Client>> finished;
		{
			std::lock_guard lock(s_clients_mutex);
			for (auto it = s_clients.begin(); it != s_clients.end();)
			{
				if ((*it)->finished.load(std::memory_order_acquire))
				{
					finished.push_back(*it);
					it = s_clients.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		for (const auto& client : finished)
			JoinAndRelease(client);
	}

	void AcceptLoop()
	{
		Threading::SetNameOfCurrentThread("PCSXROO Debug Server");

		while (!s_end.load(std::memory_order_acquire))
		{
			// Polled with a short timeout rather than blocked in accept: nothing done to a
			// listening socket from another thread reliably wakes accept everywhere (shutdown
			// does not on macOS), and Deinitialize has to be able to join this thread. The same
			// tick reaps finished clients, so their slots free up without a new connection.
			pollfd_t listener = {};
			listener.fd = s_listen_sock;
			listener.events = PCSXROO_POLL_READ;
			const int ready = socket_poll(&listener, 1, ACCEPT_POLL_MS);

			ReapFinishedClients();

			if (ready < 0)
			{
				Threading::Sleep(ACCEPT_POLL_MS); // an interrupted poll must not become a busy loop
				continue;
			}

			if (ready == 0 || s_end.load(std::memory_order_acquire))
				continue;

			const socket_t sock = accept(s_listen_sock, nullptr, nullptr);
			if (sock == PCSXROO_INVALID_SOCKET)
				continue;

#ifdef SO_NOSIGPIPE
			const int no_sigpipe = 1;
			setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

			auto client = std::make_shared<Client>();
			client->sock = sock;

			{
				std::lock_guard lock(s_clients_mutex);
				if (s_clients.size() >= MAX_CLIENTS)
				{
					Console.Warning("PCSXROO debug server: refusing connection, %zu clients already attached.",
						s_clients.size());
					socket_close(sock);
					continue;
				}

				s_clients.push_back(client);
			}

			client->writer = std::thread(WriterLoop, client);
			client->reader = std::thread(ReaderLoop, client);
		}
	}

#ifdef _WIN32
	bool InitializeWinsock()
	{
		if (s_winsock_initialized)
			return true;

		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return false;

		s_winsock_initialized = true;
		return true;
	}
#endif
} // namespace

bool DebugServer::IsInitialized()
{
	return !s_end.load(std::memory_order_acquire);
}

int DebugServer::GetPort()
{
	return s_port;
}

bool DebugServer::Initialize(int port)
{
	if (IsInitialized())
		Deinitialize();

	if (port <= 0 || port > 65535)
	{
		Console.Error("PCSXROO debug server: invalid port %d.", port);
		return false;
	}

	s_end.store(false, std::memory_order_release);
	s_port = port;
	DebuggerControl::SetWaitsCancelled(false);

#ifdef _WIN32
	if (!InitializeWinsock())
	{
		Console.Error("PCSXROO debug server: cannot initialise winsock.");
		Deinitialize();
		return false;
	}
#endif

	s_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (s_listen_sock == PCSXROO_INVALID_SOCKET)
	{
		Console.Error("PCSXROO debug server: cannot open socket.");
		Deinitialize();
		return false;
	}

	// Without this, a restart inside the TIME_WAIT window fails to bind, which in practice
	// means an agent that restarts the emulator cannot reconnect for a couple of minutes.
	int reuse = 1;
	setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	// Loopback only, and deliberately not configurable: this grants unrestricted memory
	// access and process control with no authentication.
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(static_cast<u16>(port));

	if (bind(s_listen_sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		Console.Error("PCSXROO debug server: cannot bind 127.0.0.1:%d (is another instance running?).", port);
		Deinitialize();
		return false;
	}

	if (listen(s_listen_sock, 8) != 0)
	{
		Console.Error("PCSXROO debug server: cannot listen on port %d.", port);
		Deinitialize();
		return false;
	}

	DebugServerCommands::RegisterAll();

	s_stop_callback = DebuggerControl::AddStopCallback(&BroadcastStopEvent);
	s_accept_thread = std::thread(AcceptLoop);

	Console.WriteLn("PCSXROO debug server listening on 127.0.0.1:%d (protocol %d).",
		port, PCSXROO_DEBUG_PROTOCOL_VERSION);
	return true;
}

void DebugServer::Deinitialize()
{
	s_end.store(true, std::memory_order_release);

	if (s_stop_callback != 0)
	{
		DebuggerControl::RemoveStopCallback(s_stop_callback);
		s_stop_callback = 0;
	}

	// A client blocked in wait would otherwise keep its reader thread, and so the join below,
	// busy until its timeout ran out - up to a day for an explicit one.
	DebuggerControl::SetWaitsCancelled(true);

	// The accept loop polls, so it sees s_end within one tick. The listening socket is closed
	// only once that thread can no longer be using it.
	if (s_accept_thread.joinable())
		s_accept_thread.join();

	if (s_listen_sock != PCSXROO_INVALID_SOCKET)
	{
		socket_close(s_listen_sock);
		s_listen_sock = PCSXROO_INVALID_SOCKET;
	}

	std::vector<std::shared_ptr<Client>> clients;
	{
		std::lock_guard lock(s_clients_mutex);
		clients.swap(s_clients);
	}

	for (const auto& client : clients)
		JoinAndRelease(client);

	DebuggerControl::SetWaitsCancelled(false);
	s_port = -1;
}
