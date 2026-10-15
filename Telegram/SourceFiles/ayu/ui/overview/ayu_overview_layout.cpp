// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/overview/ayu_overview_layout.h"

#include "base/unixtime.h"
#include "core/click_handler_types.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "history/view/history_view_cursor_state.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "overview/overview_layout_delegate.h"
#include "ui/image/image.h"
#include "ui/text/format_song_document_name.h"
#include "ui/text/format_values.h"
#include "ui/text/text_options.h"
#include "ui/painter.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_chat_style.h"
#include "styles/style_overview.h"

namespace Overview::Layout {
namespace {

using TextState = HistoryView::TextState;

[[nodiscard]] bool Playing(
		not_null<DocumentData*> document,
		FullMsgId context) {
	const auto state = Media::Player::instance()->getState(
		AudioMsgId::Type::Voice);
	return (state.id
			== AudioMsgId(document, context, state.id.externalPlayId()))
		&& !Media::Player::IsStoppedOrStopping(state.state);
}

// Cover crop of an image into `size`, cached by the Image itself.
void PaintCover(
		QPainter &p,
		not_null<Image*> image,
		QRect rect,
		Images::Options options) {
	const auto width = std::max(image->width(), 1);
	const auto height = std::max(image->height(), 1);
	const auto scale = std::max(
		rect.width() / float64(width),
		rect.height() / float64(height));
	const auto scaled = QSize(
		std::max(int(std::ceil(width * scale)), rect.width()),
		std::max(int(std::ceil(height * scale)), rect.height()));
	const auto &pix = image->pix(scaled, { .options = options });
	p.save();
	p.setClipRect(rect);
	p.drawPixmap(
		rect.x() + (rect.width() - scaled.width()) / 2,
		rect.y() + (rect.height() - scaled.height()) / 2,
		pix);
	p.restore();
}

} // namespace

FileTile::FileTile(
	not_null<Delegate*> delegate,
	not_null<HistoryItem*> parent,
	not_null<DocumentData*> document)
: RadialProgressItem(delegate, parent)
, _data(document)
, _generic(::Layout::DocumentGenericPreview::Create(_data))
, _namel(std::make_shared<DocumentOpenClickHandler>(
	_data,
	crl::guard(this, [=](FullMsgId id) {
		delegate->openDocument(_data, id);
	}),
	parent->fullId()))
, _ext(_generic.ext) {
	AddComponents(Info::Bit());
	setDocumentLinks(_data);
	_name.setText(
		st::semiboldTextStyle,
		Ui::Text::FormatDownloadsName(_data).text,
		Ui::NameTextOptions());
	_status.update(Ui::FileStatusSizeReady, _data->size, -1, 0);
	if (_data->hasThumbnail()) {
		_data->loadThumbnail(parent->fullId());
	}
}

void FileTile::initDimensions() {
	_maxw = st::ayuFileTileMaxSize;
	_minh = st::ayuFileTileMinSize;
}

int32 FileTile::resizeGetHeight(int32 width) {
	width = std::min(width, _maxw);
	if (_width != width) {
		_width = width;
		_height = width;
		_thumb = QPixmap();
	}
	return _height;
}

void FileTile::ensureDataMediaCreated() const {
	if (_dataMedia) {
		return;
	}
	_dataMedia = _data->createMediaView();
	_dataMedia->thumbnailWanted(parent()->fullId());
	delegate()->registerHeavyItem(this);
}

void FileTile::clearHeavyPart() {
	_dataMedia = nullptr;
	_thumb = QPixmap();
	_thumbLoaded = false;
}

void FileTile::validateThumbnail() {
	if (!_data->hasThumbnail()) {
		return;
	}
	ensureDataMediaCreated();
	const auto thumbnail = _dataMedia->thumbnail();
	const auto blurred = _dataMedia->thumbnailInline();
	const auto image = thumbnail ? thumbnail : blurred;
	if (!image || (!_thumb.isNull() && (_thumbLoaded || !thumbnail))) {
		return;
	}
	_thumbLoaded = (thumbnail != nullptr);
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		QSize(_width, _height) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);
	{
		auto q = QPainter(&result);
		PaintCover(
			q,
			image,
			QRect(0, 0, _width, _height),
			_thumbLoaded ? Images::Options() : Images::Option::Blur);
	}
	_thumb = QPixmap::fromImage(std::move(result));
}

