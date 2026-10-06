// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/folder_lock/folder_lock_ui.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/folder_lock/folder_lock.h"
#include "ayu/features/folder_lock/folder_lock_crypto.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Ayu {
namespace {

constexpr auto kAutolockOptions = std::array{ 1, 5, 15, 60, 0 };

[[nodiscard]] FolderLock &Lock(
		not_null<Window::SessionController*> controller) {
	return controller->session().data().chatsFilters().folderLock();
}

[[nodiscard]] QString WaitText(int seconds) {
	return tr::ayu_FolderLockWait(
		tr::now,
		lt_seconds,
		QString::number(seconds));
}

[[nodiscard]] QString ErrorText(UnlockResult result) {
	switch (result.error) {
	case UnlockError::None: return QString();
	case UnlockError::WrongPin: return result.waitSeconds
		? WaitText(result.waitSeconds)
		: tr::ayu_FolderLockWrongPin(tr::now);
	case UnlockError::TooManyTries: return WaitText(result.waitSeconds);
	case UnlockError::Failed: return tr::ayu_FolderLockFailed(tr::now);
	}
	return QString();
}

[[nodiscard]] QString AutolockText(int minutes) {
	return minutes
		? tr::ayu_FolderLockAutolockMinutes(
			tr::now,
			lt_minutes,
			QString::number(minutes))
		: tr::ayu_FolderLockAutolockNever(tr::now);
}

// PasswordInput is not an RpWidget, so it lives inside a container row.
[[nodiscard]] not_null<Ui::PasswordInput*> AddPinField(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> placeholder) {
	const auto &st = st::defaultInputField;
	auto container = object_ptr<Ui::RpWidget>(box);
	container->resize(container->width(), st.heightMin);
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		container.data(),
		st,
		std::move(placeholder));
	container->widthValue(
	) | rpl::on_next([=](int width) {
		field->resize(width, st.heightMin);
		field->moveToLeft(0, 0);
	}, container->lifetime());
	box->addRow(std::move(container));
	return field;
}

[[nodiscard]] not_null<Ui::FlatLabel*> AddErrorLabel(
		not_null<Ui::GenericBox*> box) {
	const auto error = box->addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel));
	error->setTextColorOverride(st::boxTextFgError->c);
	return error;
}

// A box with one PIN field. `submit` gets the PIN and a callback to report
// an error text (the box stays open then).
void PinBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		rpl::producer<QString> button,
		Fn<void(QByteArray, Fn<void(QString)>)> submit) {
	box->setTitle(std::move(title));
	const auto field = AddPinField(box, tr::ayu_FolderLockEnterPin());
	const auto error = AddErrorLabel(box);
	const auto busy = std::make_shared<bool>(false);
	const auto send = [=] {
		if (*busy) {
			return;
		}
		*busy = true;
		error->setText(QString());
		submit(field->getLastText().toUtf8(), crl::guard(box, [=](
				QString text) {
			*busy = false;
			error->setText(text);
			field->selectAll();
			field->setFocus();
		}));
	};
	QObject::connect(field, &Ui::MaskedInputField::submitted, send);
	box->setFocusCallback([=] { field->setFocusFast(); });
	box->addButton(std::move(button), send);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

static void CloseLockedChatsInWindowsNow(not_null<Main::Session*> session);

void ShowUnlockFolderBox(
		not_null<Window::SessionController*> controller,
		FilterId id,
		Fn<void()> unlocked) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		PinBox(
			box,
			tr::ayu_FolderLockEnterPin(),
			tr::ayu_FolderLockOpen(),
			[=](QByteArray pin, Fn<void(QString)> fail) {
				Lock(controller).tryUnlock(id, std::move(pin), crl::guard(
						box,
						[=](UnlockResult result) {
					if (result.error == UnlockError::None) {
						box->closeBox();
						if (unlocked) {
							unlocked();
						}
					} else {
						fail(ErrorText(result));
					}
				}));
			});
	}));
}

void ShowVerifyFolderPinBox(
		not_null<Window::SessionController*> controller,
		FilterId id,
		Fn<void()> verified) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		PinBox(
			box,
			tr::ayu_FolderLockEnterPin(),
			tr::ayu_FolderLockOpen(),
			[=](QByteArray pin, Fn<void(QString)> fail) {
				Lock(controller).verifyPin(id, std::move(pin), crl::guard(
						box,
						[=](UnlockResult result) {
					if (result.error == UnlockError::None) {
						box->closeBox();
						verified();
					} else {
						fail(ErrorText(result));
					}
				}));
			});
	}));
}

