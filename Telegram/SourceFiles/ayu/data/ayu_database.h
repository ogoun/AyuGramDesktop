// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/data/entities.h"

#include <functional>

class SchemaVersion
{
public:
	int id;
	int version;
};

namespace AyuDatabase {

void initialize();

void addEditedMessage(const EditedMessage &message);
std::vector<EditedMessage> getEditedMessages(ID userId, ID dialogId, ID messageId, ID minId, ID maxId, int totalLimit);
bool hasRevisions(ID userId, ID dialogId, ID messageId);

void addDeletedMessage(const DeletedMessage &message);
std::vector<DeletedMessage> getDeletedMessages(ID userId, ID dialogId, ID topicId, ID minId, ID maxId, int totalLimit, const std::string &searchQuery = "");
bool hasDeletedMessages(ID userId, ID dialogId, ID topicId);
void removeDeletedMessage(ID userId, ID dialogId, ID messageId);
void clearDeletedMessages(ID userId, ID dialogId, ID topicId);

// Encrypted records of the chats of encrypted folders.
struct UnsealedMessage
{
	ID sealedId = 0;
	int kind = 0; // 1 - deleted, 2 - edited.
	AyuMessageBase message;
};
ID addSealedMessage(const SealedMessage &message);
std::vector<SealedMessage> getSealedMessages(ID userId, const std::vector<char> &keyTag);
void removeSealedMessages(const std::vector<ID> &fakeIds);
void removeSealedByTag(ID userId, const std::vector<char> &keyTag);
// Plain records of the dialogs become sealed ones in one transaction,
// `seal` returns the box of a record (empty - the whole move fails).
int sealPlainMessages(
	ID userId,
	const std::vector<ID> &dialogIds,
	const std::vector<char> &keyTag,
	const std::function<std::vector<char>(int kind, const AyuMessageBase &message)> &seal);
// Opened records become plain ones, all records of the tag are removed.
void unsealMessages(
	ID userId,
	const std::vector<char> &keyTag,
	const std::vector<UnsealedMessage> &messages);

std::vector<RegexFilter> getAllRegexFilters();
RegexFilter getById(std::vector<char> id);
std::vector<RegexFilter> getShared();
std::vector<RegexFilter> getByDialogId(ID dialogId);
std::vector<RegexFilterGlobalExclusion> getAllFiltersExclusions();
std::vector<RegexFilter> getExcludedByDialogId(ID dialogId);

int getCount();


void addRegexFilter(const RegexFilter &filter);
void addRegexExclusion(const RegexFilterGlobalExclusion &exclusion);

void updateRegexFilter(const RegexFilter &filter);

void deleteFilter(const std::vector<char> &id);
void deleteExclusionsByFilterId(const std::vector<char> &id);
void deleteExclusion(ID dialogId, std::vector<char> filterId);

void deleteAllFilters();
void deleteAllExclusions();

bool hasFilters();
bool hasPerDialogFilters();

void moveCurrentDatabase();

}