void FileTile::paintBackground(QPainter &p, QRect rect) {
	validateThumbnail();
	if (!_thumb.isNull()) {
		p.drawPixmap(rect.topLeft(), _thumb);
		return;
	}
	p.fillRect(rect, _generic.color);
	if (!_ext.isEmpty()) {
		const auto &font = st::ayuFileTileExtFont;
		const auto available = rect.width() - 2 * st::overviewFileExtPadding;
		const auto ext = font->elided(_ext, available, Qt::ElideMiddle);
		p.setFont(font);
		p.setPen(st::overviewFileExtFg);
		const auto plate = st::ayuFileTilePadding.top()
			+ st::semiboldFont->height
			+ st::normalFont->height
			+ st::ayuFileTilePadding.bottom();
		p.drawText(
			QRect(rect.x(), rect.y(), rect.width(), rect.height() - plate),
			Qt::AlignCenter,
			ext);
	}
}

bool FileTile::showButton() const {
	return !dataLoaded() || _data->loading() || _data->uploading();
}

QRect FileTile::buttonRect() const {
	const auto size = st::ayuFileTileButtonSize;
	return QRect(
		(_width - size) / 2,
		(_height - size) / 2 - st::normalFont->height,
		size,
		size);
}

void FileTile::paint(
		Painter &p,
		const QRect &clip,
		TextSelection selection,
		const PaintContext *context) {
	ensureDataMediaCreated();
	const auto selected = (selection == FullSelection);
	const auto displayLoading = _data->displayLoading();
	if (displayLoading) {
		ensureRadial();
		if (!_radial->animating()) {
			_radial->start(dataProgress());
		}
	}
	updateStatusText();
	const auto radial = isRadialAnimation();
	const auto rect = QRect(0, 0, _width, _height);

	paintBackground(p, rect);

	// Name and status on a plate at the bottom.
	const auto &padding = st::ayuFileTilePadding;
	const auto plateHeight = padding.top()
		+ st::semiboldFont->height
		+ st::normalFont->height
		+ padding.bottom();
	const auto plate = QRect(0, _height - plateHeight, _width, plateHeight);
	if (clip.intersects(plate)) {
		p.fillRect(plate, st::msgDateImgBg);
		const auto available = _width - padding.left() - padding.right();
		p.setPen(st::msgDateImgFg);
		_name.drawLeftElided(
			p,
			padding.left(),
			plate.y() + padding.top(),
			available,
			_width);
		p.setFont(st::normalFont);
		p.drawTextLeft(
			padding.left(),
			plate.y() + padding.top() + st::semiboldFont->height,
			_width,
			st::normalFont->elided(_status.text(), available));
	}

	if (showButton() || radial) {
		const auto inner = buttonRect();
		const auto loading = _data->loading() || _data->uploading();
		p.setPen(Qt::NoPen);
		if (selected) {
			p.setBrush(st::msgDateImgBgSelected);
		} else {
			const auto over = ClickHandler::showAsActive(
				loading ? _cancell : _savel);
			p.setBrush(anim::brush(
				st::msgDateImgBg,
				st::msgDateImgBgOver,
				_a_iconOver.value(over ? 1. : 0.)));
		}
		{
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(inner);
		}
		const auto &icon = loading
			? (selected
				? st::historyFileThumbCancelSelected
				: st::historyFileThumbCancel)
			: (selected
				? st::historyFileThumbDownloadSelected
				: st::historyFileThumbDownload);
		icon.paintInCenter(p, inner);
		if (radial) {
			const auto line = st::msgFileRadialLine;
			_radial->draw(
				p,
				inner.marginsRemoved({ line, line, line, line }),
				line,
				selected
					? st::historyFileThumbRadialFgSelected
					: st::historyFileThumbRadialFg);
		}
	}

	if (selected) {
		p.fillRect(rect, st::overviewPhotoSelectOverlay);
	}
	const auto checkDelta = st::overviewCheckSkip + st::overviewCheck.size;
	paintCheckbox(
		p,
		{ _width - checkDelta, st::overviewCheckSkip },
		selected,
		context);
}

bool FileTile::elementsAnimating() const {
	return RadialProgressItem::elementsAnimating();
}

TextState FileTile::getState(
		QPoint point,
		StateRequest request) const {
	if (!QRect(0, 0, _width, _height).contains(point)) {
		return {};
	}
	ensureDataMediaCreated();
	const auto loading = _data->loading() || _data->uploading();
	if (loading) {
		return { parent(), _cancell };
	} else if (!dataLoaded() && buttonRect().contains(point)) {
		return { parent(), _savel };
	}
	return { parent(), _namel };
}

