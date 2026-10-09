// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/ayu_database.h"

#include "ayu/data/batched_writer.h"
#include "ayu/data/entities.h"
#include "ayu/libs/sqlite/sqlite_orm.h"
#include "base/unixtime.h"

#include <crl/crl_queue.h>

#include <atomic>
#include <mutex>

using namespace sqlite_orm;

// Recursive: queued writes are drained by functions that already hold it.
std::recursive_mutex databaseMutex;

auto storage = make_storage(
	"./tdata/ayudata.db",
	make_table<SchemaVersion>(
		"SchemaVersion",
		make_column("id", &SchemaVersion::id, primary_key()),
		make_column("version", &SchemaVersion::version)
	),
	make_index("idx_deleted_message_userId_dialogId_topicId_messageId",
			   column<DeletedMessage>(&DeletedMessage::userId),
			   column<DeletedMessage>(&DeletedMessage::dialogId),
			   column<DeletedMessage>(&DeletedMessage::topicId),
			   column<DeletedMessage>(&DeletedMessage::messageId)),
	make_index("idx_edited_message_userId_dialogId_messageId",
			   column<EditedMessage>(&EditedMessage::userId),
			   column<EditedMessage>(&EditedMessage::dialogId),
			   column<EditedMessage>(&EditedMessage::messageId)),
	make_table<DeletedMessage>(
		"DeletedMessage",
		make_column("fakeId", &DeletedMessage::fakeId, primary_key().autoincrement()),
		make_column("userId", &DeletedMessage::userId),
		make_column("dialogId", &DeletedMessage::dialogId),
		make_column("groupedId", &DeletedMessage::groupedId),
		make_column("peerId", &DeletedMessage::peerId),
		make_column("fromId", &DeletedMessage::fromId),
		make_column("topicId", &DeletedMessage::topicId),
		make_column("messageId", &DeletedMessage::messageId),
		make_column("date", &DeletedMessage::date),
		make_column("flags", &DeletedMessage::flags),
		make_column("editDate", &DeletedMessage::editDate),
		make_column("views", &DeletedMessage::views),
		make_column("fwdFlags", &DeletedMessage::fwdFlags),
		make_column("fwdFromId", &DeletedMessage::fwdFromId),
		make_column("fwdName", &DeletedMessage::fwdName),
		make_column("fwdDate", &DeletedMessage::fwdDate),
		make_column("fwdPostAuthor", &DeletedMessage::fwdPostAuthor),
		make_column("replyFlags", &DeletedMessage::replyFlags),
		make_column("replyMessageId", &DeletedMessage::replyMessageId),
		make_column("replyPeerId", &DeletedMessage::replyPeerId),
		make_column("replyTopId", &DeletedMessage::replyTopId),
		make_column("replyForumTopic", &DeletedMessage::replyForumTopic),
		make_column("replySerialized", &DeletedMessage::replySerialized),
		make_column("entityCreateDate", &DeletedMessage::entityCreateDate),
		make_column("text", &DeletedMessage::text),
		make_column("textEntities", &DeletedMessage::textEntities),
		make_column("mediaPath", &DeletedMessage::mediaPath),
		make_column("hqThumbPath", &DeletedMessage::hqThumbPath),
		make_column("documentType", &DeletedMessage::documentType),
		make_column("documentSerialized", &DeletedMessage::documentSerialized),
		make_column("thumbsSerialized", &DeletedMessage::thumbsSerialized),
		make_column("documentAttributesSerialized", &DeletedMessage::documentAttributesSerialized),
		make_column("mimeType", &DeletedMessage::mimeType)
	),
	make_table<EditedMessage>(
		"EditedMessage",
		make_column("fakeId", &EditedMessage::fakeId, primary_key().autoincrement()),
		make_column("userId", &EditedMessage::userId),
		make_column("dialogId", &EditedMessage::dialogId),
		make_column("groupedId", &EditedMessage::groupedId),
		make_column("peerId", &EditedMessage::peerId),
		make_column("fromId", &EditedMessage::fromId),
		make_column("topicId", &EditedMessage::topicId),
		make_column("messageId", &EditedMessage::messageId),
		make_column("date", &EditedMessage::date),
		make_column("flags", &EditedMessage::flags),
		make_column("editDate", &EditedMessage::editDate),
		make_column("views", &EditedMessage::views),
		make_column("fwdFlags", &EditedMessage::fwdFlags),
		make_column("fwdFromId", &EditedMessage::fwdFromId),
		make_column("fwdName", &EditedMessage::fwdName),
		make_column("fwdDate", &EditedMessage::fwdDate),
		make_column("fwdPostAuthor", &EditedMessage::fwdPostAuthor),
		make_column("replyFlags", &EditedMessage::replyFlags),
		make_column("replyMessageId", &EditedMessage::replyMessageId),
		make_column("replyPeerId", &EditedMessage::replyPeerId),
		make_column("replyTopId", &EditedMessage::replyTopId),
		make_column("replyForumTopic", &EditedMessage::replyForumTopic),
		make_column("replySerialized", &EditedMessage::replySerialized),
		make_column("entityCreateDate", &EditedMessage::entityCreateDate),
		make_column("text", &EditedMessage::text),
		make_column("textEntities", &EditedMessage::textEntities),
		make_column("mediaPath", &EditedMessage::mediaPath),
		make_column("hqThumbPath", &EditedMessage::hqThumbPath),
		make_column("documentType", &EditedMessage::documentType),
		make_column("documentSerialized", &EditedMessage::documentSerialized),
		make_column("thumbsSerialized", &EditedMessage::thumbsSerialized),
		make_column("documentAttributesSerialized", &EditedMessage::documentAttributesSerialized),
		make_column("mimeType", &EditedMessage::mimeType)
	),
	make_table<DeletedDialog>(
		"DeletedDialog",
		make_column("fakeId", &DeletedDialog::fakeId, primary_key().autoincrement()),
		make_column("userId", &DeletedDialog::userId),
		make_column("dialogId", &DeletedDialog::dialogId),
		make_column("peerId", &DeletedDialog::peerId),
		make_column("folderId", &DeletedDialog::folderId),
		make_column("topMessage", &DeletedDialog::topMessage),
		make_column("lastMessageDate", &DeletedDialog::lastMessageDate),
		make_column("flags", &DeletedDialog::flags),
		make_column("entityCreateDate", &DeletedDialog::entityCreateDate)
	),
	make_table<RegexFilter>(
		"RegexFilter",
		make_column("id", &RegexFilter::id, primary_key()),
		make_column("text", &RegexFilter::text),
		make_column("enabled", &RegexFilter::enabled),
		make_column("reversed", &RegexFilter::reversed),
		make_column("caseInsensitive", &RegexFilter::caseInsensitive),
		make_column("dialogId", &RegexFilter::dialogId)
	),
	make_table<RegexFilterGlobalExclusion>(
		"RegexFilterGlobalExclusion",
		make_column("fakeId", &RegexFilterGlobalExclusion::fakeId, primary_key().autoincrement()),
		make_column("dialogId", &RegexFilterGlobalExclusion::dialogId),
		make_column("filterId", &RegexFilterGlobalExclusion::filterId)
	),
	make_table<SealedMessage>(
		"SealedMessage",
		make_column("fakeId", &SealedMessage::fakeId, primary_key().autoincrement()),
		make_column("userId", &SealedMessage::userId),
		make_column("keyTag", &SealedMessage::keyTag),
		make_column("blob", &SealedMessage::blob)
	),
	make_table<SpyMessageRead>(
		"SpyMessageRead",
		make_column("fakeId", &SpyMessageRead::fakeId, primary_key().autoincrement()),
		make_column("userId", &SpyMessageRead::userId),
		make_column("dialogId", &SpyMessageRead::dialogId),
		make_column("messageId", &SpyMessageRead::messageId),
		make_column("entityCreateDate", &SpyMessageRead::entityCreateDate)
	),
	make_table<SpyMessageContentsRead>(
		"SpyMessageContentsRead",
		make_column("fakeId", &SpyMessageContentsRead::fakeId, primary_key().autoincrement()),
		make_column("userId", &SpyMessageContentsRead::userId),
		make_column("dialogId", &SpyMessageContentsRead::dialogId),
		make_column("messageId", &SpyMessageContentsRead::messageId),
		make_column("entityCreateDate", &SpyMessageContentsRead::entityCreateDate)
	)
);

