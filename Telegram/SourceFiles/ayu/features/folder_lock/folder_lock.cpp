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
#include "ayu/features/folder_lock/folder_vault.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_folder.h"
#include "data/data_photo.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
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
	_vault = std::make_unique<FolderVault>(this, session);
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

	// Plain data of encrypted folders left from before (or of chats that
	// came into such a folder) is sealed when the chats are known.
	_session->data().chatsListLoadedEvents(
	) | rpl::on_next([=] {
		_vault->sweepAll();
	}, _lifetime);
	crl::on_main(this, [=] {
		_vault->sweepAll();
	});
}

FolderLock::~FolderLock() = default;

uint64 FolderLock::userId() const {
	return _session->userId().bare;
}

void FolderLock::refreshProtected(bool notify) {
	if (_refreshing) {
		return; // Re-entered from our own stale records purge below.
	}
	auto &settings = AyuSettings::getInstance();
	const auto &filters = _session->data().chatsFilters();
	auto records = settings.folderProtectionIds(userId());
	if (notify && filters.loaded()) {
		// Records of folders removed while offline: a new folder may get
		// the same id later.
		_refreshing = true;
		for (const auto id : records) {
			if (!ranges::contains(filters.list(), id, &Data::ChatFilter::id)) {
				// The records of a deleted folder can't be opened anymore.
				if (const auto record = settings.folderProtection(
						userId(),
						id)) {
					_vault->destroy(record->publicKey);
				}
				settings.setFolderProtection(userId(), id, std::nullopt);
			}
		}
		_refreshing = false;
		records = settings.folderProtectionIds(userId());
	}
	const auto wasAwaiting = _awaitingRules;
	_awaitingRules = !records.empty() && !filters.loaded();

	auto now = base::flat_set<FilterId>();
	auto encrypted = base::flat_set<FilterId>();
	for (const auto &filter : filters.list()) {
		if (!filter.id()) {
			continue;
		} else if (const auto record = settings.folderProtection(
				userId(),
				filter.id())) {
			now.emplace(filter.id());
			if (!record->publicKey.isEmpty()) {
				encrypted.emplace(filter.id());
			}
		}
	}
	_encrypted = std::move(encrypted);
	_encryptedRecords = ranges::any_of(records, [&](int id) {
		const auto record = settings.folderProtection(userId(), id);
		return record && !record->publicKey.isEmpty();
	});
	if (wasAwaiting && !_awaitingRules) {
		crl::on_main(this, [=] {
			_vault->flushPending();
			_vault->sweepAll();
		});
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
	const auto protectedChanged = (now != _protected);
	_protected = std::move(now);
	for (auto i = begin(_probed); i != end(_probed);) {
		if (_protected.contains(*i)) {
			++i;
		} else {
			i = _probed.erase(i);
		}
	}
	probeRemoved();
	if (protectedChanged) {
		// A PIN was set or removed, not a change of the folder rules.
		_rules = collectRules();
	}
	if (notify && (protectedChanged || (wasAwaiting != _awaitingRules))) {
		applyChanged(added);
	}
}

bool FolderLock::isProtected(FilterId id) const {
	return id && _protected.contains(id);
}

bool FolderLock::isPinRemoved(FilterId id) const {
	const auto i = _unlocked.find(id);
	return (i != end(_unlocked)) && i->second.permanent;
}

bool FolderLock::requiresPin(FilterId id) const {
	return isProtected(id) && !isPinRemoved(id);
}

bool FolderLock::isLocked(FilterId id) const {
	return isProtected(id) && !_unlocked.contains(id);
}

bool FolderLock::anyLocked() const {
	if (_awaitingRules) {
		return true;
	}
	// A folder opened with the second PIN still hides its other chats.
	return ranges::any_of(_protected, [&](FilterId id) {
		const auto i = _unlocked.find(id);
		return (i == end(_unlocked)) || (i->second.slot == 1);
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
		}
		const auto u = _unlocked.find(id);
		if (u != end(_unlocked)
			&& (u->second.slot == 0
				|| u->second.allowed.contains(history->peer->id))) {
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
	if (i == end(list) || !i->matchesRules(history)) {
		return true;
	}
	// Opened with the second PIN: only allowed chats are in its list.
	const auto u = _unlocked.find(listId);
	return (u != end(_unlocked))
		&& (u->second.slot == 1)
		&& !u->second.allowed.contains(history->peer->id);
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
		Fn<void(UnlockResult, CheckResult)> done) {
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (!record || _checking.contains(id)) {
		Wipe(pin);
		done({ .error = UnlockError::Failed }, CheckResult());
		return;
	} else if (const auto wait = secondsUntilNextTry(id)) {
		Wipe(pin);
		done({
			.error = UnlockError::TooManyTries,
			.waitSeconds = wait,
		}, CheckResult());
		return;
	}
	_checking.emplace(id);
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	const auto slots = record->slots;
	crl::async([=, pin = std::move(pin)]() mutable {
		auto result = CheckPin(pin, salt, params, id, slots);
		Wipe(pin);
		crl::on_main(weak, [=]() mutable {
			_checking.remove(id);
			auto &settings = AyuSettings::getInstance();
			auto record = settings.folderProtection(userId(), id);
			if (!record) {
				Wipe(result.key);
				done({ .error = UnlockError::Failed }, CheckResult());
				return;
			}
			if (result.failed && !result.ok) {
				// A broken record, not a wrong PIN: no retry penalty.
				done({ .error = UnlockError::Failed }, CheckResult());
				return;
			} else if (result.ok) {
				// The second PIN may be known to a coercer: it must not reset
				// the counter of tries for the real one.
				if (record->badTries && result.slot == 0) {
					record->badTries = 0;
					record->lastBadTry = 0;
					settings.setFolderProtection(userId(), id, record);
				}
				done({}, std::move(result));
			} else {
				++record->badTries;
				record->lastBadTry = base::unixtime::now();
				settings.setFolderProtection(userId(), id, record);
				done({
					.error = UnlockError::WrongPin,
					.waitSeconds = secondsUntilNextTry(id),
				}, CheckResult());
			}
		});
	});
}

void FolderLock::tryUnlock(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult)> done) {
	check(id, std::move(pin), [=](UnlockResult result, CheckResult checked) {
		Wipe(checked.key);
		if (result.error == UnlockError::None) {
			auto unlocked = Unlocked{ .slot = checked.slot };
			if (checked.slot == 0) {
				auto secret = ParseSecret(checked.content.data);
				if (secret) {
					unlocked.folderKey = SecretBytes(
						std::move(secret->folderKey));
					Wipe(secret->decoyKey);
				}
			} else if (checked.slot == 1) {
				const auto allowed = ParseAllowed(checked.content.data);
				for (const auto peer : allowed.value_or(
						std::vector<uint64_t>())) {
					unlocked.allowed.emplace(PeerId(peer));
				}
			}
			_unlocked[id] = std::move(unlocked);
			applyChanged(false);
		}
		done(result);
	});
}

void FolderLock::verifyPin(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult, int)> done) {
	check(id, std::move(pin), [=](UnlockResult result, CheckResult checked) {
		Wipe(checked.key);
		done(result, checked.slot);
	});
}

void FolderLock::setPin(FilterId id, QByteArray pin, Fn<void(bool)> done) {
	// Never over an existing record: it would destroy the real slot (and in
	// the "removed" state the typed PIN would become the real one).
	if (AyuSettings::getInstance().folderProtection(userId(), id)) {
		Wipe(pin);
		done(false);
		return;
	}
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
			auto &settings = AyuSettings::getInstance();
			const auto stored = ok
				&& !settings.folderProtection(userId(), id);
			if (stored) {
				settings.setFolderProtection(userId(), id, record);
			}
			done(stored);
		});
	});
}

