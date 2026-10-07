// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/features/folder_lock/folder_lock_crypto.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_chat_filters.h"

class History;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ayu {

class FolderVault;

enum class UnlockError {
	None,
	WrongPin,
	TooManyTries,
	Failed,
};

struct UnlockResult {
	UnlockError error = UnlockError::None;
	int waitSeconds = 0;
};

// How the folder settings were opened: with the real PIN, with the second
// (decoy) PIN, or without a PIN after it was "removed" in the decoy mode.
enum class AccessMode {
	Real,
	Decoy,
	Removed,
};

// PIN-protected chat folders of one session. While a protected folder is
// locked its chats (by folder rules) are hidden everywhere in the client.
class FolderLock final : public base::has_weak_ptr {
public:
	explicit FolderLock(not_null<Main::Session*> session);
	~FolderLock();

	[[nodiscard]] bool isProtected(FilterId id) const;
	// The PIN was "removed" in the decoy mode: no PIN is asked, only the
	// allowed chats are shown, the hidden part stays hidden.
	[[nodiscard]] bool isPinRemoved(FilterId id) const;
	[[nodiscard]] bool requiresPin(FilterId id) const;
	[[nodiscard]] bool isLocked(FilterId id) const;
	[[nodiscard]] bool isLocked(not_null<History*> history) const;
	[[nodiscard]] bool isLocked(not_null<PeerData*> peer) const;
	// Locked and must not be shown in the list of the folder `listId`.
	[[nodiscard]] bool isLockedForList(
		not_null<History*> history,
		FilterId listId) const;
	[[nodiscard]] bool anyLocked() const;
	[[nodiscard]] rpl::producer<> lockChanges() const;

	[[nodiscard]] int autolockMinutes(FilterId id) const;
	void setAutolockMinutes(FilterId id, int minutes);
	[[nodiscard]] int secondsUntilNextTry(FilterId id) const;

	void tryUnlock(FilterId id, QByteArray pin, Fn<void(UnlockResult)> done);
	// `slot` is 0 for the real PIN, 1 for the second one.
	void verifyPin(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult, int slot)> done);
	void setPin(FilterId id, QByteArray pin, Fn<void(bool)> done);

	// Settings access: the key of the checked slot lives in memory only
	// while the folder settings box is open.
	void beginSettings(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult, AccessMode)> done);
	bool beginRemovedSettings(FilterId id);
	// The settings box took the access (others are dropped soon).
	void attachSettings(FilterId id);
	void endSettings(FilterId id);
	[[nodiscard]] std::optional<AccessMode> settingsMode(FilterId id) const;
	[[nodiscard]] bool hasDecoy(FilterId id) const;
	[[nodiscard]] std::vector<PeerId> allowedChats(FilterId id) const;

	void setupDecoy(
		FilterId id,
		QByteArray decoyPin,
		std::vector<PeerId> allowed,
		Fn<void(bool ok, bool same)> done);
	void changeDecoyPin(
		FilterId id,
		QByteArray newPin,
		Fn<void(bool ok, bool same)> done);
	bool setAllowedChats(FilterId id, std::vector<PeerId> allowed);
	bool disableDecoy(FilterId id);
	void imitateRemove(FilterId id, Fn<void(bool)> done);
	void setPinFromRemoved(
		FilterId id,
		QByteArray newPin,
		Fn<void(bool)> done);
	void changeRealPin(
		FilterId id,
		QByteArray newPin,
		Fn<void(bool ok, bool same)> done);
	bool removeRealPin(FilterId id);
	// A real-only PIN check got the second PIN: counts as a wrong one.
	void countWrongPin(FilterId id);

	// Local encryption of the folder chats (with the real PIN settings).
	[[nodiscard]] bool isEncrypted(FilterId id) const;
	bool setEncrypted(FilterId id, bool enabled);
	// Data of the chat must not be written plain on the disk.
	[[nodiscard]] bool isSealed(not_null<History*> history) const;
	[[nodiscard]] bool isSealed(not_null<PeerData*> peer) const;
	[[nodiscard]] FolderVault &vault();

	// For FolderVault.
	[[nodiscard]] bool rulesPending() const;
	[[nodiscard]] bool hasEncryptedRecords() const;
	[[nodiscard]] std::vector<FilterId> encryptedFolders() const;
	[[nodiscard]] QByteArray publicKey(FilterId id) const;
	// Only while the folder is unlocked with the real PIN.
	[[nodiscard]] QByteArray privateKey(FilterId id) const;
	[[nodiscard]] std::optional<FilterId> sealFolder(
		not_null<History*> history) const;

	void lock(FilterId id);
	void lockAll();
	// Folder rules changed (maybe on another device): close and recount.
	void rulesChanged();