void ShowSetFolderPinBox(
		not_null<Window::SessionController*> controller,
		FilterId id,
		Fn<void()> done) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::ayu_FolderLockSetPin());
		const auto first = AddPinField(box, tr::ayu_FolderLockNewPin());
		const auto second = AddPinField(box, tr::ayu_FolderLockRepeatPin());
		const auto error = AddErrorLabel(box);
		const auto busy = std::make_shared<bool>(false);
		const auto save = [=] {
			if (*busy) {
				return;
			}
			const auto pin = first->getLastText();
			if (pin.size() < FolderLockCrypto::kMinPinLength) {
				error->setText(tr::ayu_FolderLockTooShort(tr::now));
				first->setFocus();
				return;
			} else if (pin != second->getLastText()) {
				error->setText(tr::ayu_FolderLockMismatch(tr::now));
				second->selectAll();
				second->setFocus();
				return;
			}
			*busy = true;
			error->setText(QString());
			Lock(controller).setPin(id, pin.toUtf8(), crl::guard(
					box,
					[=](bool ok) {
				*busy = false;
				if (ok) {
					box->closeBox();
					done();
				} else {
					error->setText(tr::ayu_FolderLockFailed(tr::now));
				}
			}));
		};
		QObject::connect(first, &Ui::MaskedInputField::submitted, [=] {
			second->setFocus();
		});
		QObject::connect(second, &Ui::MaskedInputField::submitted, save);
		box->setFocusCallback([=] { first->setFocusFast(); });
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void AddFolderProtectionSection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		FilterId id,
		not_null<Ui::Checkbox*> hideFromAll) {
	const auto lock = &Lock(controller);
	const auto protectedState = container->lifetime().make_state<
		rpl::variable<bool>>(lock->isProtected(id));
	const auto hidden = container->lifetime().make_state<
		rpl::variable<bool>>(hideFromAll->checked());
	const auto autolockLabel = container->lifetime().make_state<
		rpl::variable<QString>>(AutolockText(lock->autolockMinutes(id)));
	const auto refresh = [=] {
		*protectedState = lock->isProtected(id);
		*autolockLabel = AutolockText(lock->autolockMinutes(id));
	};
	lock->lockChanges() | rpl::on_next(refresh, container->lifetime());

	const auto hint = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				tr::ayu_FolderLockNeedHide(),
				st::boxDividerLabel),
			st::boxRowPadding));
	const auto setWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto setPin = Settings::AddButtonWithIcon(
		setWrap->entity(),
		tr::ayu_FolderLockSetPin(),
		st::settingsButtonNoIcon);
	const auto manageWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto manage = manageWrap->entity();
	const auto autolock = Settings::AddButtonWithLabel(
		manage,
		tr::ayu_FolderLockAutolock(),
		autolockLabel->value(),
		st::settingsButtonNoIcon);
	const auto change = Settings::AddButtonWithIcon(
		manage,
		tr::ayu_FolderLockChangePin(),
		st::settingsButtonNoIcon);
	const auto remove = Settings::AddButtonWithIcon(
		manage,
		tr::ayu_FolderLockRemovePin(),
		st::settingsAttentionButton);

	hideFromAll->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (!checked && protectedState->current()) {
			hideFromAll->setChecked(true);
			controller->showToast(tr::ayu_FolderLockRemoveFirst(tr::now));
			return;
		}
		*hidden = checked;
	}, hideFromAll->lifetime());

	hint->toggleOn(hidden->value() | rpl::map(!rpl::mappers::_1));
	setWrap->toggleOn(rpl::combine(
		hidden->value(),
		protectedState->value()
	) | rpl::map(rpl::mappers::_1 && !rpl::mappers::_2));
	manageWrap->toggleOn(protectedState->value());

	setPin->setClickedCallback([=] {
		ShowSetFolderPinBox(controller, id, [=] {
			// A PIN requires "Don't show chats in All Chats", keep it even
			// if the edit folder box is cancelled.
			AyuSettings::getInstance().setFolderHiddenFromAllChats(
				controller->session().userId().bare,
				id,
				true);
			refresh();
		});
	});
	change->setClickedCallback([=] {
		ShowVerifyFolderPinBox(controller, id, [=] {
			ShowSetFolderPinBox(controller, id, refresh);
		});
	});
	remove->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			PinBox(
				box,
				tr::ayu_FolderLockRemovePin(),
				tr::ayu_FolderLockRemovePin(),
				[=](QByteArray pin, Fn<void(QString)> fail) {
					lock->removePin(id, std::move(pin), crl::guard(
							box,
							[=](UnlockResult result) {
						if (result.error == UnlockError::None) {
							box->closeBox();
							refresh();
						} else {
							fail(ErrorText(result));
						}
					}));
				});
		}));
	});
	autolock->setClickedCallback([=] {
		const auto current = lock->autolockMinutes(id);
		auto options = std::vector<QString>();
		auto selected = 0;
		for (auto i = 0; i != int(kAutolockOptions.size()); ++i) {
			options.push_back(AutolockText(kAutolockOptions[i]));
			if (kAutolockOptions[i] == current) {
				selected = i;
			}
		}
		controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
			.title = tr::ayu_FolderLockAutolock(),
			.options = options,
			.initialSelection = selected,
			.callback = [=](int index) {
				lock->setAutolockMinutes(id, kAutolockOptions[index]);
				refresh();
			},
		}));
	});
}

void CloseLockedChatsInWindows(not_null<Main::Session*> session) {
	// Deferred: locking may start inside an event handler of the very window
	// that gets closed here (minimize, hide, a shortcut).
	crl::on_main(session, [=] {
		CloseLockedChatsInWindowsNow(session);
	});
}

static void CloseLockedChatsInWindowsNow(not_null<Main::Session*> session) {
	const auto &lock = session->data().chatsFilters().folderLock();
	using WindowPointer = base::weak_ptr<Window::SessionController>;
	auto closing = std::vector<WindowPointer>();
	auto clearing = std::vector<WindowPointer>();
	const auto locked = [&](Data::Thread *thread) {
		return thread && lock.isLocked(thread->owningHistory());
	};
	for (const auto &window : session->windows()) {
		if (locked(window->windowId().chat())) {
			closing.push_back(base::make_weak(window));
		} else if (locked(window->activeChatCurrent().thread())) {
			clearing.push_back(base::make_weak(window));
		}
	}
	for (const auto &window : closing) {
		if (const auto strong = window.get()) {
			Core::App().closeWindow(&strong->window());
		}
	}
	for (const auto &window : clearing) {
		if (const auto strong = window.get()) {
			strong->clearSectionStack();
		}
	}
	// A locked folder can't stay the active tab.
	for (const auto &window : session->windows()) {
		if (lock.isLocked(window->activeChatsFilterCurrent())) {
			window->setActiveChatsFilter(0);
		}
	}
}

} // namespace Ayu