void FolderLock::probeRemoved() {
	// One at a time: each probe takes the memory of a full KDF run.
	if (_probing) {
		return;
	}
	for (const auto id : _protected) {
		if (_probed.contains(id) || isPinRemoved(id)) {
			continue;
		}
		_probed.emplace(id);
		const auto record = AyuSettings::getInstance().folderProtection(
			userId(),
			id);
		if (!record) {
			continue;
		}
		const auto weak = base::make_weak(this);
		const auto salt = record->salt;
		const auto params = ParamsOf(*record);
		const auto slots = record->slots;
		_probing = true;
		crl::async([=] {
			auto result = CheckPin(QByteArray(), salt, params, id, slots);
			crl::on_main(weak, [=]() mutable {
				_probing = false;
				Wipe(result.key);
				// The record could change while the KDF was running.
				const auto now = AyuSettings::getInstance().folderProtection(
					userId(),
					id);
				const auto stale = !now || (now->slots[1] != slots[1]);
				const auto u = _unlocked.find(id);
				const auto realOpen = (u != end(_unlocked))
					&& (u->second.slot == 0);
				if (!result.ok
					|| result.slot != 1
					|| !isProtected(id)
					|| stale
					|| realOpen) {
					if (stale) {
						_probed.remove(id);
					}
					probeRemoved();
					return;
				}
				auto unlocked = Unlocked{ .slot = 1, .permanent = true };
				const auto allowed = ParseAllowed(result.content.data);
				for (const auto peer : allowed.value_or(
						std::vector<uint64_t>())) {
					unlocked.allowed.emplace(PeerId(peer));
				}
				_unlocked[id] = std::move(unlocked);
				applyChanged(false);
				probeRemoved();
			});
		});
		return;
	}
}

