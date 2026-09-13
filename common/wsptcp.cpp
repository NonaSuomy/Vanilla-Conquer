//
// Copyright 2020 Electronic Arts Inc.
//
// TiberianDawn.DLL and RedAlert.dll and corresponding source code is free
// software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.

// TiberianDawn.DLL and RedAlert.dll and corresponding source code is distributed
// in the hope that it will be useful, but with permitted additional restrictions
// under Section 7 of the GPL. See the GNU General Public License in LICENSE.TXT
// distributed with this program. You should have received a copy of the
// GNU General Public License along with permitted additional restrictions
// with this program. If not, see https://github.com/electronicarts/CnC_Remastered_Collection

#include "wsptcp.h"
#include "debugstring.h"
#include "endianness.h"
#include "internet.h"
#include "misc.h"
#include "settings.h"
#include "wwkeyboard.h"
extern WWKeyboardClass* Keyboard;

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <ifaddrs.h>
#include <netinet/tcp.h>
#include <signal.h>
#endif

#ifdef NETWORKING

static bool Should_Keep_Outgoing(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    if (ntohl(local_ip) != ntohl(remote_ip)) {
        return ntohl(local_ip) > ntohl(remote_ip);
    }
    return ntohs(local_port) > ntohs(remote_port);
}

TCPInterfaceClass::TCPInterfaceClass(void)
    : WinsockInterfaceClass()
{
    ListenSocket = INVALID_SOCKET;
    UdpSocket = INVALID_SOCKET;
    PrimaryLocalIP = 0;
    Listening = false;
}

TCPInterfaceClass::~TCPInterfaceClass(void)
{
    Close_All_Peers();

    while (BroadcastAddresses.Count()) {
        delete[] BroadcastAddresses[0];
        BroadcastAddresses.Delete(0);
    }

    while (LocalAddresses.Count()) {
        delete[] LocalAddresses[0];
        LocalAddresses.Delete(0);
    }

    Close();
}

void TCPInterfaceClass::Set_Broadcast_Address(void* address)
{
    char* ip_addr = (char*)address;
    assert(strlen(ip_addr) <= strlen("xxx.xxx.xxx.xxx"));

    unsigned char* baddr = new unsigned char[4];
    sscanf(ip_addr, "%hhu.%hhu.%hhu.%hhu", &baddr[0], &baddr[1], &baddr[2], &baddr[3]);
    BroadcastAddresses.Add(baddr);
}

TCPInterfaceClass::TCPPeer* TCPInterfaceClass::Find_Peer(uint32_t ip, uint16_t port)
{
    for (size_t i = 0; i < Peers.size(); i++) {
        if (Peers[i]->ip == ip) {
            if (port != 0 && Peers[i]->port != 0) {
                if (Peers[i]->port == port) {
                    return Peers[i];
                }
            } else {
                return Peers[i];
            }
        }
    }
    return nullptr;
}

TCPInterfaceClass::TCPPeer* TCPInterfaceClass::Find_Peer_By_Socket(SOCKET s)
{
    for (size_t i = 0; i < Peers.size(); i++) {
        if (Peers[i]->socket == s) {
            return Peers[i];
        }
    }
    return nullptr;
}

TCPInterfaceClass::TCPPeer* TCPInterfaceClass::Connect_To_Peer(uint32_t ip, uint16_t port)
{
    if (port == 0) {
        port = (uint16_t)hton16((unsigned short)PlanetWestwoodPortNumber);
    }

    TCPPeer* existing = Find_Peer(ip, port);
    if (existing) {
        if (existing->port == 0) {
            existing->port = port;
        }
        return existing;
    }

    for (int i = 0; i < LocalAddresses.Count(); i++) {
        if (!memcmp(LocalAddresses[i], &ip, 4) && port == (uint16_t)hton16((unsigned short)PlanetWestwoodPortNumber)) {
            return nullptr;
        }
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        return nullptr;
    }

    int opt = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));
#if defined(SO_NOSIGPIPE)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (char*)&opt, sizeof(opt));
#endif
    ioctlsocket(s, FIONBIO, &opt);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = port;
    addr.sin_addr.s_addr = ip;

    int rc = connect(s, (sockaddr*)&addr, sizeof(addr));
    TCPPeer* peer = new TCPPeer;
    peer->socket = s;
    peer->ip = ip;
    peer->port = port;

    if (rc == 0) {
        peer->connected = true;
        peer->connecting = false;
    } else if (LastSocketError == WSAEWOULDBLOCK || LastSocketError == WSAEINPROGRESS
               || LastSocketError == WSAEALREADY) {
        peer->connected = false;
        peer->connecting = true;
    } else {
        closesocket(s);
        delete peer;
        return nullptr;
    }

    Peers.push_back(peer);
    return peer;
}

