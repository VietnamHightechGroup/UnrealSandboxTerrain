// Copyright blackw 2015-2020

#include "TerrainServerComponent.h"
#include "SandboxTerrainController.h"
#include "NetworkMessage.h"



UTerrainServerComponent::UTerrainServerComponent(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {

}

void UTerrainServerComponent::BeginPlay() {
	Super::BeginPlay();

	const FString VdServerSocketName = TEXT("TerrainServer");
	const int Port = (GetTerrainController()->ServerPort == 0) ? 6000 : GetTerrainController()->ServerPort;

	UE_LOG(LogSandboxTerrain, Log, TEXT("Server: Start at port %d"), Port);

	FIPv4Endpoint Endpoint(FIPv4Address(0, 0, 0, 0), Port);
	FSocket* ListenerSocketPtr = FTcpSocketBuilder(*VdServerSocketName).AsReusable().BoundToEndpoint(Endpoint).Listening(8);

	const int32 ReceiveBufferSize = 2 * 1024 * 1024;
	int32 NewSize = 0;
	ListenerSocketPtr->SetReceiveBufferSize(ReceiveBufferSize, NewSize);

	TcpListenerPtr = new FTcpListener(*ListenerSocketPtr);
	TcpListenerPtr->OnConnectionAccepted().BindUObject(this, &UTerrainServerComponent::OnConnectionAccepted);
}

void UTerrainServerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
	Super::EndPlay(EndPlayReason);

	UE_LOG(LogSandboxTerrain, Log, TEXT("Server: Shutdown voxel data server..."));

	//TODO close all sockets

	if (TcpListenerPtr) {
		TcpListenerPtr->GetSocket()->Close();
		delete TcpListenerPtr;
	}
}

void UTerrainServerComponent::BeginDestroy() {
	Super::BeginDestroy();
}

bool UTerrainServerComponent::OnConnectionAccepted(FSocket* SocketPtr, const FIPv4Endpoint& Endpoint) {
	TSharedRef<FInternetAddr> RemoteAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	SocketPtr->GetPeerAddress(*RemoteAddress);
	const FString RemoteAddressString = RemoteAddress->ToString(true);

	Mutex.lock();
	uint32 ClientId = ClientCount;
	ConnectedClientsMap.Add(ClientId, SocketPtr);
	ClientCount++;
	Mutex.unlock();

	UE_LOG(LogSandboxTerrain, Log, TEXT("Server: connection accepted -> %s"), *RemoteAddressString);

	// TODO use one thread and socket select
	GetTerrainController()->AddAsyncTask([=]() { HandleClientConnection(ClientId); });

	return true;
}

void UTerrainServerComponent::HandleClientConnection(uint32 ClientId) {
	auto SocketPtr = ConnectedClientsMap[ClientId];

	TSharedRef<FInternetAddr> RemoteAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	SocketPtr->GetPeerAddress(*RemoteAddress);
	const FString RemoteAddressString = RemoteAddress->ToString(true);

	while (true) {
		if (SocketPtr->GetConnectionState() != ESocketConnectionState::SCS_Connected) {
			UE_LOG(LogSandboxTerrain, Log, TEXT("Server: connection finished"));
			break;
		}

		if (SocketPtr->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(1))) {
			FArrayReader Data;
			FSimpleAbstractSocket_FSocket SimpleAbstractSocket(SocketPtr);
			if (FNFSMessageHeader::ReceivePayload(Data, SimpleAbstractSocket)) {
				HandleRcvData(ClientId, SocketPtr, Data);
			} else {
				UE_LOG(LogSandboxTerrain, Log, TEXT("Server: connection failed"));
				break;
			}
		}

		if (GetTerrainController()->IsWorkFinished()) {
			break;
		}
	}

	UE_LOG(LogSandboxTerrain, Log, TEXT("Server: close connection -> %s"), *RemoteAddressString);

	Mutex.lock();
	ConnectedClientsMap.Remove(ClientId);
	Mutex.unlock();

	SocketPtr->Close();
	delete SocketPtr;
}


/*
template <typename... Ts>
void UTerrainServerComponent::SendToAllClients(uint32 OpCode, Ts... Args) {
	FBufferArchive SendBuffer;

	SendBuffer << OpCode;

	for (auto Arg : { Args... }) {
		SendBuffer << Arg;
	}

	for (auto& Elem : ConnectedClientsMap) {
		FSocket* SocketPtr = Elem.Value;
		Super::NetworkSend(SocketPtr, SendBuffer);
	}
}
*/