void FolderLock::storeSlots(
		FilterId id,
		std::optional<QByteArray> slot0,
		std::optional<QByteArray> slot1) {
	auto &settings = AyuSettings::getInstance();
	auto record = settings.folderProtection(userId(), id);
	if (!record) {
		return;
	}
	if (slot0) {
		record->slots[0] = std::move(*slot0);
	}
	if (slot1) {
		record->slots[1] = std::move(*slot1);
		_probed.remove(id);
	}
	settings.setFolderProtection(userId(), id, std::move(record));
}

std::optional<QByteArray> FolderLock::sealSecret(FilterId id) const {
	const auto i = _access.find(id);
	if (i == end(_access) || i->second.mode != AccessMode::Real) {
		return std::nullopt;
	}
	auto slot = SealSlot(i->second.key, MakeAad(id), SlotContent{
		.role = Role::Full,
		.data = SerializeSecret(i->second.secret),
	});
	if (slot.size() != kSlotSize) {
		return std::nullopt;
	}
	return slot;
}

void FolderLock::resaveSecret(FilterId id) {
	if (auto slot = sealSecret(id)) {
		storeSlots(id, std::move(slot), std::nullopt);
	}
}

bool FolderLock::consumeTry(FilterId id) {
	if (secondsUntilNextTry(id) > 0) {
		return false;
	}
	countWrongPin(id);
	return true;
}

void FolderLock::countWrongPin(FilterId id) {
	auto &settings = AyuSettings::getInstance();
	auto record = settings.folderProtection(userId(), id);
	if (record) {
		++record->badTries;
		record->lastBadTry = base::unixtime::now();
		settings.setFolderProtection(userId(), id, std::move(record));
	}
}

void FolderLock::resyncDecoyKey(FilterId id) {
	const auto i = _access.find(id);
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (i == end(_access)
		|| !record
		|| i->second.secret.decoyKey.isEmpty()) {
		return;
	}
	const auto aad = MakeAad(id);
	if (const auto opened = OpenSlot(
			i->second.secret.decoyKey,
			aad,
			record->slots[1])) {
		// Pick up changes of the allowed chats made in the decoy mode.
		const auto allowed = ParseAllowed(opened->data);
		if (allowed && *allowed != i->second.secret.allowed) {
			i->second.secret.allowed = *allowed;
			resaveSecret(id);
		}
		return;
	}
	// Maybe the PIN was "removed" in the decoy mode: try the empty PIN key.
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	const auto slot1 = record->slots[1];
	crl::async([=] {
		auto key = DeriveKey(QByteArray(), salt, 1, params);
		const auto opened = OpenSlot(key, aad, slot1);
		crl::on_main(weak, [=]() mutable {
			const auto i = _access.find(id);
			if (!opened || i == end(_access)
				|| i->second.mode != AccessMode::Real) {
				// The second PIN was changed in the decoy mode: keep the
				// known copy, the owner can set it up again.
				Wipe(key);
				return;
			}
			Wipe(i->second.secret.decoyKey);
			i->second.secret.decoyKey = key;
			i->second.secret.allowed = ParseAllowed(opened->data).value_or(
				i->second.secret.allowed);
			resaveSecret(id);
		});
	});
}

void FolderLock::wipeAccess(FilterId id) {
	const auto i = _access.find(id);
	if (i != end(_access)) {
		Wipe(i->second.key);
		Wipe(i->second.secret.decoyKey);
		_access.erase(i);
	}
}

void FolderLock::wipeAllAccess() {
	while (!_access.empty()) {
		wipeAccess(_access.begin()->first);
	}
}

