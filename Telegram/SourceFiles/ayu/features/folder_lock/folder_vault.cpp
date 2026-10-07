// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_vault.h"

#include "ayu/ayu_settings.h"
#include "ayu/data/ayu_database.h"
#include "ayu/features/folder_lock/folder_lock.h"
#include "ayu/features/folder_lock/folder_lock_crypto.h"
#include "ayu/utils/telegram_helpers.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

namespace Ayu {
namespace {

using namespace FolderLockCrypto;
using namespace FolderVaultCodec;

// Records kept while the folders are not loaded yet (never written plain).
constexpr auto kMaxPending = 10000;

[[nodiscard]] std::vector<char> ToVector(const QByteArray &data) {
	return std::vector<char>(data.begin(), data.end());
}

[[nodiscard]] QByteArray ToBytes(const std::vector<char> &data) {
	return QByteArray(data.data(), int(data.size()));
}

[[nodiscard]] std::vector<char> Tag(const QByteArray &publicKey) {
	return ToVector(KeyTag(publicKey));
}

[[nodiscard]] std::vector<char> SealRecord(
		const QByteArray &publicKey,
		Kind kind,
		const AyuMessageBase &message) {
	auto data = SerializeRecord({ .kind = kind, .message = message });
	const auto box = SealBox(publicKey, data);
	Wipe(data);
	return ToVector(box);
}

void WritePlain(Kind kind, const AyuMessageBase &message) {
	if (kind == Kind::Deleted) {
		auto plain = DeletedMessage();
		static_cast<AyuMessageBase&>(plain) = message;
		AyuDatabase::addDeletedMessage(plain);
	} else {
		auto plain = EditedMessage();
		static_cast<AyuMessageBase&>(plain) = message;
		AyuDatabase::addEditedMessage(plain);
	}
}

} // namespace

FolderVault::FolderVault(
	not_null<FolderLock*> lock,
	not_null<Main::Session*> session)
: _lock(lock)
, _session(session) {
}

FolderVault::~FolderVault() {
	while (!_opened.empty()) {
		forget(_opened.begin()->first);
	}
	for (auto &pending : _pending) {
		auto record = Record{ .message = std::move(pending.message) };
		WipeRecord(record);
	}
}

ID FolderVault::userId() const {
	return ID(_session->userId().bare & PeerId::kChatTypeMask);
}

bool FolderVault::store(
		not_null<History*> history,
		Kind kind,
		const AyuMessageBase &message) {
	if (_lock->rulesPending()) {
		if (!_lock->hasEncryptedRecords()) {
			return false;
		} else if (_pending.size() < kMaxPending) {
			_pending.push_back({ history, kind, message });
		}
		return true;
	}
	const auto id = _lock->sealFolder(history);
	if (!id) {
		return false;
	} else if (!seal(*id, kind, message)) {
		// Never written plain: losing a record is better than leaking it.
		LOG(("AyuGram: could not seal a record of an encrypted folder."));
	}
	return true;
}

bool FolderVault::seal(
		FilterId id,
		Kind kind,
		const AyuMessageBase &message) {
	const auto publicKey = _lock->publicKey(id);
	if (publicKey.isEmpty()) {
		return false;
	}
	auto blob = SealRecord(publicKey, kind, message);
	if (blob.empty()) {
		return false;
	}
	const auto fakeId = AyuDatabase::addSealedMessage(SealedMessage{
		0,
		userId(),
		Tag(publicKey),
		std::move(blob),
	});
	if (!fakeId) {
		return false;
	}
	const auto i = _opened.find(id);
	if (i != end(_opened)) {
		i->second.push_back({
			.fakeId = fakeId,
			.record = { .kind = kind, .message = message },
		});
		i->second.back().record.message.fakeId = fakeId;
	}
	return true;
}

void FolderVault::flushPending() {
	if (_lock->rulesPending()) {
		return;
	}
	auto pending = base::take(_pending);
	for (auto &entry : pending) {
		if (!store(entry.history, entry.kind, entry.message)) {
			WritePlain(entry.kind, entry.message);
		}
		auto record = Record{ .message = std::move(entry.message) };
		WipeRecord(record);
	}
}

std::vector<FolderVault::Entry> *FolderVault::opened(FilterId id) {
	auto privateKey = _lock->privateKey(id);
	if (privateKey.isEmpty()) {
		forget(id);
		return nullptr;
	}
	const auto i = _opened.find(id);
	if (i != end(_opened)) {
		Wipe(privateKey);
		return &i->second;
	}
	auto entries = std::vector<Entry>();
	const auto tag = Tag(_lock->publicKey(id));
	for (const auto &row : AyuDatabase::getSealedMessages(userId(), tag)) {
		auto data = OpenBox(privateKey, ToBytes(row.blob));
		if (!data) {
			continue;
		}
		auto record = ParseRecord(*data);
		Wipe(*data);
		if (record && record->message.userId == userId()) {
			record->message.fakeId = row.fakeId;
			entries.push_back({ .fakeId = row.fakeId, .record = std::move(*record) });
		}
	}
	Wipe(privateKey);
	return &_opened.emplace(id, std::move(entries)).first->second;
}

template <typename Callback>
void FolderVault::enumerate(Callback &&callback) {
	for (const auto id : _lock->encryptedFolders()) {
		if (const auto entries = opened(id)) {
			for (const auto &entry : *entries) {
				callback(entry);
			}
		}
	}
}

void FolderVault::addDeleted(
		ID dialogId,
		ID topicId,
		ID minId,
		ID maxId,
		int limit,
		const QString &search,
		std::vector<AyuMessageBase> &result) {
	auto added = false;
	enumerate([&](const Entry &entry) {
		const auto &m = entry.record.message;
		if (entry.record.kind != Kind::Deleted
			|| m.dialogId != dialogId
			|| (topicId != 0 && m.topicId != topicId)
			|| (minId != 0 && m.messageId <= minId)
			|| (maxId != 0 && m.messageId >= maxId)) {
			return;
		} else if (!search.isEmpty()
			&& !QString::fromStdString(m.text).contains(
				search,
				Qt::CaseInsensitive)) {
			return;
		}
		result.push_back(m);
		added = true;
	});
	if (!added) {
		return;
	}
	ranges::stable_sort(result, ranges::greater(), &AyuMessageBase::messageId);
	if (limit > 0 && int(result.size()) > limit) {
		result.resize(limit);
	}
}

void FolderVault::addEdited(
		ID dialogId,
		ID messageId,
		ID minId,
		ID maxId,
		int limit,
		std::vector<AyuMessageBase> &result) {
	auto added = false;
	enumerate([&](const Entry &entry) {
		const auto &m = entry.record.message;
		if (entry.record.kind != Kind::Edited
			|| m.dialogId != dialogId
			|| m.messageId != messageId
			|| (minId != 0 && entry.fakeId <= minId)
			|| (maxId != 0 && entry.fakeId >= maxId)) {
			return;
		}
		result.push_back(m);
		added = true;
	});
	if (!added) {
		return;
	}
	ranges::stable_sort(result, ranges::greater(), &AyuMessageBase::fakeId);
	if (limit > 0 && int(result.size()) > limit) {
		result.resize(limit);
	}
}

bool FolderVault::hasDeleted(ID dialogId, ID topicId) {
	auto result = false;
	enumerate([&](const Entry &entry) {
		const auto &m = entry.record.message;
		result = result
			|| (entry.record.kind == Kind::Deleted
				&& m.dialogId == dialogId
				&& (topicId == 0 || m.topicId == topicId));
	});
	return result;
}

bool FolderVault::hasRevisions(ID dialogId, ID messageId) {
	auto result = false;
	enumerate([&](const Entry &entry) {
		const auto &m = entry.record.message;
		result = result
			|| (entry.record.kind == Kind::Edited
				&& m.dialogId == dialogId
				&& m.messageId == messageId);
	});
	return result;
}

void FolderVault::removeDeleted(ID dialogId, ID messageId) {
	auto ids = std::vector<ID>();
	enumerate([&](const Entry &entry) {
		const auto &m = entry.record.message;
		if (entry.record.kind == Kind::Deleted
			&& m.dialogId == dialogId
			&& m.messageId == messageId) {
			ids.push_back(entry.fakeId);
		}
	});
	removeEntries(ids);
}

void FolderVault::clearDeleted(ID dialogId, ID topicId) {
	auto ids = std::vector<ID>();
	enumerate([&](const Entry &entry) {
		const auto &m = entry.record.message;
		if (entry.record.kind == Kind::Deleted
			&& m.dialogId == dialogId
			&& (topicId == 0 || m.topicId == topicId)) {
			ids.push_back(entry.fakeId);
		}
	});
	removeEntries(ids);
}

void FolderVault::removeEntries(const std::vector<ID> &fakeIds) {
	if (fakeIds.empty()) {
		return;
	}
	AyuDatabase::removeSealedMessages(fakeIds);
	for (auto &[id, entries] : _opened) {
		for (auto i = begin(entries); i != end(entries);) {
			if (ranges::contains(fakeIds, i->fakeId)) {
				WipeRecord(i->record);
				i = entries.erase(i);
			} else {
				++i;
			}
		}
	}
}

std::vector<not_null<History*>> FolderVault::histories(FilterId id) const {
	const auto &filters = _session->data().chatsFilters().list();
	const auto i = ranges::find(filters, id, &Data::ChatFilter::id);
	if (i == end(filters)) {
		return {};
	}
	auto result = base::flat_set<not_null<History*>>();
	const auto add = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			if (const auto history = row->history()) {
				if (i->matchesRules(history)) {
					result.emplace(history);
				}
			}
		}
	};
	add(_session->data().chatsList());
	if (const auto folder = _session->data().folderLoaded(Data::Folder::kId)) {
		add(folder->chatsList());
	}
	for (const auto &history : i->always()) {
		result.emplace(history);
	}
	return result | ranges::to_vector;
}