private:
	// Zeroed when destroyed (best effort, see FolderLockCrypto::Wipe).
	struct SecretBytes {
		SecretBytes() = default;
		explicit SecretBytes(QByteArray data) : data(std::move(data)) {
		}
		SecretBytes(const SecretBytes &other) = default;
		SecretBytes(SecretBytes &&other) = default;
		SecretBytes &operator=(const SecretBytes &other) = default;
		SecretBytes &operator=(SecretBytes &&other) = default;
		~SecretBytes() {
			FolderLockCrypto::Wipe(data);
		}

		QByteArray data;
	};
	struct Unlocked {
		int slot = 0; // 0 = all chats, 1 = second bottom (allowed only).
		base::flat_set<PeerId> allowed;
		bool permanent = false; // The PIN was "removed".
		SecretBytes folderKey; // Slot 0 of an encrypted folder.
	};
	struct Access {
		AccessMode mode = AccessMode::Real;
		QByteArray key; // Real: slot 0 key, Decoy: slot 1 key.
		FolderLockCrypto::SecretData secret; // Real only.
		std::vector<uint64_t> allowed; // Decoy and Removed.
		crl::time created = 0;
		bool attached = false; // Taken by an open settings box.
	};
	struct Rules {
		Data::ChatFilter::Flags flags;
		base::flat_set<not_null<History*>> always;
		base::flat_set<not_null<History*>> never;

		friend inline bool operator==(const Rules &, const Rules &) = default;
	};

	void refreshProtected(bool notify);
	[[nodiscard]] base::flat_map<FilterId, Rules> collectRules() const;
	void applyChanged(bool locking);
	void cleanupLocked();
	void checkAutolock();
	void check(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult, FolderLockCrypto::CheckResult)> done);
	void probeRemoved();
	void storeSlots(
		FilterId id,
		std::optional<QByteArray> slot0,
		std::optional<QByteArray> slot1);
	void resaveSecret(FilterId id);
	[[nodiscard]] std::optional<QByteArray> sealSecret(FilterId id) const;
	// A PIN change without the real PIN is an attempt, like a wrong PIN.
	[[nodiscard]] bool consumeTry(FilterId id);
	void resyncDecoyKey(FilterId id);
	void wipeAccess(FilterId id);
	void wipeAllAccess();
	void sealDecoy(
		FilterId id,
		QByteArray pin,
		std::vector<uint64_t> allowed,
		Fn<void(QByteArray key, QByteArray slot)> done);
	void setUnlockedAllowed(FilterId id, const std::vector<uint64_t> &allowed);
	bool removeUnlocked(FilterId id);
	[[nodiscard]] uint64 userId() const;

	const not_null<Main::Session*> _session;
	base::flat_set<FilterId> _protected;
	base::flat_map<FilterId, Unlocked> _unlocked;
	base::flat_set<FilterId> _checking;
	base::flat_set<FilterId> _probed;
	bool _probing = false;
	base::flat_set<FilterId> _encrypted;
	bool _encryptedRecords = false; // Also of folders not loaded yet.
	std::unique_ptr<FolderVault> _vault;
	base::flat_map<FilterId, Access> _access;
	// Protection records exist but the folders (rules) aren't loaded yet:
	// everything is treated as locked until they are.
	bool _awaitingRules = false;
	bool _refreshing = false;
	base::flat_map<FilterId, Rules> _rules;
	rpl::event_stream<> _lockChanges;
	base::Timer _autolockTimer;
	rpl::lifetime _lifetime;

};

} // namespace Ayu