void FolderLock::beginSettings(
		FilterId id,
		QByteArray pin,
		Fn<void(UnlockResult, AccessMode)> done) {
	check(id, std::move(pin), [=](UnlockResult result, CheckResult checked) {
		if (result.error != UnlockError::None) {
			Wipe(checked.key);
			done(result, AccessMode::Real);
			return;
		}
		auto access = Access{
			.key = std::move(checked.key),
			.created = crl::now(),
		};
		if (checked.slot == 0) {
			access.mode = AccessMode::Real;
			access.secret = ParseSecret(checked.content.data).value_or(
				SecretData());
		} else {
			access.mode = AccessMode::Decoy;
			access.allowed = ParseAllowed(checked.content.data).value_or(
				std::vector<uint64_t>());
		}
		wipeAccess(id);
		const auto mode = access.mode;
		_access.emplace(id, std::move(access));
		if (mode == AccessMode::Real) {
			resyncDecoyKey(id);
		}
		done(result, mode);
	});
}

bool FolderLock::beginRemovedSettings(FilterId id) {
	if (!isPinRemoved(id)) {
		return false;
	}
	// The empty PIN key is not a secret, it is derived in the operations.
	auto access = Access{
		.mode = AccessMode::Removed,
		.created = crl::now(),
	};
	for (const auto &peer : _unlocked[id].allowed) {
		access.allowed.push_back(peer.value);
	}
	wipeAccess(id);
	_access.emplace(id, std::move(access));
	return true;
}

void FolderLock::attachSettings(FilterId id) {
	const auto i = _access.find(id);
	if (i != end(_access)) {
		i->second.attached = true;
	}
}

void FolderLock::endSettings(FilterId id) {
	wipeAccess(id);
}

std::optional<AccessMode> FolderLock::settingsMode(FilterId id) const {
	const auto i = _access.find(id);
	return (i != end(_access))
		? std::make_optional(i->second.mode)
		: std::nullopt;
}

bool FolderLock::hasDecoy(FilterId id) const {
	const auto i = _access.find(id);
	return (i != end(_access))
		&& (i->second.mode == AccessMode::Real)
		&& !i->second.secret.decoyKey.isEmpty();
}

std::vector<PeerId> FolderLock::allowedChats(FilterId id) const {
	auto result = std::vector<PeerId>();
	const auto i = _access.find(id);
	if (i == end(_access)) {
		return result;
	}
	const auto &ids = (i->second.mode == AccessMode::Real)
		? i->second.secret.allowed
		: i->second.allowed;
	for (const auto peer : ids) {
		result.push_back(PeerId(peer));
	}
	return result;
}

void FolderLock::setUnlockedAllowed(
		FilterId id,
		const std::vector<uint64_t> &allowed) {
	const auto i = _unlocked.find(id);
	if (i == end(_unlocked) || i->second.slot != 1) {
		return;
	}
	i->second.allowed.clear();
	for (const auto peer : allowed) {
		i->second.allowed.emplace(PeerId(peer));
	}
	applyChanged(false);
}

void FolderLock::sealDecoy(
		FilterId id,
		QByteArray pin,
		std::vector<uint64_t> allowed,
		Fn<void(QByteArray key, QByteArray slot)> done) {
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (!record) {
		Wipe(pin);
		done(QByteArray(), QByteArray());
		return;
	}
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	crl::async([=, pin = std::move(pin)]() mutable {
		auto key = DeriveKey(pin, salt, 1, params);
		Wipe(pin);
		auto slot = key.isEmpty()
			? QByteArray()
			: SealSlot(key, MakeAad(id), SlotContent{
				.role = Role::Decoy,
				.data = SerializeAllowed(allowed),
			});
		crl::on_main(weak, [=]() mutable {
			done(std::move(key), std::move(slot));
		});
	});
}

