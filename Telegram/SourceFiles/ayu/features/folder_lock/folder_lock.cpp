// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_lock.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/folder_lock/folder_lock_crypto.h"
#include "ayu/features/folder_lock/folder_lock_ui.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "window/notifications_manager.h"

#include <crl/crl_async.h>

namespace Ayu {
namespace {

using namespace FolderLockCrypto;

constexpr auto kAutolockCheckPeriod = 15 * crl::time(1000);

// Ctrl+Shift+L locks protected folders of all accounts.
void SetupLockShortcutOnce() {
	static auto lifetime = rpl::lifetime();
	if (lifetime) {
		return;
	}
	Shortcuts::Requests(
	) | rpl::on_next([](not_null<Shortcuts::Request*> request) {
		using Command = Shortcuts::Command;
		request->check(Command::AyuLockFolders) && request->handle([] {
			for (const auto &[index, account]
					: Core::App().domain().accounts()) {
				if (const auto session = account->maybeSession()) {
					session->data().chatsFilters().folderLock().lockAll();
				}
			}
			return true;
		});
	}, lifetime);
}

[[nodiscard]] KdfParams ParamsOf(const FolderProtectionRecord &record) {
	return KdfParams{
		.n = record.kdfN,
		.r = record.kdfR,
		.p = record.kdfP,
	};
}

} // namespace

FolderLock::FolderLock(not_null<Main::Session*> session)
: _session(session)
, _autolockTimer([=] { checkAutolock(); }) {
	// No notify here: FolderLock is created lazily, possibly from inside
	// Session::refreshChatListEntry(), and a full refresh would re-enter it.
	refreshProtected(false);
	_rules = collectRules();

	AyuSettings::getInstance().folderProtectionChanges(
	) | rpl::on_next([=] {
		refreshProtected(true);
	}, _lifetime);

	Core::App().passcodeLockChanges(
	) | rpl::filter(rpl::mappers::_1) | rpl::on_next([=] {
		lockAll();
	}, _lifetime);

	_autolockTimer.callEach(kAutolockCheckPeriod);
	SetupLockShortcutOnce();
}

FolderLock::~FolderLock() = default;

uint64 FolderLock::userId() const {
	return _session->userId().bare;
}

void FolderLock::refreshProtected(bool notify) {
	auto &settings = AyuSettings::getInstance();
	const auto &filters = _session->data().chatsFilters();
	const auto records = settings.folderProtectionIds(userId());
	const auto wasAwaiting = _awaitingRules;
	_awaitingRules = !records.empty() && !filters.loaded();
	if (notify && !_awaitingRules && filters.loaded()) {
		// Records of folders removed while offline: a new folder may get
		// the same id later.
		for (const auto id : records) {
			if (!ranges::contains(filters.list(), id, &Data::ChatFilter::id)) {
				settings.setFolderProtection(userId(), id, std::nullopt);
				return; // Re-entered through folderProtectionChanges().
			}
		}
	}
	auto now = base::flat_set<FilterId>();
	for (const auto &filter : filters.list()) {
		if (filter.id() && settings.folderProtection(userId(), filter.id())) {
			now.emplace(filter.id());
		}
	}
	for (auto i = begin(_unlocked); i != end(_unlocked);) {
		if (now.contains(i->first)) {
			++i;
		} else {
			i = _unlocked.erase(i);
		}
	}
	const auto added = ranges::any_of(now, [&](FilterId id) {
		return !_protected.contains(id);
	});
	const auto changed = (now != _protected)
		|| (wasAwaiting != _awaitingRules);
	_protected = std::move(now);
	if (notify && changed) {
		applyChanged(added);
	}
}

bool FolderLock::isProtected(FilterId id) const {
	return id && _protected.contains(id);
}

bool FolderLock::isLocked(FilterId id) const {
	return isProtected(id) && !_unlocked.contains(id);
}

bool FolderLock::anyLocked() const {
	if (_awaitingRules) {
		return true;
	}
	return ranges::any_of(_protected, [&](FilterId id) {
		return !_unlocked.contains(id);
	});
}

bool FolderLock::isLocked(not_null<History*> history) const {
	if (_awaitingRules) {
		return true;
	} else if (!anyLocked()) {
		return false;
	}
	// Hidden if it matches a locked protected folder and no unlocked one:
	// a folder unlocked by the user shows all of its chats.
	const auto &list = _session->data().chatsFilters().list();
	auto locked = false;
	for (const auto id : _protected) {
		const auto i = ranges::find(list, id, &Data::ChatFilter::id);
		if (i == end(list) || !i->matchesRules(history)) {
			continue;
		} else if (_unlocked.contains(id)) {
			return false;
		}
		locked = true;
	}
	return locked;
}

bool FolderLock::isLocked(not_null<PeerData*> peer) const {
	if (!anyLocked()) {
		return false;
	}
	// A not yet loaded chat still matches by its type.
	return isLocked(peer->owner().history(peer));
}

bool FolderLock::isLockedForList(
		not_null<History*> history,
		FilterId listId) const {
	if (!isLocked(history)) {
		return false;
	} else if (!isProtected(listId)) {
		return true;
	}
	// A protected folder keeps its own chats, its list is gated by the PIN.
	const auto &list = _session->data().chatsFilters().list();
	const auto i = ranges::find(list, listId, &Data::ChatFilter::id);
	return (i == end(list)) || !i->matchesRules(history);
}

rpl::producer<> FolderLock::lockChanges() const {
	return _lockChanges.events();
}

int FolderLock::autolockMinutes(FilterId id) const {
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	return record ? record->autolockMinutes : 0;
}

void FolderLock::setAutolockMinutes(FilterId id, int minutes) {
	auto &settings = AyuSettings::getInstance();
	auto record = settings.folderProtection(userId(), id);
	if (!record || record->autolockMinutes == minutes) {
		return;
	}
	record->autolockMinutes = minutes;
	settings.setFolderProtection(userId(), id, std::move(record));
}

int FolderLock::secondsUntilNextTry(FilterId id) const {
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (!record) {
		return 0;
	}
	const auto delay = RetryDelaySeconds(record->badTries);
	const auto passed = int64(base::unixtime::now()) - record->lastBadTry;
	return int(std::clamp(delay - passed, int64(0), int64(delay)));
}

void FolderLock::check(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult, int slot)> done) {
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (!record || _checking.contains(id)) {
		Wipe(pin);
		done({ .error = UnlockError::Failed }, -1);
		return;
	} else if (const auto wait = secondsUntilNextTry(id)) {
		Wipe(pin);
		done({ .error = UnlockError::TooManyTries, .waitSeconds = wait }, -1);
		return;
	}
	_checking.emplace(id);
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	const auto slots = record->slots;
	crl::async([=, pin = std::move(pin)]() mutable {
		const auto result = CheckPin(pin, salt, params, id, slots);
		Wipe(pin);
		crl::on_main(weak, [=] {
			_checking.remove(id);
			auto &settings = AyuSettings::getInstance();
			auto record = settings.folderProtection(userId(), id);
			if (!record) {
				done({ .error = UnlockError::Failed }, -1);
				return;
			}
			if (result.failed && !result.ok) {
				// A broken record, not a wrong PIN: no retry penalty.
				done({ .error = UnlockError::Failed }, -1);
				return;
			} else if (result.ok) {
				if (record->badTries) {
					record->badTries = 0;
					record->lastBadTry = 0;
					settings.setFolderProtection(userId(), id, record);
				}
				done({}, result.slot);
			} else {
				++record->badTries;
				record->lastBadTry = base::unixtime::now();
				settings.setFolderProtection(userId(), id, record);
				done({
					.error = UnlockError::WrongPin,
					.waitSeconds = secondsUntilNextTry(id),
				}, -1);
			}
		});
	});
}