namespace AyuMigrations {

void migrateToV1(decltype(storage) &storage) {
	// drop RegexFilter table as we've added primary_key()
	try {
		storage.drop_table_if_exists("RegexFilter");
		LOG(("Migration to V1 successful."));
	} catch (const std::exception &ex) {
		LOG(("Migration to V1 failed: %1").arg(ex.what()));
	}
}

}

void runMigrations(decltype(storage) &storage) {
	constexpr int kLatestVersion = 1;

	const std::map<int, Fn<void(decltype(storage) &)>> migrations = {
		{1, AyuMigrations::migrateToV1},
	};

	int currentVersion = 0;
	try {
		if (auto versionRow = storage.get_pointer<SchemaVersion>(1)) {
			currentVersion = versionRow->version;
		} else {
			storage.insert(SchemaVersion{1, 0});
		}
	} catch (...) {
		LOG(("No SchemaVersion, assuming 0"));
		storage.insert(SchemaVersion{1, 0});
	}

	if (currentVersion >= kLatestVersion) {
		LOG(("Database is ok"));
		return;
	}

	LOG(("Database version: %1. Latest version: %2.").arg(currentVersion).arg(kLatestVersion));

	for (int v = currentVersion + 1; v <= kLatestVersion; ++v) {
		if (migrations.contains(v)) {
			try {
				LOG(("Migration for version: %1").arg(v));
				storage.begin_transaction();

				migrations.at(v)(storage);

				storage.update_all(set(c(&SchemaVersion::version) = v), where(c(&SchemaVersion::id) == 1));
				storage.commit();
				LOG(("Applied migration for version: %1.").arg(v));
			} catch (...) {
				storage.rollback();
				LOG(("Failed to apply migration for version: %1.").arg(v));
				AyuDatabase::moveCurrentDatabase();

				return;
			}
		}
	}
}