QImage FileTile::dragPreviewImage() {
	const auto size = std::max(st::ayuFileTileMinSize, 1);
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		QSize(size, size) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	if (!_thumb.isNull()) {
		p.drawPixmap(QRect(0, 0, size, size), _thumb);
	} else {
		p.fillRect(QRect(0, 0, size, size), _generic.color);
	}
	return result;
}

float64 FileTile::dataProgress() const {
	ensureDataMediaCreated();
	return _dataMedia->progress();
}

bool FileTile::dataFinished() const {
	return !_data->loading();
}

bool FileTile::dataLoaded() const {
	ensureDataMediaCreated();
	return _dataMedia->loaded();
}

bool FileTile::iconAnimated() const {
	return true;
}

void FileTile::updateStatusText() {
	auto statusSize = int64();
	if (_data->status == FileDownloadFailed
		|| _data->status == FileUploadFailed) {
		statusSize = Ui::FileStatusSizeFailed;
	} else if (_data->uploading()) {
		statusSize = _data->uploadingData->offset;
	} else if (_data->loading()) {
		statusSize = _data->loadOffset();
	} else if (dataLoaded()) {
		statusSize = Ui::FileStatusSizeLoaded;
	} else {
		statusSize = Ui::FileStatusSizeReady;
	}
	if (statusSize != _status.size()) {
		_status.update(statusSize, _data->size, -1, 0);
	}
}

VoiceMessage::VoiceMessage(
	not_null<Delegate*> delegate,
	not_null<HistoryItem*> parent,
	not_null<DocumentData*> document)
: RadialProgressItem(delegate, parent)
, _data(document) {
	AddComponents(Info::Bit());
	setDocumentLinks(_data);
	_data->loadThumbnail(parent->fullId());
	updateName();
	const auto dateText = tr::link(
		langDateTime(base::unixtime::parse(parent->date()))); // Link 1.
	_details.setMarkedText(st::defaultTextStyle, dateText);
	_details.setLink(1, JumpToMessageClickHandler(parent));
}

bool VoiceMessage::round() const {
	return _data->isVideoMessage();
}

int VoiceMessage::roundSize() const {
	const auto &padding = st::ayuVoicePadding;
	return std::max(
		std::min(
			st::maxVideoMessageSize,
			_width - padding.left() - padding.right()),
		st::ayuVoiceButtonSize);
}

void VoiceMessage::initDimensions() {
	const auto &padding = st::ayuVoicePadding;
	_maxw = round()
		? (padding.left() + st::maxVideoMessageSize + padding.right())
		: (padding.left()
			+ st::ayuVoiceButtonSize
			+ st::ayuVoiceTextSkip
			+ st::ayuVoiceWaveformWidth
			+ padding.right());
	_minh = padding.top() + st::ayuVoiceButtonSize + padding.bottom();
}

int32 VoiceMessage::resizeGetHeight(int32 width) {
	_width = width;
	const auto &padding = st::ayuVoicePadding;
	if (round()) {
		_height = padding.top()
			+ roundSize()
			+ st::ayuVoiceLineSkip
			+ st::semiboldFont->height
			+ padding.bottom();
	} else {
		const auto text = st::semiboldFont->height
			+ st::ayuVoiceLineSkip
			+ st::msgWaveformMax
			+ st::ayuVoiceLineSkip
			+ st::normalFont->height;
		_height = padding.top()
			+ std::max(st::ayuVoiceButtonSize, text)
			+ padding.bottom();
	}
	return _height;
}

QRect VoiceMessage::roundRect() const {
	const auto &padding = st::ayuVoicePadding;
	const auto size = roundSize();
	return QRect(padding.left(), padding.top(), size, size);
}

QRect VoiceMessage::buttonRect() const {
	const auto size = st::ayuVoiceButtonSize;
	if (round()) {
		const auto circle = roundRect();
		return QRect(
			circle.x() + (circle.width() - size) / 2,
			circle.y() + (circle.height() - size) / 2,
			size,
			size);
	}
	const auto &padding = st::ayuVoicePadding;
	return QRect(padding.left(), padding.top(), size, size);
}

