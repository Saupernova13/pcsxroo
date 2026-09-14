// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsxroo/cli/Client.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define socket_close(s) closesocket(static_cast<SOCKET>(s))
#define socket_recv(s, buf, len) recv(static_cast<SOCKET>(s), (char*)(buf), (int)(len), 0)
#define socket_send(s, buf, len) send(static_cast<SOCKET>(s), (const char*)(buf), (int)(len), 0)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
// A send to a connection the emulator has just closed raises SIGPIPE, which would kill this
// process before it could report exit code 3. Linux suppresses that per call with
// MSG_NOSIGNAL; macOS has no such flag and gets SO_NOSIGPIPE on the socket in Connect instead.
#ifdef MSG_NOSIGNAL
#define PCSXROO_SEND_FLAGS MSG_NOSIGNAL
#else
#define PCSXROO_SEND_FLAGS 0
#endif
#define socket_close(s) close(static_cast<int>(s))
#define socket_recv(s, buf, len) recv(static_cast<int>(s), (buf), (len), 0)
#define socket_send(s, buf, len) send(static_cast<int>(s), (buf), (len), PCSXROO_SEND_FLAGS)
#endif

namespace
{
#ifdef _WIN32
	bool EnsureWinsock(std::string& error)
	{
		static bool initialized = false;
		if (initialized)
			return true;

		WSADATA wsa{};
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			error = "could not initialise winsock";
			return false;
		}

		initialized = true;
		return true;
	}
#endif

	void SetReceiveTimeout(std::intptr_t sock, u32 timeout_ms)
	{
#ifdef _WIN32
		const DWORD value = timeout_ms;
		setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&value), sizeof(value));
#else
		timeval value{};
		value.tv_sec = timeout_ms / 1000;
		value.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
	}

	// A receive that ran out of time, as opposed to one that failed because the connection
	// went away. The two are different exit codes.
	bool LastErrorWasTimeout()
	{
#ifdef _WIN32
		return WSAGetLastError() == WSAETIMEDOUT;
#else
		return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
	}

	bool LastErrorWasInterrupt()
	{
#ifdef _WIN32
		return false;
#else
		return errno == EINTR;
#endif
	}
} // namespace

PcsxrooClient::~PcsxrooClient()
{
	Close();
}

void PcsxrooClient::Close()
{
	if (m_sock != -1)
	{
		socket_close(m_sock);
		m_sock = -1;
	}
}

bool PcsxrooClient::Connect(const std::string& host, int port, u32 timeout_ms, std::string& error)
{
	m_failure = Failure::Closed;

#ifdef _WIN32
	if (!EnsureWinsock(error))
		return false;
#endif

	const auto sock = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
	if (sock == INVALID_SOCKET)
#else
	if (sock < 0)
#endif
	{
		error = "could not create a socket";
		return false;
	}

#ifdef SO_NOSIGPIPE
	const int no_sigpipe = 1;
	setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<u16>(port));
	if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
	{
		socket_close(static_cast<std::intptr_t>(sock));
		error = "not a valid IPv4 address: " + host;
		return false;
	}

	if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		socket_close(static_cast<std::intptr_t>(sock));
		error = "could not connect to " + host + ":" + std::to_string(port) +
				" (is PCSXROO running with -debugserver?)";
		return false;
	}

	m_sock = static_cast<std::intptr_t>(sock);
	m_failure = Failure::None;
	SetReceiveTimeout(m_sock, timeout_ms);
	return true;
}

bool PcsxrooClient::ReadLine(std::string& out, std::string& error)
{
	for (;;)
	{
		const size_t newline = m_buffer.find('\n');
		if (newline != std::string::npos)
		{
			out = m_buffer.substr(0, newline);
			m_buffer.erase(0, newline + 1);

			if (!out.empty() && out.back() == '\r')
				out.pop_back();

			return true;
		}

		char chunk[8192];
		const auto received = socket_recv(m_sock, chunk, sizeof(chunk));
		if (received == 0)
		{
			m_failure = Failure::Closed;
			error = "the emulator closed the connection";
			return false;
		}

		if (received < 0)
		{
			if (LastErrorWasInterrupt())
				continue;

			if (LastErrorWasTimeout())
			{
				m_failure = Failure::Timeout;
				error = "timed out waiting for a reply";
			}
			else
			{
				m_failure = Failure::Closed;
				error = "lost the connection to the emulator";
			}

			return false;
		}

		m_buffer.append(chunk, static_cast<size_t>(received));
	}
}

bool PcsxrooClient::Request(const std::string& line, std::string& response, std::string& error)
{
	m_failure = Failure::None;

	if (m_sock == -1)
	{
		m_failure = Failure::Closed;
		error = "not connected to the emulator";
		return false;
	}

	const std::string framed = line + "\n";
	size_t sent = 0;
	while (sent < framed.size())
	{
		const auto written = socket_send(m_sock, framed.data() + sent, framed.size() - sent);
		if (written <= 0)
		{
			m_failure = Failure::Closed;
			error = "could not send the request; the connection to the emulator was lost";
			return false;
		}

		sent += static_cast<size_t>(written);
	}

	return ReadLine(response, error);
}

bool PcsxrooClient::Stream(const std::string& line, const std::function<bool(const std::string&)>& on_line,
	std::string& error)
{
	std::string first;
	if (!Request(line, first, error))
		return false;

	if (!on_line(first))
		return true;

	// Events arrive whenever the emulator stops, which may be minutes apart, so the read
	// timeout is dropped for the streaming phase.
	SetReceiveTimeout(m_sock, 0);

	for (;;)
	{
		std::string next;
		if (!ReadLine(next, error))
			return false;

		if (!on_line(next))
			return true;
	}
}