namespace {

// AyuGram: deleted / edited message records are written off the main thread
// in batches (one transaction per batch): a synchronous commit costs tens of
// milliseconds on a disk and froze the UI for every deleted message.
sqlite3 *Connection = nullptr;
std::atomic<bool> Finished = false;

void RunBatch(std::vector<Ayu::BatchedWriter::Write> &&writes) {
	std::lock_guard lock(databaseMutex);
	try {
		storage.begin_transaction();
	} catch (std::exception &ex) {
		LOG(("Failed to begin a batch of database writes: %1").arg(ex.what()));
		return;
	}
	for (auto &write : writes) {
		try {
			write();
		} catch (std::exception &ex) {
			LOG(("Failed to write a database record: %1").arg(ex.what()));
		}
	}
	try {
		storage.commit();
	} catch (std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LOG(("Failed to commit a batch of database writes: %1").arg(ex.what()));
	}
}

Ayu::BatchedWriter &Writer();

crl::queue &WriteQueue() {
	static crl::queue queue;
	return queue;
}

Ayu::BatchedWriter &Writer() {
	static Ayu::BatchedWriter writer([] {
		WriteQueue().async([] {
			if (!Finished) {
				Writer().drain();
			}
		});
	}, RunBatch);
	return writer;
}

[[nodiscard]] std::unique_lock<std::recursive_mutex> LockAndDrain() {
	auto lock = std::unique_lock(databaseMutex);
	Writer().drain();
	return lock;
}

// Plain copies of records that became encrypted must not stay in the WAL.
void TruncateJournal() {
	if (Connection) {
		sqlite3_exec(
			Connection,
			"PRAGMA wal_checkpoint(TRUNCATE);",
			nullptr,
			nullptr,
			nullptr);
	}
}

} // namespace

