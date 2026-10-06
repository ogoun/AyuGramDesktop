/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_unread_value.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "window/notifications_manager.h"

// AyuGram includes
#include "ayu/features/folder_lock/folder_lock.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"

namespace Data {
namespace {

rpl::producer<Dialogs::UnreadState> MainListUnreadState(
		not_null<Dialogs::MainList*> list) {
	return rpl::single(rpl::empty) | rpl::then(
		list->unreadStateChanges() | rpl::to_empty
	) | rpl::map([=] {
		return list->unreadState();
	});
}

// AyuGram: unread of locked chats outside of the archive.
[[nodiscard]] Dialogs::UnreadState AyuLockedMainUnreadState(
		not_null<Main::Session*> session) {
	auto result = Dialogs::UnreadState();
	const auto &lock = session->data().chatsFilters().folderLock();
	if (!lock.anyLocked()) {
		return result;
	}
	for (const auto &row : session->data().chatsList()->indexed()->all()) {
		if (const auto history = row->history()) {
			if (lock.isLocked(not_null(history))) {
				result += history->chatListUnreadState();
			}
		}
	}
	return result;
}

} // namespace

[[nodiscard]] Dialogs::UnreadState MainListMapUnreadState(
		not_null<Main::Session*> session,
		const Dialogs::UnreadState &state) {
	const auto folderId = Data::Folder::kId;
	if (const auto folder = session->data().folderLoaded(folderId)) {
		return state - folder->chatsList()->unreadState();
	}
	return state;
}

rpl::producer<Dialogs::UnreadState> UnreadStateValue(
		not_null<Main::Session*> session,
		FilterId filterId) {
	// AyuGram: recount when a protected folder is locked or unlocked.
	const auto lock = &session->data().chatsFilters().folderLock();
	auto changes = rpl::single(rpl::empty) | rpl::then(lock->lockChanges());
	if (filterId > 0) {
		const auto filters = &session->data().chatsFilters();
		return rpl::combine(
			MainListUnreadState(filters->chatsList(filterId)),
			std::move(changes)
		) | rpl::map([=](const Dialogs::UnreadState &state, auto) {
			return lock->isLocked(filterId) ? Dialogs::UnreadState() : state;
		});
	}
	return rpl::combine(
		MainListUnreadState(session->data().chatsList()),
		std::move(changes)
	) | rpl::map([=](const Dialogs::UnreadState &state, auto) {
		return MainListMapUnreadState(session, state)
			- AyuLockedMainUnreadState(session);
	});
}

rpl::producer<bool> IncludeMutedCounterFoldersValue() {
	using namespace Window::Notifications;
	return rpl::single(rpl::empty_value()) | rpl::then(
		Core::App().notifications().settingsChanged(
		) | rpl::filter(
			rpl::mappers::_1 == ChangeType::IncludeMuted
		) | rpl::to_empty
	) | rpl::map([] {
		return Core::App().settings().includeMutedCounterFolders();
	});
}

} // namespace Data