void FolderLock::setupDecoy(
		FilterId id,
		QByteArray decoyPin,
		std::vector<PeerId> allowed,
		Fn<void(bool ok, bool same)> done) {
	const auto i = _access.find(id);
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (i == end(_access)
		|| i->second.mode != AccessMode::Real
		|| !record
		|| int(allowed.size()) > kMaxAllowed) {
		Wipe(decoyPin);
		done(false, false);
		return;
	}
	auto ids = std::vector<uint64_t>();
	for (const auto peer : allowed) {
		ids.push_back(peer.value);
	}
	// The second PIN must not open the real slot.
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	const auto slot0 = record->slots[0];
	crl::async([=, pin = std::move(decoyPin)]() mutable {
		auto realKey = DeriveKey(pin, salt, 0, params);
		const auto same = OpenSlot(realKey, MakeAad(id), slot0).has_value();
		Wipe(realKey);
		crl::on_main(weak, [=]() mutable {
			if (same) {
				Wipe(pin);
				done(false, true);
				return;
			}
			sealDecoy(id, std::move(pin), ids, [=](
					QByteArray key,
					QByteArray slot) mutable {
				const auto i = _access.find(id);
				if (slot.size() != kSlotSize
					|| i == end(_access)
					|| i->second.mode != AccessMode::Real) {
					Wipe(key);
					done(false, false);
					return;
				}
				Wipe(i->second.secret.decoyKey);
				i->second.secret.decoyKey = std::move(key);
				i->second.secret.allowed = ids;
				storeSlots(id, sealSecret(id), std::move(slot));
				done(true, false);
			});
		});
	});
}

void FolderLock::changeDecoyPin(
		FilterId id,
		QByteArray newPin,
		Fn<void(bool ok, bool same)> done) {
	const auto i = _access.find(id);
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (i == end(_access)
		|| !record
		|| (i->second.mode != AccessMode::Real
			&& i->second.mode != AccessMode::Decoy)
		|| (i->second.mode == AccessMode::Decoy && !consumeTry(id))) {
		Wipe(newPin);
		done(false, false);
		return;
	}
	const auto mode = i->second.mode;
	const auto allowed = (mode == AccessMode::Real)
		? i->second.secret.allowed
		: i->second.allowed;
	const auto seal = [=](QByteArray pin) {
		sealDecoy(id, std::move(pin), allowed, [=](
				QByteArray key,
				QByteArray slot) mutable {
			const auto i = _access.find(id);
			if (slot.size() != kSlotSize
				|| i == end(_access)
				|| i->second.mode != mode) {
				Wipe(key);
				done(false, false);
				return;
			}
			if (mode == AccessMode::Real) {
				Wipe(i->second.secret.decoyKey);
				i->second.secret.decoyKey = std::move(key);
				storeSlots(id, sealSecret(id), std::move(slot));
			} else {
				Wipe(i->second.key);
				i->second.key = std::move(key);
				storeSlots(id, std::nullopt, std::move(slot));
			}
			done(true, false);
		});
	};
	if (mode == AccessMode::Decoy) {
		// Nothing is told about the real PIN here.
		seal(std::move(newPin));
		return;
	}
	// The second PIN must not open the real slot.
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	const auto slot0 = record->slots[0];
	crl::async([=, pin = std::move(newPin)]() mutable {
		auto realKey = DeriveKey(pin, salt, 0, params);
		const auto same = OpenSlot(realKey, MakeAad(id), slot0).has_value();
		Wipe(realKey);
		crl::on_main(weak, [=]() mutable {
			if (same) {
				Wipe(pin);
				done(false, true);
			} else {
				seal(std::move(pin));
			}
		});
	});
}

bool FolderLock::setAllowedChats(
		FilterId id,
		std::vector<PeerId> allowed) {
	const auto i = _access.find(id);
	if (i == end(_access) || int(allowed.size()) > kMaxAllowed) {
		return false;
	}
	auto ids = std::vector<uint64_t>();
	for (const auto peer : allowed) {
		ids.push_back(peer.value);
	}
	const auto content = SlotContent{
		.role = Role::Decoy,
		.data = SerializeAllowed(ids),
	};
	switch (i->second.mode) {
	case AccessMode::Real: {
		if (i->second.secret.decoyKey.isEmpty()) {
			return false;
		}
		auto slot = SealSlot(i->second.secret.decoyKey, MakeAad(id), content);
		if (slot.size() != kSlotSize) {
			return false;
		}
		i->second.secret.allowed = ids;
		storeSlots(id, sealSecret(id), std::move(slot));
	} break;
	case AccessMode::Decoy: {
		auto slot = SealSlot(i->second.key, MakeAad(id), content);
		if (slot.size() != kSlotSize) {
			return false;
		}
		i->second.allowed = ids;
		storeSlots(id, std::nullopt, std::move(slot));
	} break;
	case AccessMode::Removed: {
		// The empty PIN key needs the slow KDF: seal when it is ready.
		i->second.allowed = ids;
		sealDecoy(id, QByteArray(), ids, [=](QByteArray key, QByteArray slot) {
			Wipe(key);
			const auto i = _access.find(id);
			if (slot.size() == kSlotSize
				&& i != end(_access)
				&& i->second.mode == AccessMode::Removed
				&& isPinRemoved(id)) {
				storeSlots(id, std::nullopt, std::move(slot));
			}
		});
	} break;
	}
	setUnlockedAllowed(id, ids);
	return true;
}

