// Copyright Epic Games, Inc. All Rights Reserved.

#include "Network/UdpClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogUdpClient, Log, All)

namespace UE::CaptureManager
{

static TArray<TSharedRef<FInternetAddr>> EnumerateInterfaces()
{
	TArray<TSharedRef<FInternetAddr>> Result;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		return Result;
	}

	TArray<TSharedPtr<FInternetAddr>> AdapterAddresses;
	if (!SocketSubsystem->GetLocalAdapterAddresses(AdapterAddresses))
	{
		return Result;
	}

	TSet<uint32> SeenIPs; // dedupe - some subsystems return the same IP twice

	constexpr uint32 LoopbackHostOrder = 0x7F000001; // 127.0.0.1
	for (const TSharedPtr<FInternetAddr>& Addr : AdapterAddresses)
	{
		if (!Addr.IsValid())
		{
			continue;
		}

		// Filter to IPv4 only - IPv6 multicast joins need a separate interface address type
		// (FIPv6Address / scope_id) which isn't part of this API surface.
		if (Addr->GetProtocolType() != FNetworkProtocolTypes::IPv4)
		{
			continue;
		}

		uint32 HostOrderIp = 0;
		Addr->GetIp(HostOrderIp);

		// Ignore localhost during adapter adding
		if (HostOrderIp == LoopbackHostOrder)
		{
			continue;
		}

		bool bAlreadySeen = false;
		SeenIPs.Add(HostOrderIp, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}

		Result.Add(Addr.ToSharedRef());
	}

	return Result;
}

static bool ShouldAllowMessage(const TArray<TSharedRef<FInternetAddr>>& InNetworkInterfaces,
							   const FString& InMulticastIp,
							   const uint16 InPort,
							   const FIPv4Endpoint& InEndpoint)
{
	// Checking own messages
	bool bDontAllowMessage = InNetworkInterfaces.ContainsByPredicate([InEndpoint, InPort](const TSharedRef<FInternetAddr>& InElem)
	{
		InElem->SetPort(InPort); // Checking for own messages

		return InElem->CompareEndpoints(*InEndpoint.ToInternetAddr());
	});

	// Checking messages with sender IP set to multicast address
	bDontAllowMessage = bDontAllowMessage || (InEndpoint.Address.ToString() == InMulticastIp && InEndpoint.Port == InPort);

	return !bDontAllowMessage;
}

FUdpClient::FUdpClient()
	: bRunning(false)
{
}

FUdpClient::~FUdpClient()
{
	Stop();
}

TProtocolResult<void> FUdpClient::Init(FUdpClientConfigure InConfig, FOnSocketDataReceived InReceiveHandler)
{
	if (bRunning)
	{
		return FCaptureProtocolError(TEXT("Can't initialize the client while running."));
	}

	FIPv4Endpoint Endpoint(FIPv4Address::Any, InConfig.ListenPort);

	// Prepare socket
	UdpSocket.Reset(FUdpSocketBuilder(TEXT("CPS UDP Socket"))
					.AsNonBlocking()
					.AsReusable()
					.BoundToEndpoint(Endpoint)
					.WithReceiveBufferSize(BufferSize)
					.WithSendBufferSize(BufferSize)
					.Build());

	if (!UdpSocket)
	{
		return FCaptureProtocolError(TEXT("Failed to create a client socket"));
	}

	TArray<TSharedRef<FInternetAddr>> Nics = EnumerateInterfaces();

	if (!InConfig.MulticastIpAddress.IsEmpty())
	{
		FIPv4Endpoint MulticastEndpoint;
		FString MulticastHostAnDPort = FString::Format(TEXT("{0}:{1}"), { InConfig.MulticastIpAddress, InConfig.ListenPort });

		CHECK_BOOL(FIPv4Endpoint::FromHostAndPort(MulticastHostAnDPort, MulticastEndpoint));

		int32 JoinedCount = 0;

		if (Nics.IsEmpty())
		{
			return FCaptureProtocolError(TEXT("No network interfaces detected"));
		}

		const TSharedRef<FInternetAddr> GroupAddr = MulticastEndpoint.ToInternetAddr();

		for (const TSharedRef<FInternetAddr>& Nic : Nics)
		{
			if (UdpSocket->JoinMulticastGroup(*GroupAddr, *Nic))   // two-arg = per-interface join
			{
				++JoinedCount;
			}
		}

		if (JoinedCount == 0)
		{
			return FCaptureProtocolError(TEXT("Failed to join multicast group on any interface"));
		}

		CHECK_BOOL(UdpSocket->SetMulticastLoopback(false));
	}

	// Prepare receiver
	UdpReceiver.Reset(new FUdpSocketReceiver(UdpSocket.Get(), FTimespan::FromMilliseconds(ThreadWaitTime), TEXT("CPS RECEIVER-FUdpCommunication")));

	UdpReceiver->OnDataReceived()
		.BindLambda([ReceiveHandler = MoveTemp(InReceiveHandler), 
					Nics = MoveTemp(Nics), 
					Port = InConfig.ListenPort,
					MulticastAddress = InConfig.MulticastIpAddress](const FArrayReaderPtr& InData, const FIPv4Endpoint& InEndpoint)
		{
			if (ShouldAllowMessage(Nics, MulticastAddress, Port, InEndpoint))
			{
				ReceiveHandler.ExecuteIfBound(InData, InEndpoint);
			}
		});

	return ResultOk;
}