int VoiceMessage::textLeft() const {
	return round()
		? st::ayuVoicePadding.left()
		: (st::ayuVoicePadding.left()
			+ st::ayuVoiceButtonSize
			+ st::ayuVoiceTextSkip);
}

int VoiceMessage::textTop() const {
	return round()
		? (roundRect().y() + roundRect().height() + st::ayuVoiceLineSkip)
		: st::ayuVoicePadding.top();
}

ClickHandlerPtr VoiceMessage::buttonLink() const {
	ensureDataMediaCreated();
	return (_data->loading() || _data->uploading())
		? _cancell
		: (_dataMedia->canBePlayed() || _dataMedia->loaded())
		? _openl
		: _savel;
}

void VoiceMessage::ensureDataMediaCreated() const {
	if (_dataMedia) {
		return;
	}
	_dataMedia = _data->createMediaView();
	delegate()->registerHeavyItem(this);
}

void VoiceMessage::clearHeavyPart() {
	_dataMedia = nullptr;
}

void VoiceMessage::updateName() {
	_name.setText(
		st::semiboldTextStyle,
		parent()->fromOriginal()->name(),
		Ui::NameTextOptions());
	_nameVersion = parent()->fromOriginal()->nameVersion();
}

bool VoiceMessage::updateStatusText(float64 &progress) {
	auto showPause = false;
	auto statusSize = int64();
	auto realDuration = TimeId();
	progress = 0.;
	if (_data->status == FileDownloadFailed
		|| _data->status == FileUploadFailed) {
		statusSize = Ui::FileStatusSizeFailed;
	} else if (_data->loading()) {
		statusSize = _data->loadOffset();
	} else if (dataLoaded()) {
		statusSize = Ui::FileStatusSizeLoaded;
	} else {
		statusSize = Ui::FileStatusSizeReady;
	}
	const auto state = Media::Player::instance()->getState(
		AudioMsgId::Type::Voice);
	if (state.id == AudioMsgId(_data, parent()->fullId(), state.id.externalPlayId())
		&& !Media::Player::IsStoppedOrStopping(state.state)) {
		statusSize = -1 - (state.position / state.frequency);
		realDuration = (state.length / state.frequency);
		showPause = Media::Player::ShowPauseIcon(state.state);
		progress = (state.length > 0)
			? std::clamp(state.position / float64(state.length), 0., 1.)
			: 0.;
	}
	if (statusSize != _status.size()) {
		_status.update(
			statusSize,
			_data->size,
			_data->duration() / 1000,
			realDuration);
	}
	return showPause;
}

void VoiceMessage::paintButton(
		QPainter &p,
		QRect inner,
		bool selected,
		bool showPause,
		bool overMedia) {
	const auto loading = _data->loading() || _data->uploading();
	const auto link = buttonLink();
	const auto over = ClickHandler::showAsActive(link);
	p.setPen(Qt::NoPen);
	if (overMedia) {
		p.setBrush(selected
			? st::msgDateImgBgSelected
			: anim::brush(
				st::msgDateImgBg,
				st::msgDateImgBgOver,
				_a_iconOver.value(over ? 1. : 0.)));
	} else {
		p.setBrush(selected
			? st::msgFileInBgSelected
			: anim::brush(
				st::msgFileInBg,
				st::msgFileInBgOver,
				_a_iconOver.value(over ? 1. : 0.)));
	}
	{
		PainterHighQualityEnabler hq(p);
		p.drawEllipse(inner);
	}
	if (isRadialAnimation()) {
		const auto line = st::msgFileRadialLine;
		_radial->draw(
			p,
			inner.marginsRemoved({ line, line, line, line }),
			line,
			selected
				? st::historyFileInRadialFgSelected
				: st::historyFileInRadialFg);
	}
	const auto &layout = st::overviewFileLayout;
	const auto icon = [&] {
		if (loading) {
			return &(selected ? layout.voiceCancelSelected : layout.voiceCancel);
		} else if (showPause) {
			return &(selected ? layout.voicePauseSelected : layout.voicePause);
		} else if (_dataMedia->canBePlayed() || _dataMedia->loaded()) {
			return &(selected ? layout.voicePlaySelected : layout.voicePlay);
		}
		return &(selected
			? layout.voiceDownloadSelected
			: layout.voiceDownload);
	}();
	icon->paintInCenter(p, inner);
}