bool FolderLock::disableDecoy(FilterId id) {
	const auto i = _access.find(id);
	if (i == end(_access) || i->second.mode != AccessMode::Real) {
		return false;
	}
	Wipe(i->second.secret.decoyKey);
	i->second.secret.allowed.clear();
	// An unused slot is random again: nothing tells it was ever used.
	storeSlots(id, sealSecret(id), RandomBytes(kSlotSize));
	return true;
}

void FolderLock::imitateRemove(FilterId id, Fn<void(bool)> done) {
	const auto i = _access.find(id);
	if (i == end(_access) || i->second.mode != AccessMode::Decoy) {
		done(false);
		return;
	}
	const auto allowed = i->second.allowed;
	sealDecoy(id, QByteArray(), allowed, [=](
			QByteArray key,
			QByteArray slot) mutable {
		Wipe(key);
		const auto i = _access.find(id);
		if (slot.size() != kSlotSize || i == end(_access)) {
			done(false);
			return;
		}
		storeSlots(id, std::nullopt, std::move(slot));
		auto unlocked = Unlocked{ .slot = 1, .permanent = true };
		for (const auto peer : allowed) {
			unlocked.allowed.emplace(PeerId(peer));
		}
		_unlocked[id] = std::move(unlocked);
		Wipe(i->second.key);
		i->second.mode = AccessMode::Removed;
		applyChanged(false);
		done(true);
	});
}

void FolderLock::setPinFromRemoved(
		FilterId id,
		QByteArray newPin,
		Fn<void(bool)> done) {
	const auto i = _access.find(id);
	if (i == end(_access)
		|| i->second.mode != AccessMode::Removed
		|| !consumeTry(id)) {
		Wipe(newPin);
		done(false);
		return;
	}
	const auto allowed = i->second.allowed;
	sealDecoy(id, std::move(newPin), allowed, [=](
			QByteArray key,
			QByteArray slot) mutable {
		Wipe(key);
		const auto i = _access.find(id);
		if (slot.size() != kSlotSize
			|| i == end(_access)
			|| i->second.mode != AccessMode::Removed) {
			done(false);
			return;
		}
		storeSlots(id, std::nullopt, std::move(slot));
		wipeAccess(id);
		if (removeUnlocked(id)) {
			applyChanged(true);
		}
		done(true);
	});
}

void FolderLock::changeRealPin(
		FilterId id,
		QByteArray newPin,
		Fn<void(bool ok, bool same)> done) {
	const auto i = _access.find(id);
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	if (i == end(_access) || i->second.mode != AccessMode::Real || !record) {
		Wipe(newPin);
		done(false, false);
		return;
	}
	const auto weak = base::make_weak(this);
	const auto salt = record->salt;
	const auto params = ParamsOf(*record);
	const auto secret = SerializeSecret(i->second.secret);
	const auto slot1 = record->slots[1];
	crl::async([=, pin = std::move(newPin)]() mutable {
		auto decoyKey = DeriveKey(pin, salt, 1, params);
		const auto same = OpenSlot(decoyKey, MakeAad(id), slot1).has_value();
		Wipe(decoyKey);
		if (same) {
			Wipe(pin);
			crl::on_main(weak, [=] { done(false, true); });
			return;
		}
		// The salt stays, so the second slot keeps working.
		auto key = DeriveKey(pin, salt, 0, params);
		Wipe(pin);
		auto slot = key.isEmpty()
			? QByteArray()
			: SealSlot(key, MakeAad(id), SlotContent{
				.role = Role::Full,
				.data = secret,
			});
		crl::on_main(weak, [=]() mutable {
			const auto i = _access.find(id);
			if (slot.size() != kSlotSize
				|| i == end(_access)
				|| i->second.mode != AccessMode::Real) {
				Wipe(key);
				done(false, false);
				return;
			}
			storeSlots(id, std::move(slot), std::nullopt);
			Wipe(i->second.key);
			i->second.key = std::move(key);
			done(true, false);
		});
	});
}