void TCPInterfaceClass::Close_Peer(size_t index)
{
    if (index < Peers.size()) {
        closesocket(Peers[index]->socket);
        delete Peers[index];
        Peers.erase(Peers.begin() + index);
    }
}

void TCPInterfaceClass::Close_All_Peers()
{
    for (size_t i = 0; i < Peers.size(); i++) {
        closesocket(Peers[i]->socket);
        delete Peers[i];
    }
    Peers.clear();
}

void TCPInterfaceClass::Send_Framed_Packet(TCPPeer* peer, const void* buffer, int buffer_len)
{
    if (!peer || buffer_len <= 0) {
        return;
    }

    TCPPacketHeader hdr;
    hdr.magic = (uint16_t)hton16(TCP_PACKET_MAGIC);
    hdr.length = (uint16_t)hton16((unsigned short)buffer_len);
    hdr.listen_port = (uint16_t)hton16((unsigned short)PlanetWestwoodPortNumber);

    size_t prev_size = peer->tx_buf.size();
    peer->tx_buf.resize(prev_size + sizeof(hdr) + buffer_len);
    memcpy(peer->tx_buf.data() + prev_size, &hdr, sizeof(hdr));
    memcpy(peer->tx_buf.data() + prev_size + sizeof(hdr), buffer, buffer_len);

    if (peer->connected && !peer->tx_buf.empty()) {
#ifdef MSG_NOSIGNAL
        int sent = send(peer->socket, (const char*)peer->tx_buf.data(), (int)peer->tx_buf.size(), MSG_NOSIGNAL);
#else
        int sent = send(peer->socket, (const char*)peer->tx_buf.data(), (int)peer->tx_buf.size(), 0);
#endif
        if (sent > 0) {
            peer->tx_buf.erase(peer->tx_buf.begin(), peer->tx_buf.begin() + sent);
        } else if (sent < 0 && LastSocketError != WSAEWOULDBLOCK) {
            peer->connected = false;
        }
    }
}

void TCPInterfaceClass::Process_Rx_Buffer(TCPPeer* peer)
{
    while (peer->rx_buf.size() >= sizeof(TCPPacketHeader)) {
        TCPPacketHeader hdr;
        memcpy(&hdr, peer->rx_buf.data(), sizeof(hdr));
        uint16_t magic = (uint16_t)ntoh16(hdr.magic);

        if (magic != TCP_PACKET_MAGIC) {
            size_t offset = 1;
            while (offset + sizeof(TCPPacketHeader) <= peer->rx_buf.size()) {
                uint16_t test_magic;
                memcpy(&test_magic, peer->rx_buf.data() + offset, 2);
                if (ntoh16(test_magic) == TCP_PACKET_MAGIC) {
                    break;
                }
                offset++;
            }
            peer->rx_buf.erase(peer->rx_buf.begin(), peer->rx_buf.begin() + offset);
            continue;
        }

        uint16_t length = (uint16_t)ntoh16(hdr.length);
        if (hdr.listen_port != 0) {
            peer->port = hdr.listen_port;
        }

        if (length > 2048) {
            peer->rx_buf.clear();
            break;
        }

        if (peer->rx_buf.size() < sizeof(TCPPacketHeader) + length) {
            break;
        }

        WinsockBufferType* packet = new WinsockBufferType;
        packet->BufferLen = length;
        memcpy(packet->Buffer, peer->rx_buf.data() + sizeof(TCPPacketHeader), length);
        memset(packet->Address, 0, sizeof(packet->Address));
        memcpy(packet->Address + 4, &peer->ip, 4);
        memcpy(packet->Address + 8, &peer->port, 2);
        InBuffers.Add(packet);

        peer->rx_buf.erase(peer->rx_buf.begin(), peer->rx_buf.begin() + sizeof(TCPPacketHeader) + length);
    }
}

