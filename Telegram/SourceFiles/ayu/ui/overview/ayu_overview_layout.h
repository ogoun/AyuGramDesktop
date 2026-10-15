// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "overview/overview_layout.h"

// AyuGram: shared media items shown at full size. Files are tiles in the
// grid like photos, voice and video messages are rows like in a chat.
namespace Overview::Layout {

class FileTile final : public RadialProgressItem {
public:
	FileTile(
		not_null<Delegate*> delegate,
		not_null<HistoryItem*> parent,
		not_null<DocumentData*> document);

	void initDimensions() override;
	int32 resizeGetHeight(int32 width) override;
	void paint(
		Painter &p,
		const QRect &clip,
		TextSelection selection,
		const PaintContext *context) override;
	[[nodiscard]] bool elementsAnimating() const override;
	TextState getState(
		QPoint point,
		StateRequest request) const override;

	[[nodiscard]] DocumentData *getDocument() const override {
		return _data;
	}
	[[nodiscard]] bool ayuGridTile() const override {
		return true;
	}

	void clearHeavyPart() override;
	[[nodiscard]] QImage dragPreviewImage();

protected:
	float64 dataProgress() const override;
	bool dataFinished() const override;
	bool dataLoaded() const override;
	bool iconAnimated() const override;

private:
	void ensureDataMediaCreated() const;
	void validateThumbnail();
	void updateStatusText();
	[[nodiscard]] QRect buttonRect() const;
	[[nodiscard]] bool showButton() const;
	void paintBackground(QPainter &p, QRect rect);

	const not_null<DocumentData*> _data;
	mutable std::shared_ptr<Data::DocumentMedia> _dataMedia;
	const ::Layout::DocumentGenericPreview _generic;
	StatusText _status;
	ClickHandlerPtr _namel;
	Ui::Text::String _name;
	QString _ext;
	QPixmap _thumb;
	bool _thumbLoaded = false;

};

class VoiceMessage final : public RadialProgressItem {
public:
	VoiceMessage(
		not_null<Delegate*> delegate,
		not_null<HistoryItem*> parent,
		not_null<DocumentData*> document);

	void initDimensions() override;
	int32 resizeGetHeight(int32 width) override;
	void paint(
		Painter &p,
		const QRect &clip,
		TextSelection selection,
		const PaintContext *context) override;
	[[nodiscard]] bool elementsAnimating() const override;
	TextState getState(
		QPoint point,
		StateRequest request) const override;
	[[nodiscard]] bool selectionConsumesClick(QPoint point) const override;

	[[nodiscard]] DocumentData *getDocument() const override {
		return _data;
	}

	void clearHeavyPart() override;

protected:
	float64 dataProgress() const override;
	bool dataFinished() const override;
	bool dataLoaded() const override;
	bool iconAnimated() const override;
	const style::RoundCheckbox &checkboxStyle() const override;

private:
	[[nodiscard]] bool round() const;
	[[nodiscard]] int roundSize() const;
	[[nodiscard]] QRect buttonRect() const;
	[[nodiscard]] QRect roundRect() const;
	[[nodiscard]] int textTop() const;
	[[nodiscard]] int textLeft() const;
	[[nodiscard]] ClickHandlerPtr buttonLink() const;
	void ensureDataMediaCreated() const;
	void updateName();
	// Returns true if the pause icon should be shown, `progress` is the
	// played part of the message.
	bool updateStatusText(float64 &progress);
	void paintRound(QPainter &p, bool selected, bool showPause);
	void validateRoundThumbnail();
	void paintButton(
		QPainter &p,
		QRect inner,
		bool selected,
		bool showPause,
		bool overMedia);
	void paintWaveform(
		QPainter &p,
		QRect rect,
		float64 progress,
		bool selected) const;

	const not_null<DocumentData*> _data;
	mutable std::shared_ptr<Data::DocumentMedia> _dataMedia;
	StatusText _status;
	Ui::Text::String _name;
	Ui::Text::String _details;
	Ui::Text::String _caption;
	QPixmap _roundThumb;
	QImage _roundMask;
	bool _roundThumbGood = false;
	int _nameVersion = 0;

};

} // namespace Overview::Layout