namespace AyuDatabase {

void finish() {
	Finished = true;
	Writer().finish();
}

void moveCurrentDatabase() {
	const auto time = base::unixtime::now();

	if (QFile::exists("./tdata/ayudata.db")) {
		QFile::rename("./tdata/ayudata.db", QString("./tdata/ayudata_%1.db").arg(time));
	}

	if (QFile::exists("./tdata/ayudata.db-shm")) {
		QFile::rename("./tdata/ayudata.db-shm", QString("./tdata/ayudata_%1.db-shm").arg(time));
	}

	if (QFile::exists("./tdata/ayudata.db-wal")) {
		QFile::rename("./tdata/ayudata.db-wal", QString("./tdata/ayudata_%1.db-wal").arg(time));
	}
}

void initialize() {
	const auto lock = LockAndDrain();
	// AyuGram: removed records (plain copies of encrypted ones) are zeroed.
	// WAL + synchronous=NORMAL: a commit doesn't wait for the disk (the
	// default rollback journal with fsync took tens of ms per commit).
	storage.on_open = [](sqlite3 *db) {
		Connection = db;
		sqlite3_exec(
			db,
			"PRAGMA journal_mode = WAL;"
			"PRAGMA synchronous = NORMAL;"
			"PRAGMA journal_size_limit = 4194304;"
			"PRAGMA secure_delete = ON;",
			nullptr,
			nullptr,
			nullptr);
	};
	try {
		storage.sync_schema(true);

		runMigrations(storage);

		storage.sync_schema(true);
	} catch (const std::exception &ex) {
		LOG(("Database initialization failed: %1").arg(ex.what()));
		moveCurrentDatabase();

		storage.sync_schema(true);
		if (!storage.get_pointer<SchemaVersion>(1)) {
			storage.insert(SchemaVersion{1, 0});
		}
	}

	try {
		storage.open_forever();
	} catch (const std::exception &ex) {
		LOG(("Failed to keep database open: %1").arg(ex.what()));
	}
}

void addEditedMessage(const EditedMessage &message) {
	Writer().enqueue([message] {
		storage.insert(message);
	});
}

std::vector<EditedMessage> getEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit) {
	const auto lock = LockAndDrain();
	try {
		return storage.get_all<EditedMessage>(
			where(
				column<EditedMessage>(&EditedMessage::userId) == userId and
				column<EditedMessage>(&EditedMessage::dialogId) == dialogId and
				column<EditedMessage>(&EditedMessage::messageId) == messageId and
				(column<EditedMessage>(&EditedMessage::fakeId) > minId or minId == 0) and
				(column<EditedMessage>(&EditedMessage::fakeId) < maxId or maxId == 0)
			),
			order_by(column<EditedMessage>(&EditedMessage::fakeId)).desc(),
			limit(totalLimit)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get edited messages: %1").arg(ex.what()));
		return {};
	}
}

bool hasRevisions(ID userId, ID dialogId, ID messageId) {
	const auto lock = LockAndDrain();
	try {
		return !storage.select(
			columns(column<EditedMessage>(&EditedMessage::messageId)),
			where(
				column<EditedMessage>(&EditedMessage::userId) == userId and
				column<EditedMessage>(&EditedMessage::dialogId) == dialogId and
				column<EditedMessage>(&EditedMessage::messageId) == messageId
			),
			limit(1)
		).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if message has revisions: %1").arg(ex.what()));
		return false;
	}
}

void addDeletedMessage(const DeletedMessage &message) {
	Writer().enqueue([message] {
		storage.insert(message);
	});
}

std::vector<DeletedMessage> getDeletedMessages(ID userId, ID dialogId, ID topicId, ID minId, ID maxId, int totalLimit, const std::string &searchQuery) {
	const auto lock = LockAndDrain();
	try {
		if (searchQuery.empty()) {
			return storage.get_all<DeletedMessage>(
				where(
					column<DeletedMessage>(&DeletedMessage::userId) == userId and
					column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
					(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0) and
					(column<DeletedMessage>(&DeletedMessage::messageId) > minId or minId == 0) and
					(column<DeletedMessage>(&DeletedMessage::messageId) < maxId or maxId == 0)
				),
				order_by(column<DeletedMessage>(&DeletedMessage::messageId)).desc(),
				limit(totalLimit)
			);
		}

		std::string escaped;
		escaped.reserve(searchQuery.size());
		for (const auto c : searchQuery) {
			if (c == '%' || c == '_' || c == '\\') {
				escaped += '\\';
			}
			escaped += c;
		}
		const auto pattern = "%" + escaped + "%";
		return storage.get_all<DeletedMessage>(
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
				(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0) and
				(column<DeletedMessage>(&DeletedMessage::messageId) > minId or minId == 0) and
				(column<DeletedMessage>(&DeletedMessage::messageId) < maxId or maxId == 0) and
				like(column<DeletedMessage>(&DeletedMessage::text), pattern, "\\")
			),
			order_by(column<DeletedMessage>(&DeletedMessage::messageId)).desc(),
			limit(totalLimit)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get deleted messages: %1").arg(ex.what()));
		return {};
	}
}

bool hasDeletedMessages(ID userId, ID dialogId, ID topicId) {
	const auto lock = LockAndDrain();
	try {
		return !storage.select(
			columns(column<DeletedMessage>(&DeletedMessage::dialogId)),
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
				(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0)
			),
			limit(1)
		).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if dialog has deleted message: %1").arg(ex.what()));
		return false;
	}
}

