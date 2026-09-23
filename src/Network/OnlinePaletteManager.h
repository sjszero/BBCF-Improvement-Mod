#pragma once

#include "RoomManager.h"

#include "Palette/PaletteManager.h"

#include <queue>
#include <string>

class OnlinePaletteManager
{
public:
	// Whether an opponent lets us save their custom palette. Unknown means no
	// permission packet arrived (mod version without the feature, or no mod).
	enum class PaletteDownloadPermission : uint8_t
	{
		Unknown,
		Denied,
		Granted,
	};

	OnlinePaletteManager(PaletteManager* pPaletteManager, CharPaletteHandle* pP1CharPalHandle,
		CharPaletteHandle* pP2CharPalHandle, RoomManager* pRoomManager);
	void SendPalettePackets();
	void RecvPaletteDataPacket(Packet* packet);
	void RecvPaletteInfoPacket(Packet* packet);
	void RecvPaletteDownloadPermissionPacket(Packet* packet);
	void RecvPlatinumVoiceChoicePacket(Packet* packet);
	// Opponent's forced Platinum voice choice (settings enum: 0 = Default/leave RNG,
	// 1 = Luna, 2 = Sena). Returns 0 when nothing was received for that slot (opponent
	// on vanilla RNG or without the mod). Used by the per-frame voice force.
	int GetPlayerVoiceChoice(uint16_t matchPlayerIndex) const;
	void ProcessSavedPalettePackets();
	void ClearSavedPalettePacketQueues();
	void OnMatchInit();
	void OnUpdate();
	bool CanDownloadPalette(uint16_t matchPlayerIndex) const;
	PaletteDownloadPermission GetDownloadPermission(uint16_t matchPlayerIndex) const;

	// The palette a player sent, once all of it has arrived and been checked against the
	// block list. `withheld` means it is not being shown because it or its sender is blocked.
	struct ReceivedPaletteView
	{
		bool decided = false;
		bool withheld = false;
		uint64_t hash = 0;
		const IMPL_data_t* data = nullptr; // valid until the next palette arrives
	};
	bool GetReceivedPalette(uint16_t matchPlayerIndex, ReceivedPaletteView& out) const;

	// Who is in a match slot, for blocking them. False when that slot is not a mod player.
	bool GetMatchPlayerIdentity(uint16_t matchPlayerIndex, uint64_t* steamId, std::string* name) const;

private:
	bool IsPaletteHandleReady(const CharPaletteHandle& charPalHandle) const;
	void SendPaletteDownloadPermissionPacket(uint16_t roomMemberIndex);
	void SendPlatinumVoiceChoicePacket(uint16_t roomMemberIndex);
	void SendPaletteInfoPacket(CharPaletteHandle& charPalHandle, uint16_t roomMemberIndex);
	void SendPaletteDataPackets(CharPaletteHandle& charPalHandle, uint16_t roomMemberIndex);
	CharPaletteHandle& GetPlayerCharPaletteHandle(uint16_t matchPlayerIndex);

	// A palette as it arrives: eight files and an info packet, collected per match slot
	// and only shown once complete and not blocked.
	struct ReceivedPalette
	{
		bool files[IMPL_PALETTE_FILES_COUNT] = {};
		bool haveInfo = false;
		IMPL_data_t data = {};
		bool decided = false;   // complete and checked against the block list
		bool withheld = false;  // blocked, so not shown
		uint64_t hash = 0;
		uint64_t senderSteamId = 0;
		std::string senderName;
	};

	void IdentifySender(ReceivedPalette& received, uint16_t roomMemberIndex);
	bool IsBlocked(const ReceivedPalette& received) const;
	void ShowReceived(uint16_t matchPlayerIndex);
	void TryApplyReceived(uint16_t matchPlayerIndex);
	void ReapplyBlocks();

	ReceivedPalette m_received[2];
	int m_blockRevision = 0;

	CharPaletteHandle* m_pP1CharPalHandle;
	CharPaletteHandle* m_pP2CharPalHandle;

	// Interfaces
	PaletteManager* m_pPaletteManager;
	RoomManager* m_pRoomManager;
	bool m_matchInitPending = false;
	bool m_loggedMatchInitWait = false;
	PaletteDownloadPermission m_playerPaletteDownloadPermissions[2] = {};

	// Received per-slot Platinum voice choice; -1 = none received yet (leave RNG).
	int8_t m_playerVoiceChoices[2] = { -1, -1 };
};