bool FolderLock::removeRealPin(FilterId id) {
	const auto i = _access.find(id);
	if (i == end(_access) || i->second.mode != AccessMode::Real) {
		return false;
	}
	// Without the PIN there is no place for the private key: the records
	// become plain again.
	if (isEncrypted(id)) {
		const auto &key = i->second.secret.folderKey;
		if (key.isEmpty() || !_vault->unseal(id, key)) {
			_vault->destroy(publicKey(id));
		}
	}
	wipeAccess(id);
	_unlocked.remove(id);
	AyuSettings::getInstance().setFolderProtection(
		userId(),
		id,
		std::nullopt);
	return true;
}

bool FolderLock::removeUnlocked(FilterId id) {
	wipeAccess(id);
	return _unlocked.remove(id);
}

void FolderLock::lock(FilterId id) {
	wipeAccess(id);
	const auto i = _unlocked.find(id);
	if (i != end(_unlocked) && !i->second.permanent) {
		_unlocked.erase(i);
		applyChanged(true);
	}
}

void FolderLock::lockAll() {
	const auto hadAccess = !_access.empty();
	wipeAllAccess();
	// Folders with a "removed" PIN have nothing to lock.
	auto changed = false;
	for (auto i = begin(_unlocked); i != end(_unlocked);) {
		if (i->second.permanent) {
			++i;
		} else {
			i = _unlocked.erase(i);
			changed = true;
		}
	}
	if (changed) {
		applyChanged(true);
	} else if (hadAccess) {
		_lockChanges.fire({}); // Open settings boxes lose their access.
	}
}