void removeDeletedMessage(ID userId, ID dialogId, ID messageId) {
	Writer().enqueue([=] {
		storage.remove_all<DeletedMessage>(
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
				column<DeletedMessage>(&DeletedMessage::messageId) == messageId
			)
		);
	});
}

void clearDeletedMessages(ID userId, ID dialogId, ID topicId) {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<DeletedMessage>(
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId and
				(column<DeletedMessage>(&DeletedMessage::topicId) == topicId or topicId == 0)
			)
		);
	} catch (std::exception &) {
	}
}

namespace {

template <typename Message>
Message FromBase(const AyuMessageBase &base) {
	auto result = Message();
	static_cast<AyuMessageBase&>(result) = base;
	return result;
}

void Overwrite(std::string &value) {
	std::fill(value.begin(), value.end(), '\0');
}

template <typename Message>
void OverwriteAll(std::vector<Message> &messages) {
	for (auto &message : messages) {
		Overwrite(message.text);
		Overwrite(message.fwdName);
		Overwrite(message.fwdPostAuthor);
		Overwrite(message.postAuthor);
		std::fill(message.textEntities.begin(), message.textEntities.end(), '\0');
		std::fill(message.replySerialized.begin(), message.replySerialized.end(), '\0');
	}
	messages.clear();
}

} // namespace

ID addSealedMessage(const SealedMessage &message) {
	const auto lock = LockAndDrain();
	try {
		return storage.insert(message);
	} catch (std::exception &ex) {
		LOG(("Failed to save sealed message: %1").arg(ex.what()));
		return 0;
	}
}

