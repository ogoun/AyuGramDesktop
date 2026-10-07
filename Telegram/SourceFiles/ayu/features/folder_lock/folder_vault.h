// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/features/folder_lock/folder_vault_codec.h"
#include "data/data_chat_filters.h"

class History;

namespace Main {
class Session;
} // namespace Main

namespace Ayu {

class FolderLock;

// AyuGram records (deleted / edited messages) of the chats of encrypted
// folders: sealed with the folder public key when written, opened into
// memory only while the folder is unlocked with the real PIN.
class FolderVault final {
public:
	FolderVault(not_null<FolderLock*> lock, not_null<Main::Session*> session);
	~FolderVault();

	// True if the record is taken: sealed, or kept in memory until the
	// folders are loaded. False - it is written as a plain one.
	[[nodiscard]] bool store(
		not_null<History*> history,
		FolderVaultCodec::Kind kind,
		const AyuMessageBase &message);

	// Add the opened records to the plain ones (same filters and order).
	void addDeleted(
		ID dialogId,
		ID topicId,
		ID minId,
		ID maxId,
		int limit,
		const QString &search,
		std::vector<AyuMessageBase> &result);
	void addEdited(
		ID dialogId,
		ID messageId,
		ID minId,
		ID maxId,
		int limit,
		std::vector<AyuMessageBase> &result);
	[[nodiscard]] bool hasDeleted(ID dialogId, ID topicId);
	[[nodiscard]] bool hasRevisions(ID dialogId, ID messageId);
	void removeDeleted(ID dialogId, ID messageId);
	void clearDeleted(ID dialogId, ID topicId);

	// The folders are loaded: write the kept records.
	void flushPending();
	// Plain data of the folder chats left on the disk becomes encrypted
	// (AyuGram records) or is removed (drafts, recent search chats).
	void sweep(FilterId id);
	void sweepAll();
	// Drops the opened records of folders that are not open now.
	void forgetClosed();
	// The encryption is turned off: the records become plain again.
	[[nodiscard]] bool unseal(FilterId id, const QByteArray &privateKey);
	// The folder is deleted: its records can't be opened by anyone anymore.
	void destroy(const QByteArray &publicKey);

private:
	struct Entry {
		ID fakeId = 0;
		FolderVaultCodec::Record record;
	};
	struct Pending {
		not_null<History*> history;
		FolderVaultCodec::Kind kind = FolderVaultCodec::Kind::Deleted;
		AyuMessageBase message;
	};

	[[nodiscard]] ID userId() const;
	[[nodiscard]] std::vector<Entry> *opened(FilterId id);
	template <typename Callback>
	void enumerate(Callback &&callback);
	bool seal(
		FilterId id,
		FolderVaultCodec::Kind kind,
		const AyuMessageBase &message);
	void forget(FilterId id);
	void removeEntries(const std::vector<ID> &fakeIds);
	[[nodiscard]] std::vector<not_null<History*>> histories(
		FilterId id) const;

	const not_null<FolderLock*> _lock;
	const not_null<Main::Session*> _session;
	base::flat_map<FilterId, std::vector<Entry>> _opened;
	std::vector<Pending> _pending;

};

} // namespace Ayu
