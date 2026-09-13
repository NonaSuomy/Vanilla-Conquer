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

#ifndef WSPTCP_H
#define WSPTCP_H

#include "wsproto.h"
#include "sockets.h"
#include <vector>
#include <string>

#pragma pack(push, 1)
struct TCPPacketHeader
{
    uint16_t magic;       // 0x5643 ('V', 'C')
    uint16_t length;      // length of payload in network byte order
    uint16_t listen_port; // sender's listening port in network byte order
};
#pragma pack(pop)

static const uint16_t TCP_PACKET_MAGIC = 0x5643;

class TCPInterfaceClass : public WinsockInterfaceClass
{
public:
    TCPInterfaceClass(void);
    virtual ~TCPInterfaceClass(void);

#if defined _WIN32 && !defined SDL_BUILD
    virtual int Message_Handler(HWND window, UINT message, UINT wParam, LONG lParam) override;
#else
    virtual int Message_Handler() override;
#endif

    virtual bool Open_Socket(SOCKET socketnum) override;
    virtual void Close_Socket(void) override;
    virtual bool Start_Listening(void) override;
    virtual void Stop_Listening(void) override;
    virtual void WriteTo(void* buffer, int buffer_len, void* address) override;
    virtual void Broadcast(void* buffer, int buffer_len) override;
    virtual void Set_Broadcast_Address(void* address) override;

    virtual ProtocolEnum Get_Protocol(void) override
    {
        return (PROTOCOL_TCP);
    }

    virtual int Protocol_Event_Message(void) override
    {
        return (WM_TCPASYNCEVENT);
    }

private:
    struct TCPPeer
    {
        SOCKET socket;
        uint32_t ip;                       // In network byte order (s_addr)
        uint16_t port;                     // Peer's listening port in network byte order
        bool connecting;                   // Non-blocking connect in progress
        bool connected;                    // Connection active
        std::vector<unsigned char> rx_buf; // Stream accumulation
        std::vector<unsigned char> tx_buf; // Outgoing framed bytes
    };

    TCPPeer* Find_Peer(uint32_t ip, uint16_t port);
    TCPPeer* Find_Peer_By_Socket(SOCKET s);
    TCPPeer* Connect_To_Peer(uint32_t ip, uint16_t port);
    void Close_Peer(size_t index);
    void Close_All_Peers();
    void Send_Framed_Packet(TCPPeer* peer, const void* buffer, int buffer_len);
    void Process_Rx_Buffer(TCPPeer* peer);

    SOCKET ListenSocket;
    SOCKET UdpSocket; // Optional UDP socket for LAN discovery

    std::vector<TCPPeer*> Peers;
    DynamicVectorClass<unsigned char*> BroadcastAddresses;
    DynamicVectorClass<unsigned char*> LocalAddresses;

    uint32_t PrimaryLocalIP;
    bool Listening;
};

#endif // WSPTCP_H