void FolderVault::sweep(FilterId id) {
	const auto publicKey = _lock->publicKey(id);
	if (publicKey.isEmpty() || _lock->rulesPending()) {
		return;
	}
	const auto list = histories(id);
	auto dialogIds = std::vector<ID>();
	dialogIds.reserve(list.size());
	for (const auto &history : list) {
		dialogIds.push_back(getDialogIdFromPeer(history->peer));
	}
	const auto sealed = AyuDatabase::sealPlainMessages(
		userId(),
		dialogIds,
		Tag(publicKey),
		[&](int kind, const AyuMessageBase &message) {
			return SealRecord(publicKey, Kind(kind), message);
		});
	if (sealed) {
		LOG(("AyuGram: %1 records of an encrypted folder sealed.").arg(sealed));
		forget(id);
	}
	// Writing them again removes the plain files (see storage_account.cpp).
	auto &local = _session->local();
	for (const auto &history : list) {
		local.writeDrafts(history);
		local.writeDraftCursors(history);
	}
	local.writeSearchSuggestionsDelayed();
}

void FolderVault::sweepAll() {
	for (const auto id : _lock->encryptedFolders()) {
		sweep(id);
	}
}

void FolderVault::forget(FilterId id) {
	const auto i = _opened.find(id);
	if (i == end(_opened)) {
		return;
	}
	for (auto &entry : i->second) {
		WipeRecord(entry.record);
	}
	_opened.erase(i);
}