void VoiceMessage::paintRound(QPainter &p, bool selected, bool showPause) {
	const auto circle = roundRect();
	const auto thumbnail = _dataMedia->thumbnail();
	const auto blurred = _dataMedia->thumbnailInline();
	p.setPen(Qt::NoPen);
	if (thumbnail || blurred) {
		const auto options = Images::Option::RoundCircle
			| (thumbnail ? Images::Options() : Images::Option::Blur);
		p.drawPixmap(
			circle.topLeft(),
			(thumbnail ? thumbnail : blurred)->pix(
				circle.size(),
				{ .options = options }));
	} else {
		PainterHighQualityEnabler hq(p);
		p.setBrush(st::imageBg);
		p.drawEllipse(circle);
	}
	if (selected) {
		PainterHighQualityEnabler hq(p);
		p.setBrush(st::overviewPhotoSelectOverlay);
		p.drawEllipse(circle);
	}
	paintButton(p, buttonRect(), selected, showPause, true);

	// Duration (or played time) like in a chat.
	const auto &padding = st::ayuRoundStatusPadding;
	const auto text = _status.text();
	const auto width = st::normalFont->width(text);
	const auto status = QRect(
		circle.x() + st::ayuRoundStatusSkip,
		circle.y() + circle.height()
			- st::ayuRoundStatusSkip
			- padding.top()
			- st::normalFont->height
			- padding.bottom(),
		padding.left() + width + padding.right(),
		padding.top() + st::normalFont->height + padding.bottom());
	{
		PainterHighQualityEnabler hq(p);
		p.setBrush(st::msgDateImgBg);
		const auto radius = status.height() / 2.;
		p.drawRoundedRect(status, radius, radius);
	}
	p.setFont(st::normalFont);
	p.setPen(st::msgDateImgFg);
	p.drawText(
		status.x() + padding.left(),
		status.y() + padding.top() + st::normalFont->ascent,
		text);
}

void VoiceMessage::paintWaveform(
		QPainter &p,
		QRect rect,
		float64 progress,
		bool selected) const {
	const auto voice = _data->voice();
	const auto wf = (voice
		&& !voice->waveform.isEmpty()
		&& voice->waveform.at(0) >= 0)
		? &voice->waveform
		: nullptr;
	const auto count = wf
		? int(wf->size())
		: ::Media::Player::kWaveformSamplesCount;
	const auto bar = st::msgWaveformBar;
	const auto step = bar + st::msgWaveformSkip;
	const auto bars = std::max(std::min(rect.width() / step, count), 1);
	const auto norm = (wf ? voice->wavemax : 0) + 1;
	const auto delta = st::msgWaveformMax - st::msgWaveformMin;
	const auto active = selected
		? st::msgWaveformInActiveSelected
		: st::msgWaveformInActive;
	const auto inactive = selected
		? st::msgWaveformInInactiveSelected
		: st::msgWaveformInInactive;
	const auto activeWidth = rect.width() * progress;
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	for (auto i = 0; i != bars; ++i) {
		// The loudest sample of the samples behind this bar.
		const auto from = (i * count) / bars;
		const auto till = std::max(((i + 1) * count) / bars, from + 1);
		auto value = 0;
		for (auto j = from; j != till && wf; ++j) {
			value = std::max(value, int(wf->at(j)));
		}
		const auto height = st::msgWaveformMin
			+ ((value * delta) + (norm / 2)) / norm;
		const auto left = rect.x() + i * step;
		const auto top = rect.y() + (st::msgWaveformMax - height) / 2;
		p.setBrush((left - rect.x() < activeWidth) ? active : inactive);
		p.drawRoundedRect(
			QRectF(left, top, bar, height),
			bar / 2.,
			bar / 2.);
	}
}