std::vector<SealedMessage> getSealedMessages(ID userId, const std::vector<char> &keyTag) {
	const auto lock = LockAndDrain();
	try {
		return storage.get_all<SealedMessage>(
			where(
				column<SealedMessage>(&SealedMessage::userId) == userId and
				column<SealedMessage>(&SealedMessage::keyTag) == keyTag
			),
			order_by(column<SealedMessage>(&SealedMessage::fakeId))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get sealed messages: %1").arg(ex.what()));
		return {};
	}
}

void removeSealedMessages(const std::vector<ID> &fakeIds) {
	if (fakeIds.empty()) {
		return;
	}
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<SealedMessage>(
			where(in(&SealedMessage::fakeId, fakeIds))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to remove sealed messages: %1").arg(ex.what()));
	}
}

void removeSealedByTag(ID userId, const std::vector<char> &keyTag) {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<SealedMessage>(
			where(
				column<SealedMessage>(&SealedMessage::userId) == userId and
				column<SealedMessage>(&SealedMessage::keyTag) == keyTag
			)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to remove sealed messages by tag: %1").arg(ex.what()));
	}
}

int sealPlainMessages(
		ID userId,
		const std::vector<ID> &dialogIds,
		const std::vector<char> &keyTag,
		const std::function<std::vector<char>(int kind, const AyuMessageBase &message)> &seal) {
	if (dialogIds.empty()) {
		return 0;
	}
	const auto lock = LockAndDrain();
	auto deleted = std::vector<DeletedMessage>();
	auto edited = std::vector<EditedMessage>();
	try {
		deleted = storage.get_all<DeletedMessage>(
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				in(column<DeletedMessage>(&DeletedMessage::dialogId), dialogIds)
			)
		);
		edited = storage.get_all<EditedMessage>(
			where(
				column<EditedMessage>(&EditedMessage::userId) == userId and
				in(column<EditedMessage>(&EditedMessage::dialogId), dialogIds)
			)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to read plain messages to seal: %1").arg(ex.what()));
		return 0;
	}
	if (deleted.empty() && edited.empty()) {
		return 0;
	}
	auto sealed = std::vector<SealedMessage>();
	sealed.reserve(deleted.size() + edited.size());
	const auto add = [&](int kind, const AyuMessageBase &message) {
		auto blob = seal(kind, message);
		if (blob.empty()) {
			return false;
		}
		sealed.push_back(SealedMessage{ 0, userId, keyTag, std::move(blob) });
		return true;
	};
	auto ok = true;
	for (const auto &message : deleted) {
		ok = ok && add(1, message);
	}
	for (const auto &message : edited) {
		ok = ok && add(2, message);
	}
	OverwriteAll(deleted);
	OverwriteAll(edited);
	if (!ok) {
		LOG(("Failed to seal plain messages."));
		return 0;
	}
	try {
		storage.begin_transaction();
		for (const auto &message : sealed) {
			storage.insert(message);
		}
		storage.remove_all<DeletedMessage>(
			where(
				column<DeletedMessage>(&DeletedMessage::userId) == userId and
				in(column<DeletedMessage>(&DeletedMessage::dialogId), dialogIds)
			)
		);
		storage.remove_all<EditedMessage>(
			where(
				column<EditedMessage>(&EditedMessage::userId) == userId and
				in(column<EditedMessage>(&EditedMessage::dialogId), dialogIds)
			)
		);
		storage.commit();
	} catch (std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LOG(("Failed to move plain messages to sealed: %1").arg(ex.what()));
		return 0;
	}
	TruncateJournal();
	return int(sealed.size());
}

bool unsealMessages(
		ID userId,
		const std::vector<char> &keyTag,
		const std::vector<UnsealedMessage> &messages) {
	const auto lock = LockAndDrain();
	try {
		storage.begin_transaction();
		for (const auto &message : messages) {
			if (message.kind == 1) {
				storage.insert(FromBase<DeletedMessage>(message.message));
			} else if (message.kind == 2) {
				storage.insert(FromBase<EditedMessage>(message.message));
			}
		}
		storage.remove_all<SealedMessage>(
			where(
				column<SealedMessage>(&SealedMessage::userId) == userId and
				column<SealedMessage>(&SealedMessage::keyTag) == keyTag
			)
		);
		storage.commit();
	} catch (std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LOG(("Failed to move sealed messages to plain: %1").arg(ex.what()));
		return false;
	}
	TruncateJournal();
	return true;
}

void removeSealedExcept(ID userId, const std::vector<std::vector<char>> &keep) {
	const auto lock = LockAndDrain();
	try {
		const auto rows = storage.select(
			columns(
				column<SealedMessage>(&SealedMessage::fakeId),
				column<SealedMessage>(&SealedMessage::keyTag)),
			where(column<SealedMessage>(&SealedMessage::userId) == userId)
		);
		auto remove = std::vector<ID>();
		for (const auto &[fakeId, keyTag] : rows) {
			if (std::find(keep.begin(), keep.end(), keyTag) == keep.end()) {
				remove.push_back(fakeId);
			}
		}
		if (!remove.empty()) {
			storage.remove_all<SealedMessage>(
				where(in(&SealedMessage::fakeId, remove))
			);
			LOG(("Removed %1 sealed records of deleted folders.").arg(remove.size()));
		}
	} catch (std::exception &ex) {
		LOG(("Failed to remove sealed records of deleted folders: %1").arg(ex.what()));
	}
}

template<typename T>
std::vector<T> getAllT() {
	const auto lock = LockAndDrain();
	try {
		return storage.get_all<T>();
	} catch (std::exception &ex) {
		LOG(("Failed to get all: %1").arg(ex.what()));
		return {};
	}
}

std::vector<RegexFilter> getAllRegexFilters() {
	return getAllT<RegexFilter>();
}

std::vector<RegexFilterGlobalExclusion> getAllFiltersExclusions() {
	return getAllT<RegexFilterGlobalExclusion>();
}

std::vector<RegexFilter> getExcludedByDialogId(ID dialogId) {
	const auto lock = LockAndDrain();
	try {
		return storage.get_all<RegexFilter>(
			where(in(&RegexFilter::id,
					 storage.select(columns(&RegexFilterGlobalExclusion::filterId),
									where(is_equal(&RegexFilterGlobalExclusion::dialogId, dialogId))
					 )
			))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get excluded by dialog id: %1").arg(ex.what()));
		return {};
	}
}

int getCount() {
	const auto lock = LockAndDrain();
	try {
		return storage.count<RegexFilter>();
	} catch (std::exception &ex) {
		LOG(("Failed to get count: %1").arg(ex.what()));
		return 0;
	}
}

RegexFilter getById(std::vector<char> id) {
	const auto lock = LockAndDrain();
	try {
		return storage.get<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::id) == std::move(id))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get filters by id: %1").arg(ex.what()));
		return {};
	}
}

