// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/features/folder_lock/folder_lock.h"
#include "data/data_chat_filters.h"

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class VerticalLayout;
class Checkbox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu {

// Asks the PIN and unlocks the folder, `unlocked` is called on success.
void ShowUnlockFolderBox(
	not_null<Window::SessionController*> controller,
	FilterId id,
	Fn<void()> unlocked);

// Opens the folder settings with the right access: asks the PIN when it is
// needed and calls `open(mode)` (std::nullopt = the folder is not protected).
void OpenFolderSettings(
	not_null<Window::SessionController*> controller,
	FilterId id,
	Fn<void(std::optional<AccessMode>)> open);

// A PIN box that accepts only the real PIN (removing the folder).
void ShowVerifyRealPinBox(
	not_null<Window::SessionController*> controller,
	FilterId id,
	Fn<void()> verified);

// Folder data for the edit box: the real one, or the fake one of the decoy
// mode with `restore` merging the edit back into the real rules.
struct FolderEdit {
	Data::ChatFilter shown;
	Fn<Data::ChatFilter(const Data::ChatFilter&)> restore;
	bool fake = false;
};
[[nodiscard]] FolderEdit PrepareFolderEdit(
	not_null<Main::Session*> session,
	const Data::ChatFilter &real);

// Asks a new PIN twice and sets it.
void ShowSetFolderPinBox(
	not_null<Window::SessionController*> controller,
	FilterId id,
	Fn<void()> done);

// "Protection" group of the edit folder box, under "Don't show chats in All
// Chats" (`hideFromAll`), for an existing folder only.
void AddFolderProtectionSection(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller,
	FilterId id,
	not_null<Ui::Checkbox*> hideFromAll);

// Closes chats of locked folders shown in any window of the session.
void CloseLockedChatsInWindows(not_null<Main::Session*> session);

} // namespace Ayu