void FolderLock::tryUnlock(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult)> done) {
	check(id, std::move(pin), [=](UnlockResult result, int slot) {
		if (result.error == UnlockError::None) {
			_unlocked[id] = Unlocked{ .slot = slot };
			applyChanged(false);
		}
		done(result);
	});
}

void FolderLock::verifyPin(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult)> done) {
	check(id, std::move(pin), [=](UnlockResult result, int) {
		done(result);
	});
}

void FolderLock::setPin(FilterId id, QByteArray pin, Fn<void(bool)> done) {
	const auto weak = base::make_weak(this);
	const auto minutes = autolockMinutes(id);
	crl::async([=, pin = std::move(pin)]() mutable {
		auto record = FolderProtectionRecord();
		const auto params = KdfParams();
		record.autolockMinutes = minutes ? minutes : 15;
		record.salt = RandomBytes(kSaltSize);
		record.kdfN = params.n;
		record.kdfR = params.r;
		record.kdfP = params.p;
		auto key = DeriveKey(pin, record.salt, 0, params);
		Wipe(pin);
		record.slots[0] = SealSlot(key, MakeAad(id), SlotContent());
		Wipe(key);
		// An unused slot is random: nothing tells whether it is used.
		record.slots[1] = RandomBytes(kSlotSize);
		const auto ok = (record.salt.size() == kSaltSize)
			&& (record.slots[0].size() == kSlotSize)
			&& (record.slots[1].size() == kSlotSize);
		crl::on_main(weak, [=] {
			if (ok) {
				AyuSettings::getInstance().setFolderProtection(
					userId(),
					id,
					record);
			}
			done(ok);
		});
	});
}

