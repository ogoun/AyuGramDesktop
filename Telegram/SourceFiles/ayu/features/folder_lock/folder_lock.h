// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_chat_filters.h"

class History;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Ayu {

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

// PIN-protected chat folders of one session. While a protected folder is
// locked its chats (by folder rules) are hidden everywhere in the client.
class FolderLock final : public base::has_weak_ptr {
public:
	explicit FolderLock(not_null<Main::Session*> session);
	~FolderLock();

	[[nodiscard]] bool isProtected(FilterId id) const;
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
	void verifyPin(FilterId id, QByteArray pin, Fn<void(UnlockResult)> done);
	void setPin(FilterId id, QByteArray pin, Fn<void(bool)> done);
	void changePin(
		FilterId id,
		QByteArray oldPin,
		QByteArray newPin,
		Fn<void(UnlockResult)> done);
	void removePin(FilterId id, QByteArray pin, Fn<void(UnlockResult)> done);

	void lock(FilterId id);
	void lockAll();
	// Folder rules changed (maybe on another device): close and recount.
	void rulesChanged();

private:
	struct Unlocked {
		int slot = 0;
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
		Fn<void(UnlockResult, int slot)> done);
	[[nodiscard]] uint64 userId() const;

	const not_null<Main::Session*> _session;
	base::flat_set<FilterId> _protected;
	base::flat_map<FilterId, Unlocked> _unlocked;
	base::flat_set<FilterId> _checking;
	// Protection records exist but the folders (rules) aren't loaded yet:
	// everything is treated as locked until they are.
	bool _awaitingRules = false;
	base::flat_map<FilterId, Rules> _rules;
	rpl::event_stream<> _lockChanges;
	base::Timer _autolockTimer;
	rpl::lifetime _lifetime;

};

} // namespace Ayu