void FolderVault::forgetClosed() {
	auto closed = std::vector<FilterId>();
	for (const auto &[id, entries] : _opened) {
		auto key = _lock->privateKey(id);
		if (key.isEmpty()) {
			closed.push_back(id);
		}
		Wipe(key);
	}
	for (const auto id : closed) {
		forget(id);
	}
}

bool FolderVault::unseal(FilterId id, const QByteArray &privateKey) {
	const auto publicKey = PublicKeyOf(privateKey);
	if (publicKey.isEmpty()) {
		return false;
	}
	const auto tag = Tag(publicKey);
	auto messages = std::vector<AyuDatabase::UnsealedMessage>();
	for (const auto &row : AyuDatabase::getSealedMessages(userId(), tag)) {
		auto data = OpenBox(privateKey, ToBytes(row.blob));
		if (!data) {
			continue;
		}
		auto record = ParseRecord(*data);
		Wipe(*data);
		if (record) {
			messages.push_back({
				.sealedId = row.fakeId,
				.kind = int(record->kind),
				.message = std::move(record->message),
			});
		}
	}
	const auto ok = AyuDatabase::unsealMessages(userId(), tag, messages);
	for (auto &message : messages) {
		auto record = Record{ .message = std::move(message.message) };
		WipeRecord(record);
	}
	forget(id);
	return ok;
}

void FolderVault::collectGarbage() {
	auto &settings = AyuSettings::getInstance();
	const auto user = _session->userId().bare;
	auto keep = std::vector<std::vector<char>>();
	for (const auto id : settings.folderProtectionIds(user)) {
		const auto record = settings.folderProtection(user, id);
		if (record && !record->publicKey.isEmpty()) {
			keep.push_back(Tag(record->publicKey));
		}
	}
	AyuDatabase::removeSealedExcept(userId(), keep);
}

void FolderVault::destroy(const QByteArray &publicKey) {
	if (!publicKey.isEmpty()) {
		AyuDatabase::removeSealedByTag(userId(), Tag(publicKey));
	}
}

} // namespace Ayu