void FolderLock::changePin(
		FilterId id,
		QByteArray oldPin,
		QByteArray newPin,
		Fn<void(UnlockResult)> done) {
	check(id, std::move(oldPin), [=](UnlockResult result, int) {
		if (result.error != UnlockError::None) {
			done(result);
			return;
		}
		setPin(id, newPin, [=](bool ok) {
			done({ .error = ok ? UnlockError::None : UnlockError::Failed });
		});
	});
}

void FolderLock::removePin(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult)> done) {
	check(id, std::move(pin), [=](UnlockResult result, int) {
		if (result.error == UnlockError::None) {
			AyuSettings::getInstance().setFolderProtection(
				userId(),
				id,
				std::nullopt);
		}
		done(result);
	});
}

void FolderLock::lock(FilterId id) {
	if (_unlocked.remove(id)) {
		applyChanged(true);
	}
}

void FolderLock::lockAll() {
	if (!_unlocked.empty()) {
		_unlocked.clear();
		applyChanged(true);
	}
}

auto FolderLock::collectRules() const -> base::flat_map<FilterId, Rules> {
	auto result = base::flat_map<FilterId, Rules>();
	for (const auto &filter : _session->data().chatsFilters().list()) {
		if (isProtected(filter.id())) {
			using Flag = Data::ChatFilter::Flag;
			result.emplace(filter.id(), Rules{
				.flags = (filter.flags() & Flag::RulesMask),
				.always = filter.always(),
				.never = filter.never(),
			});
		}
	}
	return result;
}

void FolderLock::rulesChanged() {
	refreshProtected(true);
	// Pinning, reordering or renaming don't change what is hidden, so they
	// don't lock. Changed rules (maybe from another device) lock everything.
	auto rules = collectRules();
	if (rules == _rules) {
		return;
	}
	_rules = std::move(rules);
	_unlocked.clear();
	applyChanged(true);
}

void FolderLock::checkAutolock() {
	if (_unlocked.empty()) {
		return;
	}
	const auto idle = crl::now() - Core::App().lastNonIdleTime();
	auto locking = std::vector<FilterId>();
	for (const auto &[id, unlocked] : _unlocked) {
		const auto minutes = autolockMinutes(id);
		if (minutes > 0 && idle >= minutes * 60 * crl::time(1000)) {
			locking.push_back(id);
		}
	}
	for (const auto id : locking) {
		_unlocked.remove(id);
	}
	if (!locking.empty()) {
		applyChanged(true);
	}
}

void FolderLock::applyChanged(bool locking) {
	_session->data().ayuRefreshAllChatLists();
	_session->data().notifyUnreadBadgeChanged();
	_lockChanges.fire({});
	if (locking) {
		cleanupLocked();
	}
}

void FolderLock::cleanupLocked() {
	auto &notifications = Core::App().notifications();
	const auto clear = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			if (const auto history = row->history()) {
				if (isLocked(not_null(history))) {
					notifications.clearFromHistory(history);
				}
			}
		}
	};
	clear(_session->data().chatsList());
	if (const auto folder = _session->data().folderLoaded(
			Data::Folder::kId)) {
		clear(folder->chatsList());
	}
	CloseLockedChatsInWindows(_session);
}

} // namespace Ayu