void UTerrainServerComponent::SendToAllVdEdit(const TEditTerrainParam& EditParams) {
	TEditTerrainParam Params = EditParams; // workaround
	FBufferArchive SendBuffer;

	static uint32 OpCode = Net_Opcode_EditVd;
	static uint32 OpCodeExt = 1;

	SendBuffer << OpCode;
	SendBuffer << OpCodeExt;
	SendBuffer << Params.Origin.X;
	SendBuffer << Params.Origin.Y;;
	SendBuffer << Params.Origin.Z;
	SendBuffer << Params.Radius;
	SendBuffer << Params.Extend;
	SendBuffer << Params.Type;

	for (auto& Elem : ConnectedClientsMap) {
		FSocket* SocketPtr = Elem.Value;
		Super::NetworkSend(SocketPtr, SendBuffer);
	}
}

bool UTerrainServerComponent::SendVdByIndex(FSocket* SocketPtr, const TVoxelIndex& ZoneIndex) {
	TVoxelIndex Index = ZoneIndex;
	static uint32 OpCode = Net_Opcode_ResponseVd;
	static uint32 OpCodeExt = Net_Opcode_None;

	FSimpleAbstractSocket_FSocket SimpleAbstractSocket(SocketPtr);
	FBufferArchive SendBuffer;

	SendBuffer << OpCode;
	SendBuffer << OpCodeExt;
	SendBuffer << Index.X;
	SendBuffer << Index.Y;
	SendBuffer << Index.Z;

	GetTerrainController()->NetworkSerializeZone(SendBuffer, Index);
	return FNFSMessageHeader::WrapAndSendPayload(SendBuffer, SimpleAbstractSocket);
}

bool UTerrainServerComponent::SendMapInfo(FSocket* SocketPtr, TArray<std::tuple<TVoxelIndex, TZoneModificationData>> Area) {
	static uint32 OpCode = Net_Opcode_ResponseMapInfo;
	static uint32 OpCodeExt = Net_Opcode_None;

	FSimpleAbstractSocket_FSocket SimpleAbstractSocket(SocketPtr);
	FBufferArchive SendBuffer;

	uint32 Size = Area.Num();

	SendBuffer << OpCode;
	SendBuffer << OpCodeExt;

	SendBuffer << Size;

	for (int32 I = 0; I != Area.Num(); ++I) {
		const auto& Element = Area[I];
		TVoxelIndex ElemIndex = std::get<0>(Element);
		TZoneModificationData ElemData = std::get<1>(Element);
		ConvertVoxelIndex(SendBuffer, ElemIndex);
		SendBuffer << ElemData.ChangeCounter;

		UE_LOG(LogSandboxTerrain, Log, TEXT("Server: change counter %d %d %d - %d"), ElemIndex.X, ElemIndex.Y, ElemIndex.Z, ElemData.ChangeCounter);
	}

	return FNFSMessageHeader::WrapAndSendPayload(SendBuffer, SimpleAbstractSocket);
}

void UTerrainServerComponent::HandleRcvData(uint32 ClientId, FSocket* SocketPtr, FArrayReader& Data) {
	TSharedRef<FInternetAddr> RemoteAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	SocketPtr->GetPeerAddress(*RemoteAddress);
	const FString RemoteAddressString = RemoteAddress->ToString(true);

	uint32 OpCode;
	Data << OpCode;

	uint32 OpCodeExt;
	Data << OpCodeExt;

	UE_LOG(LogSandboxTerrain, Log, TEXT("Server: OpCode -> %d"), OpCode);
	UE_LOG(LogSandboxTerrain, Log, TEXT("Server: OpCodeExt -> %d"), OpCodeExt);

	if (OpCode == Net_Opcode_RequestVd) {
		TVoxelIndex Index = DeserializeVoxelIndex(Data);
		UE_LOG(LogSandboxTerrain, Log, TEXT("Server: Client %s requests vd at %d %d %d"), *RemoteAddressString, Index.X, Index.Y, Index.Z);
		SendVdByIndex(SocketPtr, Index);
	}

	if (OpCode == Net_Opcode_RequestVd) {
		TVoxelIndex Index = DeserializeVoxelIndex(Data);
		UE_LOG(LogSandboxTerrain, Log, TEXT("Server: Client %s requests vd at %d %d %d"), *RemoteAddressString, Index.X, Index.Y, Index.Z);
		SendVdByIndex(SocketPtr, Index);
	}

	if (OpCode == Net_Opcode_RequestMapInfo) {
		UE_LOG(LogSandboxTerrain, Log, TEXT("Server: Client %s requests map info"), *RemoteAddressString);

		// TODO remove hardcode
		TArray<std::tuple<TVoxelIndex, TZoneModificationData>> Area = GetTerrainController()->NetworkServerMapInfo();
		SendMapInfo(SocketPtr, Area);
	}
}