TProtocolResult<void> FUdpClient::Start()
{
	if (bRunning)
	{
		return FCaptureProtocolError(TEXT("The client is already started"));
	}

	UdpReceiver->Start();

	bRunning = true;

	return ResultOk;
}

TProtocolResult<void> FUdpClient::Stop()
{
	if (!bRunning)
	{
		return FCaptureProtocolError(TEXT("The client is already stopped"));
	}

	UdpSocket->Close();
	UdpReceiver->Stop();

	UdpReceiver = nullptr;
	UdpSocket = nullptr;

	bRunning = false;

	return ResultOk;
}

TProtocolResult<int32> FUdpClient::SendMessage(const TArray<uint8>& InPayload, const FString& InEndpoint)
{
	if (!UdpSocket)
	{
		return FCaptureProtocolError(TEXT("Udp socket not configured"));
	}

	FIPv4Endpoint Endpoint;
	FIPv4Endpoint::FromHostAndPort(InEndpoint, Endpoint);

	// Unicast: OS routing table picks the NIC. Same behaviour as before.
	if (!Endpoint.Address.IsMulticastAddress())
	{
		TSharedRef<FInternetAddr> Address = Endpoint.ToInternetAddr();
		int32 Sent = 0;
		if (!UdpSocket->SendTo(InPayload.GetData(), InPayload.Num(), Sent, *Address))
		{
			return FCaptureProtocolError(TEXT("Failed to send the data"));
		}
		return Sent;
	}

	TSharedRef<FInternetAddr> GroupAddr = Endpoint.ToInternetAddr();
	TArray<TSharedRef<FInternetAddr>> Nics = EnumerateInterfaces();

	if (Nics.IsEmpty())
	{
		// Not having network interfaces is considered a valid configuration
		UE_LOGF(LogUdpClient, Warning, "No network interfaces detected");
		return 0;
	}

	int32 SentOnAny = 0;
	int32 SuccessCount = 0;
	for (const TSharedRef<FInternetAddr>& Nic : Nics)
	{
		if (!UdpSocket->SetMulticastInterface(*Nic))
		{
			continue; // warn-per-NIC, don't abort the fan-out
		}

		int32 Sent = 0;
		if (UdpSocket->SendTo(InPayload.GetData(), InPayload.Num(), Sent, *GroupAddr))
		{
			SentOnAny = Sent;
			++SuccessCount;
		}
	}

	if (SuccessCount == 0)
	{
		return FCaptureProtocolError(TEXT("Failed to send multicast on any interface"));
	}

	return SentOnAny; // bytes for one fan-out leg; payload is identical across NICs
}

}
