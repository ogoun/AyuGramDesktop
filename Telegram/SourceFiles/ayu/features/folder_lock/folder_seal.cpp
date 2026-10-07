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

bool IsSealedHistory(not_null<History*> history) {
	return history->owner().chatsFilters().folderLock().isSealed(history);
}

bool IsSealedPeer(not_null<PeerData*> peer) {
	return peer->owner().chatsFilters().folderLock().isSealed(peer);
}

bool IsSealedMedia(not_null<const DocumentData*> document) {
	return document->owner().chatsFilters().folderLock().isSealedMedia(
		document);
}

bool IsSealedMedia(not_null<const PhotoData*> photo) {
	return photo->owner().chatsFilters().folderLock().isSealedMedia(photo);
}

bool IsSealedOrigin(
		not_null<Main::Session*> session,
		const Data::FileOrigin &origin) {
	return session->data().chatsFilters().folderLock().isSealedOrigin(
		origin);
}

} // namespace Ayu
