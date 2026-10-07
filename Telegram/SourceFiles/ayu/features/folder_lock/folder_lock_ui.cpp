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
#include "ayu/features/folder_lock/folder_lock_merge.h"
#include "boxes/peer_list_box.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "ui/boxes/confirm_box.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
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

#include <set>

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


// Two PIN fields with length and match checks. `save` gets the PIN and
// callbacks to report an error (the box stays open) or to close the box.
void NewPinBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		Fn<void(QByteArray, Fn<void(QString)>, Fn<void()>)> save) {
	box->setTitle(std::move(title));
	const auto first = AddPinField(box, tr::ayu_FolderLockNewPin());
	const auto second = AddPinField(box, tr::ayu_FolderLockRepeatPin());
	const auto error = AddErrorLabel(box);
	const auto busy = std::make_shared<bool>(false);
	const auto submit = [=] {
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
		save(pin.toUtf8(), crl::guard(box, [=](QString text) {
			*busy = false;
			error->setText(text);
			first->selectAll();
			first->setFocus();
		}), crl::guard(box, [=] {
			box->closeBox();
		}));
	};
	QObject::connect(first, &Ui::MaskedInputField::submitted, [=] {
		second->setFocus();
	});
	QObject::connect(second, &Ui::MaskedInputField::submitted, submit);
	box->setFocusCallback([=] { first->setFocusFast(); });
	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

class AllowedChatsController final : public PeerListController {
public:
	AllowedChatsController(
		not_null<Main::Session*> session,
		FilterId id,
		std::vector<PeerId> initial)
	: _session(session)
	, _id(id)
	, _initial(std::move(initial)) {
	}

	Main::Session &session() const override {
		return *_session;
	}

	void prepare() override {
		delegate()->peerListSetTitle(tr::ayu_FolderLockDecoyChats());
		// The folder chats list hides locked chats, take them by the rules.
		const auto &filters = _session->data().chatsFilters().list();
		const auto i = ranges::find(filters, _id, &Data::ChatFilter::id);
		if (i == end(filters)) {
			return;
		}
		const auto &filter = *i;
		const auto addList = [&](not_null<Dialogs::MainList*> list) {
			for (const auto &row : list->indexed()->all()) {
				const auto history = row->history();
				if (!history
					|| !filter.matchesRules(history)
					|| delegate()->peerListFindRow(history->peer->id.value)) {
					continue;
				}
				auto peerRow = std::make_unique<PeerListRow>(history->peer);
				const auto raw = peerRow.get();
				delegate()->peerListAppendRow(std::move(peerRow));
				if (ranges::contains(_initial, history->peer->id)) {
					delegate()->peerListSetRowChecked(raw, true);
				}
			}
		};
		addList(_session->data().chatsList());
		if (const auto folder = _session->data().folderLoaded(
				Data::Folder::kId)) {
			addList(folder->chatsList());
		}
		delegate()->peerListRefreshRows();
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

	[[nodiscard]] std::vector<PeerId> checked() {
		auto result = std::vector<PeerId>();
		const auto count = delegate()->peerListFullRowsCount();
		for (auto i = 0; i != count; ++i) {
			const auto row = delegate()->peerListRowAt(i);
			if (row->checked()) {
				result.push_back(row->peer()->id);
			}
		}
		return result;
	}

private:
	const not_null<Main::Session*> _session;
	const FilterId _id;
	const std::vector<PeerId> _initial;

};

// The chats of the folder with check marks, `save` returns false if the
// list can't be stored (too many chats).
void ShowAllowedChatsBox(
		not_null<Window::SessionController*> controller,
		FilterId id,
		std::vector<PeerId> initial,
		Fn<bool(std::vector<PeerId>)> save) {
	auto owned = std::make_unique<AllowedChatsController>(
		&controller->session(),
		id,
		std::move(initial));
	const auto raw = owned.get();
	controller->show(Box<PeerListBox>(std::move(owned), [=](
			not_null<PeerListBox*> box) {
		box->addButton(tr::lng_settings_save(), [=] {
			if (save(raw->checked())) {
				box->closeBox();
			} else {
				controller->showToast(tr::ayu_FolderLockDecoyTooMany(tr::now));
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

// After setting a PIN in the open settings box, continue with the access
// of that PIN, so that "Change" / "Remove" work without reopening the box.
void ContinueWithPin(
		not_null<FolderLock*> lock,
		FilterId id,
		QByteArray pin,
		not_null<QWidget*> owner,
		Fn<void()> done) {
	const auto weak = QPointer<QWidget>(owner.get());
	const auto weakLock = base::make_weak(lock);
	lock->beginSettings(id, std::move(pin), [=](UnlockResult, AccessMode) {
		const auto strong = weakLock.get();
		if (!strong) {
			return;
		} else if (!weak) {
			strong->endSettings(id);
			return;
		}
		strong->attachSettings(id);
		done();
	});
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

void OpenFolderSettings(
		not_null<Window::SessionController*> controller,
		FilterId id,
		Fn<void(std::optional<AccessMode>)> open) {
	auto &lock = Lock(controller);
	if (!lock.isProtected(id)) {
		open(std::nullopt);
		return;
	} else if (lock.isPinRemoved(id)) {
		lock.beginRemovedSettings(id);
		open(AccessMode::Removed);
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		PinBox(
			box,
			tr::ayu_FolderLockEnterPin(),
			tr::ayu_FolderLockOpen(),
			[=](QByteArray pin, Fn<void(QString)> fail) {
				Lock(controller).beginSettings(id, std::move(pin), crl::guard(
						box,
						[=](UnlockResult result, AccessMode mode) {
					if (result.error == UnlockError::None) {
						box->closeBox();
						open(mode);
					} else {
						fail(ErrorText(result));
					}
				}));
			});
	}));
}

void ShowVerifyRealPinBox(
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
						[=](UnlockResult result, int slot) {
					if (result.error == UnlockError::None && slot == 0) {
						box->closeBox();
						verified();
					} else if (result.error == UnlockError::None) {
						// The second PIN looks like a wrong one here.
						Lock(controller).countWrongPin(id);
						fail(tr::ayu_FolderLockWrongPin(tr::now));
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
		not_null<QWidget*> owner,
		Fn<void()> done) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		NewPinBox(box, tr::ayu_FolderLockSetPin(), [=](
				QByteArray pin,
				Fn<void(QString)> fail,
				Fn<void()> close) {
			const auto lock = &Lock(controller);
			auto copy = pin;
			lock->setPin(id, std::move(pin), [=](bool ok) mutable {
				if (!ok) {
					FolderLockCrypto::Wipe(copy);
					fail(tr::ayu_FolderLockFailed(tr::now));
					return;
				}
				close();
				ContinueWithPin(lock, id, std::move(copy), owner, done);
			});
		});
	}));
}

FolderEdit PrepareFolderEdit(
		not_null<Main::Session*> session,
		const Data::ChatFilter &real) {
	const auto lock = &session->data().chatsFilters().folderLock();
	const auto id = real.id();
	const auto mode = lock->settingsMode(id);
	if (!mode || *mode == AccessMode::Real) {
		return {
			.shown = real,
			.restore = [](const Data::ChatFilter &filter) { return filter; },
		};
	}
	using Flag = Data::ChatFilter::Flag;
	const auto owner = &session->data();
	auto allowedSet = std::set<uint64_t>();
	auto shownAlways = base::flat_set<not_null<History*>>();
	auto typeMatched = std::set<uint64_t>();
	auto allowedOutside = std::set<uint64_t>();
	for (const auto peerId : lock->allowedChats(id)) {
		const auto history = owner->history(peerId);
		if (real.matchesRules(history)) {
			allowedSet.insert(peerId.value);
			shownAlways.emplace(history);
			if (real.matchesByType(history)) {
				typeMatched.insert(peerId.value);
			}
		} else {
			allowedOutside.insert(peerId.value);
		}
	}
	auto shownPinned = std::vector<not_null<History*>>();
	for (const auto &history : real.pinned()) {
		if (shownAlways.contains(history)) {
			shownPinned.push_back(history);
		}
	}
	auto shown = Data::ChatFilter(
		id,
		real.title(),
		real.iconEmoji(),
		real.colorIndex(),
		real.flags() & ~Flag(Flag::RulesMask),
		shownAlways,
		shownPinned,
		{});
	const auto restore = [=](const Data::ChatFilter &edited) {
		const auto toIds = [](const auto &histories) {
			auto result = std::set<uint64_t>();
			for (const auto &history : histories) {
				result.insert(history->peer->id.value);
			}
			return result;
		};
		auto realPinned = std::vector<uint64_t>();
		for (const auto &history : real.pinned()) {
			realPinned.push_back(history->peer->id.value);
		}
		const auto merged = FolderLockMerge::MergeDecoyEdit({
			.always = toIds(real.always()),
			.never = toIds(real.never()),
			.pinned = std::move(realPinned),
		}, {
			.allowedOld = allowedSet,
			.shownAlways = toIds(edited.always()),
			.shownNever = toIds(edited.never()),
			.typeMatched = typeMatched,
			.allowedOutside = allowedOutside,
		});
		const auto toHistories = [&](const std::set<uint64_t> &ids) {
			auto result = base::flat_set<not_null<History*>>();
			for (const auto value : ids) {
				result.emplace(owner->history(PeerId(value)));
			}
			return result;
		};
		auto pinned = std::vector<not_null<History*>>();
		for (const auto value : merged.pinned) {
			pinned.push_back(owner->history(PeerId(value)));
		}
		// Chats pinned in the fake view go first, hidden pinned ones after.
		for (const auto &history : ranges::views::reverse(edited.pinned())) {
			if (!ranges::contains(pinned, history)) {
				pinned.insert(begin(pinned), history);
			}
		}
		auto allowed = std::vector<PeerId>();
		for (const auto value : merged.allowed) {
			allowed.push_back(PeerId(value));
		}
		lock->setAllowedChats(id, std::move(allowed));
		return Data::ChatFilter(
			id,
			edited.title(),
			edited.iconEmoji(),
			edited.colorIndex(),
			(real.flags() & Flag::RulesMask)
				| (edited.flags() & ~Flag(Flag::RulesMask)),
			toHistories(merged.always),
			std::move(pinned),
			toHistories(merged.never));
	};
	return { .shown = std::move(shown), .restore = restore, .fake = true };
}

void AddFolderProtectionSection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		FilterId id,
		not_null<Ui::Checkbox*> hideFromAll) {
	const auto lock = &Lock(controller);
	const auto shownProtected = [=] {
		// With a "removed" PIN the folder looks unprotected.
		return lock->isProtected(id)
			&& (lock->settingsMode(id) != AccessMode::Removed);
	};
	const auto protectedState = container->lifetime().make_state<
		rpl::variable<bool>>(shownProtected());
	const auto realState = container->lifetime().make_state<
		rpl::variable<bool>>(lock->settingsMode(id) == AccessMode::Real);
	const auto decoyState = container->lifetime().make_state<
		rpl::variable<bool>>(lock->hasDecoy(id));
	const auto hidden = container->lifetime().make_state<
		rpl::variable<bool>>(hideFromAll->checked());
	const auto autolockLabel = container->lifetime().make_state<
		rpl::variable<QString>>(AutolockText(lock->autolockMinutes(id)));
	const auto refresh = crl::guard(container, [=] {
		*protectedState = shownProtected();
		*realState = (lock->settingsMode(id) == AccessMode::Real);
		*decoyState = lock->hasDecoy(id);
		*autolockLabel = AutolockText(lock->autolockMinutes(id));
	});
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

	// Second bottom: only with the real PIN.
	const auto decoyWrap = manage->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			manage,
			object_ptr<Ui::VerticalLayout>(manage)));
	const auto decoy = decoyWrap->entity();
	Ui::AddSkip(decoy);
	Ui::AddSubsectionTitle(decoy, tr::ayu_FolderLockDecoy());
	const auto setupWrap = decoy->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			decoy,
			object_ptr<Ui::VerticalLayout>(decoy)));
	const auto setup = Settings::AddButtonWithIcon(
		setupWrap->entity(),
		tr::ayu_FolderLockDecoySetup(),
		st::settingsButtonNoIcon);
	const auto decoyManageWrap = decoy->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			decoy,
			object_ptr<Ui::VerticalLayout>(decoy)));
	const auto decoyManage = decoyManageWrap->entity();
	const auto decoyChange = Settings::AddButtonWithIcon(
		decoyManage,
		tr::ayu_FolderLockDecoyChangePin(),
		st::settingsButtonNoIcon);
	const auto decoyChats = Settings::AddButtonWithIcon(
		decoyManage,
		tr::ayu_FolderLockDecoyChats(),
		st::settingsButtonNoIcon);
	const auto decoyOff = Settings::AddButtonWithIcon(
		decoyManage,
		tr::ayu_FolderLockDecoyOff(),
		st::settingsAttentionButton);
	Ui::AddDividerText(decoy, tr::ayu_FolderLockDecoyAbout());

	hideFromAll->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (!checked
			&& lock->isProtected(id)
			&& lock->settingsMode(id) != AccessMode::Removed) {
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
	decoyWrap->toggleOn(realState->value());
	setupWrap->toggleOn(decoyState->value() | rpl::map(!rpl::mappers::_1));
	decoyManageWrap->toggleOn(decoyState->value());

	const auto failText = tr::ayu_FolderLockFailed(tr::now);
	setPin->setClickedCallback([=] {
		if (lock->isPinRemoved(id)
			&& lock->settingsMode(id) != AccessMode::Removed) {
			// The access was dropped (locked meanwhile), take it again.
			lock->beginRemovedSettings(id);
			lock->attachSettings(id);
		}
		if (lock->isProtected(id)
			&& lock->settingsMode(id) != AccessMode::Removed) {
			controller->showToast(failText);
			return;
		}
		if (lock->settingsMode(id) == AccessMode::Removed) {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				NewPinBox(box, tr::ayu_FolderLockSetPin(), [=](
						QByteArray pin,
						Fn<void(QString)> fail,
						Fn<void()> close) {
					auto copy = pin;
					lock->setPinFromRemoved(id, std::move(pin), [=](
							bool ok) mutable {
						if (!ok) {
							FolderLockCrypto::Wipe(copy);
							fail(failText);
							return;
						}
						close();
						ContinueWithPin(
							lock,
							id,
							std::move(copy),
							container,
							refresh);
					});
				});
			}));
			return;
		}
		ShowSetFolderPinBox(controller, id, container, [=] {
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
		const auto mode = lock->settingsMode(id);
		if (!mode || *mode == AccessMode::Removed) {
			return;
		}
		const auto decoyMode = (*mode == AccessMode::Decoy);
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			NewPinBox(box, tr::ayu_FolderLockChangePin(), [=](
					QByteArray pin,
					Fn<void(QString)> fail,
					Fn<void()> close) {
				const auto done = [=](bool ok, bool same) {
					if (ok) {
						close();
						refresh();
					} else if (same) {
						fail(decoyMode
							? failText
							: tr::ayu_FolderLockRealSame(tr::now));
					} else {
						fail(failText);
					}
				};
				if (decoyMode) {
					lock->changeDecoyPin(id, std::move(pin), done);
				} else {
					lock->changeRealPin(id, std::move(pin), done);
				}
			});
		}));
	});
	remove->setClickedCallback([=] {
		const auto mode = lock->settingsMode(id);
		if (!mode || *mode == AccessMode::Removed) {
			return;
		}
		const auto decoyMode = (*mode == AccessMode::Decoy);
		controller->show(Ui::MakeConfirmBox({
			.text = tr::ayu_FolderLockRemovePin(),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				if (decoyMode) {
					// Looks like the PIN is gone, the hidden part stays.
					lock->imitateRemove(id, [=](bool) { refresh(); });
				} else {
					lock->removeRealPin(id);
					refresh();
				}
			},
			.confirmText = tr::ayu_FolderLockRemovePin(),
			.confirmStyle = &st::attentionBoxButton,
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
	setup->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			NewPinBox(box, tr::ayu_FolderLockDecoyPin(), [=](
					QByteArray pin,
					Fn<void(QString)> fail,
					Fn<void()> close) {
				lock->setupDecoy(id, std::move(pin), {}, [=](
						bool ok,
						bool same) {
					if (same) {
						fail(tr::ayu_FolderLockDecoySame(tr::now));
					} else if (!ok) {
						fail(failText);
					} else {
						close();
						refresh();
						ShowAllowedChatsBox(controller, id, {}, [=](
								std::vector<PeerId> allowed) {
							return lock->setAllowedChats(id, std::move(allowed));
						});
					}
				});
			});
		}));
	});
	decoyChange->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			NewPinBox(box, tr::ayu_FolderLockDecoyChangePin(), [=](
					QByteArray pin,
					Fn<void(QString)> fail,
					Fn<void()> close) {
				lock->changeDecoyPin(id, std::move(pin), [=](
						bool ok,
						bool same) {
					if (ok) {
						close();
					} else if (same) {
						fail(tr::ayu_FolderLockDecoySame(tr::now));
					} else {
						fail(failText);
					}
				});
			});
		}));
	});
	decoyChats->setClickedCallback([=] {
		ShowAllowedChatsBox(controller, id, lock->allowedChats(id), [=](
				std::vector<PeerId> allowed) {
			return lock->setAllowedChats(id, std::move(allowed));
		});
	});
	decoyOff->setClickedCallback([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::ayu_FolderLockDecoyOff(),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				lock->disableDecoy(id);
				refresh();
			},
			.confirmText = tr::ayu_FolderLockDecoyOff(),
			.confirmStyle = &st::attentionBoxButton,
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
			window->setActiveChatsFilter(
				session->data().chatsFilters().defaultId());
		}
	}
}

} // namespace Ayu