bool TCPInterfaceClass::Open_Socket(SOCKET)
{
    if (!WinsockInitialised) {
        if (!Init()) {
            return false;
        }
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    while (LocalAddresses.Count()) {
        delete[] LocalAddresses[0];
        LocalAddresses.Delete(0);
    }

#ifdef _WIN32
    char hostname[128];
    gethostname(hostname, 128);
    struct hostent* host_info = gethostbyname(hostname);
    if (host_info && host_info->h_addr_list) {
        unsigned int** addresses = (unsigned int**)(host_info->h_addr_list);
        while (*addresses) {
            unsigned int address = **addresses++;
            unsigned char* a = new unsigned char[4];
            *((uint32_t*)a) = address;
            LocalAddresses.Add(a);
        }
    }
#else
    struct ifaddrs* if_addr = NULL;
    getifaddrs(&if_addr);

    for (struct ifaddrs* ifa = if_addr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct in_addr* tmp_addr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, tmp_addr, buf, INET_ADDRSTRLEN);

            unsigned char* a = new unsigned char[4];
            *((uint32_t*)a) = tmp_addr->s_addr;
            LocalAddresses.Add(a);

            if ((struct sockaddr_in*)ifa->ifa_broadaddr != 0) {
                tmp_addr = &((struct sockaddr_in*)ifa->ifa_broadaddr)->sin_addr;
                inet_ntop(AF_INET, tmp_addr, buf, INET_ADDRSTRLEN);
                Set_Broadcast_Address(buf);
            }
        }
    }

    if (if_addr != NULL) {
        freeifaddrs(if_addr);
    }