void VoiceMessage::paint(
		Painter &p,
		const QRect &clip,
		TextSelection selection,
		const PaintContext *context) {
	ensureDataMediaCreated();
	const auto selected = (selection == FullSelection);
	if (_data->displayLoading()) {
		ensureRadial();
		if (!_radial->animating()) {
			_radial->start(dataProgress());
		}
	}
	auto progress = 0.;
	const auto showPause = updateStatusText(progress);
	if (_nameVersion < parent()->fromOriginal()->nameVersion()) {
		updateName();
	}
	const auto left = textLeft();
	const auto available = std::max(
		_width - left - st::ayuVoicePadding.right(),
		1);
	auto top = textTop();

	if (round()) {
		paintRound(p, selected, showPause);
	} else {
		paintButton(p, buttonRect(), selected, showPause, false);
	}

	// Sender and date.
	p.setPen(st::historyFileNameInFg);
	const auto nameWidth = std::min(available, _name.maxWidth());
	_name.drawLeftElided(p, left, top, nameWidth, _width);
	const auto dateLeft = left + nameWidth + st::ayuVoiceTextSkip;
	if (dateLeft < left + available) {
		p.setPen(selected ? st::mediaInFgSelected : st::mediaInFg);
		p.setTextPalette(selected
			? st::mediaInPaletteSelected
			: st::mediaInPalette);
		_details.drawLeftElided(
			p,
			dateLeft,
			top + (st::semiboldFont->height - st::normalFont->height),
			left + available - dateLeft,
			_width);
		p.restoreTextPalette();
	}

	if (!round()) {
		top += st::semiboldFont->height + st::ayuVoiceLineSkip;
		const auto waveWidth = std::min(available, st::ayuVoiceWaveformWidth);
		paintWaveform(
			p,
			QRect(left, top, waveWidth, st::msgWaveformMax),
			progress,
			selected);
		top += st::msgWaveformMax + st::ayuVoiceLineSkip;
		p.setFont(st::normalFont);
		p.setPen(selected ? st::mediaInFgSelected : st::mediaInFg);
		const auto status = _status.text();
		p.drawTextLeft(left, top, _width, status);
		if (parent()->hasUnreadMediaFlag()) {
			const auto x = left
				+ st::normalFont->width(status)
				+ st::mediaUnreadSkip;
			p.setPen(Qt::NoPen);
			p.setBrush(selected ? st::msgFileInBgSelected : st::msgFileInBg);
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(QRect(
				x,
				top + st::mediaUnreadTop,
				st::mediaUnreadSize,
				st::mediaUnreadSize));
		}
	}

	const auto button = round() ? roundRect() : buttonRect();
	const auto checkDelta = st::overviewCheckSkip + st::overviewSmallCheck.size;
	paintCheckbox(
		p,
		{
			button.x() + button.width() - checkDelta,
			button.y() + button.height() - checkDelta,
		},
		selected,
		context);
}

bool VoiceMessage::elementsAnimating() const {
	return RadialProgressItem::elementsAnimating()
		|| _details.hasPersistentAnimation()
		|| Playing(_data, parent()->fullId());
}

TextState VoiceMessage::getState(
		QPoint point,
		StateRequest request) const {
	const auto media = round() ? roundRect() : buttonRect();
	if (round()) {
		const auto center = QPointF(media.center());
		const auto radius = media.width() / 2.;
		const auto dx = point.x() - center.x();
		const auto dy = point.y() - center.y();
		if (dx * dx + dy * dy <= radius * radius) {
			return { parent(), buttonLink() };
		}
	} else if (media.contains(point)) {
		return { parent(), buttonLink() };
	}
	auto result = TextState(parent());
	const auto left = textLeft();
	const auto top = textTop();
	const auto available = _width - left - st::ayuVoicePadding.right();
	const auto nameWidth = std::min(available, _name.maxWidth());
	const auto dateLeft = left + nameWidth + st::ayuVoiceTextSkip;
	const auto dateTop = top
		+ (st::semiboldFont->height - st::normalFont->height);
	if (QRect(dateLeft, dateTop, _details.maxWidth(), st::normalFont->height)
		.contains(point)) {
		const auto state = _details.getStateLeft(
			point - QPoint(dateLeft, dateTop),
			_width,
			_width);
		result.link = state.link;
		result.cursor = state.uponSymbol
			? HistoryView::CursorState::Text
			: HistoryView::CursorState::None;
	}
	return result;
}

bool VoiceMessage::selectionConsumesClick(QPoint point) const {
	const auto media = round() ? roundRect() : buttonRect();
	return !media.contains(point);
}

float64 VoiceMessage::dataProgress() const {
	ensureDataMediaCreated();
	return _dataMedia->progress();
}

bool VoiceMessage::dataFinished() const {
	return !_data->loading();
}

bool VoiceMessage::dataLoaded() const {
	ensureDataMediaCreated();
	return _dataMedia->loaded();
}

bool VoiceMessage::iconAnimated() const {
	return true;
}

const style::RoundCheckbox &VoiceMessage::checkboxStyle() const {
	return st::overviewSmallCheck;
}

} // namespace Overview::Layout
