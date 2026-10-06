// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

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

// Asks the PIN without unlocking, e.g. before opening folder settings.
void ShowVerifyFolderPinBox(
	not_null<Window::SessionController*> controller,
	FilterId id,
	Fn<void()> verified);

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