#endif

    Set_Broadcast_Address((void*)"255.255.255.255");

    PrimaryLocalIP = LocalAddresses.Count() ? *(uint32_t*)LocalAddresses[0] : 0;

    ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (ListenSocket == INVALID_SOCKET) {
        return false;
    }

    int opt = 1;
    setsockopt(ListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#if defined(SO_NOSIGPIPE)
    setsockopt(ListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (char*)&opt, sizeof(opt));
#endif
    ioctlsocket(ListenSocket, FIONBIO, &opt);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = (unsigned short)hton16((unsigned short)PlanetWestwoodPortNumber);
    addr.sin_addr.s_addr = hton32(INADDR_ANY);

    if (bind(ListenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Close_Socket();
        return false;
    }

    if (listen(ListenSocket, 10) == SOCKET_ERROR) {
        Close_Socket();
        return false;
    }

    Socket = ListenSocket;

    if (Settings.Network.TCPDiscovery) {
        UdpSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (UdpSocket != INVALID_SOCKET) {
            int yes = 1;
            setsockopt(UdpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));
            setsockopt(UdpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
            ioctlsocket(UdpSocket, FIONBIO, &yes);

            sockaddr_in uaddr;
            memset(&uaddr, 0, sizeof(uaddr));
            uaddr.sin_family = AF_INET;
            uaddr.sin_port = (unsigned short)hton16((unsigned short)PlanetWestwoodPortNumber);
            uaddr.sin_addr.s_addr = hton32(INADDR_ANY);

            if (bind(UdpSocket, (sockaddr*)&uaddr, sizeof(uaddr)) == SOCKET_ERROR) {
                closesocket(UdpSocket);
                UdpSocket = INVALID_SOCKET;
            }
        }
    }

    if (!Settings.Network.Host.empty()) {
        std::string host_str = Settings.Network.Host;
        uint16_t host_port = (uint16_t)PlanetWestwoodPortNumber;
        size_t colon_pos = host_str.find(':');
        if (colon_pos != std::string::npos) {
            host_port = (uint16_t)atoi(host_str.substr(colon_pos + 1).c_str());
            host_str = host_str.substr(0, colon_pos);
        }

        struct hostent* host_info = gethostbyname(host_str.c_str());
        if (host_info && host_info->h_addr_list && host_info->h_addr_list[0]) {
            uint32_t host_ip = *(uint32_t*)host_info->h_addr_list[0];
            Connect_To_Peer(host_ip, (uint16_t)hton16(host_port));
        }
    }

    return true;
}

void TCPInterfaceClass::Close_Socket(void)
{
    Stop_Listening();

    if (ListenSocket != INVALID_SOCKET) {
        closesocket(ListenSocket);
        ListenSocket = INVALID_SOCKET;
    }

    if (UdpSocket != INVALID_SOCKET) {
        closesocket(UdpSocket);
        UdpSocket = INVALID_SOCKET;
    }

    Close_All_Peers();
    Socket = INVALID_SOCKET;
}

bool TCPInterfaceClass::Start_Listening(void)
{
    Listening = true;
    return true;
}

void TCPInterfaceClass::Stop_Listening(void)
{
    Listening = false;
}

void TCPInterfaceClass::WriteTo(void* buffer, int buffer_len, void* address)
{
    if (address == nullptr) {
        Broadcast(buffer, buffer_len);
        return;
    }

    unsigned char* addr_bytes = (unsigned char*)address;
    uint32_t dest_ip = 0;
    memcpy(&dest_ip, addr_bytes + 4, 4);
    uint16_t dest_port = 0;
    memcpy(&dest_port, addr_bytes + 8, 2);

    if (dest_ip == 0 || dest_ip == 0xffffffff) {
        Broadcast(buffer, buffer_len);
        return;
    }

    if (dest_port == 0) {
        dest_port = (uint16_t)hton16((unsigned short)PlanetWestwoodPortNumber);
    }

    TCPPeer* peer = Find_Peer(dest_ip, dest_port);
    if (!peer) {
        peer = Connect_To_Peer(dest_ip, dest_port);
    }

    if (peer) {
        Send_Framed_Packet(peer, buffer, buffer_len);
    }

    Keyboard->Check();
}

void TCPInterfaceClass::Broadcast(void* buffer, int buffer_len)
{
    for (size_t i = 0; i < Peers.size(); i++) {
        Send_Framed_Packet(Peers[i], buffer, buffer_len);
    }

    if (!Settings.Network.Host.empty() && Peers.empty()) {
        std::string host_str = Settings.Network.Host;
        uint16_t host_port = (uint16_t)PlanetWestwoodPortNumber;
        size_t colon_pos = host_str.find(':');
        if (colon_pos != std::string::npos) {
            host_port = (uint16_t)atoi(host_str.substr(colon_pos + 1).c_str());
            host_str = host_str.substr(0, colon_pos);
        }
        struct hostent* host_info = gethostbyname(host_str.c_str());
        if (host_info && host_info->h_addr_list && host_info->h_addr_list[0]) {
            uint32_t host_ip = *(uint32_t*)host_info->h_addr_list[0];
            TCPPeer* p = Connect_To_Peer(host_ip, (uint16_t)hton16(host_port));
            if (p) {
                Send_Framed_Packet(p, buffer, buffer_len);
            }
        }
    }

    if (UdpSocket != INVALID_SOCKET) {
        for (int i = 0; i < BroadcastAddresses.Count(); i++) {
            sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = (unsigned short)hton16((unsigned short)PlanetWestwoodPortNumber);
            memcpy(&addr.sin_addr.s_addr, BroadcastAddresses[i], 4);
            sendto(UdpSocket, (const char*)buffer, buffer_len, 0, (sockaddr*)&addr, sizeof(addr));
        }
    }

    Keyboard->Check();
}

#if defined _WIN32 && !defined SDL_BUILD
int TCPInterfaceClass::Message_Handler(HWND, UINT message, UINT, LONG)
{
    if (message != WM_TCPASYNCEVENT) {
        return 1;
    }
    return 0;
}
#else
int TCPInterfaceClass::Message_Handler()
{
    if (!Listening && ListenSocket == INVALID_SOCKET) {
        return 0;
    }

    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);

    SOCKET max_fd = INVALID_SOCKET;

    if (ListenSocket != INVALID_SOCKET) {
        FD_SET(ListenSocket, &read_set);
        if (ListenSocket > max_fd) {
            max_fd = ListenSocket;
        }
    }

    if (UdpSocket != INVALID_SOCKET) {
        FD_SET(UdpSocket, &read_set);
        if (UdpSocket > max_fd) {
            max_fd = UdpSocket;
        }
    }

    for (size_t i = 0; i < Peers.size(); i++) {
        TCPPeer* peer = Peers[i];
        if (peer->socket != INVALID_SOCKET) {
            FD_SET(peer->socket, &read_set);
            if (peer->connecting || !peer->tx_buf.empty()) {
                FD_SET(peer->socket, &write_set);
            }
            if (peer->socket > max_fd) {
                max_fd = peer->socket;
            }
        }
    }

    if (max_fd == INVALID_SOCKET) {
        return 0;
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    int rc = select(max_fd + 1, &read_set, &write_set, nullptr, &tv);
    if (rc < 0) {
        return 0;
    }

    if (ListenSocket != INVALID_SOCKET && FD_ISSET(ListenSocket, &read_set)) {
        while (true) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            SOCKET client_s = accept(ListenSocket, (sockaddr*)&client_addr, &client_len);
            if (client_s == INVALID_SOCKET) {
                break;
            }

            int opt = 1;
            setsockopt(client_s, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));
#if defined(SO_NOSIGPIPE)
            setsockopt(client_s, SOL_SOCKET, SO_NOSIGPIPE, (char*)&opt, sizeof(opt));
#endif
            ioctlsocket(client_s, FIONBIO, &opt);

            uint32_t remote_ip = client_addr.sin_addr.s_addr;

            TCPPeer* existing = nullptr;
            for (size_t i = 0; i < Peers.size(); i++) {
                if (Peers[i]->ip == remote_ip) {
                    existing = Peers[i];
                    break;
                }
            }

            if (existing != nullptr) {
                if (!existing->connected && !existing->connecting) {
                    closesocket(existing->socket);
                    existing->socket = client_s;
                    existing->connected = true;
                    existing->connecting = false;
                    continue;
                }
                if (Should_Keep_Outgoing(PrimaryLocalIP, hton16(PlanetWestwoodPortNumber), remote_ip, existing->port)) {
                    closesocket(client_s);
                    continue;
                } else {
                    closesocket(existing->socket);
                    existing->socket = client_s;
                    existing->connected = true;
                    existing->connecting = false;
                    continue;
                }
            }

            TCPPeer* new_peer = new TCPPeer;
            new_peer->socket = client_s;
            new_peer->ip = remote_ip;
            new_peer->port = 0;
            new_peer->connecting = false;
            new_peer->connected = true;
            Peers.push_back(new_peer);
        }
    }

    if (UdpSocket != INVALID_SOCKET && FD_ISSET(UdpSocket, &read_set)) {
        while (true) {
            sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            int bytes =
                recvfrom(UdpSocket, (char*)ReceiveBuffer, sizeof(ReceiveBuffer), 0, (sockaddr*)&addr, &addr_len);
            if (bytes <= 0) {
                break;
            }

            bool from_self = false;
            for (int i = 0; i < LocalAddresses.Count(); i++) {
                if (!memcmp(LocalAddresses[i], &addr.sin_addr.s_addr, 4)) {
                    from_self = true;
                    break;
                }
            }
            if (from_self) {
                continue;
            }

            WinsockBufferType* packet = new WinsockBufferType;
            packet->BufferLen = bytes;
            memcpy(packet->Buffer, ReceiveBuffer, bytes);
            memset(packet->Address, 0, sizeof(packet->Address));
            memcpy(packet->Address + 4, &addr.sin_addr.s_addr, 4);
            InBuffers.Add(packet);

            TCPPeer* p = Find_Peer(addr.sin_addr.s_addr, 0);
            if (!p) {
                Connect_To_Peer(addr.sin_addr.s_addr, (uint16_t)hton16((unsigned short)PlanetWestwoodPortNumber));
            }
        }
    }

    for (size_t i = 0; i < Peers.size(); i++) {
        TCPPeer* peer = Peers[i];
        if (peer->socket == INVALID_SOCKET) {
            continue;
        }

        if (FD_ISSET(peer->socket, &write_set)) {
            if (peer->connecting) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(peer->socket, SOL_SOCKET, SO_ERROR, (char*)&err, &len) < 0 || err != 0) {
                    peer->connecting = false;
                    peer->connected = false;
                } else {
                    peer->connecting = false;
                    peer->connected = true;
                }
            }
            if (peer->connected && !peer->tx_buf.empty()) {
#ifdef MSG_NOSIGNAL
                int sent = send(peer->socket, (const char*)peer->tx_buf.data(), (int)peer->tx_buf.size(), MSG_NOSIGNAL);
#else
                int sent = send(peer->socket, (const char*)peer->tx_buf.data(), (int)peer->tx_buf.size(), 0);
#endif
                if (sent > 0) {
                    peer->tx_buf.erase(peer->tx_buf.begin(), peer->tx_buf.begin() + sent);
                } else if (sent < 0 && LastSocketError != WSAEWOULDBLOCK) {
                    peer->connected = false;
                }
            }
        }

        if (peer->connected && FD_ISSET(peer->socket, &read_set)) {
            char temp[2048];
            int n = recv(peer->socket, temp, sizeof(temp), 0);
            if (n > 0) {
                peer->rx_buf.insert(peer->rx_buf.end(), temp, temp + n);
                Process_Rx_Buffer(peer);
            } else if (n == 0 || (n < 0 && LastSocketError != WSAEWOULDBLOCK)) {
                peer->connected = false;
            }
        }
    }

    for (size_t i = 0; i < Peers.size();) {
        if (!Peers[i]->connected && !Peers[i]->connecting) {
            closesocket(Peers[i]->socket);
            delete Peers[i];
            Peers.erase(Peers.begin() + i);
        } else {
            i++;
        }
    }

    return 0;
}
#endif

#endif // NETWORKING