std::vector<RegexFilter> getShared() {
	const auto lock = LockAndDrain();
	try {
		return storage.get_all<RegexFilter>(
			where(is_null(column<RegexFilter>(&RegexFilter::dialogId)))
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get shared filters: %1").arg(ex.what()));
		return {};
	}
}

std::vector<RegexFilter> getByDialogId(ID dialogId) {
	const auto lock = LockAndDrain();
	try {
		return storage.get_all<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::dialogId) == dialogId)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to get filters by dialog id: %1").arg(ex.what()));
		return {};
	}
}

void addRegexFilter(const RegexFilter &filter) {
	const auto lock = LockAndDrain();
	try {
		storage.begin_transaction();
		storage.replace(filter); // we're using replace as we set std::vector<char> as primary key
		storage.commit();
	} catch (std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LOG(("Failed to save regex filter for some reason: %1").arg(ex.what()));
	}
}

void addRegexExclusion(const RegexFilterGlobalExclusion &exclusion) {
	const auto lock = LockAndDrain();
	try {
		storage.begin_transaction();
		storage.insert(exclusion);
		storage.commit();
	} catch (std::exception &ex) {
		try {
			storage.rollback();
		} catch (...) {
		}
		LOG(("Failed to save regex filter exclusion for some reason: %1").arg(ex.what()));
	}
}

void updateRegexFilter(const RegexFilter &filter) {
	const auto lock = LockAndDrain();
	try {
		storage.update_all(
			set(
				c(&RegexFilter::text) = filter.text,
				c(&RegexFilter::enabled) = filter.enabled,
				c(&RegexFilter::reversed) = filter.reversed,
				c(&RegexFilter::caseInsensitive) = filter.caseInsensitive,
				c(&RegexFilter::dialogId) = filter.dialogId
			),
			where(c(&RegexFilter::id) == filter.id)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to update regex filter for some reason: %1").arg(ex.what()));
	}
}

void deleteFilter(const std::vector<char> &id) {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<RegexFilter>(
			where(column<RegexFilter>(&RegexFilter::id) == id)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to delete regex filter for some reason: %1").arg(ex.what()));
	}
}

void deleteExclusionsByFilterId(const std::vector<char> &id) {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<RegexFilterGlobalExclusion>(
			where(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::filterId) == id)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to delete regex filter exclusion by filter id for some reason: %1").arg(ex.what()));
	}
}

void deleteExclusion(ID dialogId, std::vector<char> filterId) {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<RegexFilterGlobalExclusion>(
			where(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::filterId) == filterId and
				column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::dialogId) == dialogId
			)
		);
	} catch (std::exception &ex) {
		LOG(("Failed to delete regex filter exclusion for some reason: %1").arg(ex.what()));
	}
}

void deleteAllFilters() {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<RegexFilter>();
	} catch (std::exception &ex) {
		LOG(("Failed to delete all regex filter for some reason: %1").arg(ex.what()));
	}
}

void deleteAllExclusions() {
	const auto lock = LockAndDrain();
	try {
		storage.remove_all<RegexFilterGlobalExclusion>();
	} catch (std::exception &ex) {
		LOG(("Failed to delete all regex filter exclusions for some reason: %1").arg(ex.what()));
	}
}

bool hasFilters() {
	const auto lock = LockAndDrain();
	try {
		return !storage.select(
			columns(column<RegexFilter>(&RegexFilter::id)),
			limit(1)
		).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if there's any filters: %1").arg(ex.what()));
		return false;
	}
}

bool hasPerDialogFilters() {
	const auto lock = LockAndDrain();
	try {
		return
			!storage.select(
				columns(column<RegexFilter>(&RegexFilter::id)),
				where(is_not_null(column<RegexFilter>(&RegexFilter::dialogId))),
				limit(1)
			).empty() ||
			!storage.select(
				columns(column<RegexFilterGlobalExclusion>(&RegexFilterGlobalExclusion::fakeId)),
				limit(1)
			).empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if there's any filters: %1").arg(ex.what()));
		return false;
	}
}

}