auto FolderLock::collectRules() const -> base::flat_map<FilterId, Rules> {
	auto result = base::flat_map<FilterId, Rules>();
	for (const auto &filter : _session->data().chatsFilters().list()) {
		if (isProtected(filter.id())) {
			using Flag = Data::ChatFilter::Flag;
			// Pinned chats come inside always(), pinning a chat that is in
			// the folder by its type anyway doesn't change what is hidden.
			auto always = filter.always();
			for (const auto &history : filter.pinned()) {
				if (filter.matchesByType(history)
					&& !filter.never().contains(history)) {
					always.remove(history);
				}
			}
			result.emplace(filter.id(), Rules{
				.flags = (filter.flags() & Flag::RulesMask),
				.always = std::move(always),
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
	// Chats that came into an encrypted folder.
	crl::on_main(this, [=] {
		_vault->sweepAll();
	});
	wipeAllAccess();
	for (auto i = begin(_unlocked); i != end(_unlocked);) {
		if (i->second.permanent) {
			++i;
		} else {
			i = _unlocked.erase(i);
		}
	}
	applyChanged(true);
}

void FolderLock::checkAutolock() {
	auto orphans = std::vector<FilterId>();
	for (const auto &[id, access] : _access) {
		if (!access.attached
			&& crl::now() - access.created >= kAutolockCheckPeriod) {
			orphans.push_back(id);
		}
	}
	for (const auto id : orphans) {
		wipeAccess(id);
	}
	if (_unlocked.empty()) {
		return;
	}
	const auto idle = crl::now() - Core::App().lastNonIdleTime();
	auto locking = std::vector<FilterId>();
	for (const auto &[id, unlocked] : _unlocked) {
		if (unlocked.permanent) {
			continue;
		}
		const auto minutes = autolockMinutes(id);
		if (minutes > 0 && idle >= minutes * 60 * crl::time(1000)) {
			locking.push_back(id);
		}
	}
	for (const auto id : locking) {
		_unlocked.remove(id);
		wipeAccess(id);
	}
	if (!locking.empty()) {
		applyChanged(true);
	}
}

bool FolderLock::isEncrypted(FilterId id) const {
	return _encrypted.contains(id);
}

bool FolderLock::setEncrypted(FilterId id, bool enabled) {
	const auto i = _access.find(id);
	auto &settings = AyuSettings::getInstance();
	auto record = settings.folderProtection(userId(), id);
	if (i == end(_access) || i->second.mode != AccessMode::Real || !record) {
		return false;
	} else if (enabled == !record->publicKey.isEmpty()) {
		return true;
	}
	auto &secret = i->second.secret;
	if (enabled) {
		auto pair = GenerateKeyPair();
		if (pair.publicKey.isEmpty()) {
			return false;
		}
		Wipe(secret.folderKey);
		secret.folderKey = pair.privateKey;
		auto slot0 = sealSecret(id);
		if (!slot0) {
			Wipe(secret.folderKey);
			Wipe(pair.privateKey);
			return false;
		}
		record->slots[0] = std::move(*slot0);
		record->publicKey = pair.publicKey;
		settings.setFolderProtection(userId(), id, std::move(record));
		const auto u = _unlocked.find(id);
		if (u != end(_unlocked) && u->second.slot == 0) {
			u->second.folderKey = SecretBytes(pair.privateKey);
		}
		Wipe(pair.privateKey);
		_vault->sweep(id);
		// Media of the folder chats could be cached before.
		_session->data().cache().clear();
		_session->data().cacheBigFile().clear();
	} else {
		if (!secret.folderKey.isEmpty()
			&& !_vault->unseal(id, secret.folderKey)) {
			return false;
		}
		const auto oldPublicKey = record->publicKey;
		Wipe(secret.folderKey);
		auto slot0 = sealSecret(id);
		if (!slot0) {
			return false;
		}
		record->slots[0] = std::move(*slot0);
		record->publicKey = QByteArray();
		settings.setFolderProtection(userId(), id, std::move(record));
		_vault->destroy(oldPublicKey); // Left only if they couldn't be opened.
		const auto u = _unlocked.find(id);
		if (u != end(_unlocked)) {
			u->second.folderKey = SecretBytes();
		}
		_vault->forgetClosed();
	}
	_lockChanges.fire({});
	return true;
}

bool FolderLock::isSealed(not_null<History*> history) const {
	if (_awaitingRules) {
		return _encryptedRecords;
	}
	return sealFolder(history).has_value();
}

bool FolderLock::isSealed(not_null<PeerData*> peer) const {
	if (_awaitingRules) {
		return _encryptedRecords;
	} else if (_encrypted.empty()) {
		return false;
	}
	return isSealed(peer->owner().history(peer));
}

bool FolderLock::isSealedMedia(
		not_null<const DocumentData*> document) const {
	if (!_encryptedRecords) {
		return false;
	} else if (_awaitingRules) {
		return true;
	}
	return _session->data().ayuAnyMediaItem(document, [&](
			not_null<HistoryItem*> item) {
		return isSealed(item->history());
	});
}

bool FolderLock::isSealedMedia(not_null<const PhotoData*> photo) const {
	if (!_encryptedRecords) {
		return false;
	} else if (_awaitingRules) {
		return true;
	}
	return _session->data().ayuAnyMediaItem(photo, [&](
			not_null<HistoryItem*> item) {
		return isSealed(item->history());
	});
}

bool FolderLock::isSealedOrigin(const Data::FileOrigin &origin) const {
	if (!_encryptedRecords) {
		return false;
	} else if (_awaitingRules) {
		return true;
	}
	const auto peer = [&](PeerId id) {
		return id && isSealed(_session->data().peer(id));
	};
	return v::match(origin.data, [&](const Data::FileOriginMessage &data) {
		return peer(data.peer);
	}, [&](const Data::FileOriginPeerPhoto &data) {
		return peer(data.peerId);
	}, [&](const Data::FileOriginUserPhoto &data) {
		return peer(peerFromUser(data.userId));
	}, [&](const Data::FileOriginFullUser &data) {
		return peer(peerFromUser(data.userId));
	}, [](const auto &) {
		return false;
	});
}

FolderVault &FolderLock::vault() {
	return *_vault;
}

bool FolderLock::rulesPending() const {
	return _awaitingRules;
}

bool FolderLock::hasEncryptedRecords() const {
	return _encryptedRecords;
}

std::vector<FilterId> FolderLock::encryptedFolders() const {
	return _encrypted | ranges::to_vector;
}

QByteArray FolderLock::publicKey(FilterId id) const {
	const auto record = AyuSettings::getInstance().folderProtection(
		userId(),
		id);
	return record ? record->publicKey : QByteArray();
}

QByteArray FolderLock::privateKey(FilterId id) const {
	const auto i = _unlocked.find(id);
	return (i != end(_unlocked) && i->second.slot == 0 && isEncrypted(id))
		? i->second.folderKey.data
		: QByteArray();
}

std::optional<FilterId> FolderLock::sealFolder(
		not_null<History*> history) const {
	if (_encrypted.empty()) {
		return std::nullopt;
	}
	const auto &list = _session->data().chatsFilters().list();
	for (const auto id : _encrypted) {
		const auto i = ranges::find(list, id, &Data::ChatFilter::id);
		if (i != end(list) && i->matchesRules(history)) {
			return id;
		}
	}
	return std::nullopt;
}

void FolderLock::applyChanged(bool locking) {
	_vault->forgetClosed();
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
