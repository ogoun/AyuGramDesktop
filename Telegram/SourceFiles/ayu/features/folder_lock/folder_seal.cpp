// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_seal.h"

#include "ayu/features/folder_lock/folder_lock.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"

namespace Ayu {
namespace {

int SealedScopeDepth = 0;

} // namespace

SealedScope::SealedScope(bool sealed) : _sealed(sealed) {
	if (_sealed) {
		++SealedScopeDepth;
	}
}

SealedScope::~SealedScope() {
	if (_sealed) {
		--SealedScopeDepth;
	}
}

bool SealedScope::Active() {
	return SealedScopeDepth > 0;
}

bool IsSealUnknown(not_null<History*> history) {
	const auto &lock = history->owner().chatsFilters().folderLock();
	return lock.rulesPending() && lock.hasEncryptedRecords();
}

bool IsKnownSealedPeer(not_null<PeerData*> peer) {
	const auto &lock = peer->owner().chatsFilters().folderLock();
	return !lock.rulesPending() && lock.isSealed(peer);
}

bool IsSealedHistory(not_null<History*> history) {
	return history->owner().chatsFilters().folderLock().isSealed(history);
}

bool IsSealedPeer(not_null<PeerData*> peer) {
	return peer->owner().chatsFilters().folderLock().isSealed(peer);
}

bool IsSealedMedia(not_null<const DocumentData*> document) {
	return SealedScope::Active() || document->owner().chatsFilters().folderLock().isSealedMedia(
		document);
}

bool IsSealedMedia(not_null<const PhotoData*> photo) {
	return SealedScope::Active() || photo->owner().chatsFilters().folderLock().isSealedMedia(photo);
}

bool IsSealedOrigin(
		not_null<Main::Session*> session,
		const Data::FileOrigin &origin) {
	return session->data().chatsFilters().folderLock().isSealedOrigin(
		origin);
}

} // namespace Ayu
